/*
*
*    This program is free software; you can redistribute it and/or modify it
*    under the terms of the GNU General Public License as published by the
*    Free Software Foundation; either version 2 of the License, or (at
*    your option) any later version.
*
*    This program is distributed in the hope that it will be useful, but
*    WITHOUT ANY WARRANTY; without even the implied warranty of
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
*    General Public License for more details.
*
*    You should have received a copy of the GNU General Public License
*    along with this program; if not, write to the Free Software Foundation,
*    Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*
*    In addition, as a special exception, the author gives permission to
*    link the code of this program with the Half-Life Game Engine ("HL
*    Engine") and Modified Game Libraries ("MODs") developed by Valve,
*    L.L.C ("Valve").  You must obey the GNU General Public License in all
*    respects for all of the code used other than the HL Engine and MODs
*    from Valve.  If you modify this file, you may extend this exception
*    to your version of the file, but you are not obligated to do so.  If
*    you do not wish to do so, delete this exception statement from your
*    version.
*
*/

// Xash3D master server announcer.
//
// Protocol (mirrors xash3d-fwgs masterlist.c / sv_main.c, and the
// xash3d-master Rust reference implementation):
//
//   server -> master   "q\xff" + u32le server_challenge        (heartbeat)
//   master -> server   "\xff\xff\xff\xffs\n" + u32le master_challenge
//                                            + u32le server_challenge echo
//   server -> master   "0\n" + infostring (\protocol\...\quic\1)
//   server -> master   "b\n"                                   (shutdown)
//
// The master challenge arrives 0xFFFFFFFF-framed, so it flows through
// SV_ConnectionlessPacket; the other packets are raw UDP payloads.

#include "precompiled.h"

#ifdef REHLDS_QUIC

#include "net_webtransport.h"
#include "sv_xashmaster.h"

// Address of the xash3d-master to announce to ("host:port", empty = disabled).
// QUIC capability is reported from the WebTransport server state.
cvar_t sv_xashmaster = { "sv_xashmaster", "", 0, 0.0f, NULL };

// Heartbeat every 5 minutes (matches xash3d-fwgs HEARTBEAT_SECONDS, non-NAT)
const double XASHMASTER_HEARTBEAT_SECONDS = 300.0;

static double s_last_heartbeat = -99999.0;
static uint32 s_heartbeat_challenge;
static netadr_t s_master_adr;
static qboolean s_master_resolved;
static char s_master_str[256];

/*
==================
XashMaster_Resolve

Resolve the cvar address; re-resolve when the cvar changes.
==================
*/
static qboolean XashMaster_Resolve()
{
	if (!sv_xashmaster.string[0])
	{
		s_master_resolved = FALSE;
		s_master_str[0] = '\0';
		return FALSE;
	}

	if (s_master_resolved && !Q_strcmp(s_master_str, sv_xashmaster.string))
		return TRUE;

	Q_memset(&s_master_adr, 0, sizeof(s_master_adr));
	if (!NET_StringToAdr(sv_xashmaster.string, &s_master_adr))
	{
		Con_Printf("XashMaster: couldn't resolve %s\n", sv_xashmaster.string);
		s_master_resolved = FALSE;
		return FALSE;
	}

	// xash3d-master default port
	if (!s_master_adr.port)
		s_master_adr.port = htons(27010);

	Q_strncpy(s_master_str, sv_xashmaster.string, sizeof(s_master_str) - 1);
	s_master_str[sizeof(s_master_str) - 1] = '\0';
	s_master_resolved = TRUE;
	return TRUE;
}

/*
==================
XashMaster_IsMasterAdr
==================
*/
qboolean XashMaster_IsMasterAdr(const netadr_t &adr)
{
	if (!s_master_resolved)
		return FALSE;

	if (adr.type != NA_IP || s_master_adr.type != NA_IP)
		return FALSE;

	return (adr.ip[0] == s_master_adr.ip[0] && adr.ip[1] == s_master_adr.ip[1] &&
		adr.ip[2] == s_master_adr.ip[2] && adr.ip[3] == s_master_adr.ip[3] &&
		adr.port == s_master_adr.port) ? TRUE : FALSE;
}

/*
==================
XashMaster_Frame

Send a heartbeat to the configured master if it's due
==================
*/
void XashMaster_Frame()
{
	unsigned char buf[6];

	if (!g_psv.active || !sv_xashmaster.string[0])
		return;

	if (realtime - s_last_heartbeat < XASHMASTER_HEARTBEAT_SECONDS)
		return;

	if (!XashMaster_Resolve())
	{
		s_last_heartbeat = realtime;	// don't hammer DNS every frame
		return;
	}

	s_last_heartbeat = realtime;
	s_heartbeat_challenge = (uint32)RandomLong(0, 0x7fffffff);

	// S2M_HEARTBEAT: "q\xff" + u32le challenge
	buf[0] = 'q';
	buf[1] = 0xff;
	*(uint32 *)&buf[2] = s_heartbeat_challenge;

	NET_SendPacket(NS_SERVER, sizeof(buf), buf, s_master_adr);
}

/*
==================
XashMaster_ChallengeResponse

Master answered our heartbeat with "s\n" + master_challenge + our challenge.
Validate and reply with the server info string. Keep the keys in sync with
xash3d-fwgs SV_AddToMaster.
==================
*/
void XashMaster_ChallengeResponse()
{
	char s[512];
	char gd[64];
	int players;
	uint32 challenge, challenge2;

	if (!s_master_resolved || !g_psv.active)
		return;

	// SV_ConnectionlessPacket already consumed the -1 marker and the "s" line;
	// the two challenge dwords follow
	challenge = (uint32)MSG_ReadLong();
	challenge2 = (uint32)MSG_ReadLong();

	if (msg_badread || challenge2 != s_heartbeat_challenge)
	{
		Con_DPrintf("XashMaster: unexpected challenge from %s (got 0x%x want 0x%x)\n",
			NET_AdrToString(net_from), challenge2, s_heartbeat_challenge);
		return;
	}

	int maxplayers = (int)sv_visiblemaxplayers.value;
	if (maxplayers < 0)
		maxplayers = g_psvs.maxclients;

	SV_CountPlayers(&players);
	COM_FileBase(com_gamedir, gd);

	qboolean hasPW = sv_password.string[0] && Q_stricmp(sv_password.string, "none") != 0;

	// S2M_INFO header, then the infostring
	Q_strcpy(s, "0\n");
	const int len = sizeof(s);
	Info_SetValueForKey(s, "protocol", va("%i", PROTOCOL_VERSION), len);
	Info_SetValueForKey(s, "challenge", va("%u", challenge), len);
	Info_SetValueForKey(s, "players", va("%i", players - SV_GetFakeClientCount()), len);
	Info_SetValueForKey(s, "max", va("%i", maxplayers), len);
	Info_SetValueForKey(s, "bots", va("%i", SV_GetFakeClientCount()), len);
	Info_SetValueForKey(s, "gamedir", gd, len);
	Info_SetValueForKey(s, "map", g_psv.name, len);
	Info_SetValueForKey(s, "type", "d", len);
	Info_SetValueForKey(s, "password", hasPW ? "1" : "0", len);
#ifdef _WIN32
	Info_SetValueForKey(s, "os", "w", len);
#else
	Info_SetValueForKey(s, "os", "l", len);
#endif
	Info_SetValueForKey(s, "secure", Steam_GSBSecure() ? "1" : "0", len);
	Info_SetValueForKey(s, "lan", "0", len);
	// xash3d-master rejects servers below its min_server_version (0.21 by
	// default); report the protocol-compatible engine version it expects
	Info_SetValueForKey(s, "version", "0.21", len);
	Info_SetValueForKey(s, "region", "255", len);
	Info_SetValueForKey(s, "product", gd, len);
	Info_SetValueForKey(s, "nat", "0", len);
	Info_SetValueForKey(s, "quic", WT_ServerIsActive() ? "1" : "0", len);

	NET_SendPacket(NS_SERVER, Q_strlen(s), s, net_from);
}

/*
==================
XashMaster_Shutdown

Tell the master we're going away (S2M_SHUTDOWN "b\n")
==================
*/
void XashMaster_Shutdown()
{
	if (!s_master_resolved)
		return;

	NET_SendPacket(NS_SERVER, 2, (void *)"b\n", s_master_adr);
	s_last_heartbeat = -99999.0;
}

/*
==================
XashMaster_Init
==================
*/
void XashMaster_Init()
{
	Cvar_RegisterVariable(&sv_xashmaster);
}

#endif // REHLDS_QUIC

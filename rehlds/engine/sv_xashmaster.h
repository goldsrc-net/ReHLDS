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

// Xash3D master server announcer. Publishes this server (including its
// QUIC/WebTransport capability) to a xash3d-master so web clients can
// discover it. Wire format mirrors xash3d-fwgs NET_AnnounceToMaster /
// SV_AddToMaster — keep the two in sync.

#pragma once

#ifdef REHLDS_QUIC

void XashMaster_Init();			// register cvars
void XashMaster_Frame();		// periodic heartbeat (call from SV_Frame)
void XashMaster_Shutdown();		// notify master on server shutdown
qboolean XashMaster_IsMasterAdr(const netadr_t &adr);
void XashMaster_ChallengeResponse();	// handle M2S 's' challenge (net_message/net_from)

#endif // REHLDS_QUIC

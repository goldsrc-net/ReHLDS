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

// WebTransport (HTTP/3 over QUIC) server transport, multiplexed onto the
// game UDP socket.  Ported from xash3d-fwgs net_webtransport_server.c
// (goldsrc-net fork, `emscripten` branch).  Built on quiche + BoringSSL.
//
// Steam/UDP clients are unaffected: incoming datagrams are classified
// per-packet (QUIC long-header Initial detection + known-client address
// table) and only QUIC traffic is diverted into this module.

#pragma once

#ifdef REHLDS_QUIC

#include "maintypes.h"
#include "common/netadr.h"

// WebTransport server connection (one per QUIC client)
typedef struct wt_server_conn_s
{
	netadr_t	client_addr;
	void		*quiche_conn;		// quiche_conn pointer
	void		*h3_conn;		// quiche_h3_conn pointer (NULL in raw mode)
	uint64		last_activity;
	qboolean	active;
	qboolean	wt_session_established;	// WebTransport session ready
	int64		wt_session_id;		// WebTransport session stream ID
	int		client_id;

	// Raw HTTP/3 mode (bypasses quiche H3 layer for WebTransport)
	qboolean	raw_h3_mode;
	qboolean	settings_sent;
	qboolean	peer_settings_received;
	uint64		control_stream_id;	// Our control stream
	uint64		peer_control_stream_id;	// Peer's control stream ((uint64)-1 = not yet found)
} wt_server_conn_t;

#define WT_MAX_CLIENTS 64

typedef struct wt_server_s
{
	qboolean	initialized;
	int		port;
	void		*quiche_config;		// quiche_config pointer
	void		*h3_config;		// quiche_h3_config pointer
	void		*ssl_ctx;		// SSL_CTX pointer
	wt_server_conn_t clients[WT_MAX_CLIENTS];
	int		client_count;

	// Certificate SHA-256 hash (64 hex chars + null) for browser
	// serverCertificateHashes pinning
	char		cert_hash[65];
} wt_server_t;

qboolean WT_ServerInit();				// Load certs + build configs; no socket yet
void WT_ServerSetSocket(int socket, int port);		// Attach the shared game UDP socket
void WT_ServerShutdown();
void WT_ServerFrame();					// Pump timeouts + flush pending QUIC packets
qboolean WT_IsQuicInitial(const unsigned char *data, int len);	// QUIC long-header detection
qboolean WT_IsClientAddr(const netadr_t *addr);		// Known WebTransport client?
int WT_GetClientIdByAddr(const netadr_t *addr);		// -1 if not found
void WT_ProcessIncomingPacket(const unsigned char *data, int len, const netadr_t *from);
qboolean WT_ServerSendDatagram(int client_id, const void *data, int len);
qboolean WT_ServerSendToAddr(const netadr_t *to, const void *data, int len);	// FALSE if not a WT client
qboolean WT_ServerRecvDatagram(void *data, int *len, netadr_t *from, int *client_id);
int WT_ServerGetClientCount();
void WT_ServerDisconnectClient(int client_id);
qboolean WT_ServerIsActive();			// QUIC attached to the game socket and accepting

#endif // REHLDS_QUIC

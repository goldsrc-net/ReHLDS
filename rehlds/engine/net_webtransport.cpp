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

// WebTransport server using quiche (HTTP/3 over QUIC).
// Ported from xash3d-fwgs engine/common/net_webtransport_server.c
// (goldsrc-net fork, `emscripten` branch tip).

#include "precompiled.h"

#ifdef REHLDS_QUIC

#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <quiche.h>
#include "net_webtransport.h"

// Forward declarations for BoringSSL/OpenSSL (linked via quiche)
typedef struct ssl_ctx_st SSL_CTX;
typedef struct ssl_st SSL;
typedef struct bio_st BIO;
typedef struct x509_st X509;
typedef struct evp_pkey_st EVP_PKEY;
typedef struct ssl_method_st SSL_METHOD;

// BoringSSL/OpenSSL functions
extern "C" {
extern const SSL_METHOD *TLS_method(void);
extern SSL_CTX *SSL_CTX_new(const SSL_METHOD *method);
extern void SSL_CTX_free(SSL_CTX *ctx);
extern int SSL_CTX_use_certificate(SSL_CTX *ctx, X509 *cert);
extern int SSL_CTX_use_PrivateKey(SSL_CTX *ctx, EVP_PKEY *pkey);
extern int SSL_CTX_check_private_key(const SSL_CTX *ctx);
extern SSL *SSL_new(SSL_CTX *ctx);
extern void SSL_free(SSL *ssl);
extern void X509_free(X509 *cert);
extern void EVP_PKEY_free(EVP_PKEY *pkey);
extern int SSL_CTX_set_min_proto_version(SSL_CTX *ctx, int version);
extern int SSL_CTX_set_max_proto_version(SSL_CTX *ctx, int version);
extern int SSL_CTX_set_alpn_protos(SSL_CTX *ctx, const unsigned char *protos, unsigned protos_len);
extern unsigned long ERR_get_error(void);
extern void ERR_error_string_n(unsigned long e, char *buf, size_t len);
typedef int (*SSL_CTX_alpn_select_cb_func)(SSL *ssl, const unsigned char **out, unsigned char *outlen,
                                           const unsigned char *in, unsigned int inlen, void *arg);
extern void SSL_CTX_set_alpn_select_cb(SSL_CTX *ctx, SSL_CTX_alpn_select_cb_func cb, void *arg);
extern const void *EVP_sha256(void);
extern int X509_digest(const X509 *cert, const void *type, unsigned char *md, unsigned int *len);

// X509 builder + EC keygen for self-managed certificates
typedef struct ec_key_st EC_KEY;
typedef struct asn1_string_st ASN1_INTEGER;
typedef struct asn1_string_st ASN1_TIME;
typedef struct X509_name_st X509_NAME;

extern EC_KEY *EC_KEY_new_by_curve_name(int nid);
extern int EC_KEY_generate_key(EC_KEY *key);
extern void EC_KEY_free(EC_KEY *key);
extern EVP_PKEY *EVP_PKEY_new(void);
extern int EVP_PKEY_assign_EC_KEY(EVP_PKEY *pkey, EC_KEY *key);
extern X509 *X509_new(void);
extern int X509_set_version(X509 *x509, long version);
extern ASN1_INTEGER *X509_get_serialNumber(X509 *x509);
extern int ASN1_INTEGER_set(ASN1_INTEGER *a, long v);
extern ASN1_TIME *X509_getm_notBefore(const X509 *x509);
extern ASN1_TIME *X509_getm_notAfter(const X509 *x509);
extern ASN1_TIME *X509_gmtime_adj(ASN1_TIME *s, long offset_sec);
extern X509_NAME *X509_get_subject_name(const X509 *x509);
extern int X509_NAME_add_entry_by_txt(X509_NAME *name, const char *field, int type,
                                      const unsigned char *bytes, int len, int loc, int set);
extern int X509_set_issuer_name(X509 *x509, X509_NAME *name);
extern int X509_set_pubkey(X509 *x509, EVP_PKEY *pkey);
extern int X509_sign(X509 *x509, EVP_PKEY *pkey, const void *md);

typedef struct X509_extension_st X509_EXTENSION;
extern X509_EXTENSION *X509V3_EXT_nconf_nid(void *conf, void *ctx, int ext_nid, const char *value);
extern int X509_add_ext(X509 *x509, const X509_EXTENSION *ex, int loc);
extern void X509_EXTENSION_free(X509_EXTENSION *ex);
}

#define TLS1_3_VERSION 0x0304
#define SSL_TLSEXT_ERR_OK 0
#define SSL_TLSEXT_ERR_NOACK 3

#define WT_MAX_DATAGRAM_SIZE 65535
#define WT_TIMEOUT_MS 30000
#define WT_MAX_PENDING_DATAGRAMS 64

// Certificates are always self-managed: a self-signed P-256 certificate is
// generated in-process at startup and rotated automatically (browsers cap
// serverCertificateHashes-pinned certs at 14 days validity). Clients learn
// the hash through the master server heartbeat -> discovery API pipeline,
// so operators provision nothing.
#define WT_CERT_VALID_SECONDS  (14 * 86400)	// browser-imposed maximum for pinned certs
#define WT_CERT_ROTATE_SECONDS (7 * 86400)	// regenerate half-way through validity

// BoringSSL constants (no headers; values are ABI-stable)
#define NID_X9_62_prime256v1 415
#define MBSTRING_ASC 0x1001
#define NID_subject_alt_name 85
#define NID_basic_constraints 87

/*
==================
WT_HashToHex

Convert binary hash to lowercase hex string
==================
*/
static void WT_HashToHex(const unsigned char *hash, unsigned int hash_len, char *hex_out)
{
	unsigned int i;
	for (i = 0; i < hash_len; i++)
	{
		hex_out[i * 2] = "0123456789abcdef"[hash[i] >> 4];
		hex_out[i * 2 + 1] = "0123456789abcdef"[hash[i] & 0x0f];
	}
	hex_out[hash_len * 2] = '\0';
}

// HTTP/3 frame types
#define H3_FRAME_DATA 0x00
#define H3_FRAME_HEADERS 0x01
#define H3_FRAME_SETTINGS 0x04
#define H3_FRAME_GOAWAY 0x07

// HTTP/3 settings
#define H3_SETTINGS_QPACK_MAX_TABLE_CAPACITY 0x01
#define H3_SETTINGS_MAX_FIELD_SECTION_SIZE 0x06
#define H3_SETTINGS_QPACK_BLOCKED_STREAMS 0x07
#define H3_SETTINGS_ENABLE_CONNECT_PROTOCOL 0x08
#define H3_SETTINGS_H3_DATAGRAM 0x33
#define H3_SETTINGS_ENABLE_WEBTRANSPORT 0x2b603742

// HTTP/3 unidirectional stream types
#define H3_STREAM_TYPE_CONTROL 0x00
#define H3_STREAM_TYPE_QPACK_ENCODER 0x02
#define H3_STREAM_TYPE_QPACK_DECODER 0x03

// Write a variable-length integer (QUIC varint format)
static size_t WT_WriteVarint(uint8_t *buf, uint64_t value)
{
	if (value < 0x40)
	{
		buf[0] = (uint8_t)value;
		return 1;
	}
	else if (value < 0x4000)
	{
		buf[0] = (uint8_t)((value >> 8) | 0x40);
		buf[1] = (uint8_t)value;
		return 2;
	}
	else if (value < 0x40000000)
	{
		buf[0] = (uint8_t)((value >> 24) | 0x80);
		buf[1] = (uint8_t)(value >> 16);
		buf[2] = (uint8_t)(value >> 8);
		buf[3] = (uint8_t)value;
		return 4;
	}
	else
	{
		buf[0] = (uint8_t)((value >> 56) | 0xc0);
		buf[1] = (uint8_t)(value >> 48);
		buf[2] = (uint8_t)(value >> 40);
		buf[3] = (uint8_t)(value >> 32);
		buf[4] = (uint8_t)(value >> 24);
		buf[5] = (uint8_t)(value >> 16);
		buf[6] = (uint8_t)(value >> 8);
		buf[7] = (uint8_t)value;
		return 8;
	}
}

// Read a variable-length integer
static size_t WT_ReadVarint(const uint8_t *buf, size_t len, uint64_t *value)
{
	if (len < 1)
		return 0;

	uint8_t prefix = buf[0] >> 6;
	size_t varint_len = (size_t)1 << prefix;

	if (len < varint_len)
		return 0;

	*value = buf[0] & 0x3f;
	for (size_t i = 1; i < varint_len; i++)
	{
		*value = (*value << 8) | buf[i];
	}

	return varint_len;
}

// Request info extracted from QPACK headers
typedef struct
{
	qboolean is_webtransport;	// CONNECT method over HTTP/3 = WebTransport
} wt_request_info_t;

/*
==================
WT_ParseQpackHeaders

Minimal QPACK header parser to extract :method = CONNECT
QPACK static table index (RFC 9204): 15 = :method = CONNECT
==================
*/
static void WT_ParseQpackHeaders(const uint8_t *data, size_t len, wt_request_info_t *info)
{
	size_t pos = 0;

	Q_memset(info, 0, sizeof(*info));

	// Skip section prefix (Required Insert Count + Delta Base)
	// Simplified: assume both are encoded in 1 byte each (values < 64)
	if (len < 2)
		return;
	pos = 2;

	while (pos < len)
	{
		uint8_t byte = data[pos];

		if (byte & 0x80)
		{
			// Indexed header field (static or dynamic table)
			// Format: 1 T index
			// T=1 means static table
			qboolean is_static = (byte & 0x40) != 0;
			uint8_t index = byte & 0x3f;
			pos++;

			if (is_static && index == 15)	// :method = CONNECT
				info->is_webtransport = TRUE;
		}
		else if ((byte & 0xc0) == 0x40)
		{
			// Literal header with name reference
			// Format: 01 N T name_index, then H value_len value
			qboolean is_static = (byte & 0x10) != 0;
			uint8_t name_index = byte & 0x0f;
			size_t value_len;
			qboolean huffman;
			pos++;

			if (pos >= len)
				break;

			// Read value length (simplified: assume < 127)
			huffman = (data[pos] & 0x80) != 0;
			value_len = data[pos] & 0x7f;
			pos++;

			if (pos + value_len > len)
				break;

			// Check for :method literal with CONNECT value
			if (is_static && name_index == 15 && value_len == 7 && !huffman &&
			    Q_memcmp(data + pos, "CONNECT", 7) == 0)
			{
				info->is_webtransport = TRUE;
			}

			pos += value_len;
		}
		else if ((byte & 0xe0) == 0x20)
		{
			// Literal header with literal name - skip
			size_t name_len, value_len;
			pos++;

			if (pos >= len)
				break;

			name_len = data[pos] & 0x7f;
			pos++;

			if (pos + name_len > len)
				break;

			pos += name_len;
			if (pos >= len)
				break;

			value_len = data[pos] & 0x7f;
			pos++;
			pos += value_len;
		}
		else
		{
			// Unknown encoding, skip
			pos++;
		}
	}
}

// Create and send our own SETTINGS frame with WebTransport support on a new control stream
static qboolean WT_SendRawSettings(quiche_conn *conn)
{
	uint8_t buf[256];
	size_t pos = 0;
	ssize_t sent;

	// First, create a server-initiated unidirectional stream for control
	// Server-initiated unidirectional streams have IDs: 3, 7, 11, 15, ... (0x03 + 4*n)
	// Stream ID 3 = first server unidirectional stream
	uint64_t control_stream_id = 3;

	// Write stream type (control = 0x00)
	pos += WT_WriteVarint(buf + pos, H3_STREAM_TYPE_CONTROL);

	// Build SETTINGS payload
	uint8_t settings_payload[128];
	size_t payload_pos = 0;

	// SETTINGS_ENABLE_CONNECT_PROTOCOL = 1
	payload_pos += WT_WriteVarint(settings_payload + payload_pos, H3_SETTINGS_ENABLE_CONNECT_PROTOCOL);
	payload_pos += WT_WriteVarint(settings_payload + payload_pos, 1);

	// SETTINGS_H3_DATAGRAM = 1
	payload_pos += WT_WriteVarint(settings_payload + payload_pos, H3_SETTINGS_H3_DATAGRAM);
	payload_pos += WT_WriteVarint(settings_payload + payload_pos, 1);

	// SETTINGS_ENABLE_WEBTRANSPORT = 1
	payload_pos += WT_WriteVarint(settings_payload + payload_pos, H3_SETTINGS_ENABLE_WEBTRANSPORT);
	payload_pos += WT_WriteVarint(settings_payload + payload_pos, 1);

	// Write SETTINGS frame header
	pos += WT_WriteVarint(buf + pos, H3_FRAME_SETTINGS);
	pos += WT_WriteVarint(buf + pos, payload_pos);

	// Write SETTINGS payload
	Q_memcpy(buf + pos, settings_payload, payload_pos);
	pos += payload_pos;

	// Send on control stream
	uint64_t send_error = 0;
	sent = quiche_conn_stream_send(conn, control_stream_id, buf, pos, false, &send_error);
	if (sent < 0)
	{
		Con_DPrintf("WT: Failed to send raw SETTINGS on stream %llu: %zd\n",
			(unsigned long long)control_stream_id, sent);
		return FALSE;
	}

	Con_DPrintf("WT: Sent raw SETTINGS frame (%zu bytes) on stream %llu with WT support\n",
		pos, (unsigned long long)control_stream_id);

	return TRUE;
}

static wt_server_t wt_server;

// Pending datagram queue for received data
static struct
{
	unsigned char data[NET_MAX_MESSAGE];
	int len;
	int client_id;
} wt_recv_queue[WT_MAX_PENDING_DATAGRAMS];
static int wt_recv_queue_head = 0;
static int wt_recv_queue_tail = 0;

// Shared game UDP socket used for QUIC transport
static int wt_socket = -1;

// Self-managed certificate state
static double wt_cert_created;		// Sys_FloatTime() at generation
static double wt_cert_next_check;	// throttle for the rotation age check

// Local address for QUIC
static struct sockaddr_storage wt_local_addr;
static socklen_t wt_local_addr_len;

/*
==================
WT_CompareAdr

Address comparison local to this module (NET_CompareAdr takes non-const refs)
==================
*/
static qboolean WT_CompareAdr(const netadr_t &a, const netadr_t &b)
{
	if (a.type != b.type)
		return FALSE;

	if (a.type == NA_IP)
	{
		return (a.ip[0] == b.ip[0] && a.ip[1] == b.ip[1] &&
			a.ip[2] == b.ip[2] && a.ip[3] == b.ip[3] &&
			a.port == b.port) ? TRUE : FALSE;
	}

	return FALSE;
}

/*
==================
WT_FindClientByAddr

Find client by network address
==================
*/
static wt_server_conn_t *WT_FindClientByAddr(const netadr_t *addr)
{
	int i;
	for (i = 0; i < WT_MAX_CLIENTS; i++)
	{
		if (wt_server.clients[i].active)
		{
			if (WT_CompareAdr(wt_server.clients[i].client_addr, *addr))
				return &wt_server.clients[i];
		}
	}
	return NULL;
}

/*
==================
WT_FindClientById

Find client by ID
==================
*/
static wt_server_conn_t *WT_FindClientById(int client_id)
{
	int i;
	for (i = 0; i < WT_MAX_CLIENTS; i++)
	{
		if (wt_server.clients[i].active && wt_server.clients[i].client_id == client_id)
			return &wt_server.clients[i];
	}
	return NULL;
}

/*
==================
WT_AllocClient

Allocate a new client slot
==================
*/
static wt_server_conn_t *WT_AllocClient()
{
	int i;
	for (i = 0; i < WT_MAX_CLIENTS; i++)
	{
		if (!wt_server.clients[i].active)
		{
			Q_memset(&wt_server.clients[i], 0, sizeof(wt_server_conn_t));
			wt_server.clients[i].active = TRUE;
			wt_server.clients[i].client_id = i;
			wt_server.clients[i].wt_session_id = -1;
			wt_server.client_count++;
			return &wt_server.clients[i];
		}
	}
	return NULL;
}

/*
==================
WT_FreeClient

Free a client slot
==================
*/
static void WT_FreeClient(wt_server_conn_t *client)
{
	if (!client || !client->active)
		return;

	if (client->h3_conn)
	{
		quiche_h3_conn_free((quiche_h3_conn *)client->h3_conn);
		client->h3_conn = NULL;
	}

	if (client->quiche_conn)
	{
		quiche_conn_free((quiche_conn *)client->quiche_conn);
		client->quiche_conn = NULL;
	}

	client->active = FALSE;
	client->wt_session_established = FALSE;
	client->wt_session_id = -1;
	wt_server.client_count--;
}

/*
==================
WT_DropEngineClient

A WebTransport connection went away - a clean CONNECTION_CLOSE from a closed
browser tab, or the QUIC idle timeout for an abrupt close. Classic UDP has no
connection state, so the engine only reaps silent clients via SV_CheckTimeouts
(~sv_timeout, tens of seconds) - which leaves a browser-tab-close as a zombie
occupying a player slot. But QUIC *is* connection-oriented, so the transport
tells us the peer is gone now: find the matching live game client and drop it
immediately. Call this just before WT_FreeClient at the per-frame close paths
only (never from WT_ShutdownServer, which is tearing everything down anyway).
==================
*/
static void WT_DropEngineClient(const netadr_t *addr)
{
	int i;
	client_t *cl;

	if (!g_psvs.clients)
		return;

	for (i = 0, cl = g_psvs.clients; i < g_psvs.maxclients; i++, cl++)
	{
		if (cl->fakeclient)
			continue;
		if (!cl->connected && !cl->active && !cl->spawned)
			continue;
		if (WT_CompareAdr(cl->netchan.remote_address, *addr))
		{
			SV_DropClient(cl, FALSE, "WebTransport connection closed");
			return;
		}
	}
}

/*
==================
WT_QueueDatagram

Queue a received datagram for later retrieval
==================
*/
static qboolean WT_QueueDatagram(const void *data, int len, int client_id)
{
	int next_tail = (wt_recv_queue_tail + 1) % WT_MAX_PENDING_DATAGRAMS;

	if (next_tail == wt_recv_queue_head)
	{
		// Queue full, drop oldest
		wt_recv_queue_head = (wt_recv_queue_head + 1) % WT_MAX_PENDING_DATAGRAMS;
	}

	if (len > NET_MAX_MESSAGE)
		len = NET_MAX_MESSAGE;

	Q_memcpy(wt_recv_queue[wt_recv_queue_tail].data, data, len);
	wt_recv_queue[wt_recv_queue_tail].len = len;
	wt_recv_queue[wt_recv_queue_tail].client_id = client_id;
	wt_recv_queue_tail = next_tail;

	return TRUE;
}

/*
==================
WT_ALPNSelectCallback

ALPN protocol selection callback for HTTP/3
==================
*/
static int WT_ALPNSelectCallback(SSL *ssl, const unsigned char **out, unsigned char *outlen,
                                 const unsigned char *in, unsigned int inlen, void *arg)
{
	// Look for "h3" in the client's offered protocols
	// ALPN format: length-prefixed strings (e.g., "\x02h3")
	const unsigned char *p = in;
	const unsigned char *end = in + inlen;

	while (p < end)
	{
		unsigned char len = *p++;
		if (p + len > end)
			break;

		// Check for "h3" (length 2)
		if (len == 2 && p[0] == 'h' && p[1] == '3')
		{
			*out = p;
			*outlen = len;
			return SSL_TLSEXT_ERR_OK;
		}
		p += len;
	}

	// No matching protocol found
	return SSL_TLSEXT_ERR_NOACK;
}

/*
==================
WT_BuildSSLCtx

Build a TLS 1.3 SSL_CTX from a cert/key pair and compute the cert hash.
Does not consume cert/pkey; the caller frees them.
==================
*/
static SSL_CTX *WT_BuildSSLCtx(X509 *cert, EVP_PKEY *pkey, char *hash_out)
{
	SSL_CTX *ctx;
	unsigned char hash[32];
	unsigned int hash_len;

	ctx = SSL_CTX_new(TLS_method());
	if (!ctx)
		return NULL;

	// QUIC requires TLS 1.3
	SSL_CTX_set_min_proto_version(ctx, TLS1_3_VERSION);
	SSL_CTX_set_max_proto_version(ctx, TLS1_3_VERSION);

	if (hash_out)
	{
		if (X509_digest(cert, EVP_sha256(), hash, &hash_len) && hash_len == 32)
			WT_HashToHex(hash, hash_len, hash_out);
		else
			hash_out[0] = '\0';
	}

	if (SSL_CTX_use_certificate(ctx, cert) != 1 ||
	    SSL_CTX_use_PrivateKey(ctx, pkey) != 1 ||
	    SSL_CTX_check_private_key(ctx) != 1)
	{
		Con_DPrintf("WT: ERROR: Failed to load cert/key into SSL_CTX\n");
		SSL_CTX_free(ctx);
		return NULL;
	}

	// Set ALPN callback for HTTP/3 protocol selection
	SSL_CTX_set_alpn_select_cb(ctx, WT_ALPNSelectCallback, NULL);
	return ctx;
}

/*
==================
WT_GenerateCertPair

Generate a fresh P-256 key and self-signed certificate (14-day validity,
the browser-imposed maximum for serverCertificateHashes pinning).
==================
*/
static qboolean WT_GenerateCertPair(X509 **out_cert, EVP_PKEY **out_key)
{
	EC_KEY *ec;
	EVP_PKEY *pkey;
	X509 *cert;
	X509_NAME *name;

	ec = EC_KEY_new_by_curve_name(NID_X9_62_prime256v1);
	if (!ec || EC_KEY_generate_key(ec) != 1)
	{
		if (ec) EC_KEY_free(ec);
		return FALSE;
	}

	pkey = EVP_PKEY_new();
	if (!pkey || EVP_PKEY_assign_EC_KEY(pkey, ec) != 1)	// assign takes ownership of ec
	{
		EC_KEY_free(ec);
		if (pkey) EVP_PKEY_free(pkey);
		return FALSE;
	}

	cert = X509_new();
	if (!cert)
	{
		EVP_PKEY_free(pkey);
		return FALSE;
	}

	X509_set_version(cert, 2);	// X509v3
	ASN1_INTEGER_set(X509_get_serialNumber(cert), (long)RandomLong(1, 0x7fffffff));
	// browsers enforce TOTAL validity (notAfter - notBefore) <= 14 days
	// for pinned certs, so the clock-skew backdate must come out of the
	// window, with margin
	X509_gmtime_adj(X509_getm_notBefore(cert), -300);
	X509_gmtime_adj(X509_getm_notAfter(cert), WT_CERT_VALID_SECONDS - 3600);

	// identity is established by hash pinning, not by name; CN is cosmetic
	name = X509_get_subject_name(cert);
	X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, (const unsigned char *)"hlds", -1, -1, 0);
	X509_set_issuer_name(cert, name);	// self-signed

	// Chrome's QUIC certificate parser rejects extension-less certs even
	// under serverCertificateHashes pinning; mirror the profile openssl
	// produces (the names themselves are ignored by hash pinning)
	{
		static const struct { int nid; const char *value; } exts[] = {
			{ NID_subject_alt_name, "DNS:hlds" },
			{ NID_basic_constraints, "critical,CA:TRUE" },
		};
		for (size_t i = 0; i < ARRAYSIZE(exts); i++)
		{
			X509_EXTENSION *ex = X509V3_EXT_nconf_nid(NULL, NULL, exts[i].nid, exts[i].value);
			if (!ex)
			{
				X509_free(cert);
				EVP_PKEY_free(pkey);
				return FALSE;
			}
			X509_add_ext(cert, ex, -1);
			X509_EXTENSION_free(ex);
		}
	}

	if (X509_set_pubkey(cert, pkey) != 1 || X509_sign(cert, pkey, EVP_sha256()) == 0)
	{
		X509_free(cert);
		EVP_PKEY_free(pkey);
		return FALSE;
	}

	*out_cert = cert;
	*out_key = pkey;
	return TRUE;
}

/*
==================
WT_CreateSSLCtx

Generate the self-signed certificate and build the TLS context. Certs
are always self-managed: clients learn the hash through the master's
heartbeat pipeline, so there is nothing for an operator to provision,
and WT_ServerFrame rotates well before the validity cliff.
==================
*/
static SSL_CTX *WT_CreateSSLCtx(char *hash_out)
{
	X509 *cert = NULL;
	EVP_PKEY *pkey = NULL;
	SSL_CTX *ctx;

	if (!WT_GenerateCertPair(&cert, &pkey))
	{
		Con_Printf("WT: ERROR: Failed to generate certificate\n");
		return NULL;
	}
	wt_cert_created = Sys_FloatTime();
	wt_cert_next_check = 0.0;
	Con_Printf("WT: Generated self-signed certificate (rotates every %d days)\n",
		WT_CERT_ROTATE_SECONDS / 86400);

	ctx = WT_BuildSSLCtx(cert, pkey, hash_out);
	X509_free(cert);
	EVP_PKEY_free(pkey);
	return ctx;
}

/*
==================
WT_RotateCert

Swap in a freshly generated certificate. Only new handshakes see the new
cert: every established connection owns an SSL object bound to the old
SSL_CTX, which BoringSSL keeps alive by refcount until the last user is
freed. The previous hash is retained so the discovery API can offer a
grace window to clients holding the old one.
==================
*/
static void WT_RotateCert()
{
	X509 *cert = NULL;
	EVP_PKEY *pkey = NULL;
	SSL_CTX *ctx;
	char new_hash[65];

	if (!WT_GenerateCertPair(&cert, &pkey))
	{
		Con_DPrintf("WT: ERROR: cert rotation: generation failed, keeping current cert\n");
		return;
	}

	ctx = WT_BuildSSLCtx(cert, pkey, new_hash);
	X509_free(cert);
	EVP_PKEY_free(pkey);
	if (!ctx)
	{
		Con_DPrintf("WT: ERROR: cert rotation: SSL_CTX build failed, keeping current cert\n");
		return;
	}

	Q_strcpy(wt_server.prev_cert_hash, wt_server.cert_hash);
	Q_strcpy(wt_server.cert_hash, new_hash);
	SSL_CTX_free((SSL_CTX *)wt_server.ssl_ctx);	// refcounted; live connections unaffected
	wt_server.ssl_ctx = ctx;
	wt_cert_created = Sys_FloatTime();

	Con_Printf("WT: Rotated self-signed certificate, new SHA-256 hash: %s\n", wt_server.cert_hash);
}

/*
==================
WT_ServerInit

Initialize WebTransport server config with TLS certificates (no socket)
==================
*/
qboolean WT_ServerInit()
{
	quiche_config *config;
	quiche_h3_config *h3_config;

	if (wt_server.initialized)
		return TRUE;

	Q_memset(&wt_server, 0, sizeof(wt_server));
	wt_socket = -1;

	// Create SSL_CTX from cert files and compute hash.
	// Do this first: missing cert files is the "QUIC disabled" path.
	wt_server.ssl_ctx = WT_CreateSSLCtx(wt_server.cert_hash);
	if (!wt_server.ssl_ctx)
		return FALSE;
	Con_Printf("WT: Certificate loaded, SHA-256 hash: %s\n", wt_server.cert_hash);

	// Create quiche config
	config = quiche_config_new(QUICHE_PROTOCOL_VERSION);
	if (!config)
	{
		Con_DPrintf("WT: ERROR: Failed to create quiche config\n");
		SSL_CTX_free((SSL_CTX *)wt_server.ssl_ctx);
		wt_server.ssl_ctx = NULL;
		return FALSE;
	}

	// Configure QUIC parameters for HTTP/3
	quiche_config_set_application_protos(config, (uint8_t *)"\x02h3", 3);
	quiche_config_set_max_idle_timeout(config, WT_TIMEOUT_MS);
	quiche_config_set_max_recv_udp_payload_size(config, WT_MAX_DATAGRAM_SIZE);
	quiche_config_set_max_send_udp_payload_size(config, WT_MAX_DATAGRAM_SIZE);
	quiche_config_set_initial_max_data(config, 10000000);
	quiche_config_set_initial_max_stream_data_bidi_local(config, 1000000);
	quiche_config_set_initial_max_stream_data_bidi_remote(config, 1000000);
	quiche_config_set_initial_max_stream_data_uni(config, 1000000);
	quiche_config_set_initial_max_streams_bidi(config, 100);
	quiche_config_set_initial_max_streams_uni(config, 100);
	quiche_config_set_disable_active_migration(config, true);

	// Enable datagrams for WebTransport (required for unreliable data)
	quiche_config_enable_dgram(config, true, 1000, 1000);

	wt_server.quiche_config = config;

	// Create HTTP/3 config
	h3_config = quiche_h3_config_new();
	if (!h3_config)
	{
		Con_DPrintf("WT: ERROR: Failed to create HTTP/3 config\n");
		quiche_config_free(config);
		wt_server.quiche_config = NULL;
		SSL_CTX_free((SSL_CTX *)wt_server.ssl_ctx);
		wt_server.ssl_ctx = NULL;
		return FALSE;
	}

	// Enable WebTransport in HTTP/3 config
	quiche_h3_config_enable_extended_connect(h3_config, true);

	wt_server.h3_config = h3_config;

	Con_DPrintf("WT: Config initialized (waiting for socket)\n");
	return TRUE;
}

/*
==================
WT_ServerSetSocket

Set the shared UDP socket for WebTransport to use
==================
*/
void WT_ServerSetSocket(int socket, int port)
{
	if (!wt_server.quiche_config)
	{
		Con_DPrintf("WT: ERROR: SetSocket called before ServerInit!\n");
		return;
	}

	wt_socket = socket;
	wt_server.port = port;

	// Store local address for QUIC
	wt_local_addr_len = sizeof(wt_local_addr);
	getsockname(wt_socket, (struct sockaddr *)&wt_local_addr, &wt_local_addr_len);

	wt_server.initialized = TRUE;
	Con_Printf("WT: QUIC/WebTransport ready on port %d (shared game socket)\n", port);
}

/*
==================
WT_IsClientAddr

Check if an address is a known WebTransport client
==================
*/
qboolean WT_IsClientAddr(const netadr_t *addr)
{
	if (!addr || !wt_server.initialized)
		return FALSE;

	return WT_FindClientByAddr(addr) != NULL ? TRUE : FALSE;
}

/*
==================
WT_GetClientIdByAddr

Get the WebTransport client ID for an address, or -1 if not found
==================
*/
int WT_GetClientIdByAddr(const netadr_t *addr)
{
	wt_server_conn_t *client;

	if (!addr || !wt_server.initialized)
		return -1;

	client = WT_FindClientByAddr(addr);
	return client ? client->client_id : -1;
}

/*
==================
WT_IsQuicInitial

Check if a packet is a QUIC Initial packet (long header with valid version)
Used for detecting new WebTransport connections on the shared socket
==================
*/
qboolean WT_IsQuicInitial(const unsigned char *data, int len)
{
	uint32 version;

	if (len < 5)
		return FALSE;

	// Game connectionless packets start with 0xFFFFFFFF - exclude these
	if (data[0] == 0xFF && data[1] == 0xFF && data[2] == 0xFF && data[3] == 0xFF)
		return FALSE;

	// QUIC long header: first byte has form bit (0x80) and fixed bit (0x40) set
	// Long header format: 1 (form) 1 (fixed) XX (type) XXXX (reserved/pn_len)
	if ((data[0] & 0xC0) != 0xC0)
		return FALSE;

	// Check QUIC version field at bytes 1-4 (big-endian/network order)
	version = ((uint32)data[1] << 24) | ((uint32)data[2] << 16) |
	          ((uint32)data[3] << 8) | data[4];

	// QUIC v1 = 0x00000001, QUIC v2 = 0x6b3343cf, Version negotiation = 0x00000000
	if (version == 0x00000001 || version == 0x6b3343cf || version == 0x00000000)
		return TRUE;

	return FALSE;
}

/*
==================
WT_ServerShutdown

Shutdown WebTransport server
==================
*/
void WT_ServerShutdown()
{
	int i;

	if (!wt_server.initialized)
		return;

	// Disconnect all clients
	for (i = 0; i < WT_MAX_CLIENTS; i++)
	{
		if (wt_server.clients[i].active)
			WT_FreeClient(&wt_server.clients[i]);
	}

	// Don't close wt_socket - it's the shared game socket, not ours
	wt_socket = -1;

	if (wt_server.h3_config)
	{
		quiche_h3_config_free((quiche_h3_config *)wt_server.h3_config);
		wt_server.h3_config = NULL;
	}

	if (wt_server.quiche_config)
	{
		quiche_config_free((quiche_config *)wt_server.quiche_config);
		wt_server.quiche_config = NULL;
	}

	if (wt_server.ssl_ctx)
	{
		SSL_CTX_free((SSL_CTX *)wt_server.ssl_ctx);
		wt_server.ssl_ctx = NULL;
	}

	wt_server.initialized = FALSE;
	wt_recv_queue_head = 0;
	wt_recv_queue_tail = 0;

	Con_DPrintf("WT: Server shutdown\n");
}

/*
==================
WT_ProcessRawH3

Process raw HTTP/3 streams for WebTransport (bypassing quiche's H3 layer)
==================
*/
static void WT_ProcessRawH3(wt_server_conn_t *client)
{
	quiche_conn *conn;
	uint8_t buf[4096];
	ssize_t recv_len;

	if (!client || !client->quiche_conn)
		return;

	conn = (quiche_conn *)client->quiche_conn;

	// Send our SETTINGS if not done yet
	if (!client->settings_sent)
	{
		if (WT_SendRawSettings(conn))
		{
			client->settings_sent = TRUE;
			client->control_stream_id = 3;	// Server-initiated unidirectional stream

			// Also create QPACK encoder stream (type 0x02) on stream 7
			uint8_t qpack_enc_type = H3_STREAM_TYPE_QPACK_ENCODER;
			uint64_t qpack_err = 0;
			quiche_conn_stream_send(conn, 7, &qpack_enc_type, 1, false, &qpack_err);

			// And QPACK decoder stream (type 0x03) on stream 11
			uint8_t qpack_dec_type = H3_STREAM_TYPE_QPACK_DECODER;
			quiche_conn_stream_send(conn, 11, &qpack_dec_type, 1, false, &qpack_err);

			Con_DPrintf("WT: Raw H3 streams initialized (control=3, qpack_enc=7, qpack_dec=11)\n");
		}
	}

	// Check for readable streams
	quiche_stream_iter *readable = quiche_conn_readable(conn);
	if (readable)
	{
		uint64_t stream_id;
		while (quiche_stream_iter_next(readable, &stream_id))
		{
			bool stream_fin = false;
			uint64_t stream_err = 0;
			recv_len = quiche_conn_stream_recv(conn, stream_id, buf, sizeof(buf), &stream_fin, &stream_err);
			if (recv_len < 0)
				continue;

			// Client-initiated bidi streams (0, 4, 8, ...) are request streams
			// Stream ID & 0x03 == 0 means client-initiated bidi
			if ((stream_id & 0x03) == 0 && recv_len > 0)
			{
				// This is likely a HEADERS frame with CONNECT request
				// Frame format: type (varint) + length (varint) + payload
				size_t pos = 0;
				uint64_t frame_type = 0, frame_len = 0;

				pos += WT_ReadVarint(buf + pos, recv_len - pos, &frame_type);
				if (pos > 0)
				{
					pos += WT_ReadVarint(buf + pos, recv_len - pos, &frame_len);

					if (frame_type == H3_FRAME_HEADERS && pos + frame_len <= (size_t)recv_len)
					{
						// Parse QPACK headers to determine request type
						wt_request_info_t req_info;
						WT_ParseQpackHeaders(buf + pos, frame_len, &req_info);

						// CONNECT over HTTP/3 is only used for WebTransport, so treat any CONNECT as WebTransport
						// (our QPACK parser doesn't handle Huffman-encoded :protocol header)
						if (req_info.is_webtransport)
						{
							// WebTransport CONNECT - send 200 OK
							uint8_t response[64];
							size_t resp_pos = 0;

							resp_pos += WT_WriteVarint(response + resp_pos, H3_FRAME_HEADERS);
							uint8_t qpack_headers[] = { 0x00, 0x00, 0xd9 };	// index 25 = :status: 200
							resp_pos += WT_WriteVarint(response + resp_pos, sizeof(qpack_headers));
							Q_memcpy(response + resp_pos, qpack_headers, sizeof(qpack_headers));
							resp_pos += sizeof(qpack_headers);

							uint64_t resp_err = 0;
							ssize_t sent = quiche_conn_stream_send(conn, stream_id, response, resp_pos, false, &resp_err);
							if (sent > 0)
							{
								client->wt_session_established = TRUE;
								client->wt_session_id = stream_id;
								Con_DPrintf("WT: Session established for client %d\n", client->client_id);
							}
						}
						else
						{
							// Unknown request - send 400 Bad Request
							uint8_t response[64];
							size_t resp_pos = 0;

							resp_pos += WT_WriteVarint(response + resp_pos, H3_FRAME_HEADERS);
							uint8_t qpack_400[] = { 0x00, 0x00, 0xda };	// index 26 = :status: 400
							resp_pos += WT_WriteVarint(response + resp_pos, sizeof(qpack_400));
							Q_memcpy(response + resp_pos, qpack_400, sizeof(qpack_400));
							resp_pos += sizeof(qpack_400);

							uint64_t resp_err = 0;
							quiche_conn_stream_send(conn, stream_id, response, resp_pos, true, &resp_err);
							Con_DPrintf("WT: Rejected unknown request\n");
						}
					}
				}
			}
			// Client-initiated unidirectional streams (2, 6, 10, ...) are control/QPACK streams
			else if ((stream_id & 0x03) == 2 && recv_len > 0)
			{
				// First byte is stream type
				uint64_t stream_type = 0;
				size_t type_len = WT_ReadVarint(buf, recv_len, &stream_type);

				if (stream_type == H3_STREAM_TYPE_CONTROL)
				{
					client->peer_control_stream_id = stream_id;
					// Rest of data is frames - look for SETTINGS
					if ((size_t)recv_len > type_len)
					{
						uint64_t frame_type = 0, frame_len = 0;
						size_t pos = type_len;
						pos += WT_ReadVarint(buf + pos, recv_len - pos, &frame_type);
						pos += WT_ReadVarint(buf + pos, recv_len - pos, &frame_len);
						if (frame_type == H3_FRAME_SETTINGS)
						{
							client->peer_settings_received = TRUE;
						}
					}
				}
			}
		}
		quiche_stream_iter_free(readable);
	}

	// Check for datagrams once session is established
	if (client->wt_session_established)
	{
		uint8_t dgram_buf[NET_MAX_MESSAGE];
		ssize_t dgram_len;

		while ((dgram_len = quiche_conn_dgram_recv(conn, dgram_buf, sizeof(dgram_buf))) > 0)
		{
			// Skip the session ID varint prefix
			size_t offset = 0;
			if (dgram_buf[0] < 0x40)
				offset = 1;
			else if (dgram_buf[0] < 0x80)
				offset = 2;
			else
				offset = 4;

			if (dgram_len > (ssize_t)offset)
			{
				WT_QueueDatagram(dgram_buf + offset, dgram_len - offset, client->client_id);
			}
		}
	}
}

// Forward declaration
static void WT_SendPendingPackets(wt_server_conn_t *client);

/*
==================
WT_ProcessIncomingPacket

Process an incoming QUIC packet (called from NET_QueuePacket when QUIC detected)
==================
*/
void WT_ProcessIncomingPacket(const unsigned char *data, int len, const netadr_t *from)
{
	struct sockaddr_in from_addr;
	wt_server_conn_t *client;
	quiche_conn *conn;
	uint8_t dcid[QUICHE_MAX_CONN_ID_LEN];
	uint8_t scid[QUICHE_MAX_CONN_ID_LEN];
	uint8_t odcid[QUICHE_MAX_CONN_ID_LEN];
	uint8_t token[1024];
	size_t dcid_len = sizeof(dcid);
	size_t scid_len = sizeof(scid);
	size_t odcid_len = sizeof(odcid);
	size_t token_len = sizeof(token);
	uint32_t version;
	uint8_t pkt_type;
	ssize_t recv_len;

	if (!wt_server.initialized || wt_socket < 0)
		return;

	// Convert netadr_t to sockaddr_in
	// Note: netadr_t.port is kept in network byte order (consistent with rest of codebase)
	Q_memset(&from_addr, 0, sizeof(from_addr));
	from_addr.sin_family = AF_INET;
	Q_memcpy(&from_addr.sin_addr.s_addr, from->ip, 4);
	from_addr.sin_port = from->port;

	// Parse QUIC header
	if (quiche_header_info(data, len, QUICHE_MAX_CONN_ID_LEN,
		&version, &pkt_type, scid, &scid_len, dcid, &dcid_len,
		token, &token_len) < 0)
	{
		return;
	}

	// Look for existing connection
	client = WT_FindClientByAddr(from);

	if (client && client->quiche_conn)
	{
		conn = (quiche_conn *)client->quiche_conn;
	}
	else
	{
		// New connection - try to accept any long header packet
		// quiche will validate the packet type internally

		// Accept new connection
		client = WT_AllocClient();
		if (!client)
		{
			Con_DPrintf("WT: No free client slots\n");
			return;
		}

		// Create SSL object and QUIC connection
		SSL *ssl = SSL_new((SSL_CTX *)wt_server.ssl_ctx);
		if (!ssl)
		{
			Con_DPrintf("WT: ERROR: Failed to create SSL object\n");
			WT_FreeClient(client);
			return;
		}

		conn = quiche_conn_new_with_tls(scid, scid_len,
			(token_len > 0) ? odcid : NULL,
			(token_len > 0) ? odcid_len : 0,
			(struct sockaddr *)&wt_local_addr, wt_local_addr_len,
			(struct sockaddr *)&from_addr, sizeof(from_addr),
			(quiche_config *)wt_server.quiche_config, ssl, true);

		if (!conn)
		{
			Con_DPrintf("WT: ERROR: quiche_conn_new_with_tls failed\n");
			SSL_free(ssl);
			WT_FreeClient(client);
			return;
		}

		client->quiche_conn = conn;
		client->client_addr = *from;
		client->last_activity = (uint64)(Sys_FloatTime() * 1000);
		Con_DPrintf("WT: New connection from %s\n", NET_AdrToString(*from));
	}

	// Feed packet to quiche
	quiche_recv_info recv_info;
	recv_info.from = (struct sockaddr *)&from_addr;
	recv_info.from_len = sizeof(from_addr);
	recv_info.to = (struct sockaddr *)&wt_local_addr;
	recv_info.to_len = wt_local_addr_len;

	recv_len = quiche_conn_recv(conn, (uint8_t *)data, len, &recv_info);

	if (recv_len < 0 && recv_len != QUICHE_ERR_DONE)
	{
		Con_DPrintf("WT: quiche_conn_recv error %zd\n", recv_len);
		if (recv_len == QUICHE_ERR_TLS_FAIL)
		{
			unsigned long err;
			char err_buf[256];
			while ((err = ERR_get_error()) != 0)
			{
				ERR_error_string_n(err, err_buf, sizeof(err_buf));
				Con_DPrintf("WT: TLS error: %s\n", err_buf);
			}
		}
	}

	client->last_activity = (uint64)(Sys_FloatTime() * 1000);

	// Use raw HTTP/3 mode (bypass quiche's H3 layer for WebTransport)
	if (quiche_conn_is_established(conn))
	{
		if (!client->raw_h3_mode)
		{
			client->raw_h3_mode = TRUE;
			client->peer_control_stream_id = (uint64)-1;	// Not found yet
		}

		// Process raw HTTP/3 streams
		WT_ProcessRawH3(client);
	}

	// Send pending response packets immediately (critical for handshake!)
	WT_SendPendingPackets(client);

	// Check if connection is closed
	if (quiche_conn_is_closed(conn))
	{
		Con_DPrintf("WT: Client %s disconnected\n", NET_AdrToString(*from));
		WT_DropEngineClient(&client->client_addr);
		WT_FreeClient(client);
	}
}

/*
==================
WT_SendPendingPackets

Send any pending QUIC packets for a connection
==================
*/
static void WT_SendPendingPackets(wt_server_conn_t *client)
{
	uint8_t out[WT_MAX_DATAGRAM_SIZE];
	quiche_send_info send_info;
	ssize_t written;
	struct sockaddr_in dest_addr;

	if (!client || !client->quiche_conn)
		return;

	// Convert netadr_t to sockaddr
	// Note: netadr_t.port is already in network byte order
	Q_memset(&dest_addr, 0, sizeof(dest_addr));
	dest_addr.sin_family = AF_INET;
	Q_memcpy(&dest_addr.sin_addr.s_addr, client->client_addr.ip, 4);
	dest_addr.sin_port = client->client_addr.port;

	while (1)
	{
		written = quiche_conn_send((quiche_conn *)client->quiche_conn, out, sizeof(out), &send_info);

		if (written == QUICHE_ERR_DONE)
			break;

		if (written < 0)
		{
			Con_DPrintf("WT: ERROR: quiche_conn_send error %zd\n", written);
			break;
		}

		if (sendto(wt_socket, out, written, 0, (struct sockaddr *)&dest_addr, sizeof(dest_addr)) < 0)
		{
			Con_DPrintf("WT: ERROR: sendto failed: %s\n", strerror(errno));
		}
	}
}

/*
==================
WT_ServerFrame

Process timeouts and send pending packets (call this every frame)
Packet receiving is done via WT_ProcessIncomingPacket from NET_QueuePacket
==================
*/
void WT_ServerFrame()
{
	int i;
	uint64 now;

	if (!wt_server.initialized)
		return;

	// Rotate self-generated certificates well before the validity cliff so
	// permanently-running servers never serve an expired cert (age check
	// throttled to once a minute)
	double now_sec = Sys_FloatTime();
	if (now_sec > wt_cert_next_check)
	{
		wt_cert_next_check = now_sec + 60.0;
		if (now_sec - wt_cert_created > WT_CERT_ROTATE_SECONDS)
			WT_RotateCert();
	}

	// Send pending packets and check timeouts
	now = (uint64)(Sys_FloatTime() * 1000);
	for (i = 0; i < WT_MAX_CLIENTS; i++)
	{
		if (!wt_server.clients[i].active)
			continue;

		// Check for timeout
		if (now - wt_server.clients[i].last_activity > WT_TIMEOUT_MS)
		{
			Con_DPrintf("WT: Client %d timed out\n", i);
			WT_DropEngineClient(&wt_server.clients[i].client_addr);
			WT_FreeClient(&wt_server.clients[i]);
			continue;
		}

		// Process quiche timeouts
		if (wt_server.clients[i].quiche_conn)
		{
			quiche_conn_on_timeout((quiche_conn *)wt_server.clients[i].quiche_conn);
		}

		WT_SendPendingPackets(&wt_server.clients[i]);
	}
}

/*
==================
WT_ServerSendDatagram

Send an unreliable datagram to a client
==================
*/
qboolean WT_ServerSendDatagram(int client_id, const void *data, int len)
{
	wt_server_conn_t *client;
	quiche_conn *conn;
	uint8_t dgram_buf[NET_MAX_MESSAGE + 8];
	size_t dgram_len;
	ssize_t sent;

	client = WT_FindClientById(client_id);
	if (!client || !client->quiche_conn || !client->wt_session_established)
		return FALSE;

	conn = (quiche_conn *)client->quiche_conn;

	// WebTransport datagrams need session ID prefix (quarter stream ID as varint)
	// Session ID = stream_id / 4
	{
		uint64 session_id = client->wt_session_id / 4;
		size_t offset = 0;

		// Encode varint (simplified - handles up to 2-byte varints)
		if (session_id < 0x40)
		{
			dgram_buf[0] = (uint8_t)session_id;
			offset = 1;
		}
		else
		{
			dgram_buf[0] = 0x40 | (uint8_t)(session_id >> 8);
			dgram_buf[1] = (uint8_t)(session_id & 0xFF);
			offset = 2;
		}

		if ((size_t)len > sizeof(dgram_buf) - offset)
			len = sizeof(dgram_buf) - offset;

		Q_memcpy(dgram_buf + offset, data, len);
		dgram_len = offset + len;
	}

	sent = quiche_conn_dgram_send(conn, dgram_buf, dgram_len);
	if (sent < 0)
	{
		Con_DPrintf("WT: dgram_send error %zd\n", sent);
		return FALSE;
	}

	// Send any pending packets immediately
	WT_SendPendingPackets(client);

	return TRUE;
}

/*
==================
WT_ServerSendToAddr

Send to an address if it's a WebTransport client
Returns TRUE if sent via WebTransport, FALSE if not a WT client
==================
*/
qboolean WT_ServerSendToAddr(const netadr_t *to, const void *data, int len)
{
	wt_server_conn_t *client;

	if (!wt_server.initialized || !to)
		return FALSE;

	client = WT_FindClientByAddr(to);
	if (!client || !client->wt_session_established)
		return FALSE;

	return WT_ServerSendDatagram(client->client_id, data, len);
}

/*
==================
WT_ServerRecvDatagram

Receive an unreliable datagram from any client (non-blocking)
Returns TRUE if a datagram was received
==================
*/
qboolean WT_ServerRecvDatagram(void *data, int *len, netadr_t *from, int *client_id)
{
	wt_server_conn_t *client;

	if (wt_recv_queue_head == wt_recv_queue_tail)
		return FALSE;

	*len = wt_recv_queue[wt_recv_queue_head].len;
	Q_memcpy(data, wt_recv_queue[wt_recv_queue_head].data, *len);

	if (client_id)
		*client_id = wt_recv_queue[wt_recv_queue_head].client_id;

	if (from)
	{
		client = WT_FindClientById(wt_recv_queue[wt_recv_queue_head].client_id);
		if (client)
			*from = client->client_addr;
	}

	wt_recv_queue_head = (wt_recv_queue_head + 1) % WT_MAX_PENDING_DATAGRAMS;

	return TRUE;
}

/*
==================
WT_ServerGetClientCount

Get the number of connected clients
==================
*/
int WT_ServerGetClientCount()
{
	return wt_server.client_count;
}

/*
==================
WT_ServerIsActive

QUIC/WebTransport is attached to the game socket and accepting connections
==================
*/
qboolean WT_ServerIsActive()
{
	return wt_server.initialized;
}

/*
==================
WT_GetCertHash / WT_GetPrevCertHash

Current and pre-rotation certificate hashes (empty string when unset),
announced to the master so web clients can pin before connecting
==================
*/
const char *WT_GetCertHash()
{
	return wt_server.cert_hash;
}

const char *WT_GetPrevCertHash()
{
	return wt_server.prev_cert_hash;
}

/*
==================
WT_ServerDisconnectClient

Disconnect a specific client
==================
*/
void WT_ServerDisconnectClient(int client_id)
{
	wt_server_conn_t *client;

	client = WT_FindClientById(client_id);
	if (client)
	{
		if (client->quiche_conn)
		{
			quiche_conn_close((quiche_conn *)client->quiche_conn, true, 0, NULL, 0);
			WT_SendPendingPackets(client);
		}
		WT_FreeClient(client);
	}
}

#endif // REHLDS_QUIC

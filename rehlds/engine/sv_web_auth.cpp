/*
*    Web-account admission for browser (WebTransport) clients — see sv_web_auth.h.
*/

#include "precompiled.h"

#include <curl/curl.h>
#include <stdlib.h> // getenv
#include <string.h> // strstr/strncmp/memcpy

#include "sv_web_auth.h"

// Small response buffer for the fixed JSON:
//   {"valid":true,"account_id":1,"username":"...","banned":false}
struct wa_buf_t
{
	char   data[2048];
	size_t len;
};

static size_t WA_Write(void *ptr, size_t size, size_t nmemb, void *userp)
{
	wa_buf_t *b = static_cast<wa_buf_t *>(userp);
	size_t total = size * nmemb;
	size_t space = sizeof(b->data) - 1 - b->len;
	size_t take  = (total < space) ? total : space;
	if (take)
	{
		memcpy(b->data + b->len, ptr, take);
		b->len += take;
		b->data[b->len] = '\0';
	}
	// Report all bytes consumed so libcurl doesn't treat a full buffer as an error.
	return total;
}

static const char *WA_Url()
{
	const char *u = getenv("GAME_AUTH_URL");
	return (u && u[0]) ? u : "https://goldsrc.net/api/game-ticket/validate";
}

qboolean SV_ValidateGameTicket(const char *ticket, qboolean *outValid, uint32 *outAccountId, qboolean *outBanned)
{
	*outValid = FALSE;
	*outAccountId = 0;
	*outBanned = FALSE;

	if (!ticket || !ticket[0])
		return FALSE;

	const char *secret = getenv("GAME_SERVER_SECRET");
	if (!secret || !secret[0])
	{
		Con_DPrintf("web-auth: GAME_SERVER_SECRET not set; skipping ticket validation\n");
		return FALSE;
	}

	CURL *curl = curl_easy_init();
	if (!curl)
		return FALSE;

	wa_buf_t buf;
	buf.len = 0;
	buf.data[0] = '\0';

	char authHdr[512];
	char tktHdr[320];
	Q_snprintf(authHdr, sizeof(authHdr), "Authorization: Bearer %s", secret);
	Q_snprintf(tktHdr, sizeof(tktHdr), "X-Game-Ticket: %s", ticket);

	struct curl_slist *hdrs = NULL;
	hdrs = curl_slist_append(hdrs, authHdr);
	hdrs = curl_slist_append(hdrs, tktHdr);
	hdrs = curl_slist_append(hdrs, "Content-Length: 0");

	curl_easy_setopt(curl, CURLOPT_URL, WA_Url());
	curl_easy_setopt(curl, CURLOPT_POST, 1L);
	curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, 0L);
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WA_Write);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 4L);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 6L);
	curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L); // don't use SIGALRM in a multi-thread-ish process
	curl_easy_setopt(curl, CURLOPT_USERAGENT, "goldsrc-rehlds/1");

	CURLcode rc = curl_easy_perform(curl);
	long http = 0;
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http);
	curl_slist_free_all(hdrs);
	curl_easy_cleanup(curl);

	if (rc != CURLE_OK)
	{
		Con_DPrintf("web-auth: request failed: %s\n", curl_easy_strerror(rc));
		return FALSE;
	}

	// Scan the small JSON directly (no JSON lib in the engine). Keys are emitted
	// in a fixed shape by the site; a missing key just leaves its default.
	const char *p;
	if ((p = strstr(buf.data, "\"valid\":")) != NULL)
		*outValid = (strncmp(p + 8, "true", 4) == 0) ? TRUE : FALSE;
	if ((p = strstr(buf.data, "\"account_id\":")) != NULL)
		*outAccountId = (uint32)Q_atoi(p + 13);
	if ((p = strstr(buf.data, "\"banned\":")) != NULL)
		*outBanned = (strncmp(p + 9, "true", 4) == 0) ? TRUE : FALSE;

	if (http != 200)
		Con_DPrintf("web-auth: HTTP %ld (valid=%d)\n", http, (int)*outValid);

	return TRUE;
}

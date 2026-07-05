/*
*    Web-account admission for browser (WebTransport) clients.
*
*    A logged-in goldsrc.net player carries a short one-time connect ticket in
*    userinfo ("_gt"). This redeems it against the site's introspection endpoint
*    (https://goldsrc.net/api/game-ticket/validate by default) to obtain a stable
*    per-account identity, so web players are no longer the shared SteamID 0.
*
*    Only compiled/linked when ENABLE_QUIC (REHLDS_QUIC) is set.
*/

#pragma once

// Redeem a connect ticket against the site.
//   Returns TRUE  if the site was reached and returned a decision
//                 (results in the out params).
//   Returns FALSE on transport/config error — the caller keeps the fallback
//                 (admit as SteamID 0 during rollout).
//   *outValid     — ticket accepted AND the account is not banned
//   *outAccountId — the site account id (0 if none)
//   *outBanned    — the account is banned (reject even though "reached")
qboolean SV_ValidateGameTicket(const char *ticket, qboolean *outValid, uint32 *outAccountId, qboolean *outBanned);

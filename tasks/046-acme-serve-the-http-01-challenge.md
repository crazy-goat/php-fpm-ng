# 046 — ACME: serve the HTTP-01 challenge from the local-answer hook

**Priority:** medium. Small, and the shape it must take is already decided.
**Status:** open. Depends on 042 (the plain port) and 045 (handover from the
dedicated PHP ACME process chosen in 043).

## Context

The gateway already has exactly one place for responses it produces without
occupying a worker: `fpm_http_try_local()` (`sapi/fpmng/fpm/fpm_http.c:1303`),
under a header comment that names this case explicitly
(`fpm_http.c:1041-1048`): "Today: static files; later, add the ACME challenge
(/.well-known/acme-challenge/) and /status here". There is also a note in the body
that ordering will matter once ACME arrives: fixed paths first, files from disk
last (`fpm_http.c:1339-1340`).

`docs/NOTES.md` section 3l lists "one point that answers without a worker" as a
thing to design early precisely so this task does not become a rewrite. It was
designed early. This task is the payoff.

## Problem

Answer `GET /.well-known/acme-challenge/<token>` with the key authorization for
that token, over plain HTTP, without touching a worker and without depending on
`http.static`. The dedicated ACME cron process supplies and removes token state;
it does not write a file under the document root.

## Acceptance criteria

1. A request for a known token returns 200, `Content-Type: text/plain`, and the
   exact key authorization with no trailing newline added.
2. A request for an unknown token returns 404 and does not reach a worker.
   Verified from the pool's own metrics or access log, not by eyeballing.
3. The challenge path is answered **before** static files are considered, and a
   file physically present under the document root at that path cannot shadow
   or leak into the response.
4. It works with `http.static = 0`. The challenge is not a static file feature.
5. Path handling rejects traversal exactly as the existing local-answer path
   does (`fpm_http.c:1325-1331`); a token is a flat opaque string, never a path.
6. Every gateway process answers it, whichever one `SO_REUSEPORT` hands the
   connection to — the CA gets one connection and no retry guarantee.
   Verified with `http.gateways` greater than 1.
7. Starting, completing or failing a challenge in the dedicated ACME process
   adds or removes the token in every gateway. No gateway executes the PHP
   client and no challenge key authorization is written to a static-file path.

## Notes

- Point 6 is the reason this is not simply "write the token to a file": the
  answering process may not be the process that created it. That is 045's
  problem, but this task is where it becomes visible.

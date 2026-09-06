# 042 — A plain HTTP listener alongside the TLS one (redirect, and ACME HTTP-01)

**Priority:** medium. Blocks 020 if HTTP-01 is the chosen challenge type.
**Status:** open, config-model question first.

## Context

A pool has one `http.listen` (`sapi/fpmng/fpm/fpm_http.c`, `http.listen`
directive; task 015 made it always required). A TLS setup needs two ports at
once:

- **:443** for traffic
- **:80** for the HTTP-01 challenge (`/.well-known/acme-challenge/<token>` must
  be reachable over plain HTTP on port 80 — the CA will not follow a redirect
  to a port it did not ask for) and for redirecting human traffic to HTTPS

`docs/NOTES.md`, section 3l, flags this exactly: "sprawdzić, czy 'jeden pool,
jeden port' wystarcza". It does not, and the answer shapes the configuration
file that the whole project is selling.

## Problem

Decide how one application gets both ports, then implement it.

Candidate shapes, none chosen:

- **Two pools.** Works today with no new code: a `pool.type = http` on :80 with
  `http.static` serving the challenge directory, and another on :443. Costs a
  second pool block plus duplicated settings in a config file that is supposed
  to fit an application in forty lines (`docs/NOTES.md`, section 6,
  "Konfiguracja").
- **A second listener inside one pool.** One `http.listen` for TLS plus
  something like a plain-HTTP companion port. Keeps the config small, but the
  gateway's listener setup is currently one socket per gateway process.
- **Redirect-only companion.** The plain port answers only a redirect and the
  ACME challenge, never reaches a worker. Smallest surface, and it fits the
  local-answer hook (`fpm_http.c:1041-1048`, which already names the ACME
  challenge as a future case there).

## Acceptance criteria

1. The chosen shape is written down here and in `docs/NOTES.md` before
   implementation, with the config file for "one application on :80 and :443"
   shown in full. If it does not fit in a handful of lines, that is an argument
   against the shape, not a detail.
2. A request to `http://host/anything` returns 301 or 308 to the same path on
   `https://host/`, preserving path and query string.
3. A request under `/.well-known/acme-challenge/` on the plain port is **not**
   redirected and is answered by the gateway itself, without occupying a worker.
4. The plain port never serves application content and never hands a request to
   a worker, unless the operator explicitly configured it to.
5. Verified with `http.gateways` greater than 1 and `http.reuseport` both on
   and off.

## Explicitly out of scope

- The ACME protocol itself. This task only makes the port and the path
  reachable; 046 fills the challenge response in.

# 042 — A plain HTTP listener alongside the TLS one (redirect, and ACME HTTP-01)

**Priority:** medium. Blocks 020 if HTTP-01 is the chosen challenge type.
**Status:** decided (2026-09-06, project owner): redirect-only companion.
Implementation open.

## Decision

Redirect-only companion, inside the same pool as the TLS listener. The plain
port never reaches a worker: it answers only a 301/308 redirect to the same
path on `https://host/`, and — once 046 lands — the ACME HTTP-01 challenge
under `/.well-known/acme-challenge/`, both from the existing local-answer hook
(`fpm_http.c:1041-1048`). This is the smallest surface of the three candidates
and keeps the config file small (one extra directive, not a second pool
block). Cost accepted: the gateway's "one socket per gateway process" listener
setup gains a second, plain-HTTP socket per process for pools that opt in.

## Context

A pool has one `http.listen` (`sapi/fpmng/fpm/fpm_http.c`, `http.listen`
directive; task 015 made it always required). A TLS setup needs two ports at
once:

- **:443** for traffic
- **:80** for the HTTP-01 challenge (`/.well-known/acme-challenge/<token>` must
  be reachable over plain HTTP on port 80 — the CA will not follow a redirect
  to a port it did not ask for) and for redirecting human traffic to HTTPS

`docs/NOTES.md`, section 3l, flags this exactly: "check whether 'one pool,
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

## Outcome

Implemented `http.plain_listen` as an optional second listener in the same HTTP
pool. It accepts only GET and HEAD, redirects ordinary requests with 308 while
preserving host, path, and query, and locally returns 404 for the reserved
`/.well-known/acme-challenge/` path until task 046 supplies challenge data.
The plain listener has no path to the FastCGI request queue.

`build/test-http-plain-listener.sh` exercises three gateways with
`http.reuseport` both disabled and enabled. It verifies TLS application
dispatch, redirect preservation, the empty redirect body, and the local ACME
404 without application content.

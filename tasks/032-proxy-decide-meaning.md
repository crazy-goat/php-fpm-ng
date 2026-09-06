# 032 — `pool.type = proxy`: decide what it means before anything is built

**Priority:** low until decided; the decision itself is cheap and unblocks
nothing else, so there is no urgency pressure other than avoiding
speculative code.
**Status:** open. Decision task — no implementation should start against this
file.

## Context

The project owner has mentioned "proxy" as a direction, the same way
"scheduler" was mentioned (see task 034). Neither word has a requirement
behind it yet. This task is scoped to `proxy` only.

**No code or directive named `proxy` exists.** Checked: no `pool.type =
proxy` string, no `proxy` directive, no partial implementation anywhere
under `sapi/fpmng/fpm/*.c` or `*.h`.

What **does** exist, and matters here because it points the opposite
direction, is `http.trusted_proxies` (`fpm_conf.c:179`,
`fpm_http.c:189-190`). It configures which upstream peers this gateway
should **trust** to have already terminated the real client connection —
i.e. it assumes **someone else's** proxy sits in front of FPM-NG, and FPM-NG
reads `X-Forwarded-For`/`-Proto`/`-Port` from it (`fpm_http.c:1368-1371`,
`fpm_http_forwarded_resolve()`). A `pool.type = proxy` would be FPM-NG
**acting as** a proxy — the reverse role. The two are not in tension, but
they are not the same feature, and naming a new pool type "proxy" next to a
directive that means "trust an external proxy" is worth being deliberate
about.

`docs/node_server_gaps.md` already touches this from one angle: it lists
WebSocket support as "not the model's job to add directly" and names "a
future `pool.type = proxy`" as the most architecturally consistent way to get
WebSocket tunneling without changing the PHP execution model. That is one
data point, not a decision — the document itself marks WebSocket as "nie jest
obecnie zatwierdzonym punktem roadmapy."

## The candidate meanings

None of these has been requested. They are laid out so whoever picks up this
task has the options in front of them, not so this task pre-empts the
decision.

### A. Reverse proxy in front of pools of the same fpm-ng process

A `proxy` pool type that receives a request and routes it to one of
**this process's own** other pools (e.g. by `Host` header or path prefix),
the way nginx routes to multiple upstreams from one `server` block.

- **What it would be for:** running several distinct pools (different
  document roots, different pool types) behind one externally-visible
  address, so an operator doesn't need one `listen` per pool.
- **Size:** moderate — mostly routing/config logic, no new I/O model, reuses
  the existing gateway-to-pool FastCGI path.
- **What it makes redundant:** partially overlaps `http.front_controller`
  (already does path-based dispatch to one script) and the fact that each
  pool already gets its own `listen` address. An operator can already run N
  pools on N ports/addresses and put a single `listen` in front with nothing
  fpm-ng specific. This meaning adds convenience, not a missing capability.

### B. `proxy_pass` to an external backend (a Node service, a second container, any HTTP upstream)

A `proxy` pool type that terminates HTTP/TLS the same way `http` does today,
but instead of speaking FastCGI to a PHP worker, forwards the request
(and, per the WebSocket note in `docs/node_server_gaps.md`, potentially an
Upgrade) to an arbitrary HTTP backend — same container or a different one.

- **What it would be for:** the project's own thesis is "one binary plus
  application code, no nginx" — but a real deployment is rarely 100% PHP.
  This is the meaning that lets a non-PHP sidecar (a Node WebSocket service,
  a queue worker with its own HTTP health/metrics endpoint) sit behind the
  same edge without reintroducing nginx just for that one route.
- **Size:** large. New pool type, a second upstream protocol path distinct
  from FastCGI, connection lifecycle for Upgrade/WebSocket if that is
  included, and a TLS-terminated front end that already exists and would be
  reused.
- **What it makes redundant:** nginx/Traefik/Caddy as the thing bolted on
  purely to route one non-PHP route — the exact case the project's thesis
  says it wants to avoid reintroducing.

### C. Load balancing between workers

A `proxy` pool type distributing requests across multiple worker processes
or pools by some policy (round robin, least-connections).

- **What it would be for:** nothing that isn't already done. Classic FPM's
  worker model already has many processes accepting from one listening
  socket (or, with `http.reuseport`, several gateway processes sharing one);
  the kernel/accept-queue already distributes connections across idle
  workers. There is no described gap this closes.
- **Size:** N/A — no problem identified to size.
- **What it makes redundant:** nothing; it would duplicate the accept-queue
  model FPM already uses.

### D. Forwarding to a FastCGI pool over a unix socket

A `proxy` pool type whose job is purely to hand a connection to another
**FastCGI** pool over a unix socket — effectively a FastCGI-to-FastCGI relay.

- **What it would be for:** unclear. Any pool can already listen on a unix
  socket directly; a relay in front of a FastCGI pool adds a hop without an
  identified benefit (no protocol translation happens, since both ends speak
  FastCGI).
- **Size:** small, but only because it does very little.
- **What it makes redundant:** itself — a second `listen` unix socket
  achieves the same routing without a new pool type.

## Problem

Pick one of the above (or a meaning not listed here), reject the rest with
reasons, or decide the feature isn't needed at all. **This requirement has
not been given by the project owner** — "scheduler" was named as a
direction, "proxy" less so, and this task exists to force the question
before any code gets written under that name.

## Acceptance criteria

1. Exactly one meaning is chosen, in writing, with the reasoning for
   rejecting the others recorded in this file or its replacement — not just
   "we picked B."
2. The choice states explicitly what capability gap it closes, referencing a
   concrete, currently-impossible scenario (not a hypothetical one).
3. `http.trusted_proxies` and the chosen meaning are reconciled in one
   paragraph — if the project both trusts an external proxy in front of it
   *and* acts as one itself, the two configurations must not be confusable
   by an operator reading `docs/`.
4. A legitimate, acceptable outcome of this task is "we don't build this."
   If that is the answer, record it here (or move this file to `done/` with
   that outcome) rather than leaving the question open indefinitely.

## Explicitly out of scope

- Any implementation. This is a decision task; a chosen meaning does not
  imply this task also covers building it — that would be filed separately
  once the meaning is fixed.
- WebSocket support as a whole; it is one motivating scenario for meaning B,
  not something this task commits to delivering.

## Notes

- `docs/node_server_gaps.md`'s own lean (meaning B, for WebSocket
  specifically) is worth reading before deciding, but it is one document's
  opinion, formed while looking at a different question (Node parity), not
  a decision made by the project owner.

# 031 — Turn the "already in plan" half of the Node gap analysis into owned work

**Priority:** medium. Nothing here is a regression, but three of the six items
below are user-visible gaps with no open task, which means nobody is
accountable for them.
**Status:** open.

## Context

`docs/node_server_gaps.md` compares the HTTP gateway to a typical Node.js
application server and ends with a `## Klasyfikacja` section that sorts every
finding into three buckets. This task only looks at the first bucket, `### Już
w planie` (six items). The other two buckets (`Do rozpoznania, ale nie w
roadmapie`, `Świadomie nie traktować jako brak kompatybilności`) are
deliberately out of scope here — they are exploration or non-goals, not
commitments.

The document is dated "Stan na 2026-09-06" but carries no reference to which
commit it was written against, and code has moved since (TLS, ACME task
filing, `http.front_controller`, `http.trusted_proxies` all landed after some
of these gaps were first noticed by hand). The point of this task is to check
each of the six "already in plan" items against what is actually in the tree
today, not against the document's own words.

## The six items, checked against the code

### 1. Routing / `try_files` equivalent — done, remaining gaps already tracked

`http.front_controller` exists (`fpm_conf.c:181`), defaults to `/index.php`,
and falls back a request whose computed `SCRIPT_FILENAME` doesn't exist to the
front controller with the original path in `PATH_INFO`. This is not a guess —
task `018-front-controller-remaining-gaps.md` verifies the exact behaviour
(`/mix?x=1` → `SCRIPT_NAME=/index.php`, `PATH_INFO=/mix`) and lists the two
things left over: a directory without an index doesn't fall back, and the
containment check is evaluated lazily on first request.

**Verdict: implemented. Do not duplicate — 018 already owns what's left.**

### 2. Client timeouts — only partially done, and the part that's missing has no task

The only client-facing timeout directive that exists is `http.idle_timeout`
(`fpm_conf.c:177`, default 500ms, `fpm_http.c:141`). Its own comment describes
its purpose precisely: "release a pinned worker after this much idle time"
(`fpm_http.c:184`) — it protects a **worker slot** from an idle keep-alive
connection, wired into the event loop at `fpm_http.c:810` and validated at
`fpm_http.c:1960`.

There is nothing else. Searching `fpm_http.c` for a header-read timeout, a
request-body-read timeout, or any slow-loris-style guard on an
**in-progress** read from a slow client turns up no second mechanism — only
`idle_timeout`. A client that trickles headers or a request body in slowly
(a few bytes every few seconds, never going fully idle) is not covered by
`idle_timeout` at all, because the connection is never idle from the
gateway's point of view.

**Verdict: genuinely open, and untracked. `docs/node_server_gaps.md` itself
says "nie ma jeszcze kompletnej ochrony" — this is the missing part.**

### 3. Request body streaming / backpressure — not implemented, untracked

The gateway reads the body via `evhttp_request_get_input_buffer()`
(`fpm_http.c:443`), which is libevent's `evhttp` handing over a buffer that is
already fully assembled — `evhttp` itself buffers the whole request (headers
and body) before invoking the request callback. The only body-size control is
`evhttp_set_max_body_size(gw->http, FPM_HTTP_MAX_BODY)` at `fpm_http.c:1533`,
which is a hard cap that rejects an oversized request outright, not
backpressure or streaming.

**Verdict: genuinely open, and untracked, exactly as the document says.**

### 4. `503 Service Unavailable` + `Retry-After` on a full pool — not implemented, untracked

`FPM_HTTP_BAD_GATEWAY` is `502` (`fpm_http.c:150`, with the comment "libevent
has no constant for it"). It has exactly two call sites:

- `fpm_http.c:754-755`, in `fpm_http_finish()`, for "no answer from upstream" —
  this fires whenever the connection to the pool failed or produced no
  response, which includes the pool being unable to accept a new connection
  (i.e. a full pool), but is indistinguishable in the response from any other
  upstream failure (worker crash, FastCGI protocol error).
- `fpm_http.c:1290-1291`, for a static-file `evbuffer_add_file()` failure —
  unrelated to pool capacity.

There is no `503`, no `Retry-After` header, and no code path that
distinguishes "pool is at capacity, try again shortly" from "something is
broken." A client hammering a full pool gets a generic `502` with no signal
that backing off would help.

**Verdict: genuinely open, and untracked, exactly as the document says.**

### 5. TLS + ACME — TLS done, ACME already tracked

TLS termination is implemented: `http.tls_cert`, `http.tls_key`,
`http.tls_min_version` are real directives (`fpm_conf.c:182-184`), backed by
`fpm_http_tls.c` (referenced from task `010` and `020`). ACME was explicitly
scoped out at that point and is tracked as its own decision-first task,
`020-acme-certificates.md`, which is open.

**Verdict: TLS done. ACME already has an owner. Do not duplicate.**

### 6. `pool.type = proxy` — not implemented, tracked by a new task in this batch

`grep` for `proxy` across `sapi/fpmng/fpm/*.c` and `*.h` finds nothing — no
pool type, no directive, no partial implementation. This is real, but it is a
decision problem before it is an engineering problem (what would "proxy" even
mean here — see task `032`, filed alongside this one).

**Verdict: genuinely open, but do not add acceptance criteria for it here —
032 owns the decision, and no implementation should start before that decision
is made.**

### HTTP/2 and gzip — correctly deprioritized, no action needed now

The document files these as "nice to have po realizacji podstawowego planu,"
explicitly after TLS/ALPN, timeouts and backpressure. `grep` confirms neither
exists in `fpm_http.c`. Since items 2, 3 and 4 above (timeouts, backpressure,
`503`) are themselves still open, HTTP/2 and gzip are correctly still waiting
their turn. This task does not file a stub for them — doing so now would just
be noise ahead of its own stated prerequisites.

## Problem

File the concrete, checkable gaps that `docs/node_server_gaps.md` names as
"already in plan" but that no task currently owns: full client timeout
coverage (item 2), request body backpressure (item 3), and `503 Retry-After`
on a full pool (item 4).

## Acceptance criteria

1. **Client timeouts.** A decision on what "timeout" means beyond
   `http.idle_timeout`: at minimum, whether a header-read timeout and a
   request-body-read timeout are separate directives or share one budget, and
   what happens to a connection that exceeds it (a defined HTTP status, sent
   if possible; the connection dropped if not). Verified by a client that
   sends one byte every few seconds and never goes idle — it must be cut off
   within a bounded, documented time, not left connected indefinitely.
2. **Backpressure.** A decision on where the memory bound moves to: today
   `http.max_body`-style limits (see `FPM_HTTP_MAX_BODY`) are the whole
   answer, applied only after the fact. Either a streaming path to the
   worker exists, or the decision to keep whole-body buffering is written
   down with its memory-usage consequence made explicit (N gateway processes
   × body size under concurrent slow uploads).
3. **`503` + `Retry-After`.** A full pool is distinguishable, in the HTTP
   response, from any other upstream failure. Verified by filling a pool
   (`pm.max_children` small, enough concurrent slow requests to exhaust it)
   and confirming the next request gets `503` with a `Retry-After` value,
   not `502`.
4. None of the above touch `pool.type = proxy` — that stays task 032's
   decision to make first.

## Explicitly out of scope

- `pool.type = proxy` (task 032).
- ACME (task 020, already open).
- The front-controller directory-index and lazy-containment-check gaps
  (task 018, already open).
- HTTP/2 and gzip — correctly waiting behind the items above.

## Notes

- `docs/node_server_gaps.md` is undated against a commit, only against a
  calendar date. Whoever revisits this document next should consider adding a
  commit hash, since this task only exists because the classification had
  drifted from the code without anyone noticing.

# The per-pool operator endpoint

> **Planned change (v0.10.0):** the four `pm.*` directives on this page are
> being renamed to an `operator.*` namespace, and a new `pool.type = gateway`
> will expose every pool's pages under `<base>/<pool name>` on its public port.
> This page describes what runs today; the target is in
> [`gateway.md`](gateway.md).

A `cron`, `supervisor`, `http` or `http-direct` pool has nothing in front of it
that could answer a monitoring scrape. A `fastcgi` pool does — the web server
that speaks FastCGI to it — which is why upstream FPM answers `pm.status_path`
inside a request and why that arrangement is left alone here.

For the four types above, php-fpm-ng serves the answer itself, from a small
HTTP listener of its own:

```ini
[tick]
pool.type = cron
cron.schedule = */5 * * * *
cron.script = /srv/app/bin/tick.php

pm.status_listen = 127.0.0.1:8080
pm.status_path   = /tick/status
pm.metrics_listen = 127.0.0.1:8080
pm.metrics_path   = /tick/metrics
```

| Directive | Meaning |
| --- | --- |
| `pm.status_path` | Path answering JSON for this pool. Unset: off. |
| `pm.metrics_path` | Path answering Prometheus text for this pool. Unset: off. |
| `pm.status_listen` | Where `pm.status_path` binds. Default `127.0.0.1:8080`. |
| `pm.metrics_listen` | Where `pm.metrics_path` binds. Default `127.0.0.1:8080`. |

There is **no on/off directive**. The endpoint exists exactly when a path is
set. A `pm.status_listen` with no `pm.status_path` binds nothing — the address
is where an endpoint would go, not an instruction to open one.

The default is a loopback address on purpose. These pages describe the inside of
your process tree, and `listen.allowed_clients` is a weaker boundary than not
binding the port outward at all.

## One socket, many pools

Pools that name the same listen address share one socket and one process; their
paths are routes on it. That process is created by php-fpm-ng, not by you: it is
not a pool you can name, configure or see in `pool.type`'s list of known types.
It runs no PHP and speaks no FastCGI.

What is refused is two answers for one URL. An address, a port and a path
identify one endpoint, so:

```ini
[a]
pm.status_listen = 127.0.0.1:8080
pm.status_path   = /status

[b]
pm.status_listen = 127.0.0.1:8080
pm.status_path   = /status
```

fails at startup, naming both pools. Give one of them a different path, or a
different address. The same path on two different addresses is fine, and so is
one pool serving both formats from one address.

A request for a path no pool claimed gets a 404 listing the paths that listener
does answer.

One socket is one process, so it can have only one identity. The `user`, `group`,
`listen.owner`, `listen.group` and `listen.mode` of the pools sharing an address
must agree; if they do not, startup fails naming the directive and both pools.
Give one of them its own address. This matters more than it looks, because the
default address is the same for every pool: sharing is the normal case, not the
unusual one.

## Replacing a `pool.type = status` pool

That pool type is gone (issue #278). It was a listener of its own that reported
on every other pool in the master — which is what the operator endpoint does,
so keeping both meant two ways to configure the same listener and one page whose
contents depended on a pool that was not the one you were reading about.

A configuration that still names it does not start: the master says
`pool.type 'status' no longer exists` and names what to set instead, rather
than ignoring the pool and leaving you with a port nobody answers on.

To migrate, delete the pool and put the two paths on the pools you actually
want to watch, pointing them at the address the status pool used:

```ini
; before
[monitor]
listen = 127.0.0.1:9001
pool.type = status

; after
[api]
; …
pm.status_listen = 127.0.0.1:9001
pm.status_path = /api/status
pm.metrics_path = /api/metrics

[worker]
; …
pm.status_listen = 127.0.0.1:9001
pm.status_path = /worker/status
pm.metrics_path = /worker/metrics
```

`pm.metrics_listen` defaults to `pm.status_listen`, so the four paths above
share the one port the scraper was already pointed at. What changes for that
scraper is the path: one target per pool instead of one target holding every
pool. The JSON body is unchanged in shape — still `{"pools":[…]}` — but the
array is one element long, so a client that iterated it keeps working.

## Upgrading an `http` pool that already set `pm.status_path`

On `pool.type = http` this directive used to be answered by upstream FPM's
in-child handler, on the pool's **public** listener — reachable whenever
`http.front_controller` was empty, because that is what makes `SCRIPT_NAME` the
request path. It is now answered on the operator listener instead, and the
public listener hands the path to your application like any other.

The page is not the same page: it is the per-pool JSON described below, not
upstream's `text`/`html`/`json`/`xml` status body. A scraper pointed at the
public listener has to move to the operator address. `pool.type = fastcgi` is
untouched, and keeps `pm.status_path` with its upstream meaning in full.

## Upgrading an `http-direct` pool that already set `pm.status_path`

The page is the same page — the one described in
[`docs/http-direct.md`](http-direct.md#pingpath-and-pmstatus_path), with the
per-connection counters, the `direct schema` version and the per-child rows on
`?full`, unchanged field for field and byte for byte. What changed is the socket
it is on: the operator listener instead of the pool's own, so a scraper moves
from `http://<listen>/status` to `http://<pm.status_listen>/status` and keeps
parsing exactly what it parsed before. `?json` and `?full` work there too.

Two consequences worth knowing before you compare numbers across the upgrade:

- The scrape is no longer one of the pool's own requests. It used to arrive on
  the pool's listener and be counted — `accepted conn` and `non-php requests`
  both included it, and it appeared in `access.log`. Now it reaches a different
  process entirely, so those counters describe your traffic and nothing else.
- The path is free on the public listener again. A request for `/status` there
  goes to your application like any other URL.

`pm.status_listen` is accepted on this type since the move; it used to be
refused, because under its upstream meaning it asked for a second FastCGI socket
a direct child has nowhere to put.

## What the pages contain

Both formats report the pool that configured the path, and only that pool.

The status page is the pool type's own where the type has one: `http-direct`
answers the page described above, and every other type answers the per-pool JSON
described here. The metrics page is the same exposition format on every type, on
purpose — a scraper reads one endpoint and compares labelled series across pools.

A pool that serves requests (`http`, `http-direct`) reports its worker counts
and request total. On `pool.type = http-direct` with `pool.executor = worker`
that request total is a real, per-request count since issue #333 — see
[`http-direct.md`](http-direct.md#pingpath-and-pmstatus_path) for what else
that executor does and does not report, including the two extra gauges
(`fpmng_pool_worker_pending`, `fpmng_pool_worker_watchers`) it adds on top of
the shape below, and [Per-slot worker metrics](#per-slot-worker-metrics-issue-339)
below for a further, more detailed set. A pool that does not serve requests (`cron`, `supervisor`) reports its state,
when it last started, how many consecutive failures it has had, its last exit
code, and — for `cron` — when it next runs. A `cron` pool with
`cron.expect_within` set also reports `stale`, and a `supervisor` pool whose
script has called `fpmng_supervisor_heartbeat()` also reports `heartbeat_age`
— both purely observational (issue #327); see
[`cron.md`](cron.md#cronexpect_within-issue-327) and
[`supervisor.md`](supervisor.md#fpmng_supervisor_heartbeat-issue-327).

### The baseline counter

Every pool reports one counter of its own invocations whether or not its PHP
code ever calls `fpm_metric_*()`. What an invocation is depends on the type, so
the counter's name does too:

| Pool type | Counter | Counts |
|---|---|---|
| `http`, `http-direct`, `fastcgi`, `fastcgi-ng` | `requests` | Requests served. |
| `cron` | `runs` | Scheduled runs started. |
| `supervisor` | `restarts` | Times the supervised script was started again. |

The name is the JSON key on the status page and, as `fpmng_pool_<name>_total`,
the series on the metrics page — one name, two spellings of it, and they cannot
disagree.

All three are monotonic and survive a worker being replaced. A `pm.max_requests`
recycle does not reset `requests`, and a supervised child exiting does not reset
`restarts` — the numbers live with the pool, not with the process.

`restarts` counts every start of the supervised script past the pool's first
`supervisor.processes` of them — the starts that were meant to happen are not
restarts, and a pool that has been up since boot without its script ever exiting
reads zero.

`restarts` counts restarts, not failures. A script that exits 0 and is brought
back by `supervisor.restart = always` leaves `consecutive_failures` at zero
forever while `restarts` climbs; that combination is the signature of a pool
that is flapping silently, and reading the two together is how you see it.

### Per-slot worker metrics (issue #339)

On `pool.type = http-direct` with `pool.executor = worker`, `pm.metrics_path`
also carries a set of series keyed by `slot` (this pool's scoreboard index,
one worker child per slot) or by a `reason`/`type` label, on top of the
pool-wide `fpmng_pool_worker_pending` and `fpmng_pool_worker_watchers` gauges
issue #333 already added. They exist to answer three questions issue #333's
two gauges cannot: which slot is under pressure, why a worker was refused or
recycled, and how long its event loop has gone quiet.

`pm.status_path` stays unsupported on this executor (see
[`http-direct.md`](http-direct.md#pingpath-and-pmstatus_path)) — only
`pm.metrics_path` reports these.

Per-slot gauges. Each is only meaningful for a slot currently holding a live
worker; a slot with no worker in it (not yet spawned, or between an exit and
its replacement) reports `0` rather than a stale number from whichever worker
had the slot before:

| Series | Labels | Meaning |
|---|---|---|
| `fpmng_pool_worker_queued` | `pool`, `slot` | Requests accepted but not yet dispatched to the script (`fw.ready_count`). |
| `fpmng_pool_worker_pending_oldest_seconds` | `pool`, `slot` | Age, in seconds, of the oldest request this worker has accepted but not yet answered. `0` when nothing is pending. |
| `fpmng_pool_worker_loop_stall_seconds_max` | `pool`, `slot` | Longest gap seen so far between two consecutive `fpmng_worker_loop()` calls — how long the script's own code, not libevent, has ever held the loop. |
| `fpmng_pool_worker_watchers` | `pool`, `slot`, `type` (`read`, `write`, `timer`) | Libevent watchers this worker currently has registered, split by kind. The sum across `type` for one slot equals that slot's `fpmng_pool_worker_watchers` without the split, minus nothing — the two counts agree by construction. |
| `fpmng_pool_worker_memory_bytes` | `pool`, `slot` | This worker's peak RSS (`getrusage(RUSAGE_SELF).ru_maxrss`), the same sample `worker.max_memory` (issue #334) already computes for its own recycling decision. |
| `fpmng_pool_worker_responses_unflushed` | `pool`, `slot` | Bytes this worker has queued to a client but not yet confirmed written to the socket (`worker.send_buffer_limit`, issue #332). |

Per-slot counter. Unlike the gauges above, this one is owned by the slot, not
by whichever worker currently occupies it — it keeps counting across a
`pm.max_requests` recycle instead of resetting to `0` when a new worker takes
the slot:

| Series | Labels | Meaning |
|---|---|---|
| `fpmng_pool_worker_accepted_total` | `pool`, `slot` | TCP connections accepted by this slot, ever. Counts connections, not requests — keep-alive means this can be lower than `fpmng_pool_<name>_total`'s per-request count for the same traffic. |

Pool-wide counters, one number for the whole pool rather than one per slot,
because what they count outlives any one worker occupying any one slot:

| Series | Labels | Meaning |
|---|---|---|
| `fpmng_pool_worker_refused_total` | `pool`, `reason` (`saturated`, `stopping`, `acl`, `bad_request`) | Requests this pool answered with an error instead of dispatching to a script, by reason. `saturated`: every ready slot was full. `stopping`: the worker holding the connection was already draining. `acl`: `listen.allowed_clients` rejected the peer. `bad_request`: the request itself was malformed. |
| `fpmng_pool_worker_recycles_total` | `pool`, `reason` (`max_requests`, `saturation`, `max_memory`, `max_lifetime`, `script_returned`, `signal`) | Times a worker in this pool was told to stop, by reason. `max_requests`/`max_memory`/`max_lifetime` are the matching `pm.max_requests`/`worker.max_memory`/`worker.max_lifetime` directives; `saturation` is this worker recycling itself after refusing a request it had no room for; `script_returned` is a script that exited on its own without ever being asked to stop; `signal` is every other stop signal (shutdown, reload, `FPM\Tester::terminate()`, …). `max_lifetime` is an addition beyond the metric's original proposal in issue #339, added because `worker.max_lifetime` (issue #334) is an existing, distinct recycle trigger the reason list would otherwise have no way to report. |
| `fpmng_pool_worker_abandoned_total` | `pool` | Requests this pool never answered at all: still pending when a worker's shutdown drain gave up on them, or a streamed response whose bytes never reached the socket before the worker exited. |
| `fpmng_pool_worker_client_gone_total` | `pool` | Requests whose client disconnected before this pool had sent any answer for them. |

## Application metrics on a per-pool metrics path

`pm.metrics_path` carries the series your PHP code registered with
`fpm_metric_register()` and fed with `fpm_metric_inc()`, `fpm_metric_set()` and
`fpm_metric_observe()`, appended to the pool metrics above in the same
exposition format.

**It reports the scraped pool's workers and nobody else's.** The store is one
shared region with a slot per worker, and each pool owns a contiguous run of
slots, so a per-pool scrape aggregates that run alone. Two pools that register a
series under the same name get two independent numbers on two endpoints; a pool
whose code registered nothing adds no lines at all.

Every series keeps its automatic `pool="…"` label, even though on a per-pool
endpoint that label never varies. It is what lets a scraper that reads several
pools' endpoints put the series side by side once they are in one database —
without it, the same series name would mean a different pool depending on which
port it was collected from.

There is no aggregate endpoint. Until issue #278 a `pool.type = status` pool
answered `/metrics` with every pool's series at once; it was removed, because
one pool that reported on all the others is the same listener the operator
endpoint already runs, with a second configuration language for it. A scraper
that wants several pools reads several paths — on one port if they share a
`pm.metrics_listen`, which is the usual arrangement.

### Turning metrics off

Unset `pm.metrics_path`. That is the off switch, and it is a real one: the path
is not answered, and if nothing else on that address needs a listener the port
is not bound at all.

What it does **not** switch off is the API. `fpm_metric_register()` and the rest
keep working in every pool, keep writing to the same shared slots, and keep
costing exactly what they cost. A script cannot tell whether its pool exposes a
metrics path, which is deliberate: turning off an endpoint is an operator's
decision about exposure, not a change to what the application may call.

### `?openmetrics` is not an alias

Upstream FPM emits OpenMetrics as a query flag on the status page. php-fpm-ng
does not. `GET <pm.status_path>?openmetrics` is the status page being asked for
a variant it does not have, exactly like `?json` or `?xml`; metrics live on
`pm.metrics_path` and nowhere else. Two paths are what gives the two pages
independent on/off switches, since "on" means "the path is set".

## What is not here yet

- `ping.path` stays on the pool's own listener on every type. It is a liveness
  probe for whatever is in front of the pool, so that is where it belongs.

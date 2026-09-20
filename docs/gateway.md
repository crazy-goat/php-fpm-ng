# The gateway: ping, metrics and status across pool types

> **Status: `pool.type = gateway` landed in #388 and `pool.type = http` is
> retired; two pieces of this design are still open.** The type, its explicit
> `http.route[]`-only routing, `ping.path` answered in the gateway process and
> its own `operator.metrics_path`/`operator.status_path` defaults are
> implemented. Still to come: `http.operator` and the `<base>/<pool name>`
> forwarding of every exposed pool's pages (#389), and the gateway's own
> shared-memory counters rendered on its `/metrics` page (#390). The operator
> directives are `operator.*` since #386 (see
> [`operator-endpoint.md`](operator-endpoint.md)). This page is the target,
> decided 2026-09-17; the issues that carry the rest are listed at the end.
> When the last of them lands this banner goes and the two pages merge.

## Why

`pool.type = http` is two things welded into one section: a pool of PHP
workers, and a proxy in front of them. Every awkward question about monitoring
comes from that weld -- whose status page is it, the workers' or the proxy's;
what does `/metrics` mean on a pool that has gateway processes, PHP children and
a shared operator child; why does `/ping` reach the application. Separate the
two and the questions do not get answered, they stop being asked.

The gateway becomes a type of its own: a minimal proxy, like nginx with a
minimal configuration, that routes to other pools and exposes their operator
pages. It runs no PHP. It has no process manager. Every route is explicit.

## Three rules

Everything below follows from these. They are stated once so nobody re-derives
them differently.

1. **`ping.path` is answered on the request listener, by a process that serves
   requests there.** Never on the operator listener. The probe's whole value is
   that it walks the same accept queue as a real request; answered anywhere
   else it proves nothing. A type with no request listener (`cron`,
   `supervisor`) has no ping and refuses the directive. On the gateway the
   request listener *is* the gateway process, so the gateway answers it itself,
   before routing (#382) -- proving the gateway is alive, not its targets. A
   target is pinged on its own listener.

2. **Metrics are answered on the operator listener, one path per pool, never on
   the request listener.** They must answer when every worker is busy, and they
   must have one exposition format on every type so one scraper compares
   series across pools.

3. **Status follows metrics onto the operator listener**, with the type's own
   page where the type has one (`http-direct`). The one exception is
   `pool.type = fastcgi`, where upstream's `pm.status_path` keeps upstream's
   meaning -- a path on the pool's own FastCGI socket, for the web server in
   front -- *in addition to* the operator page, under a different name.

Rules 2 and 3 look symmetrical to rule 1 and are its exact opposite. That is
the thing to remember.

## Directives

### `operator.*` -- on every pool type

The operator listener has nothing to do with the process manager, so its
directives leave the `pm.` namespace. The same four names on every type; `cron`
and `supervisor` no longer need to carve them out of a rejected `pm.`
namespace (#283).

| Directive | Meaning | Default |
| --- | --- | --- |
| `operator.metrics_path` | This pool's Prometheus page, on the operator listener. | unset = off (gateway: `/metrics`) |
| `operator.status_path` | This pool's status page, on the operator listener. | unset = off (gateway: `/status`) |
| `operator.metrics` | `on` = expose metrics at `/metrics/<pool name>`. | `off` |
| `operator.status` | `on` = expose status at `/status/<pool name>`. | `off` |
| `operator.metrics_listen` | Where the metrics path binds. | `127.0.0.1:9253` |
| `operator.status_listen` | Where the status path binds. | `127.0.0.1:9253` |

`operator.metrics = on` and `operator.metrics_path = …` are two spellings of
one switch. `on` picks the path for you, and picks the one the gateway will use
(below), so direct and gateway URLs coincide. Setting both is a startup error.
A pool that sets neither is not exposed and, if nothing else on the address
needs a listener, binds nothing.

The old names (`pm.status_path` on non-`fastcgi` types, `pm.metrics_path`,
`pm.status_listen`, `pm.metrics_listen` everywhere) are **refused with a message
naming the new directive**, not accepted as aliases. Two names for one
mechanism is the disease this rename cures; keeping them as synonyms would be
a small dose of it.

**Pool names become URL components.** A pool that exposes anything must have a
section name made of `[alphanum]/_-.~` -- the operator path character set
(`fpm_operator_endpoint.c`). `[queue:high]` or `[api v2]` can exist, but
cannot set `operator.*`. Startup refuses with the name and the character; it
does not skip the pool with a warning, because a skipped pool is metrics you
notice are missing only when you need them.

### `http.*` -- on the gateway

Existing `http.*` directives keep their names. They configure what the gateway
does on its public port -- TLS, static files, body limits, routing -- and that
is honest for a type called `gateway`. Two are new:

| Directive | Meaning | Default |
| --- | --- | --- |
| `http.route[<pool>]` | Comma-separated path prefixes routed to `<pool>`. | -- |
| `http.operator` | Serve every exposed pool's operator pages through this public port. | `no` |
| `http.operator_allowed_clients` | Who may reach them. Separate from `http.allowed_clients`. | -- (required when `http.operator = yes`) |

`http.operator*` stays in `http.`, on purpose: it does not configure the
operator listener, it configures what the gateway does with its own port.

`http.route[]` is keyed by pool name (#340): the key validates itself against
the configured sections, the value is free to grow a pattern syntax later, and
an INI key cannot sensibly hold `/`, `.` or `|`. Several prefixes may name one
pool; they share that pool's budget and queue, because one set of workers
enforces it.

### What the gateway type refuses

No PHP runs in a gateway, so nothing that configures PHP applies: `pm`,
`pm.*`, `php_admin_value[]`, `php_value[]`, `request_terminate_timeout`,
`request_slowlog_timeout`, `slowlog`, `security.limit_extensions`. What stays:
`listen` (the public port), `user`/`group`, `chdir` (docroot for static files
and the front controller), `access.*`, `ping.*`, `operator.*`, `http.*`.

## URLs: local and through the gateway

A pool's operator pages have a **local** URL, on the operator listener, at the
path the pool declared. Through the gateway they have a **second** URL, which
the pool does not choose:

```
<gateway operator.metrics_path>/<pool name>
<gateway operator.status_path>/<pool name>
```

whatever the pool set locally. The gateway builds this map once, at
configuration time, from the pool list it already has: pool name -> (operator
listener address, local path). Two consequences:

- **Exact matches only.** `/metrics/api` is forwarded because `api` exposed
  itself; `/metrics/anything-else` is a local 404 from the gateway and is never
  forwarded. This is a guarantee, not an optimisation: the operator listener's
  own 404 lists every path it knows, which on loopback is a convenience and on
  a public port would enumerate your pools.
- **`operator.metrics = on` gives one URL, an explicit path gives two.** With
  `on`, the local path is `/metrics/<pool>` -- the same string the gateway
  uses. The only reason to write an explicit local path is a local agent that
  needs a particular one; then the pool has `/_m` locally and `/metrics/api`
  through the gateway, and you knew that when you wrote it.

The gateway's own pages sit at the bare base: `/metrics` is the gateway's own
series, `/status` its own page. On this type both paths **default to being
set**, so a fresh gateway binds the operator listener on loopback without being
asked. Set either to `""` to turn it off; that logs a warning and disables it,
and disables the `<base>/<pool>` forwarding for that format with it, because
there is no base to forward under. `http.operator = yes` with both bases empty
is a configuration error. Publicly nothing is exposed until `http.operator =
yes`, which is the switch that matters.

Two gateways with `http.operator = yes` expose the same set of pools, each
under its own base. To keep one gateway out of it, turn its `http.operator` off
or empty its base paths. To keep one *pool* out of it, do not expose the pool.
There is no per-gateway pool list; if one is ever needed it is a list, not a
flag.

This is not the derived-membership design that was assessed and dropped
(#383): membership is still declared by the pool -- exposed iff it set a path
or a flag (#273, point 4) -- and nothing is inferred from `http.route[]`. The
gateway renames what pools declared; it does not decide who is on the list.

## What the gateway forwards with

The operator listener speaks HTTP/1.1, so the forwarding uses the same client
transport #344 adds for `http-direct` targets. One transport, two uses: a
target pool's request listener, and the operator listener. Nothing new is
invented for it.

## The gateway's own numbers

Gateway processes are not workers and have no scoreboard slot. What they know
-- requests routed, per target; 503s, per target; open client connections;
which pools are exposed -- lives in shared memory the master allocates, as
`upstreams_used` already does, and is rendered by the operator child from
there, the way `live_gauges` renders the worker executor's own segment (#333).
The `/metrics` page on the gateway is therefore its own series plus an index of
the exposed pools' paths -- small on purpose. It is **not** an aggregate of the
pools' series; that endpoint was removed in #278 and stays removed.

## A complete configuration

```ini
[global]
pid = /run/php-fpm-ng.pid
error_log = /var/log/php-fpm-ng/error.log
log_level = notice

; ===========================================================================
; THE GATEWAY -- a pure proxy. No PHP, no process manager, no workers.
; Every route is explicit: there is no implicit "own pool at /".
; ===========================================================================
[gw]
pool.type = gateway
user = www-data
group = www-data
listen = 0.0.0.0:443
chdir = /srv/www/public                 ; docroot for static files + front controller

http.route[app] = /                     ; key = pool, value = prefix list
http.route[api] = /api,/v2/api

http.tls_cert = /etc/ssl/site.crt
http.tls_key  = /etc/ssl/site.key
http.static = yes
http.front_controller = index.php
http.access_log = /var/log/php-fpm-ng/access.log
http.max_body = 16M

ping.path = /ping                       ; answered IN the gateway, before routing.
ping.response = pong                    ; Proves the gateway lives -- NOT the targets.

; The gateway's own operator pages. On this type both DEFAULT to the values
; below; written out only for clarity. "" turns one off (with a warning).
operator.metrics_path = /metrics
operator.status_path  = /status
; operator.*_listen unset -> 127.0.0.1:9253, shared with every pool below
; (9253 is the Prometheus registry's PHP-FPM exporter port; moves off 8080 in #386)

; Expose the operator pages through this public port. For every pool that
; exposed itself the gateway serves <base>/<pool name>, regardless of the
; pool's local path. Exact matches from a map built at config time; anything
; else is a local 404, never a forward.
http.operator = yes
http.operator_allowed_clients = 10.0.0.0/8

; ===========================================================================
; The application. An ordinary upstream FastCGI pool; it does not know
; HTTP exists.
; ===========================================================================
[app]
pool.type = fastcgi
user = www-data
group = www-data
listen = /run/php-fpm-ng/app.sock
listen.owner = www-data
listen.mode = 0660
chdir = /srv/www/public
pm = dynamic
pm.max_children = 32
pm.start_servers = 8
pm.min_spare_servers = 4
pm.max_spare_servers = 12
pm.max_requests = 500
php_admin_value[memory_limit] = 256M

ping.path = /ping                       ; upstream meaning: on this pool's own socket
pm.status_path = /_fpm_status           ; upstream meaning: on this pool's own socket, for nginx

operator.metrics = on                   ; -> local /metrics/app, identical to the gateway URL
operator.status  = on                   ; -> local /status/app

; ===========================================================================
; A pool that speaks HTTP itself. Routed to over HTTP/1.1 (#344).
; Explicit local paths here, to show the other form.
; ===========================================================================
[api]
pool.type = http-direct
user = www-data
group = www-data
listen = 127.0.0.1:9000
chdir = /srv/www/api/public
pm = static
pm.max_children = 16
http.front_controller = index.php
http.max_connections = 256

ping.path = /ping                       ; on this pool's own listener, in the child

operator.metrics_path = /_m              ; deliberate: short path for a local agent.
operator.status_path  = /_s              ; Through the gateway these are STILL
                                         ; /metrics/api and /status/api.

; ===========================================================================
; Long-lived connections. NOT routed through the gateway -- see below.
; ===========================================================================
[ws]
pool.type = http-direct
pool.executor = worker
user = www-data
group = www-data
listen = 0.0.0.0:8443
pm = static
pm.max_children = 4
worker.max_pending = 1024
worker.request_timeout = 30
worker.max_memory = 512M
worker.max_lifetime = 12h

operator.metrics = on
operator.status  = on                   ; reduced page: no per-request stage (#387)
ping.path = /ping                       ; 503 while the worker's queue is full (#387)

; ===========================================================================
; No HTTP endpoint of their own at all. The operator listener is their ONLY
; exposition, which is exactly why the gateway forwarding matters here.
; ===========================================================================
[cronjobs]
pool.type = cron
user = www-data
group = www-data
cron.schedule = */5 * * * *
cron.script = /srv/www/app/bin/tick.php
cron.timeout = 4m
cron.timezone = Europe/Warsaw
cron.expect_within = 10m
cron.output_log = /var/log/php-fpm-ng/cron.log

operator.metrics = on
operator.status  = on

[queue]
pool.type = supervisor
user = www-data
group = www-data
supervisor.script = /srv/www/app/bin/worker.php
supervisor.processes = 4
supervisor.restart = always
supervisor.restart_delay = 2s
supervisor.restart_delay_max = 60s
supervisor.max_memory = 256M
supervisor.output_log = /var/log/php-fpm-ng/queue.log

operator.metrics = on
operator.status  = on
```

### What is reachable where

On the public port `443`, through the gateway (operator paths only from
`10.0.0.0/8`):

```
/  /api  /v2/api                        -> app, api
/ping                                   -> the gateway itself, locally
/metrics  /status                       -> the gateway's own series and page
/metrics/app       /status/app
/metrics/api       /status/api          <- although the pool's local paths are /_m and /_s
/metrics/ws        /status/ws           <- status is the reduced worker page (#387)
/metrics/cronjobs  /status/cronjobs
/metrics/queue     /status/queue
```

On the loopback operator listener, one process, the **local** paths:

```
/metrics  /status                       (gw)
/metrics/app  /status/app               (operator.metrics = on -> same URLs as via the gateway)
/_m  /_s                                (api -- the one place the URL differs)
/metrics/ws
/metrics/cronjobs  /status/cronjobs
/metrics/queue  /status/queue
```

Besides: `8443` for `[ws]`, and `app.sock` for an nginx that might stand
beside the gateway, with `/ping` and `/_fpm_status` in their upstream meaning.

### Three things the example shows that are easy to forget

- **`[ws]` is not routed through the gateway.** WebSockets cannot be carried
  over FastCGI at all, and #344's HTTP/1.1 client is not a passthrough either;
  #343 does them natively on the worker executor. That pool has its own public
  port, and it is the first place where "the gateway is the entry point" is
  not the whole truth.
- **`[app]` has status under two names and it is not a duplicate.**
  `pm.status_path` is the page on the FastCGI socket for nginx;
  `operator.status = on` is the page on the operator listener. Two sockets,
  two answering processes, two different sets of numbers. Under the old names
  this could not be expressed at all -- which is what the rename buys.
- **`[cronjobs]` sets `cron.expect_within`, and staleness has no timer of its
  own** (#357): it is noticed at scrape time. With the gateway in front, that
  scrape is `/metrics/cronjobs`, so the check depends on Prometheus asking for
  that exact path.

## The worker executor catches up (#387)

`pool.type = http-direct` with `pool.executor = worker` refuses `ping.path`
and `pm.status_path` today, both under one comment about the scoreboard having
no per-request stage, duration or CPU to report. Decided 2026-09-17: that
reason covers the access log, and only half of status.

- **`ping.path` is answered.** It is a literal whole-path match in the
  connection handler (`fpm_http_direct_ops_try_local()`), with no PHP and no
  scoreboard involved; the worker executor already shares that file and
  `ops->ping_path` is populated for it. It is answered from
  `fpm_worker_accept()` **after** the ACL and **after** the saturation check,
  so a worker whose queue is full answers `503` on the ping path as well: ping
  means "would a real request be accepted right now", which is what it means
  on the classic executor and what a load balancer needs. Pings do not consume
  `pm.max_requests`.
- **`operator.status` is a reduced page** -- only what the executor records
  honestly: requests answered (#333), `worker_pending`, `worker_watchers`, the
  http-direct totals, and per-child rows without stage, duration, CPU or peak
  memory. The renderer is already wired on the worker struct
  (`fpm_pool_type.c:262`); only the reject list keeps it dark.
- **`access.log` / `access.format` stay refused.** There is no per-request
  timing to log, and that was the honest half of the original comment.

Rule 1 then holds on every type that has a request listener, and the gateway's
`/status/<pool>` table has no gap.

## Issues

| Issue | Carries |
| --- | --- |
| #340 (v0.8.0) | `http.route[<pool>] = <prefixes>`; today on `pool.type = http`, with target 0 |
| #382 (v0.8.0) | ping answered in the gateway process, before routing |
| #344 (v0.9.0) | HTTP/1.1 client transport; also the transport for operator forwarding |
| #386 (v0.10.0) | `operator.*` rename, `on`/`off` flags, path-safe pool names, old names refused |
| #388 (v0.10.0) | `pool.type = gateway`; `http` retired; target 0 gone; supersedes #345 |
| #389 (v0.10.0) | `http.operator`, `http.operator_allowed_clients`, the `<base>/<pool>` map |
| #390 (v0.10.0) | shm counters rendered by the operator child; the `/metrics` index |
| #387 (v0.10.0) | worker executor: `ping.path` answered after the saturation check; reduced status page |
| #383 (v0.11.0) | `fastcgi` on the operator listener -- simplified by the rename |
| #385 (v0.11.0) | fold this page into `operator-endpoint.md` once the above has landed |

#386 lands first: every later issue writes `operator.*`.

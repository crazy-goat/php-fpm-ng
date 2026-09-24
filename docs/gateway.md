# The gateway: ping, metrics and status across pool types

This is the living gateway guide: it covers gateway routing, operator-page
forwarding, ping, and the gateway's own metrics. The canonical directive table,
listener defaults, collision rules, and per-pool operator-page contract live in
[`operator-endpoint.md`](operator-endpoint.md); consult that page when
configuring `operator.*`. `pool.type = http` is retired: configure a `pool.type = gateway`
proxy plus a separate `fastcgi` or `http-direct` pool for PHP.

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
   page where the type has one (`http-direct`). On `pool.type = fastcgi`,
   upstream's `pm.status_path` keeps its meaning -- a path on the pool's own
   FastCGI socket, for the web server in front. The pool may separately opt in
   to an operator status page with `operator.status_path` or `operator.status`.

Rules 2 and 3 look symmetrical to rule 1 and are its exact opposite. That is
the thing to remember.

## Directives

### `operator.*` -- directive reference

The operator listener is separate from the process manager, and the directives
use the `operator.*` namespace on every supported pool type, including FastCGI.
`operator-endpoint.md` is the canonical reference for path and listener
directives, shorthand flags, defaults, path-safe pool names, and collision
rules. `pm.status_path` remains upstream-only on FastCGI and is separate from
`operator.status_path`.

Gateway-specific defaults: its own status and metrics pages are on by default
at `/status` and `/metrics`; set `operator.status = off` or
`operator.metrics = off` (or an empty path) to disable one. All gateway and
pool operator listeners default to `127.0.0.1:9253`, so pools that omit
`operator.*_listen` share one listener process. Explicit listen addresses are
only needed to split endpoints across interfaces/firewall rules; they are not
required for the gateway-first topology below.

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

`http.allowed_clients` is this listener's ACL. `listen.allowed_clients` is a
FastCGI-worker ACL and is **refused** on a gateway (issue #493): a gateway has
no worker socket -- `listen` *is* the public port -- so accepting it would leave
an operator who wrote it believing the public listener was restricted while it
served everyone. On the retired combined `http` pool it restricted the FastCGI
half, never the public port; use `http.allowed_clients` here.

`http.route[]` is keyed by pool name (#340): the key validates itself against
the configured sections, the value is free to grow a pattern syntax later, and
an INI key cannot sensibly hold `/`, `.` or `|`. Several prefixes may name one
pool; they share that pool's budget and queue, because one set of workers
enforces it.

**Cleartext routing boundary.** FastCGI targets use their FastCGI socket. An
`http-direct` target is contacted over cleartext HTTP/1.1, so its `listen` must
be a Unix socket, a numeric IPv4 address in 127/8, or the IPv6 loopback literal
`::1`. Public and wildcard addresses, hostnames (which could resolve or rebind
to a public address), IPv4-mapped IPv6 addresses and other non-loopback targets
are refused by `php-fpm-ng -t`; TLS-terminating
`http-direct` targets remain refused too. To route over the network, use a
transport with TLS rather than exposing the gateway's cleartext target hop.

### What the gateway type refuses

No PHP runs in a gateway, so nothing that configures PHP applies: `pm`,
`pm.*`, `php_admin_value[]`, `php_value[]`, `request_terminate_timeout`,
`request_slowlog_timeout`, `slowlog`, `security.limit_extensions`. What stays:
`listen` (the public port), `user`/`group`, `chdir` (docroot for static files
and the front controller), `access.*`, `ping.*`, `operator.*`, `http.*`.

## URLs: local and through the gateway

Landed in #389: `http.operator = yes` builds the map described here once, in
the master at configuration time, before the first gateway forks; `fork()`
copies it into every gateway process and a reload rebuilds it.

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

Because those two paths default, two gateways with no `operator.*_listen` both
land on `127.0.0.1:9253`. That still starts (issue #388): the first gateway to
register a default path keeps it, and a later one whose *derived* `/status` or
`/metrics` would collide drops that page with a NOTICE rather than refusing the
whole configuration. An **explicit** path is not offered in that way -- an
explicit collision is still a startup error, as for any pool. An explicit
`operator.status = off` / `operator.metrics = off` is honoured too and
suppresses only the default.

Two gateways with `http.operator = yes` expose the same set of pools, each
under its own base. To keep one gateway out of it, turn its `http.operator` off
or empty its base paths. To keep one *pool* out of it, do not expose the pool.
There is no per-gateway pool list; if one is ever needed it is a list, not a
flag.

Membership is declared by the pool -- exposed iff it set an operator path or
flag (#273, point 4) -- and is not inferred from `http.route[]`. The gateway
forwards only the operator pages the target pool exposed; routing an application
request does not implicitly expose its status or metrics.

## What the gateway forwards with

The operator listener speaks HTTP/1.1, so the forwarding uses the same client
transport #344 adds for `http-direct` targets. One transport, two uses: a
target pool's request listener, and the operator listener. Nothing new is
invented for it.

## The gateway's own numbers

Gateway processes are not workers and have no scoreboard slot. Their numbers
live in **one shared-memory segment per gateway pool** that the master
allocates in `.init_main`, before the first fork. It holds two kinds of data,
and the difference is the point:

- **Pool-wide monotonic counters** -- the baseline `requests` and ping totals,
  and the per-target request/rejection counts. Every gateway process bumps them
  with cmp-set atomics and no locking, and they **survive a respawned gateway
  process**: the segment belongs to the pool, not the process.
- **Per gateway process gauges** -- `connections_open` and the per-target
  `upstreams_used`. Each process writes only its own block, the renderer
  **sums** every block (the #333 live-gauges shape), and the master **zeroes a
  dead process's block** in `fpm_http_gateway_on_exit()`. A gauge is "currently
  open", and a process killed with connections open runs no close callback, so
  a single shared gauge could only ever leak; per process, its connections
  leave the sum with it. The master also returns that process's upstream
  reservations to the shared admission budget, so a crash does not shrink the
  pool's budget for the life of the segment. (A process killed inside the one
  instruction between reserving the shared budget and publishing its own gauge
  can leak a single reservation; the ordering fails closed rather than
  over-spending.)

The operator child renders all of it; it is not a gateway process, so it reads
shared memory and configuration only.

`fpmng_pool_requests_total{pool="<gw>"}` is the pool's **baseline counter** --
the `requests` key the status page reports -- bumped for every request the
gateway accepts, before the ACL, so a denied request still counts. Both public
listeners feed it: the TLS one and `http.plain_listen`, whose redirects, ACME
HTTP-01 answers, NO_CERT 503s and 400s are all local. So the baseline always
equals the sum of the target rows below. Its own series label a **target**:

| series | `target` | counts |
|---|---|---|
| `fpmng_gateway_requests_total` | a routed pool | requests routed to that target |
| `fpmng_gateway_rejected_total` | a routed pool | of those, 503s from a full target (the #341 series) |
| `fpmng_gateway_upstreams_used` | a routed pool | persistent connections currently held to it |
| `fpmng_gateway_upstreams_max` | a routed pool | the target's own `pm.max_children` |
| `fpmng_gateway_requests_total` | `operator` | operator pages forwarded through `http.operator` (#389) |
| `fpmng_gateway_requests_total` | `-` | requests the gateway answered itself (ping, static, ACME, 404, 403) |

`fpmng_gateway_connections_open{pool="<gw>"}` and
`fpmng_gateway_ping_total{pool="<gw>"}` are the numbers no target owns: the
first is the per-process sum described above, the second a pool-wide counter.
The `/metrics` page also carries an **index**: one
`fpmng_gateway_exposed_pool{pool="<pool>",metrics="<base>/<pool>",status="<base>/<pool>"} 1`
line per pool the gateway forwards for (#389), so a scraper that found the
gateway knows where `<base>/<pool>` points. It is a discovery aid, not an
aggregate of their series; that endpoint was removed in #278 and stays removed.
`/status` on the gateway is the same numbers as JSON, one row per target plus a
pool row, in the generic `{"pools":[...]}` shape.

Only the monotonic counters survive a respawned gateway; the gauges are
reconciled when a process dies, and the whole segment is rebuilt by a reload:
an exec-reload re-execs the master and the allocation is `MAP_ANONYMOUS`
(#330), so like every other pool's counters, the gateway's reset on reload.

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
- **`[cronjobs]` sets `cron.expect_within`.** The master checks stale-enabled
  cron pools once per second, so the `WARNING` fires even without a scrape
  (#357). The `stale`/`stale_since` page fields still appear when you scrape
  `/metrics/cronjobs` or `/status/cronjobs`; the gateway forwards them like any
  other operator page.

## Worker-executor operator pages and ping (issue #387)

`pool.type = http-direct` with `pool.executor = worker` supports `ping.path` and
`operator.status` even though it has no per-request scoreboard stage, duration
or CPU accounting. `access.log` / `access.format` remain refused because they
need per-request timing; the reduced status page reports only what this
executor measures honestly.

- **`ping.path` is answered by the worker on its request listener.** It is a
  literal path match in the connection handler, requires no PHP or scoreboard
  read, and is checked after the ACL and saturation gate. A worker whose pending
  queue is full answers `503` on the ping path too: ping means "would this
  worker accept a request now?" Pings do not consume `pm.max_requests`.
- **The operator status page is reduced**, showing pool-level answered-request
  counters, `worker_pending`, `worker_watchers`, HTTP-direct totals and per-child
  rows without request stage, duration, CPU or peak memory.
- **Gateway forwarding works for both pages.** When the pool exposes
  `operator.status_path`/`operator.status` or `operator.metrics_path`/
  `operator.metrics`, `http.operator = yes` can forward them under the gateway's
  `<base>/<pool name>` URL just like any other exposed target.

Thus the gateway-first topology has a ping on each request listener and status
and metrics on operator listeners; the pages describe the process that owns
them, not a synthetic aggregate inferred from `http.route[]`.

## Related decisions

These issues established the gateway and operator-endpoint contract documented
above; they are implementation history, not pending work:

| Issue | Decision or feature |
| --- | --- |
| #340 | Explicit `http.route[<pool>]` path-prefix routing |
| #382 | Gateway answers its own `ping.path` before routing |
| #344 | HTTP/1.1 client transport used for routed HTTP-direct pools and operator forwarding |
| #386 | `operator.*` namespace, shorthand flags, and path-safe pool names |
| #387 | Worker executor ping and reduced status page |
| #388 | Separate `pool.type = gateway`; retire the combined `http` type |
| #389 | Optional `<base>/<pool>` operator-page forwarding through the gateway |
| #390 | Gateway shared-memory counters and exposed-pool metrics index |
| #383 | FastCGI pools may opt into the shared operator listener while retaining upstream `pm.status_path` |

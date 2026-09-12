# The per-pool operator endpoint

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

## Upgrading an `http` pool that already set `pm.status_path`

On `pool.type = http` this directive used to be answered by upstream FPM's
in-child handler, on the pool's **public** listener — reachable whenever
`http.front_controller` was empty, because that is what makes `SCRIPT_NAME` the
request path. It is now answered on the operator listener instead, and the
public listener hands the path to your application like any other.

The page is not the same page: it is the per-pool JSON described below, not
upstream's `text`/`html`/`json`/`xml` status body. A scraper pointed at the
public listener has to move to the operator address; one pointed at a
`pool.type = status` pool is untouched, and so is `pool.type = fastcgi`, where
`pm.status_path` keeps its upstream meaning in full.

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
and request total. A pool that does not (`cron`, `supervisor`) reports its state,
when it last started, how many consecutive failures it has had, its last exit
code, and — for `cron` — when it next runs.

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

The aggregate endpoint has not changed: `pool.type = status` still answers
`/metrics` with every pool's series at once. The difference is what you point a
scraper at — one port for the whole master, or one port per pool with the pool
chosen by path.

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

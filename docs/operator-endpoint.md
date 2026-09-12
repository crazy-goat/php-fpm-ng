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

On `http-direct`, `pm.status_path` is not on this listener yet — see
[What is not here yet](#what-is-not-here-yet). There the operator endpoint serves
`pm.metrics_path` and nothing else, so an `http-direct` pool that sets only
`pm.status_path` binds no new socket and behaves exactly as it did before.

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

## What the pages contain

Both formats report the pool that configured the path, and only that pool.

A pool that serves requests (`http`, `http-direct`) reports its worker counts
and request total. A pool that does not (`cron`, `supervisor`) reports its state,
when it last started, how many consecutive failures it has had, its last exit
code, and — for `cron` — when it next runs.

## What is not here yet

- `http-direct` keeps `pm.status_path` on its **own** listener for now. That page
  is built inside the child that answers and reports per-child rows and
  per-connection counters, which no other process can render today; see
  [`docs/http-direct.md`](http-direct.md). `pm.status_listen` is refused on that
  type until the page moves (issue #275). `pm.metrics_path` already goes to the
  operator listener.
- `ping.path` stays on the pool's own listener on every type. It is a liveness
  probe for whatever is in front of the pool, so that is where it belongs.
- Application metrics registered from PHP with `fpm_metric_inc()` are reported
  by `pool.type = status`, not yet by a per-pool metrics path (issue #276).

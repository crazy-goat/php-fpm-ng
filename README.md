# php-fpm-ng

PHP-FPM with additional operating modes: HTTP, supervisor, cron and metrics —
so that the container image holds one binary and the application code, without
nginx, without supervisord and without a system cron. Which parts are supported
and which are beta is the table under [Support tiers](#support-tiers).

**New here?** [`docs/guides/getting-started.md`](docs/guides/getting-started.md) goes from the
`.deb` to a working combined configuration. Coming from something else:
[php-fpm + nginx](docs/guides/migrate-php-fpm-nginx.md),
[NGINX Unit](docs/guides/migrate-nginx-unit.md),
[supervisord + cron](docs/guides/migrate-supervisord-cron.md). Symfony and Laravel:
[`docs/guides/framework-recipes.md`](docs/guides/framework-recipes.md).

**Target audience: small projects.** One VPS, one instance, typically an
application plus one or two consumers plus a few cron jobs. Not k8s.

The argument is not performance — measured a few percent CPU under real load
(details in the notes). The argument is one configuration file describing the
whole thing and one binary to scan.

## State as of today

`sapi/fpmng` is a separate SAPI, built against the distribution's PHP 8.5 SDK
(NTS, dynamic, Debian/Ubuntu glibc and Alpine musl; see `docs/install.md`). The
HTTP gateway on libevent started as a branch in php-src
(https://github.com/s2x/php-src/tree/fpm-http-poc); `main` carries no php-src
patch. The static musl artefact of the early measurements was retired from
`main` in #424.

Current state of `sapi/fpmng`:

- the frontend selects `pool.type = gateway | fastcgi | http-direct`; no
  directive means classic `fastcgi` and stays compatible with upstream FPM.
  `pool.executor` is available on `http-direct`: `classic` is the default, and
  `worker` is the beta executor used for WebSocket/SSE and other long-lived
  connections. Issue #388 retired `pool.type = http`: the proxy is `gateway`
  (its `listen` is the public port, `http.route[]` says what it forwards to) and
  the PHP workers are an ordinary `fastcgi` pool behind it. FastCGI pools may
  opt into per-pool operator metrics on the shared HTTP listener while keeping
  upstream `pm.status_path` on the FastCGI socket (`docs/gateway.md` and
  `docs/operator-endpoint.md`).
- the `fiber` and `async` executors moved to branch `async` (issue #373) and
  are not available on `main`
- metrics: `operator.status_path` (JSON) and `operator.metrics_path`
  (Prometheus) expose one pool on an operator listener named by
  `operator.status_listen` / `operator.metrics_listen`, one target per pool
  (`docs/operator-endpoint.md`); `operator.metrics` / `operator.status` are
  shorthands for `/metrics/<pool>` / `/status/<pool>`; application metrics from
  PHP
  (`fpm_metric_register/inc/set/observe`, NOTES 3k/3w) through the
  `ext/fpmng_metrics/` extension, also from CLI via `fpm_metric_render()`
- the `fiber` and `async` executors live on branch `async` of this repository
  (issue #373), not on `main`

## Support tiers

Each tier is defined by the promise it **withholds**, not by how finished the
code feels (decided in issue #269).

| tier | directives | regressions | security reports | may disappear |
| --- | --- | --- | --- | --- |
| **supported** | stable within a major release | bugs, fixed with priority | in scope | no |
| **beta** | may change in a minor release, with a release-note entry | fixed, no response-time commitment | in scope, no response-time commitment | no, but may be redesigned |
| **experimental** | may change in any release | best effort | best effort, offered as-is | yes, in any release |

Where things stand today (audited against the bar below in issue #380):

| | tier |
| --- | --- |
| `pool.type = fastcgi`, `gateway`, `supervisor`, `cron` | supported |
| `pool.type = http-direct` with the default `classic` executor | supported |
| the operator endpoint (`operator.status_path`, `operator.metrics_path`) | supported |
| `pool.type = http-direct` with `pool.executor = worker` | beta |
| TLS termination (`--enable-fpmng-tls`, `http.tls_*`) | beta |
| ACME certificate issuance (`--enable-fpmng-acme`) | beta |
| `pool.executor = fiber` / `async` | moved to branch `async` (issue #373) |

`pool.type = http`, `pool.type = fastcgi-ng` and `pool.type = status` are
retired names, not tiers: `http` was split into `gateway` plus an ordinary
`fastcgi` pool and `fastcgi-ng` folded into `fastcgi` (issues #388, #376), and
`status` became the operator endpoint (issue #278). A configuration that still
names one is refused with its replacement rather than "unknown pool.type".

The worker executor is the only live beta pool type. The #180-#183 spikes named
its exit condition -- the absence of a streaming primitive, not concurrency --
and that was resolved in v0.7.0 (`fpmng_worker_respond_start()/_chunk()/_end()`
with backpressure, honest metrics, memory recycling, connection info). What is
still missing is the cross-worker wakeup (issue #191, open), so the long-lived
fan-out shape has no measurement under a load resembling use yet; that is the
criterion that keeps it beta, not any open correctness issue.

A pool that is not supported says so in `error_log` once at startup: a `NOTICE`
for beta, a `WARNING` for experimental, naming the pool and the tier. A
supported pool says nothing, so the lines that are there are the ones worth
reading.

**Leaving beta** takes a PR that flips the tier and states which of these it
claims, with links: tests in CI on every PR covering the failure modes and not
only the happy path; a measurement on real hardware under a load resembling
use, recorded in `docs/NOTES.md`; no open correctness issue; every directive
documented, including what it refuses and why; and, for anything an
unauthenticated stranger can reach, an adversarial pass and a read by someone
who did not write it. Leaving *experimental* for *beta* is the first, third and
fourth of those.

## Installing

There is a `.deb` and an `.apk` that contain no PHP: they depend on the
distribution's `libphp` (`libphp8.5-embed`, `php85-embed`), so a machine needs
no compiler and no php-src to run `pool.type = fastcgi`, `pool.type =
gateway` or `pool.type = http-direct`. No pool type needs a patch applied
inside `libphp` any more (issue #388 retired the last one, `pool.type = http`),
so the packages run everything this tree ships. Commands, the supported matrix
and what happens on a version mismatch: [`docs/install.md`](docs/install.md).

Each release carries **two** of each (#294): `php-fpm-ng`, which terminates no
TLS, and `php-fpm-ng-tls`, the same commit built with `--enable-fpmng-tls
--enable-fpmng-acme`. Both of those are beta by the table above, which is why
they are a package of their own rather than the default one; the two conflict
and either can be installed over the other.

## Upgrading from v0.2.0

Two behaviour changes in the operator surface. Both fail loudly at startup
rather than being ignored, so an affected configuration does not start with a
port nobody answers on.

- **`pool.type = status` is gone** (#278). A pool that reported on every other
  pool from a listener of its own is what the operator endpoint (#274) already
  is. Put `operator.status_path` and `operator.metrics_path` on the pools you
  want to watch, pointing `operator.status_listen` at the address the status
  pool used. The
  scraper keeps its port and gains one target per pool instead of one target
  carrying all of them; the JSON body keeps its `{"pools":[…]}` shape, one
  element long. Worked example:
  [`docs/operator-endpoint.md`](docs/operator-endpoint.md#replacing-a-pooltype--status-pool).
- **`pm.status_listen` means something else, and is refused on `fastcgi`**
  (#278). It used to auto-create a second FastCGI pool named
  `<pool>_status`; it now names where a type's own operator endpoint binds, and
  the types with a web server in front of them do not have one. On those, keep
  `pm.status_path` on the pool's own socket and restrict it at that web server,
  which is where access to a path is already decided.

## Upgrading to v0.10.0: the operator directives moved to `operator.*`

Since issue #386 the operator endpoint's directives are `operator.status_path`,
`operator.metrics_path`, `operator.status_listen` and `operator.metrics_listen`,
with `operator.status` / `operator.metrics` as shorthands for
`/status/<pool>` / `/metrics/<pool>`. The old `pm.` spellings are **refused by
name**, not aliased: an affected configuration does not start. `pm.status_path`
is the one name that stays, and only on `pool.type = fastcgi`, where it keeps
its upstream meaning. See
[`docs/operator-endpoint.md`](docs/operator-endpoint.md).

## Documentation map

php-fpm-ng is a **separate SAPI** in `sapi/fpmng/`, not a fork of php-src:
`configure.ac` finds directories under `sapi/` by glob, so no existing file needs
to be touched. Details, decisions, measured numbers and the list of known
issues: [`docs/NOTES.md`](docs/NOTES.md).

`pool.type = cron` directives (`cron.schedule`, `cron.timezone`, `cron.log`,
...) are documented for operators in [`docs/cron.md`](docs/cron.md).

`pool.type = supervisor` restarts a script that exits 0 with no delay, on
purpose — the script sets the pace, and a script that returns instead of
looping now says so in the log. Who decides the interval, and the fast-restart
warning: [`docs/supervisor.md`](docs/supervisor.md).

A `cron`, `supervisor`, `gateway` or `http-direct` pool answers
`operator.status_path`
and `operator.metrics_path` on an operator listener of its own rather than on the
socket carrying its traffic — the directives, the default of `127.0.0.1:9253`,
the collision rule and what each page contains are in
[`docs/operator-endpoint.md`](docs/operator-endpoint.md).

Shutdown grace (`process_control_timeout`, `supervisor.stop_timeout`,
`cron.timeout`, `request_terminate_timeout`) and what stock defaults do on
`docker stop` are documented in
[`docs/shutdown-timeouts.md`](docs/shutdown-timeouts.md).

A gateway serves several pools: `http.route[<pool>] = <prefix>[,...]` sends a
path prefix to a `fastcgi` or `http-direct` pool, so an API or a stream
endpoint gets its own workers and its own saturation behaviour without its own
listener. Every route is explicit (issue #388): there is no implicit own-pool
target, a request matching none is a local 404, and at least one route is
required. The longest-prefix rule and why the connection budget belongs to a
target pool rather than to a prefix:
[`docs/http-route.md`](docs/http-route.md).

The HTTP gateway's TLS directives (`http.tls_cert`, `http.tls_reload_check`,
...), including how a renewed certificate reaches every gateway process
without a restart, are documented in [`docs/tls.md`](docs/tls.md). TLS
termination is a build toggle -- `FPMNG_TLS=1 ./build/libphp-build.sh`, off by
default and only in the `php-fpm-ng-tls` package (issue #280).

The gateway answers the ACME HTTP-01 challenge itself, on both its own
`listen` and the plain `http.plain_listen` companion, from state a `cron` or
`supervisor` pool publishes with `fpmng_acme_challenge_set()` — see
[`docs/acme-challenge.md`](docs/acme-challenge.md). Only one process may
renew a given certificate, and the result reaches every gateway through the
existing no-restart certificate reload —
[`docs/acme-renewal.md`](docs/acme-renewal.md). ACME is a build toggle of its
own on top of the TLS one -- `FPMNG_TLS=1 FPMNG_ACME=1`, off by default and only
in the `php-fpm-ng-tls` package (issue #281);
a build without it carries neither the challenge state nor the client, and
refuses an ACME `cron.script` at startup.

## Direct HTTP

`pool.type = http-direct` runs HTTP and PHP in the same FPM child, without the
FastCGI gateway hop. It is **supported with the default `classic` executor** and
beta with `pool.executor = worker` (see the tier table). It supports **`pm =
static` only**, and responses are buffered by default (`http.stream = yes`
streams them). The FPM master still manages the workers. A pool that
needs routing to several targets, or TLS, uses the gateway in front instead.
Configuration, limits, and benchmark methodology:
[`docs/http-direct.md`](docs/http-direct.md).

## Framework support

`main` ships the `classic` executor, which runs one request at a time per worker
like upstream FPM, and the beta `worker` executor on `pool.type = http-direct`,
for long-lived connections: one worker holds several requests at once. See
[`docs/frameworks.md`](docs/frameworks.md). CI runs a Slim 4 smoke test on `gateway`+`fastcgi` and on
`http-direct` classic (`tests/frameworks/`).
The framework measurements for `pool.executor = fiber` (Symfony, Laravel, Slim 4)
belong to the `fiber` executor and live on branch `async`
(`async/docs/frameworks-fiber.md`).

## Recommended pool configuration for lightweight endpoints

Gain with no line of code, measured on the test box (details: `docs/NOTES.md`, 3m and 3t):

```ini
listen = /run/php/pool.sock          ; UDS instead of TCP loopback: -7..-11 us/req
php_admin_value[max_execution_time] = 0   ; no setitimer per request: -3 us/req
catch_workers_output = no            ; logs via error_log()/stderr to our own stack
request_cpu_tracking = no            ; if nobody reads "last request cpu" or %C
```

`request_terminate_timeout` still guards wall-clock time, so
`max_execution_time = 0` does not leave a request without a guard.

## Building

`main` builds against the distribution's prebuilt PHP 8.5 SDK (NTS, Linux,
dynamic). It needs no php-src tree and does not compile PHP:

```sh
# Debian / Ubuntu 26.04
apt-get install build-essential libevent-dev libacl1-dev \
    php8.5-dev libphp8.5-embed php8.5-cli
# Alpine: build-base libevent-dev acl-dev php85-dev php85-embed php85

./build/libphp-build.sh out        # out/php-fpm-ng, out/commands.log
FPMNG_TLS=1 FPMNG_ACME=1 ./build/libphp-build.sh out   # needs libssl-dev
```

The platform matrix, the packages and the known limitations of this build are
in `docs/install.md`. The test suites run against this binary:
`./build/run-fpmng-phpt.sh - out-results` (see `docs/fpmng-phpt.md`). macOS is a
from-source development platform only (`build/prepare.sh` against a php-src
tree).

## Contributing

Work is tracked in **GitHub Issues**, not in the tree. `gh issue list` shows
what is open; labels carry type (`bug`, `enhancement`, `spike`, `refactor`,
`decision`, ...), area (`area:http-direct`, `area:tls`, `area:fiber`, ...) and
priority. Everything under `track:nice-to-have` applies only to
`pool.executor = fiber`, which is not on `main` (branch `async`).

Before touching anything, read [`AGENTS.md`](AGENTS.md): the English-only
rule, the architecture contract (new behaviour in new files under
`sapi/fpmng/fpm/`, never `strcmp(type->name, ...)`), the evidence rule, the
shared test box, and the build, lint and test commands. The step-by-step
process from issue to merged PR is [`docs/workflow.md`](docs/workflow.md), and
releases follow [`docs/release-workflow.md`](docs/release-workflow.md). What a
comment in this codebase is for — and which comments must never be deleted
without re-establishing the fact first — is
[`AGENTS.md`](AGENTS.md#comments-what-earns-one).

Until 2026-09-08 work was tracked as one Markdown file per task under `tasks/`.
That directory is gone; [`docs/task-archive.md`](docs/task-archive.md) maps
every `task NNN` reference still in the comments and commit messages to either
its issue or the git command that prints the original file.

## License

MIT, Copyright (c) 2026 Crazy Goat Software, see [LICENSE](LICENSE), except for the code taken from
or derived from php-src:

- **PHP License 3.01**:
  - `third_party/php-src/` (vendored subset)
  - `build/phpt-fixture-patches/`
  - `sapi/fpmng/config.m4` and `sapi/fpmng/Makefile.frag`
  - `sapi/fpmng/fpm/fpm.c`, `fpm_children.c`, `fpm_conf.c`, `fpm_conf.h`, `fpm_process_ctl.c`,
    `fpm_request.c`, `fpm_request.h`, `fpm_stdio.c` and `zlog.h` (modified copies of `sapi/fpm/` files)
- **BSD-2-Clause**, Copyright (c) 2007-2009 Andrei Nigmatulin (original FPM code, text in
  `third_party/php-src/sapi/fpm/LICENSE`): the `sapi/fpmng/fpm/` copies listed above.

Those files carry no license header of their own, mostly only the original "(c) 2007,2008 Andrei
Nigmatulin" line; this list is what states their license. The same list is in [LICENSE](LICENSE).

The `.deb` and `.apk` packages ship all three texts in one file: `/usr/share/doc/<pkg>/copyright` on
Debian and `/usr/share/licenses/<pkg>/LICENSE` on Alpine (assembled by `build/package-licenses.sh`).

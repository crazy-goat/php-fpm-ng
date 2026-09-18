# php-fpm-ng

POC. PHP-FPM with additional operating modes: HTTP, supervisor, cron and
metrics — so that the container image holds one binary and the application
code, without nginx, without supervisord and without a system cron.

**Target audience: small projects.** One VPS, one instance, typically an
application plus one or two consumers plus a few cron jobs. Not k8s.

The argument is not performance — measured a few percent CPU under real load
(details in the notes). The argument is one configuration file describing the
whole thing and one binary to scan.

## State as of today

The HTTP gateway POC on libevent works, as a branch in php-src:
https://github.com/s2x/php-src/tree/fpm-http-poc

Verified 2026-09-07 against `sapi/fpmng`:

- full static build on musl (`-static-pie`) with opcache, mbstring, curl
  + OpenSSL, zlib, pdo_mysql, sockets, pcntl, posix
- runs in a bare `FROM scratch`, php-fpm-ng as PID 1, HTTP 200, whole image
  33,885,546 bytes with the full, unstripped binary
- the frontend selects `pool.type = fastcgi | http`; no directive means
  classic `fastcgi` and stays compatible with upstream FPM
- `http` accepts an optional `pool.executor = classic | fiber | async`
  (default `classic`); `fastcgi-ng` was retired in 0.9.0 (issue #376) and its
  name is refused with a dedicated message
- metrics: `pm.status_path` (JSON) and `pm.metrics_path` (Prometheus) expose
  one pool on an operator listener named by `pm.status_listen` /
  `pm.metrics_listen`, one target per pool (`docs/operator-endpoint.md`);
  application metrics from PHP
  (`fpm_metric_register/inc/set/observe`, NOTES 3k/3w) through the
  `ext/fpmng_metrics/` extension, also from CLI via `fpm_metric_render()`
- the `fiber` executor is experimental and not intended for production, while
  `async` is currently rejected during configuration validation until it has
  the same hardening; limitations are described in `docs/async_errors.md` and
  `docs/NOTES.md`, sections 3t-3u

## Support tiers

Each tier is defined by the promise it **withholds**, not by how finished the
code feels (decided in issue #269).

| tier | directives | regressions | security reports | may disappear |
| --- | --- | --- | --- | --- |
| **supported** | stable within a major release | bugs, fixed with priority | in scope | no |
| **beta** | may change in a minor release, with a release-note entry | fixed, no response-time commitment | in scope, no response-time commitment | no, but may be redesigned |
| **experimental** | may change in any release | best effort | best effort, offered as-is | yes, in any release |

Where things stand today:

| | tier |
| --- | --- |
| `pool.type = fastcgi`, `http`, `supervisor`, `cron` | supported |
| `pool.type = http-direct` with the default `classic` executor | supported |
| the operator endpoint (`pm.status_path`, `pm.metrics_path`) | supported |
| `pool.type = http-direct` with `pool.executor = worker` | beta |
| TLS termination (`--enable-fpmng-tls`, `http.tls_*`) | beta |
| ACME certificate issuance (`--enable-fpmng-acme`) | beta |
| `pool.executor = fiber` (`--enable-fpmng-fiber`) | experimental |
| `pool.executor = async` (`--enable-fpmng-async`) | experimental |

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
no compiler and no php-src to run `pool.type = fastcgi` or
`pool.type = http-direct`. The other pool types need patches that apply inside
`libphp` and still need a build from source. Commands, the supported matrix and
what happens on a version mismatch: [`docs/install.md`](docs/install.md).

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
  is. Put `pm.status_path` and `pm.metrics_path` on the pools you want to
  watch, pointing `pm.status_listen` at the address the status pool used. The
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

## Plan

Eventually a **separate SAPI** in `sapi/fpmng/`, not a fork of php-src —
`configure.ac` finds directories under `sapi/` by glob, so no existing file
needs to be touched. Details, decisions, measured numbers and the list of
known issues: [`docs/NOTES.md`](docs/NOTES.md).

`pool.type = cron` directives (`cron.schedule`, `cron.timezone`, `cron.log`,
...) are documented for operators in [`docs/cron.md`](docs/cron.md).

`pool.type = supervisor` restarts a script that exits 0 with no delay, on
purpose — the script sets the pace, and a script that returns instead of
looping now says so in the log. Who decides the interval, and the fast-restart
warning: [`docs/supervisor.md`](docs/supervisor.md).

A `cron`, `supervisor`, `http` or `http-direct` pool answers `pm.status_path`
and `pm.metrics_path` on an operator listener of its own rather than on the
socket carrying its traffic — the directives, the default of `127.0.0.1:8080`,
the collision rule and what each page contains are in
[`docs/operator-endpoint.md`](docs/operator-endpoint.md).

Shutdown grace (`process_control_timeout`, `supervisor.stop_timeout`,
`cron.timeout`, `request_terminate_timeout`) and what stock defaults do on
`docker stop` are documented in
[`docs/shutdown-timeouts.md`](docs/shutdown-timeouts.md).

One gateway can serve several pools: `http.route[<pool>] = <prefix>[,...]`
sends a path prefix to another `fastcgi` pool, so an API or a
stream endpoint gets its own workers and its own saturation behaviour without
its own listener. The longest-prefix rule, why `/` is an ordinary entry, and
why the connection budget belongs to a target pool rather than to a prefix:
[`docs/http-route.md`](docs/http-route.md).

The HTTP gateway's TLS directives (`http.tls_cert`, `http.tls_reload_check`,
...), including how a renewed certificate reaches every gateway process
without a restart, are documented in [`docs/tls.md`](docs/tls.md). TLS
termination is a build flag -- `./configure --enable-fpmng-tls`, off by
default and not in the packages (issue #280).

The gateway answers the ACME HTTP-01 challenge itself, on both `http.listen`
and the plain `http.plain_listen` companion, from state a `cron` or
`supervisor` pool publishes with `fpmng_acme_challenge_set()` — see
[`docs/acme-challenge.md`](docs/acme-challenge.md). Only one process may
renew a given certificate, and the result reaches every gateway through the
existing no-restart certificate reload —
[`docs/acme-renewal.md`](docs/acme-renewal.md). ACME is a build flag of its
own on top of the TLS one -- `./configure --enable-fpmng-tls
--enable-fpmng-acme`, off by default and not in the packages (issue #281);
a build without it carries neither the challenge state nor the client, and
refuses an ACME `cron.script` at startup.

## Experimental direct HTTP

`pool.type = http-direct` runs HTTP and PHP in the same FPM child, without the
FastCGI gateway hop. It supports **classic execution and `pm = static` only**.
The FPM master still manages the workers. This is a buffered, front-controller-only
POC, not a production frontend; configuration, limits, and benchmark methodology:
[`docs/http-direct.md`](docs/http-direct.md).

## Framework support on `pool.executor = fiber`

The `fiber` executor runs several requests concurrently in one worker process,
which only helps a framework that keeps no state outside what is isolated per
request. Full measurements, root causes and required configuration:
[`docs/frameworks.md`](docs/frameworks.md).

- **Symfony — supported, with required configuration.** Verified only on
  **Symfony 8.1.6** (skeleton + orm-pack + security-bundle, Doctrine ORM,
  sessions and cache on Redis). Requires `env[FPMNG_SHARED_INCLUDES] = 1` and a
  hand-written `public/index.php` without `symfony/runtime`
  ([#78](https://github.com/crazy-goat/php-fpm-ng/issues/78)); needs
  **no** `fiber.isolate_statics` entries. Covered by an automated probe
  (`tests/frameworks/symfony/`): sessions, the stateful `http_basic` firewall,
  Doctrine identity, Twig, form validation, synchronous Messenger dispatch,
  `APP_ENV=prod`, `pm.max_children > 1`, `fiber.revalidate_freq` deploys, and a
  200-request RSS run all pass. Other Symfony major versions (6.4 LTS, 7.x,
  other 8.x releases) are **not verified**: the `symfony/runtime` interaction
  that forces the hand-written `index.php` is version-sensitive and must be
  re-checked before extending this claim to another version.
- **Laravel — supported for the measured surface, with a required statics
  list.** Verified on **Laravel 13.30.1**. Needs
  `env[FPMNG_SHARED_INCLUDES] = 1` and `fiber.isolate_statics` naming the
  framework's request-scoped class statics (`Container::instance`,
  `Facade::app`, `Facade::resolvedInstance`, `Model::resolver`,
  `Model::dispatcher`, `Model::globalScopes`) — the exact versioned snippet
  and where each entry came from are in `docs/frameworks.md`, section
  "Laravel: the versioned configuration snippet and how it is verified".
  **Warning: an incomplete list does not crash — Laravel returns HTTP 200
  while silently serving one request's session, identity or query results to
  another, and logs nothing.** The list is verified by an automated audit
  (`/statics-audit` in `tests/frameworks/laravel/`) that snapshots every
  static property across a suspension, plus data-asserting scenarios
  covering sessions, auth, Eloquent, rate limiting, mail, Blade composers,
  route model binding and per-request observers/global scopes. The list
  must be re-verified for every Laravel minor version. Covered by
  `tests/frameworks/laravel/`.
- **Slim 4 — supported for the measured surface.** Verified on **Slim
  4.15.3** with `slim/psr7`; needs `env[FPMNG_SHARED_INCLUDES] = 1` and no
  `fiber.isolate_statics` entries. Covered by `tests/frameworks/slim4/`.

None of this applies to the default `classic` executor, which runs one request
at a time per worker like upstream FPM.

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

First prepare a pinned php-src tree with this repository's `sapi/fpmng`, then
run the static build in Alpine, building out-of-tree:

```sh
./build/prepare.sh /path/to/php-src
rm -rf "$PWD/build-dir" "$PWD/out"
mkdir "$PWD/build-dir" "$PWD/out"

docker run --rm \
  -v /path/to/php-src:/src \
  -v "$PWD/build-dir:/build" \
  -v "$PWD:/repo" \
  -v "$PWD/out:/out" \
  alpine:3.22 sh /repo/build/static-full.sh

./build/test-static-full.sh "$PWD/out/php-fpm-ng-full"
```

Two flags without which this looks broken for no reason:

- `LDFLAGS=-static-pie` — plain `-static` does not work, because the Alpine
  toolchain defaults to PIE and the linker silently produces a dynamic
  binary, and the build still succeeds
- `PKG_CONFIG="pkg-config --static"` — otherwise static curl fails the
  configure test, because its dependencies are missing from the link line

Alpine has no `oniguruma-static`, so mbstring is built with `--disable-mbregex`.

## Contributing

Work is tracked in **GitHub Issues**, not in the tree. `gh issue list` shows
what is open; labels carry type (`bug`, `enhancement`, `spike`, `refactor`,
`decision`, ...), area (`area:http-direct`, `area:tls`, `area:fiber`, ...) and
priority. Everything under `track:nice-to-have` applies only to
`pool.executor = fiber`, which is behind a build flag that is off by default.

Before touching anything, read [`workflow.md`](workflow.md): the English-only
rule, the architecture contract (new behaviour in new files under
`sapi/fpmng/fpm/`, never `strcmp(type->name, ...)`), the evidence rule, the
shared test box, and the step-by-step process from issue to merged PR. What a
comment in this codebase is for — and which comments must never be deleted
without re-establishing the fact first — is
[`workflow.md`](workflow.md#comments-what-earns-one).

Until 2026-09-08 work was tracked as one Markdown file per task under `tasks/`.
That directory is gone; [`docs/task-archive.md`](docs/task-archive.md) maps
every `task NNN` reference still in the comments and commit messages to either
its issue or the git command that prints the original file.

## License

PHP License 3.01 — code comes from PHP-FPM.

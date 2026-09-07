# 035 — Examples: there is nothing to copy from

**Priority:** highest in the main line. The project's whole thesis — "one
binary plus application code in the image, no nginx, no supervisord, no
system cron" — is currently unverifiable by anyone outside this project,
because there is nothing to copy and run that demonstrates it. Every other
main-line task (the gateway, cron, the scheduler question, proxying) only
matters if someone outside the project can see it work; examples are the
mechanism by which this project is evaluated at all, not a nice-to-have
layered on top once the features are done.
**Status:** open.

## Context

There is no `examples/` directory — checked, does not exist at the
repository root. The only configuration file in the repository is
`docker/fpm.conf`, 10 lines, two sections (`[global]`, `[www]`):

```
[global]
error_log = /dev/stderr
daemonize = no
[www]
user = 65534
group = 65534
listen = 0.0.0.0:9000
pm = static
pm.max_children = 2
chdir = /www
```

This is worth being precise about, because it undersells even what it
covers: it has no `pool.type` directive at all, meaning it exercises plain
upstream-compatible FastCGI (the default type), not the HTTP gateway, not
cron, not supervisor, not status. It is also inconsistent with its own
`docker/Dockerfile.scratch`, which does `EXPOSE 9001` while the conf file
`listen`s on `9000`. Nothing in this repository demonstrates `pool.type =
http`, `pool.type = cron`, `pool.type = supervisor`, or `pool.type = status`
running, let alone TLS, static file serving, or more than one pool type at
once.

`docker/Dockerfile.scratch` itself is a reasonable base (scratch image,
copies a binary, a conf, and `/www`), but it currently packages a
configuration that doesn't showcase anything specific to this project.

## Why this needs two levels, not one

A single example cannot do both jobs a reader needs:

- Someone deciding whether this project is worth adopting needs to see the
  **whole thesis** in one run — gateway, cron, supervisor and status
  together, in one process, one config, one container — because "one binary
  instead of four systems" is the entire pitch, and a single-pool example
  can't demonstrate that a claim about *combining* things is true.
- Someone who already believes the pitch and wants to configure **one**
  pool type needs the smallest possible, fully-readable example for that
  type alone — copying one piece of a five-pool combined config and hoping
  the rest doesn't matter is exactly the failure mode this task exists to
  avoid.

Both are required. Neither substitutes for the other.

## Problem

Build two tiers of complete, **working** examples, plus the container image
that ties them together, verified by actually starting and driving them —
not by their existing on disk.

### Tier 1 — one combined example demonstrating the whole thesis

A single `fpm-ng.conf` (or equivalent) plus one container image running, at
the same time, in one process:

- an HTTP gateway pool (`pool.type = http`) serving a small application
- a `pool.type = cron` pool on a short, verifiable schedule
- a `pool.type = supervisor` pool running a small long-lived worker script
- a `pool.type = status` pool exposing `/status` and `/metrics`

This is the example someone runs first, and the one that either proves or
disproves "no nginx, no supervisord, no system cron, one binary" in a single
`docker run`.

### Tier 2 — one minimal example per main-line pool type

Separate, standalone, minimal configs, each small enough to read in full
and copy without dragging in the other three:

- `http`, including TLS (`http.tls_cert`/`http.tls_key`) and static file
  serving (`http.static`) — this is the pool type with the most directives
  (`fpm_conf.c:173-184`) and the one doing sendfile/mmap for statics
  (`fpm_http.c:1279`, per prior verification), so it deserves its own
  focused example rather than only appearing bundled into Tier 1.
- `cron`
- `supervisor`
- `status` / metrics

## Acceptance criteria

Both tiers are judged the same way: **the example starts and does the
thing**, not "the file exists on disk."

### Tier 1 (combined)

1. One container image, one config, one `docker run` (or equivalent single
   command) starts all four pool types successfully.
2. All four are demonstrated **working at the same time**, in the same
   running instance, not in sequence across separate runs — this is the
   entire point of the tier. Concretely: while the cron pool and the
   supervisor pool are both active, the HTTP gateway answers a real request
   to the sample application, and `pool.type = status` reflects the state
   of all three (cron's `last_start`/`next_run`, supervisor's running
   process, the gateway pool) at once.
3. The HTTP request is answered with the expected body/status; the cron
   pool's scheduled script visibly ran (a file it writes, or a log line) by
   the time its schedule says it should have; the supervisor's worker
   process is confirmed alive (e.g. present in `pool.type = status`'s
   output) throughout.

### Tier 2 (per pool type)

1. `http`: starts, serves a static file over the sendfile/mmap path
   (`http.static`), serves a PHP request, and does so over TLS with a
   provided or self-signed certificate — verified with a real client
   request (e.g. `curl`), not by inspecting configuration.
2. `cron`: starts, and the scheduled script is observed to run at least once
   within a bounded wait tied to its own schedule (e.g. a schedule of a
   few minutes, not `@daily`, so the example doesn't require an hours-long
   wait to verify).
3. `supervisor`: starts, the worker process is confirmed running, and a
   forced exit of the worker script is followed by an observed restart per
   `supervisor.restart`.
4. `status`: starts, and `/status` and `/metrics` return real data reflecting
   at least one other running pool.
5. Each example is small enough that its entire config fits on one screen
   and a reader can tell, without cross-referencing anything else, what
   every line does.

## Dependency

This depends on task 002 (CI — `.github/` does not exist yet). Examples
that are never run automatically will drift the same way `docker/fpm.conf`
already has (wrong port vs. `Dockerfile.scratch`, no `pool.type` at all) —
this is not a hypothetical risk, it is the exact state the current single
example is already in. Task 002 or a follow-up to it should run both tiers
of examples on every change that touches `sapi/fpmng/` or the example files
themselves.

## Explicitly out of scope

- `pool.type = proxy` — not built, and its meaning isn't decided yet (task
  032). Excluded from both tiers until it exists.
- ACME (task 020, open, decision-first) — the TLS example in Tier 2 uses a
  provided or self-signed certificate, not automated issuance.
- Framework examples (Symfony/Laravel/Slim) — those live under `tests/`
  today and are a different concern (measurement harnesses, task 027),
  not a "here's how you configure this" example.

## Notes

- `docker/fpm.conf`'s mismatch with `docker/Dockerfile.scratch` (port 9000
  vs. `EXPOSE 9001`) is worth fixing as part of this task regardless of
  which example replaces it, since it is the one piece of configuration
  this project currently ships and it doesn't run as documented.

## Outcome (2026-09-07)

Built both tiers under `examples/`, plus fixed the `docker/fpm.conf` /
`docker/Dockerfile.scratch` port mismatch directly (`listen` changed from
`0.0.0.0:9000` to `0.0.0.0:9001`).

### What was built

- `examples/README.md` -- index, plus the exact build recipe (matches
  `.github/workflows/build-matrix.yml`) to produce `php-fpm-ng` once and
  copy it next to whichever example's `Dockerfile` is used.
- `examples/combined/` -- Tier 1: one `fpm-ng.conf` with `[app]`
  (`pool.type = http`), `[tick]` (`pool.type = cron`, `* * * * *`),
  `[worker]` (`pool.type = supervisor`) and `[metrics]`
  (`pool.type = status`), one `Dockerfile`, one `docker run`.
- `examples/http/`, `examples/cron/`, `examples/supervisor/`,
  `examples/status/` -- Tier 2: one pool type each, each config under 20
  lines. `http` includes a `generate-cert.sh` for a self-signed cert (not
  committed -- see `.gitignore`).
- Not `FROM scratch`: task 004 (open) has not yet verified a static build
  for `sapi/fpmng`, so every example's `Dockerfile` ships the dynamically-
  linked binary CI actually produces, on `ubuntu:24.04` (same glibc as the
  GitHub-hosted runner that builds it -- a `debian:bookworm-slim` runtime
  base was tried first and failed with a `GLIBC_2.38 not found` error,
  confirming the two toolchains aren't ABI-compatible here).

### What was verified, and how (all done locally against a binary built
from this branch, `php-8.5.9`, `--enable-fpmng --enable-session
--with-openssl`, matching CI exactly -- not "the file exists on disk")

**Tier 1** (`examples/combined/`), one running container, `docker run -d
... -p 18080:8080 -p 18081:8081 fpmng-combined-example`:

1. One image, one config, one `docker run` started all four pool types;
   `docker logs` showed `fpm is running` / `ready to handle connections`
   with no warnings (after adding `process_control_timeout = 15s` to
   satisfy a real startup warning that appeared on the first run).
2. All four observed together, same instance, same time window:
   `curl http://localhost:18080/index.php` answered while `[tick]` and
   `[worker]` were both already active; `curl http://localhost:18081/status`
   in the same window returned one JSON body with `app`, `tick` and
   `worker` all present at once.
3. `curl /index.php` returned the expected body; `curl /hello.txt` was
   served statically (`http.static`); `curl /nonexistent-route` fell
   through to `http.front_controller` with `PATH_INFO=/nonexistent-route`;
   `cron-tick.txt` and `cron.log` inside the container gained one line at
   `09:56:00` and a second at `09:57:00` (real minute boundaries, not
   synthetic); `worker-heartbeat.txt` updated continuously (pid 12,
   `uptime` in `/status` climbing 6s -> 74s across the same window).

**Tier 2**, one container each:

1. `http`: `docker build` + `./generate-cert.sh` + `docker run`, then
   `curl -ks https://localhost:18443/hello.txt` (static, confirmed
   `HTTP/1.1 200 OK` over a real `TLSv1.3` handshake, `curl -v` showing
   `subject: CN=php-fpm-ng.example`) and
   `curl -ks https://localhost:18443/index.php` (PHP, body showed
   `scheme: https`, confirming `$_SERVER['HTTPS']` from `fpm_http.c:589`).
2. `cron`: started, waited for a real minute boundary (`sleep 65`),
   `docker exec ... cat /www/data/tick.txt` and `.../cron.log` both showed
   a fresh `09:54:00`-timestamped line.
3. `supervisor`: `docker exec ... cat /www/data/heartbeat.txt` showed pid 7
   alive; `kill -9 7` inside the container, then the same file showed pid
   26 with a fresh timestamp within 3s -- `supervisor.restart = always`
   confirmed restarting after a forced exit.
4. `status`: baseline `curl /status` showed `"requests":0`; three
   `curl /index.php` against the paired `[app]` pool, then `/status` showed
   `"requests":3` and `/metrics` showed
   `fpmng_pool_requests_total{pool="app"} 3` -- real data from the other
   running pool, not a static shape.
5. Each Tier 2 config is 8-20 lines, one pool section (plus `[global]`),
   verified readable in full without cross-referencing the other examples.

### Left out / scoped down

- No static-musl / `FROM scratch` build for these examples -- that's task
  004 (open), not re-litigated here; noted explicitly in
  `examples/README.md`'s "Why not `docker/`" section instead of silently
  diverging from the existing `docker/Dockerfile.scratch` pattern.
- CI wiring for the two tiers (task 002's follow-up, per this task's own
  "Dependency" section) is not done here -- left for a follow-up task so
  examples don't silently drift again. Filed as a natural next step, not
  as part of this PR.
- `pool.type = proxy` and ACME are out of scope per the task's own
  "Explicitly out of scope" section; nothing built here touches either.

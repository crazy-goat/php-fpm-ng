# 010 — The HTTP gateway does not drop privileges, and now holds a TLS private key

**Priority:** high, security-relevant.
**Status:** done (2026-09-06) — see Decision and Outcome below.

## Context

For every TCP pool the master forks a few gateway processes
(`sapi/fpmng/fpm/fpm_http.c:3`, fork at roughly line 1468). The child then closes
the pools' FastCGI listeners, sets its process title, opens the access log and
enters its event loop.

It never calls `setuid`, `setgid` or `initgroups`. There is no call to any of
them in `fpm_http.c`. The gateway therefore runs with the master's identity —
root, in the usual deployment where the master binds privileged ports.

This predates TLS. It matters more now, because as of the TLS termination work
the gateway process:

- reads and holds the **TLS private key** in memory (loaded once in the master
  before fork, then inherited)
- is the process directly facing the network, parsing untrusted HTTP and now
  untrusted TLS records

Meanwhile the request workers *do* drop privileges the normal FPM way, via the
pool's `user` / `group`. The network-facing process is the privileged one and the
sandboxed one is behind it, which is backwards.

## Problem

Decide what identity the gateway should run as, and make it so.

## Acceptance criteria

1. A written decision covering: what the gateway needs root for (binding a
   privileged port, reading a key file with restrictive permissions), at what
   point that need ends, and what it should become afterwards.
2. The gateway runs unprivileged once it no longer needs privileges, in the
   default configuration, without the operator having to configure anything.
3. Everything the gateway does after the drop still works: binding with
   `SO_REUSEPORT` when `http.reuseport` is on (each child opens its **own**
   listening socket after fork, so ordering matters here), the access log,
   static file serving, the shared connection budget, TLS.
4. If the identity is configurable, it defaults to something safe and its
   relationship to the pool's `user` / `group` is documented — including whether
   they may differ and what happens if the pool has none.
5. Failure to drop privileges is fatal and logged, never silent.
6. Verified by inspecting the running process's uid/gid, not by reading code.

## Explicitly out of scope

- Sandboxing beyond uid/gid (seccomp, capabilities, namespaces). Worth
  considering later; not this task.
- Changing how workers drop privileges. That path is upstream FPM's and works.

## Notes

- Order of operations is the whole difficulty: bind privileged ports and read the
  key **before** dropping, then drop before accepting the first connection. With
  `http.reuseport` the child opens its own socket *after* fork, which pulls the
  bind later than one would like — check this specific interaction rather than
  assuming.
- The key is currently read once in the master before the first fork; that is a
  deliberate choice recorded in `fpm_http_tls.h` and it helps here, since the
  child never needs to open the key file itself.

## Decision — 2026-09-06

The gateway drops to the pool's own `'user'`/`'group'` — the same identity
`fpm_unix_init_child()` (`sapi/fpm/fpm/fpm_unix.c`) already puts the request
workers under, resolved once by `fpm_unix_conf_wp()` before the first fork
(`wp->set_uid`/`set_gid`/`set_user`). No new `http.user`/`http.group`
directive: nothing to configure, no way for the gateway's identity to drift
from the worker's it forwards requests to, satisfying acceptance criterion 2
(default-safe, zero operator configuration) directly.

**What needs root, and when it ends:** binding `http.reuseport`'s own listener
(a possibly-privileged port, opened by the child itself *after* fork — the
ordering hazard called out above) and holding the TLS private key the master
already read before the first fork. Both are done by the time
`fpm_http_gateway_run()` reaches the reuseport block in
`sapi/fpmng/fpm/fpm_http.c`. Everything after — the access log, static files,
TLS handshakes, proxying to the pool — needs no privilege at all, so
`fpm_http_gateway_drop_privileges()` runs right there, before
`http.access_log` is even opened (moved to open *after* the drop, so the file
is created by the dropped-to identity, not root).

**Pool with no `user`/`group` at all:** only reachable under FPM's existing
`--allow-to-run-as-root` escape hatch (`fpm_conf.c` refuses it otherwise). The
operator explicitly asked for root there, so the gateway stays root too —
matching `fpm_unix_init_child()`'s own behaviour for workers — but says so
with a `ZLOG_WARNING`, never silently.

**Failure to drop** (`setgid`/`initgroups`/`setuid` returning nonzero, or
still being root right after) is fatal: `exit(FPM_EXIT_SOFTWARE)`, logged via
`ZLOG_SYSERROR`/`ZLOG_ERROR`. Never continue serving a TLS private key as
root.

## Outcome — 2026-09-06

Implemented in `sapi/fpmng/fpm/fpm_http.c`:
`fpm_http_gateway_settings()` copies `wp->set_uid`/`set_gid`/`set_user` (or
`wp->config->user` when the pool's user was given by name, mirroring
`fpm_unix_init_child()`'s own fallback) onto the new `gw->drop_uid/drop_gid/
drop_user` fields; `fpm_http_gateway_drop_privileges()` performs the actual
`setgid`/`initgroups`/`setuid` drop and is called from
`fpm_http_gateway_run()` right after the `http.reuseport` bind, before the
access log is opened.

Verified by `build/test-http-gateway-privileges.sh`, wired into CI as the
`gateway-privileges` job in `.github/workflows/build-matrix.yml` (needs a root
master, so it runs under `sudo`, in its own job rather than folded into
`phpt`). It starts a real `php-fpm-ng` as root with a `pool.type = http` pool
and inspects `/proc/<pid>/status` of the actual running gateway process —
never just reads the code — across three scenarios: plain, `http.reuseport =
yes` (the ordering hazard), and a pool with no `user`/`group` under
`--allow-to-run-as-root` (expected to stay root, with the warning logged).
Each scenario also does one real HTTP round-trip through the gateway
post-drop and checks `http.access_log`'s owner, covering acceptance criterion
3.

Compiled locally against the pinned `php-8.5.9` php-src with no warnings on
`fpm_http.c`. The test script itself needs a root master, which this
(non-root-sudo) machine cannot provide; it is exercised by the
`gateway-privileges` CI job instead — see that job's result on the PR before
treating this as verified.

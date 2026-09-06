# 010 — The HTTP gateway does not drop privileges, and now holds a TLS private key

**Priority:** high, security-relevant.
**Status:** open.

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

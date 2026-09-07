# 040 — TLS: replace the certificate without restarting the gateway

**Priority:** high. Hard prerequisite for 020 (ACME renewal), and useful on its
own for a certificate that arrives on a mounted volume.
**Status:** done (2026-09-07) — see Outcome below.

## Context

TLS material is resolved once, in the master, before any gateway child is
forked: `fpm_http_tls_load()` reads the PEM files into
`struct fpm_http_tls_s` and `fork()` copies the bytes into every child
(`sapi/fpmng/fpm/fpm_http_tls.h`, header comment). Each child then builds its
own `SSL_CTX` from those bytes in `fpm_http_tls_ctx_new()`. Nothing re-reads
the files afterwards.

So a renewed certificate on disk is invisible until the pool is restarted.
Graceful reload exists (`docs/NOTES.md`, section "3x. Graceful reload"), but it
recycles processes; the point of this task is that a certificate change must
not require that.

## Problem

Make a new certificate take effect in every gateway process of a pool, without
dropping connections and without an operator action beyond whatever signal or
write triggers it.

## Open questions the implementer must settle and write down

- **What triggers it.** A signal to the master, a config-reload path, or the
  gateway noticing an mtime change. Polling `stat()` in the request path is not
  acceptable; a timer in the event loop is.
- **How the new bytes reach the children.** The current design is deliberate:
  the master owns the file reads, the children never touch the key file. A
  reload must keep that property or explicitly argue why it changes.
- **Torn reads.** A certificate and its key written non-atomically will be read
  half-updated. Decide whether the contract is "write to a temp file and
  rename" (documented) or "validate the pair before installing" (enforced).

## Acceptance criteria

1. With `http.gateways` greater than 1, replacing the certificate and key files
   and triggering the reload makes every gateway process serve the new
   certificate. Verified by connecting repeatedly and comparing the served
   certificate's serial number, with enough connections that every process is
   hit under `SO_REUSEPORT`.
2. Connections in flight during the reload complete normally. Verified with a
   load generator running across the reload: zero connection errors, zero
   truncated responses.
3. A broken new certificate (unparsable, or key not matching) leaves the old
   one serving and logs an error naming the problem. The listener never falls
   back to plain HTTP and never stops accepting.
4. No log line and no error message ever contains key material. The project has
   already had one near-miss logging a credential-bearing identifier.
5. Session resumption still works across gateway processes after the reload —
   the shared ticket key exists precisely for that
   (`fpm_http_tls.h`, `ticket_key`).

## Notes

- 039 (chain) should land first: reloading a chain-less certificate just
  reproduces that bug on a timer.

## Decision — 2026-09-07

**Existing "reload" does not apply here.** The signal-based reload
(`fpm_pctl_exec()`, `fpm_process_ctl.c:86-108`) sends `SIGQUIT`/`SIGTERM` to
every child, waits for them to exit, then `execvp()`s the master itself
(`docs/NOTES.md:3413-3446` — explicitly documented as *not* a real
config-diffing hot reload). Reusing it would recreate every gateway
process, which is exactly what this task exists to avoid. This task needs
its own, separate mechanism, independent of that reload path.

**Why an in-process swap is possible at all.** Each gateway child owns one
`event_base`/`evhttp` for its whole life and calls
`evhttp_set_bevcb(gw->http, fpm_http_tls_bevcb, gw->tls_ctx)` once, at
startup (`fpm_http.c:1592`). `fpm_http_tls_bevcb()` only reads `arg` (the
`SSL_CTX*`) at **accept time**, once per incoming connection
(`fpm_http_tls.c:340-346`) — it is not baked into the listener or the event
base. `evhttp_set_bevcb()` can be called again later on the same `gw->http`
to point future connections at a new `SSL_CTX*`, without touching
`gw->listen_fd` or `gw->base`. OpenSSL reference-counts `SSL_CTX` internally
(`SSL_new()` takes a ref), so `SSL_CTX_free()`-ing the old one after the
swap does not disturb `SSL*` objects already in use by in-flight
connections — this is what makes acceptance criterion 2 (zero errors on
connections in flight) achievable without any drain logic.

**Trigger: a self-rearming timer in the master, not a signal and not
per-request `stat()`.** The master already runs its own event loop after
fork (used today for `pctl` timeouts, `fpm_process_ctl.c:57-63`); a
periodic timer on that loop calls `stat()` on the configured cert/key paths
every few seconds (a new `http.tls_reload_check` directive, seconds,
default e.g. 5 — same idiom as `fiber.revalidate_freq`'s timer,
`fpm_pool_fiber.c:576-583`) and compares mtimes against the last-loaded
pair. This keeps `stat()` entirely out of the request path (satisfying the
task's explicit constraint) and needs no operator-triggered signal — a
certificate landing on a mounted volume (the task's own second use case,
alongside ACME) has nobody to send a signal. A signal-based trigger is not
ruled out for a later task, but is not required for this one's acceptance
criteria.

**Torn reads: enforced, not just documented.** On an mtime change the
master calls the existing, already-stateless `fpm_http_tls_validate()`
(`fpm_http_tls.c:194-227` — reads both files itself, builds a throwaway
`SSL_CTX`, returns 0/-1, logs a message naming the problem, never logs key
material) against the *candidate* paths before touching anything the
children can see. A write-then-rename on the operator's side (documented as
recommended practice, e.g. what `certbot`/ACME clients already do) makes
the candidate atomic from the filesystem's point of view; the validate call
is the enforced backstop for operators who don't rename atomically or who
momentarily write a half-written pair. If validation fails, the master logs
the error and does **not** advance anything — the old certificate keeps
serving, satisfying acceptance criterion 3 without any special-casing in
the children.

**Propagation: master keeps owning the file reads; children get bytes, not
paths, via a double-buffered shared-memory slot.** The existing
"master-only file access" property (`fpm_http_tls.h` header comment) is
kept, not relaxed — the alternative (each child re-reading the key file
itself on its own timer) would multiply file opens across every gateway
process and contradicts the documented reason that property exists in the
first place. Concretely:

- The shared-memory primitive available today (`fpm_shm_alloc()`,
  upstream `fpm_shm.c:18-38`) is a plain anonymous `mmap(MAP_SHARED)` with
  **no locking** — the only existing multi-process pattern
  (`gw->upstreams_used`, `fpm_http.c:1986-1992`) is a single lock-free
  atomic word (`atomic_cmp_set()`, `fpm_http.c:286-302`), which is not
  enough by itself for variable-length PEM bytes.
- So the new region holds **two** fixed-size slots (sized for the largest
  chain the pool's config allows, same bound `fpm_http_tls_load()` already
  enforces at startup) plus one `atomic_t generation` word. The master
  writes a newly validated cert/key/ticket-key triple into whichever slot
  is *not* the currently-published one, then bumps `generation` with the
  same `atomic_cmp_set()` idiom already in use — the bump is the only thing
  a reader needs to observe, and it publishes a fully-written slot,
  never a partially-written one.
- Each gateway child adds its **own** timer, in its own `gw->base` (same
  `event_new(base, -1, EV_PERSIST, cb, NULL)` idiom as
  `fpm_pool_fiber.c:576-583`), that cheaply reads `generation` every couple
  of seconds. On change, it copies the now-published slot into a local
  `struct fpm_http_tls_s`, builds a new `SSL_CTX` with the **existing,
  unmodified** `fpm_http_tls_ctx_new()` (it already takes a
  `struct fpm_http_tls_s*`, not a path — no change needed there), calls
  `evhttp_set_bevcb()` again with the new context, and frees the old
  `SSL_CTX`. No IPC signal from master to child is needed; the child polls
  its own copy of `generation`, which is cheaper and simpler than
  coordinating wakeups across an unbounded number of gateway processes.
- Session resumption (criterion 5) falls out of this for free: the ticket
  key lives in the same published slot, so every child adopts the same new
  `ticket_key` at the same `generation` bump — no separate mechanism.

**Signal budget, if a later task wants an explicit trigger too.** Gateway
children reset `SIGUSR1`/`SIGUSR2` to `SIG_DFL` at startup and repurpose
neither (`fpm_http.c:1532-1541`), so both are free if an explicit
"reload now" signal is ever wanted in addition to the timer. Not needed for
this task's acceptance criteria and left out of scope here.

### Still open, for whoever implements this

- The exact fixed slot size (bound the maximum chain size the way
  `fpm_http_tls_load()` already bounds file reads at startup) and the
  default `http.tls_reload_check` interval.
- Whether the master's own `stat()` timer needs to exist per-pool or can be
  one timer walking all TLS pools — a config/perf question, not a
  correctness one, and not required to resolve before implementation
  starts.

## Outcome — 2026-09-07

Implemented exactly as decided above, in one new file plus minimal hooks
into `fpm_http.c`:

- **New file `sapi/fpmng/fpm/fpm_http_tls_reload.{c,h}`.** Owns the
  double-buffered shared-memory region (`fpm_http_tls_reload_shared_s`: one
  `atomic_t generation` plus two `fpm_http_tls_reload_slot_s` slots, each a
  fixed `FPM_HTTP_TLS_RELOAD_MAX_CERT` = 64 KiB / `FPM_HTTP_TLS_RELOAD_MAX_KEY`
  = 16 KiB buffer plus `ticket_key`/`min_version` — sizes picked as "generous
  for a real fullchain.pem", not measured against a specific deployment; a
  candidate exceeding them is rejected exactly like any other invalid one),
  the master's mtime timer (`fpm_http_tls_reload_master_tick()`,
  `fpm_http_tls_reload.c:98`, armed by `fpm_http_tls_reload_master_init()`,
  `fpm_http_tls_reload.c:174`, via `fpm_event_set_timer()`/`fpm_event_add()` —
  the same upstream FPM epoll timer `fpm_pctl_heartbeat()` uses, not
  libevent), and each gateway child's adoption timer
  (`fpm_http_tls_reload_child_tick()`, `fpm_http_tls_reload.c:234`, armed by
  `fpm_http_tls_reload_child_init()`, `fpm_http_tls_reload.c:282`, via
  libevent `event_new()`/`event_add()` on the child's own `gw->base` — the
  same idiom as `fiber.revalidate_freq`'s timer). Reuses
  `fpm_http_tls_validate()`, `fpm_http_tls_load()`, `fpm_http_tls_free()` and
  `fpm_http_tls_ctx_new()` from `fpm_http_tls.c` completely unmodified — that
  file did not need to change at all for this task.
- **`fpm_http.c` hooks (all under the existing `HAVE_FPM_HTTP_TLS` guard):**
  a `struct fpm_http_tls_reload_s *reload` field on `fpm_http_gateway_s`;
  `fpm_http_tls_reload_master_init()` called right after
  `fpm_http_tls_load()` in `fpm_http_gateway_settings()`
  (`fpm_http.c:1900`); `fpm_http_tls_reload_child_init()` called right after
  `evhttp_set_bevcb()` in `fpm_http_gateway_run()` (`fpm_http.c:1600`);
  `fpm_http_tls_reload_free()` in `fpm_http_cleanup()` (`fpm_http.c:1774`).
- **New directive `http.tls_reload_check`** (seconds; `fpm_conf.c`/
  `fpm_conf.h`, `fpm_conf_set_time`), unset → `FPM_HTTP_TLS_RELOAD_CHECK_DEFAULT`
  = 5s (a config/perf choice, not measured against a target load), `0` →
  off, same was-it-actually-set resolution pattern `http.gateways` already
  uses (`fpm_conf_directive_was_set()`).
- **Torn reads**, per the Decision: `fpm_http_tls_validate()` runs against
  the candidate paths before anything is published; on failure the mtimes
  are still recorded (so a persistently broken pair does not re-log every
  tick) but nothing changes for any gateway child.
- Adding this file needed `buildconf --force` + a config.nice reconfigure to
  get picked up, per `build/prepare.sh`'s own end-of-run warning — confirmed
  the hard way (first build linked with undefined references to the three
  public functions until the reconfigure).

### Verification

Compiled cleanly (no warnings on any touched file) against the pinned
`php-8.5.9` php-src, confirmed with `strings` on the resulting binary that
the new log lines (`"TLS certificate reloaded from disk"`,
`"adopted reloaded TLS certificate"`) are actually present before testing —
i.e. testing the binary this change produced, not a stale one.

All five acceptance criteria verified against a running gateway over real
TLS handshakes (a locally generated 2-level test CA, root → intermediate →
two leaves with different serials), both by hand and by the new
`build/test-http-tls-reload.sh`, wired into CI as the `tls-reload` job in
`.github/workflows/build-matrix.yml` (`needs: build`, no root required,
unlike `gateway-privileges`):

1. **Every gateway process serves the new certificate.** With
   `http.gateways = 3` and `http.reuseport = yes`, 12 separate
   `openssl s_client -CAfile root.crt` connections after the swap all show
   the new leaf's serial and `Verify return code: 0 (ok)`.
2. **Zero errors on connections in flight.** 300 sequential HTTPS requests
   (via `curl --insecure`) run in a background loop spanning the file swap;
   zero non-`ok` responses.
3. **A broken candidate (cert/key mismatch) is rejected, old certificate
   keeps serving.** `error_log` gets a
   `"http.tls_cert/http.tls_key: private key rejected by OpenSSL"` line
   (`fpm_http_tls_validate()`'s existing message, reused as-is); the
   gateway keeps serving the previous (valid) leaf and keeps accepting
   requests (`curl` still gets `ok`).
4. **No key material in any log line** — grepped `error_log` and the
   master's stdout for a PEM `BEGIN ... PRIVATE KEY` marker: none found.
5. **Session resumption survives the reload.** A TLS1.2 session saved
   against the post-reload certificate is reused (`Reused, TLSv1.2, ...`,
   not a full handshake) across 5 further connections, each potentially
   landing on a different `SO_REUSEPORT` gateway process — confirms the
   shared `ticket_key` travels through the same `generation` bump as the
   certificate.

Not separately measured: behaviour under `http.gateways` values other than
3, or under sustained production-scale load rather than a 300-request
smoke loop — `build/test-http-tls-reload.sh`'s numbers (3 gateways, 300
requests, 1s check interval) are what CI actually runs, not claimed to be
exhaustive.

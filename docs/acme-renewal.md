# One renewer, and how the result reaches every gateway

This document covers the two halves of issue #47: making sure only one
process renews a given certificate, and getting the result into every
gateway process. Serving the HTTP-01 challenge is
[`docs/acme-challenge.md`](acme-challenge.md); obtaining and renewing a
certificate against a CA is [`docs/acme-client.md`](acme-client.md)
(issue #49).

## The unit of exclusion is a certificate, not a pool

`pool.type = cron` is pinned to one process (`fpm_pool_cron.c` forces
`pm = static` and `pm.max_children = 1`), so a single ACME cron pool cannot
overlap with itself. That is not enough on its own:

- a configuration may declare two cron pools, or a `supervisor` pool,
  pointing at the same ACME state directory;
- a reload or a master restart may start a new renewer while the previous
  process is still finishing an order;
- the same state volume may be mounted into more than one pool.

What must hold is therefore *one renewer per certificate*, and a certificate
is identified by its directory under the ACME state root. The exclusion
lives there: an advisory whole-file lock on
`$ACME_STATE_DIR/<domain>/renewal.lock`, held for the whole order.

```php
use FpmNg\Acme\RenewalInProgress;
use FpmNg\Acme\RenewalLock;
use FpmNg\Acme\State;

$state = State::fromEnvironment();          // env[ACME_STATE_DIR]
$lock  = new RenewalLock($state, 'example.com');

try {
    $lock->run(function () use ($state) {
        // the order: authorize, finalize, install
    });
} catch (RenewalInProgress $e) {
    // Another process is renewing this certificate. Not an error: this
    // tick has nothing to do. $e->getMessage() names the holder.
}
```

`RenewalLock::holder()` answers "why did this tick do nothing" with the
holder's pid, host and start time. It is diagnostics: it is never consulted
to decide whether the lock may be taken, and it probes with a *shared* lock
so that asking the question cannot make a renewer ticking at that instant
see the certificate as busy. Call it from a process that is not itself
inside `run()`.

Because the lock is named after the certificate's directory, the domain is
canonicalised first (`State::canonicalDomain()`): lowercased, with the
trailing root dot removed, and rejected outright if it is not a hostname.
`Example.com`, `example.com.` and `example.com` are one certificate to a CA,
so they must be one lock — two pools spelling one name differently would
otherwise both run an order.

### Why flock() and not the shared memory the challenge store uses

The challenge answers live in the master's shared memory
(`fpm_acme_challenge.h`) because they must be read by gateway processes on
the request path. The renewal lock is a different problem:

| | challenge store | renewal lock |
|---|---|---|
| Scope | one master's process tree | everything writing one state volume |
| Held for | microseconds | the length of an order |
| On holder death | nothing to release | must release automatically |

The last row is the decisive one. The kernel drops an `flock()` when the
holding process dies, however it dies, so a renewer killed mid-order needs
no operator action — and, just as importantly, no stale-lock timeout, which
would have to guess how long a legitimate order may take and would either
block a real recovery or allow a real double-renewal.

**Recovery time is one scheduler tick of the ACME cron pool.** Nothing has
to notice the death; the next tick simply acquires the lock.

### What this does not cover

`flock()` is advisory and local to one kernel. Two containers sharing one
ACME state directory over a network filesystem are not protected by it, and
that is not a supported deployment. One state volume belongs to one
php-fpm-ng master.

## Handover: nothing new

Issue #47 requires that after a successful renewal every gateway process
serves the new certificate, and explicitly forbids growing a second
mechanism for it. There is none. The path is:

1. The renewer writes `fullchain.pem` into the state directory with
   `State::installCertificateChain()`, which writes a temporary file in the
   same directory and `rename()`s it, so a reader never sees a partial file.
2. `http.tls_cert` and `http.tls_key` point straight at those files.
3. The master's reload timer (task 040, `fpm_http_tls_reload.c`) digests
   both files every `http.tls_reload_check` seconds — a SHA-256 of the
   contents, not `st_mtime` (issue #71) — validates the pair, and publishes
   the new bytes into a double-buffered shared-memory region.
4. Each gateway process adopts the new generation on its own timer and
   builds its own `SSL_CTX`. A gateway respawned after the change starts
   from the newest published generation (issue #91), so it cannot come back
   serving the old certificate.

Two practical consequences for a renewer:

- **Reuse the certificate key.** `state.php` keeps `privkey.pem` per domain
  and reuses it, so a renewal changes exactly one file. If a renewer ever
  rotates the key too, the master can briefly digest a new key against an
  old chain; the pair fails validation, the current certificate keeps
  serving, and the next poll succeeds — safe, but it logs an error, so
  write the key first and the chain second if you must rotate both.
- **`http.tls_reload_check` must not be 0** for a pool whose certificate is
  managed by ACME. Zero means "never poll", which turns a successful
  renewal into a certificate nobody picks up until a restart.

SNI certificates (`http.tls_sni_cert`) are validated at startup but are not
part of the reload poll — only the primary `http.tls_cert` /
`http.tls_key` pair reloads without a restart. A pool serving several ACME
certificates over SNI needs that gap closed first; it is out of scope here.

## Where the per-pool-type behaviour lives

Issue #47 criterion 5 requires ACME scheduling and handover to be pool-type
data, never `if (type == ...)`. Nothing in this change compares a type name,
and each half already has its home:

| Behaviour | Where |
|---|---|
| When the renewer runs | `pool.type = cron`'s own schedule (`fpm_cron_schedule.c`, `next_run` in `struct fpm_pool_type_s`) |
| Who may publish a challenge | `publishes_acme_challenges` in `struct fpm_pool_type_s` (issue #48) |
| Who runs a script at all | the type's `child_main` callback |
| Certificate handover | the `http` type's TLS reload state, built in its `init_main` and adopted per child |
| Who may renew | not a type property at all — the lock above, keyed by the certificate |

The last row is the one worth stating explicitly: "may renew" deliberately
did *not* become a type flag. A flag would say "processes of this type may
run the client", which is not the property that has to hold — two pools of
a permitted type would both pass it.

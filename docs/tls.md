# HTTP gateway TLS — operator reference

This is the operator-facing reference for TLS termination on `pool.type =
http`'s gateway: the directives that turn it on, and how a renewed
certificate reaches every gateway process (task 040). For the internal
design rationale, see the header comments in `sapi/fpmng/fpm/fpm_tls_http.c`
and `sapi/fpmng/fpm/fpm_tls_reload.c`, and
task 040 (done; see [`task-archive.md`](task-archive.md)) for why each choice
was made the way it was.

## The build flag

TLS termination is **not** in a default build. It is compiled in only by
`./configure --enable-fpmng --enable-fpmng-tls`, and the shipped `.deb`/`.apk`
packages are built **without** it (issue #280, part of #279): this code is
beta, unaudited and network-facing, so the binary that terminates TLS for the
world is one somebody chose to build. Until v0.4.0 the answer depended on
whether `libevent_openssl` happened to be installed on the build host, which
is not a default anyone chose.

What that means in practice:

- `--enable-fpmng-tls` needs `libevent_openssl >= 2.1` and OpenSSL >= 1.1.1
  development files. If they are missing, `configure` **fails** and names the
  package; it does not downgrade.
- Without the flag nothing here links OpenSSL at all (`ldd` shows no
  `libssl`/`libcrypto`/`libevent_openssl`), and the `fpm_tls_*.c` sources are
  not compiled.
- A pool with `http.tls_cert` on such a binary is **refused at startup**,
  naming the flag to rebuild with. It never falls back to plain HTTP on a
  port the operator configured as HTTPS.

The same applies to `pool.type = http-direct`'s TLS
([`http-direct.md`](http-direct.md)) and to the ACME client
([`acme-renewal.md`](acme-renewal.md)), which is TLS with extra steps.

## Directives

- **`http.tls_cert`** (optional, default: unset = plain HTTP) — path to a PEM
  file with the certificate. May be a full chain (leaf followed by one or
  more intermediates, e.g. Let's Encrypt's `fullchain.pem`): every
  certificate in the file is sent to the client, in file order.
- **`http.tls_key`** (required if `http.tls_cert` is set) — path to the PEM
  private key matching the certificate's leaf.
- **`http.tls_min_version`** (optional, default: `TLSv1.2`) — `TLSv1.2` or
  `TLSv1.3`.
- **`http.tls_reload_check`** (optional, seconds, default: `5`) — how often
  the master checks `http.tls_cert`/`http.tls_key` for changes on disk.
  `0` disables reload entirely (a certificate change then needs a restart,
  same as before this directive existed).

## How reload works

Replace the files at `http.tls_cert`/`http.tls_key` (in place, or by
writing new files and renaming them over the old paths — either works) and
do nothing else. Within `http.tls_reload_check` seconds the master notices
the change, validates the new pair the same way it does at startup, and —
only if it is valid — makes it available to every gateway process of the
pool. Each gateway process then adopts it, independently, within another
`http.tls_reload_check` seconds, without dropping any connection already in
progress and without re-binding its listening socket.

The change is detected from the **contents** of the two files, not their
timestamps, so it does not matter how close in time the write lands to the
one before it: a renewal, a rollback to the previous pair, or a correction
written in the same second as the file it fixes are all picked up. Rewriting
the same bytes is correctly seen as no change at all.

No signal, no reload command, no operator action beyond the write. This
matters most for a certificate that arrives on a mounted volume (an ACME
client, or any external renewal process) with nothing running inside the
container to send a signal.

**A broken candidate never replaces a working certificate.** If the new
files do not parse, or the key does not match the certificate, the change is
logged (naming the problem, never the key's content) and ignored — the
gateway keeps serving the certificate it already had. The listener never
falls back to plain HTTP and never stops accepting connections over this.

**Session resumption keeps working across the reload.** All gateway
processes of a pool share one TLS session ticket key so a client can resume
a session against whichever process `http.reuseport` hands it to next; a
reload replaces that shared key too, atomically with the certificate, so
this stays true for sessions established after the reload.

## Sizing

Certificates and keys published through a reload are copied into a
fixed-size shared-memory buffer (64 KiB for the certificate/chain, 16 KiB
for the key — generous for a real-world `fullchain.pem`, not a hard
protocol limit). A pair larger than that is rejected like any other invalid
candidate: logged, not installed. This bound does not apply to the
certificate a pool starts up with, only to a later reload.

## Starting before the certificate exists (`http.tls_wait_for_cert`)

- **`http.tls_wait_for_cert`** (optional, boolean, default: `no`) — start the
  pool even though `http.tls_cert` does not exist yet, and begin serving TLS
  by itself once it appears. Requires `http.tls_cert` to be configured and
  `http.tls_reload_check` to be non-zero.

**Read the default as a safety property, not as an omission.** With the
default `no`, a missing or unreadable `http.tls_cert` fails the master's
startup outright. That is fail-closed: a pool that was meant to serve HTTPS
never quietly comes up as something else. Turning this on trades that
guarantee for the one case where it is wrong — a container booting for the
very first time, where the certificate is about to be issued by the ACME
client inside the same binary (see
[`acme-renewal.md`](acme-renewal.md)) and cannot exist before the pool that
answers the challenge does. **Do not set it on a pool whose certificate is
provisioned externally**, where a missing file means an operator mistake and
you want to hear about it at boot.

A cert path that exists but does not parse still fails startup, opt-in or
not. That is an operator error, not an unfinished issuance.

### The two states

**NO_CERT** — the certificate is not there yet.

- `http.listen` (the TLS port) is **bound but not listening**. A client gets
  a connection refused, not a TLS handshake failure. This is deliberate: a
  refusal says "not ready", whereas answering the handshake with a
  self-signed or expired certificate teaches clients to distrust the name.
- `http.plain_listen` answers `/.well-known/acme-challenge/...` from the
  challenge store as usual, and answers everything else with **503 Service
  Unavailable** and `Retry-After: 60` instead of its normal 308 redirect to
  a port that is not there.
- The master logs the state once at startup, naming the path it is waiting
  for and the re-check interval.

**READY** — the certificate has appeared.

- Each gateway process notices the new generation on its own timer, within
  `http.tls_reload_check` seconds, calls `listen()` on the socket it already
  holds and starts accepting. It logs `leaving NO_CERT` with the generation
  number, once, per process.
- The plain listener goes back to redirecting with 308.

The transition is **one-way within the life of a process**. Deleting the
certificate afterwards does not take TLS back down: the loaded certificate
keeps serving, the master logs one warning that renewal is blocked until the
pair is readable again, and a later restore logs that it is readable again.
Same rule as an ordinary reload — a broken or absent candidate never replaces
a working certificate.

### What an operator polls

There is no TLS-state field in `fpm_status`: the status callback on a pool
type is wired only for types that do not serve requests (supervisor, cron),
so exposing one here would be a larger change than this feature warrants.
Two things are enough, and both are already there:

- **The log.** `NO_CERT` at startup, then one `leaving NO_CERT` line per
  gateway process. Count them: the pool is fully in READY when that count
  equals `http.gateways`.
- **The port itself.** A TCP connect to `http.listen` is refused in NO_CERT
  and accepted in READY. `ss -lnt` shows the port only once it is listening.

### Caveat: binding early does not reserve the port

The socket is bound before `listen()`, but that does **not** hold the port
against another process. Measured on 2026-09-11: a second process with
`SO_REUSEADDR` bound *and* listened on the same port while our socket sat
bound-not-listening, and our later `listen()` then failed with `EADDRINUSE`.
That failure is logged as an error naming the gateway that is not serving,
and only that gateway is affected. Do not run anything else on the TLS port.

### Not compatible with `http.tls_sni_cert`

A pool that sets `http.tls_wait_for_cert` may not also set
`http.tls_sni_cert`; the combination is refused at configuration time. SNI
certificates are loaded once, as part of the startup pair, and are not part
of the certificate-watch poll — a pool that started without them would go on
serving the primary certificate for every SNI name after the transition, and
silently, because the validation that would have complained was skipped too.
Closing that gap means making SNI reloadable, which is out of scope here.

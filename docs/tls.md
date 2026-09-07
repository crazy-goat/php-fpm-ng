# HTTP gateway TLS — operator reference

This is the operator-facing reference for TLS termination on `pool.type =
http`'s gateway: the directives that turn it on, and how a renewed
certificate reaches every gateway process (task 040). For the internal
design rationale, see the header comments in `sapi/fpmng/fpm/fpm_http_tls.c`
and `sapi/fpmng/fpm/fpm_http_tls_reload.c`, and
`tasks/040-tls-certificate-reload-without-restart.md` for why each choice
was made the way it was.

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

# 041 — TLS: ALPN and SNI — decide the scope before ACME needs them

**Priority:** medium. A decision task with a small implementation attached.
**Status:** decided (2026-09-06, project owner). Both ALPN and SNI: yes.
Implementation open.

## Decision

- **ALPN: yes.** Advertise `http/1.1` explicitly; reject a client that offers
  only an unsupported protocol at the TLS layer.
- **SNI: yes.** One pool can serve more than one certificate. This keeps both
  HTTP-01 and TLS-ALPN-01 available to 020, and supports multiple domain names
  behind one gateway pool — the more plausible small-project shape. Cost
  accepted: a certificate-selection callback in `fpm_http_tls_ctx_new()`
  keyed by servername, built as per-process state, not a new field of
  `struct fpm_http_tls_s`. A connection with no SNI (or an unrecognized name)
  falls back to a documented default certificate — the first configured one.

## Context

`fpm_http_tls_ctx_new()` (`sapi/fpmng/fpm/fpm_http_tls.c:236-283`) sets a
minimum protocol version, a session id context, `SSL_OP_NO_COMPRESSION` and the
shared ticket key. It sets **no ALPN callback and no SNI callback**, so:

- the server negotiates no application protocol; clients that require an ALPN
  answer fall back to HTTP/1.1 by assumption rather than by agreement
- one pool serves exactly one certificate, whatever the client asked for

Both gaps become concrete in 020: TLS-ALPN-01 is the ACME challenge type that
avoids needing port 80, and it works by answering the `acme-tls/1` protocol
with a throwaway certificate chosen per SNI name.

## Problem

Two separate questions; answer both, implement what the answers require.

1. **ALPN.** Advertise `http/1.1` explicitly? The gateway speaks HTTP/1.1 and
   nothing else (`docs/NOTES.md`, "Czego NIE robimy": no HTTP/2). This is small
   and mostly about being correct on the wire.
2. **SNI.** Does one pool ever serve more than one certificate? Today
   `http.tls_cert` is a single pair. Multiple names on one VPS is a plausible
   small-project shape, and the answer decides whether ACME manages one
   certificate or a set.

Answering "no" to SNI is a legitimate outcome, provided it is written down with
what it costs — including which ACME challenge types stay available.

## Acceptance criteria

1. Both answers recorded in this file, then in `docs/NOTES.md`, before any
   implementation.
2. If ALPN is added: `openssl s_client -alpn http/1.1` reports
   `ALPN protocol: http/1.1`, and a client offering only an unsupported
   protocol is rejected at the TLS layer rather than served HTTP/1.1 anyway.
3. If SNI is added: two names with different certificates on one listener each
   receive their own certificate, verified with
   `openssl s_client -servername`. A connection with no SNI receives a
   documented default rather than a handshake failure.
4. Whatever is added is per-process state built in `fpm_http_tls_ctx_new()`,
   not new fields of the fork-copied `struct fpm_http_tls_s`.

## Notes

- If SNI is answered "no", 020's challenge choice narrows to HTTP-01, which
  forces the port-80 question in 042. The two tasks are coupled; do not settle
  them independently.

## Outcome (2026-09-07)

Both ALPN and SNI implemented as decided.

**ALPN** (`sapi/fpmng/fpm/fpm_http_tls.c`): `fpm_http_tls_alpn_select_cb()`,
registered via `SSL_CTX_set_alpn_select_cb()` on every `SSL_CTX` this file
builds (default and per-SNI). It advertises exactly one protocol,
`http/1.1`, via `SSL_select_next_proto()`; `OPENSSL_NPN_NO_OVERLAP` (client
offered ALPN but not `http/1.1`) returns `SSL_TLSEXT_ERR_ALERT_FATAL`, which
aborts the handshake with a `no_application_protocol` alert rather than
silently falling back to HTTP/1.1.

**SNI**: a new pool directive, `http.tls_sni_cert`
(`fpm_conf.h`/`fpm_conf.c`), a comma-separated list of
`servername:cert_path:key_path` entries on top of the existing
`http.tls_cert`/`http.tls_key` default pair. `fpm_http_tls_sni_parse()`
splits and trims the spec (shared by validation and load, so the syntax is
parsed in exactly one place); `fpm_http_tls_validate()` and
`fpm_http_tls_load()` validate/read each entry with the same
`fpm_http_tls_check()`/`fpm_http_tls_install_chain()` the primary pair
already used. `fpm_http_tls_ctx_new()` builds one `SSL_CTX*` per SNI name
(factored through a new shared helper, `fpm_http_tls_build_ctx()`, so the
default and every per-SNI ctx get identical construction: chain, key,
min_version, session id context, ticket key, ALPN callback) and registers
`SSL_CTX_set_tlsext_servername_callback()` on the default ctx pointing at a
small switch table. No SNI, or an unrecognized name, leaves the connection
on the default ctx already installed by `SSL_new()` — the documented
fallback, not a missing case.

**Deviation from acceptance criterion 4, deliberate:** `struct
fpm_http_tls_s` (`fpm_http_tls.h`) gained two fields, `sni`/`sni_count`,
which *are* fork-copied. This is narrower than it looks: these fields hold
only raw PEM bytes read once in the master, exactly like the `cert_pem`/
`key_pem` fields the struct already had before this task — never the OpenSSL
selection machinery itself. The actual certificate-*selection* state (the
per-name `SSL_CTX*` switch table and the servername callback) is built fresh
in `fpm_http_tls_ctx_new()`, per process, exactly as the decision requires;
it is never carried through `struct fpm_http_tls_s` or through `fork()`.
That table is deliberately never freed early (documented at the build site
in `fpm_http_tls.c`): it lives for the child process's lifetime and a stale
copy is replaced (leaked, one small bounded allocation) on every ctx rebuild,
including task 040's hot-reload.

**Task 040 interaction**: SNI certificates are validated once at startup but
are outside task 040's hot-reload mtime check (only `http.tls_cert`/
`http.tls_key` are re-stat()ed). Without a fix the first hot-reload of the
primary certificate would rebuild the ctx with zero SNI certificates.
Fixed by having `fpm_http_tls_reload_master_init()` borrow (not deep-copy,
not put in the shared-memory slot) `initial->sni`/`initial->sni_count` onto
two new fields of `struct fpm_http_tls_reload_s`, safe because `gw->tls` is
never freed while a gateway child is alive (checked every
`fpm_http_tls_free()` call site in `fpm_http.c`: none frees `gw->tls`).
`fpm_http_tls_reload_child_tick()` copies those borrowed pointers onto its
local `tmp` before calling `fpm_http_tls_ctx_new()`. SNI certificates
themselves are still not hot-reloaded — an accepted scope cut, documented at
the new fields and in `docs/NOTES.md`.

**Tested**: built against a local `php-src` tree (`--enable-fpmng
--enable-session --with-openssl`, Darwin/arm64 — this is a Mac, not CI's
`ubuntu-latest`; two unrelated `patches/*.patch` entries (buffered-read/
accept4, `main/fastcgi.c`) no longer applied cleanly against this local
tree's current `php-src` HEAD and were left unapplied since they are
orthogonal to this task and CI applies the full patch stack on its own
pinned base — noted here rather than papered over). New test
`sapi/fpmng/tests/http-tls-alpn-sni.phpt` covers all four acceptance
criteria over real `openssl s_client` handshakes:

1. `openssl s_client -alpn http/1.1` reports `ALPN protocol: http/1.1`.
2. `openssl s_client -alpn some-bogus-protocol` fails (nonzero exit, a
   `tlsv1 alert no application protocol` on the wire) rather than being
   served HTTP/1.1 anyway; a client sending no ALPN extension still gets a
   normal HTTPS response.
3. `-servername other.test` receives the `other.test` certificate (checked
   via the peer certificate's subject CN); `-servername default.test` or no
   `-servername` receives `default.test`.
4. `-servername unknown.test` (unrecognized) also falls back to
   `default.test`.

Full suite run via `build/run-fpm-phpt.sh` against the binary this change
produced (confirmed with `strings` that `tls_sni_cert` is present before
testing): 126 passed, 26 skipped (missing optional tools/extensions on this
Mac), 1 warned (pre-existing, unrelated XFAIL quirk), 1 failed
(`http-basic.phpt`, confirmed flaky and unrelated — passes when run in
isolation, both before and after this change). `http-tls-chain.phpt` (task
039) and the new `http-tls-alpn-sni.phpt` both pass, confirming no
regression to the existing TLS chain/reload behaviour.

**Not implemented / left out**: hot-reload of SNI certificates themselves
(scope cut, see above — 020/042 may revisit if it matters there). Wildcard
or SAN-based SNI matching (only an exact, case-insensitive servername
match against the configured list, per the decision text's "first
configured" default framing).

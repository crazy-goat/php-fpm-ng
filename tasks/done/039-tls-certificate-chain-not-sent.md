# 039 — TLS: only the leaf certificate is sent, the chain is dropped

**Priority:** high. This breaks real clients today, and it blocks 020 (ACME):
Let's Encrypt issues a `fullchain.pem` whose intermediate would be silently
discarded.
**Status:** done (2026-09-06) — see Outcome below.

## Context

`fpm_http_tls_ctx_new()` parses the PEM bytes with a single
`PEM_read_bio_X509()` and installs the result with `SSL_CTX_use_certificate()`
(`sapi/fpmng/fpm/fpm_http_tls.c:248-257`). Both calls handle exactly one
certificate. A `fullchain.pem` holds the leaf followed by one or more
intermediates; everything after the first block is read into memory
(`fpm_http_tls_load()` slurps the whole file) and then never installed into the
`SSL_CTX`.

The validation path has the same shape (`fpm_http_tls_check()`,
`fpm_http_tls.c:82-135`), so a `fullchain.pem` passes validation and starts the
listener while serving an incomplete chain.

## Consequence

A client that does not already hold the intermediate cannot build a path to the
root. Browsers often paper over this with AIA fetching; `curl`, most language
HTTP clients, and mobile apps do not. The failure looks like "works in Chrome,
fails everywhere else", which is expensive to diagnose.

## Problem

Install the full certificate chain from the configured PEM file, and validate
the same way it will be served.

## Acceptance criteria

1. With a `fullchain.pem` (leaf + at least one intermediate) in `http.tls_cert`,
   `openssl s_client -connect <host>:<port> -showcerts` lists every certificate
   from the file, in file order.
2. `openssl s_client` with only the root in its trust store reports
   `Verify return code: 0 (ok)`. Today it reports a verification failure.
3. A single-certificate PEM keeps working unchanged — no new warning, no new
   error.
4. A PEM whose blocks are not a valid chain is either accepted and served as
   given, or refused at validation time with a message naming the problem.
   Whichever is chosen is stated in the outcome; silently serving something
   different from the file is not acceptable.
5. The chain is parsed once in the master, exactly like the leaf is today
   (`fpm_http_tls.h` documents that no gateway child re-reads the key file);
   the fix must not add a per-child file read.

## Notes

- Order matters to the acceptance criteria, so state whether the implementation
  preserves file order or reorders.
- Whatever holds the parsed chain is process state, not shared state — it must
  not end up in the fork-copied `struct fpm_http_tls_s` as a live OpenSSL
  object. That struct deliberately holds only bytes.

## Outcome — 2026-09-06

Implemented in `sapi/fpmng/fpm/fpm_http_tls.c` via a new
`fpm_http_tls_install_chain()`, shared by `fpm_http_tls_check()` (the
throwaway validation `SSL_CTX`) and `fpm_http_tls_ctx_new()` (the real
per-child one): the first PEM block is installed as the leaf via
`SSL_CTX_use_certificate()` exactly as before, and every block after it is
installed via `SSL_CTX_add_extra_chain_cert()`, in file order — satisfying
criterion 1 and the file-order note. It still parses only the in-memory
bytes `fpm_http_tls_load()` already read once in the master; no per-child
file read was added (criterion 5).

Criterion 4's decision: a PEM whose blocks are not actually a valid chain is
installed and served exactly as given — this only checks that each block
parses as an X.509 certificate, it does not build a path against a trust
store. A broken chain surfaces at the client, not at load time. Documented
in the function's comment, not only here.

Verified two ways, both against a locally generated 2-level test CA
(root → intermediate → leaf):
- Manually with `openssl s_client -showcerts`: a `fullchain.pem` (leaf +
  intermediate) lists both certs in file order, and `-CAfile root.crt`
  reports `Verify return code: 0 (ok)` (criteria 1-2); a single-cert PEM
  still serves exactly one certificate, matching pre-fix behaviour
  (criterion 3).
- With an automated regression test, `sapi/fpmng/tests/http-tls-chain.phpt`:
  builds the same 2-level CA at test time and checks over a real TLS
  handshake (`stream_socket_client` with `capture_peer_cert_chain`), not
  just by reading the PEM, that a `fullchain.pem` pool sends leaf +
  intermediate and verifies against the root, and that a leaf-only pool
  still sends exactly one certificate. Confirmed to fail against the
  pre-fix code (1 cert, handshake failure against the root) and pass
  against the fix. Lives under `sapi/fpmng/tests/`, already staged and run
  by `.github/workflows/build-matrix.yml` — no separate CI wiring needed.

Code: `92e2de0` (fix), `6c0c2d1` (regression test).

# 039 — TLS: only the leaf certificate is sent, the chain is dropped

**Priority:** high. This breaks real clients today, and it blocks 020 (ACME):
Let's Encrypt issues a `fullchain.pem` whose intermediate would be silently
discarded.
**Status:** open. Verified in the code, not measured against a client yet.

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

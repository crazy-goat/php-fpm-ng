# 040 — TLS: replace the certificate without restarting the gateway

**Priority:** high. Hard prerequisite for 020 (ACME renewal), and useful on its
own for a certificate that arrives on a mounted volume.
**Status:** open.

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

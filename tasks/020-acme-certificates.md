# 020 — ACME: obtain and renew TLS certificates (umbrella)

**Priority:** the umbrella carries no priority of its own; see the individual
tasks.
**Status:** open. The "build it or not" question is **answered: we build it**
(project owner, 2026-09-06). This file is now an index; the work is split
across 039–047.

## Context

The project's premise is one binary plus application code in a container image:
no nginx, no supervisord, no system cron, no shell. Certificates are the
remaining piece that normally requires another process.

TLS termination with a static certificate exists — `http.tls_cert`,
`http.tls_key`, `http.tls_min_version`, a session-ticket key shared across
gateway processes (`sapi/fpmng/fpm/fpm_http_tls.c`). ACME was scoped out at
that point on purpose. `docs/NOTES.md` section 5 still says "TLS at the end",
which predates that work.

## The split

**TLS gaps that ACME would otherwise inherit** — all of them stand on their own,
independent of whether ACME is ever finished:

- **039** — only the leaf certificate is sent; a `fullchain.pem` intermediate is
  silently dropped. Verified in the code. Do this first.
- **040** — replace a certificate without restarting the gateway. Renewal is
  worthless without it.
- **041** — ALPN and SNI: decide the scope. Coupled to 042 through the choice of
  challenge type.
- **042** — a plain HTTP listener alongside the TLS one, for the redirect and
  for HTTP-01. Answers `docs/NOTES.md` 3l's "is one pool, one port enough" —
  it is not.

**ACME proper:**

- **043** — decide where the client lives: C in the gateway, or PHP in a `cron`
  pool. Blocks the rest; the three checks to run are in `docs/NOTES.md` 3l.
- **044** — define the state layout on a writable volume, and its ownership.
  Shared with the self-runner; settle once.
- **045** — exactly one process renews, and the others pick the result up.
- **046** — serve the HTTP-01 challenge from the existing local-answer hook
  (`fpm_http.c:1303`), which was designed for this.
- **047** — issuance and renewal end to end, Let's Encrypt **staging** by
  default.

## Suggested order

039 → 043 (decision) → 044 → 040 → 042 → 046 → 045 → 047, with 041 settled
before 042 because it decides whether port 80 is required at all.

039 and 040 are worth doing regardless: a mounted certificate that renews
outside the binary needs both.

## The honest counter-argument, kept on the record

For "one small VPS, one container", a certificate mounted as a volume or a
secret and renewed by certbot alongside covers a large share of cases. ACME in
the binary is elegant, but it is a week of work and permanent maintenance
surface. If 043 concludes that neither option pays for itself, recording that
and documenting the mounted-certificate path is a complete outcome — but 039
and 040 still get fixed.

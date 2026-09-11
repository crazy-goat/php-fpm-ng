# Obtaining a certificate: the ACME client

This document covers issue #49: the RFC 8555 client that actually gets a
certificate, and the policy that decides when to run it. Serving the HTTP-01
challenge is [`docs/acme-challenge.md`](acme-challenge.md); making sure only
one process renews a certificate, and getting the result into every gateway,
is [`docs/acme-renewal.md`](acme-renewal.md).

The client is **PHP, not C.** It runs in a pool, as a script, the same way
any other scheduled work does. Nothing about RFC 8555 needs to be in the
master: it is HTTP and JSON on a timescale of seconds, it must not block the
event loop, and writing it in C would mean carrying a JOSE implementation and
an HTTP client in a process whose job is to supervise other processes. The
one thing the client genuinely cannot do from userland — publish a challenge
answer that *every* gateway process can serve — is already a C builtin from
issue #48 (`fpmng_acme_challenge_set()`).

## Where it runs

```ini
[acme]
pool.type = cron
cron.schedule = 17 3,15 * * *
cron.script = /usr/local/lib/fpmng/acme/renew.php

env[ACME_STATE_DIR] = /var/lib/fpmng/acme
env[ACME_DOMAINS]   = example.com,www.example.com
```

`pool.type = cron` is the intended home: it is pinned to one process, it
carries `publishes_acme_challenges`, and its schedule is the renewal
schedule. A `supervisor` pool works too and is what the test suite uses,
because `cron.schedule` has minute granularity and a test should not wait for
a tick.

Running the script anywhere else fails immediately and says why, rather than
half-way through an order:

```
this build cannot run the ACME client:
  - the fpmng_acme_challenge_* builtins: they exist only in a pool whose type
    may publish challenges (pool.type = cron or supervisor). Check which pool
    runs this script
```

`Client::preflight()` checks the whole list — the OpenSSL extension,
`openssl_csr_new()`, the `https://` stream wrapper, `allow_url_fopen`, and the
challenge builtins — and reports **every** missing piece at once, because
fixing one build flag only to hit the next one is how a half-hour job becomes
an afternoon.

## Configuration

All of it is `env[]`, read by `Renewer::fromEnvironment()`. There is no new C
directive namespace: a cron pool already has `env[]`, and a second place to
keep in sync is a second place to get out of sync.

| Variable | Default | Meaning |
|---|---|---|
| `ACME_STATE_DIR` | *(required)* | the state volume; see `docs/acme-renewal.md` |
| `ACME_DOMAINS` | *(required)* | comma-separated; the first is the subject, all become SANs |
| `ACME_DIRECTORY` | Let's Encrypt **staging** | the ACME directory URL |
| `ACME_ALLOW_PRODUCTION` | off | required before a production directory is accepted |
| `ACME_RENEW_DAYS` | `30` | renew with fewer than this many days left |
| `ACME_CA_BUNDLE` | system store | verify the CA's TLS certificate against this file |

### Staging is the default, and production is a decision

A misconfigured renewer pointed at Let's Encrypt production can burn a real
domain's rate limit for a week. The same mistake against staging produces a
certificate nobody trusts: loud, local, and fixed by editing one line. So the
default is staging and a production URL is refused until
`env[ACME_ALLOW_PRODUCTION] = 1` is set.

The check is against the **endpoint**, not against a flag someone named
"production" — the hazard is which server is asked, not what the
configuration calls it. Loopback, `*.localhost`, `*.internal` and any host
whose name contains `staging` or `test` are accepted without the opt-in,
because none of them can be a CA that rate-limits a real name.

## When it renews

Never on a calendar; always on **remaining lifetime**. A fixed "every 60
days" schedule reissues a perfectly good certificate on every restart, and
renews on the wrong day the moment a CA changes its validity period.
`Renewer::dueReason()` returns a reason, or `null`:

- no certificate is installed;
- the installed certificate cannot be parsed (a truncated or half-copied
  file is a reason to renew, not a reason to crash);
- it does not carry every configured name as a SAN — so adding a name to a
  pool gets a new certificate rather than a silently wrong one;
- fewer than `ACME_RENEW_DAYS` days of validity are left.

A tick reports one word: `up-to-date`, `renewed`, `backoff`, `held-elsewhere`
or `failed`. "The cron pool ran and did nothing" and "the cron pool is
broken" have to be distinguishable in a log.

Everything the renewer says goes out through `error_log()`, not `echo`. A
pool's stdout only reaches the FPM error log when `catch_workers_output` is
on, so a renewer that printed its account of the run would be silent by
default in most configurations — which is exactly the failure mode this is
supposed to prevent.

## When it fails

Nothing on disk changes. The installed certificate and key are untouched, so
the listener keeps serving whatever it was serving — a renewer that clears
the old certificate before it has a new one takes a working site down at
exactly the worst moment, when expiry is already close.

What does happen:

1. an `ERROR` line in the FPM error log naming the domain, the attempt
   number, the CA's own reason, and how long until the next attempt;
2. a `WARNING` with the expiry date, if the certificate is inside the renewal
   threshold — the thing that hurts is the expiry, not the attempt count, so
   this escalates on days left;
3. a record in `<domain>/renewal.json` (mode 0600) with the failure count and
   `next_attempt`.

The backoff is `0, 15m, 30m, 1h, 2h, 4h, 6h`, capped. It starts high enough
that a CA rejecting us is not asked again immediately, and tops out well
below a day so that an outage does not turn into a certificate that expires
while the renewer sleeps. Without it, a cron pool ticking every minute turns
a CA outage into a request flood that earns a rate limit *on top of* the
outage.

## What is in the box

| File | What it is |
|---|---|
| `sapi/fpmng/acme/jws.php` | JOSE: ES256 signing, the JWK, the RFC 7638 thumbprint, key authorizations |
| `sapi/fpmng/acme/http.php` | the HTTP client: `https://` stream wrapper, nonce tracking, problem documents |
| `sapi/fpmng/acme/client.php` | RFC 8555: directory, account, order, HTTP-01, finalize, certificate |
| `sapi/fpmng/acme/renew.php` | the policy, and the script a cron pool runs |
| `sapi/fpmng/acme/state.php` | the state volume (issue #47) |
| `sapi/fpmng/acme/lock.php` | one renewer per certificate (issue #47) |

### Deliberate limits

- **HTTP-01 only.** No DNS-01, so no wildcards. DNS-01 means a provider API
  per DNS host; that is a different feature with a different failure surface.
- **ES256 only.** RFC 8555 allows RSA accounts; one algorithm is one code
  path to get right, and P-256 is what every CA supports.
- **No alternate chains, no OCSP stapling, no account key rollover.** Each is
  a separate decision.
- **The certificate key is reused across renewals**, so a renewal changes
  exactly one file. `docs/acme-renewal.md` explains why that matters to the
  reload poll.

### Why the stream wrapper and not curl

The canonical CI build has no libcurl: `.github/docker/ci.Dockerfile:17`
installs `libevent-dev libssl-dev zlib1g-dev` and nothing else, and
`.github/workflows/build-matrix.yml:178` configures without `--with-curl`
(only `build/static-full.sh:39-45` enables it). A client that needed ext/curl
would be a client the project's own CI cannot test. `https://` over the
OpenSSL stream wrappers is always present in a build that can do TLS at all —
which an ACME client needs regardless.

The stream context sets `verify_peer` and `verify_peer_name` unconditionally;
`ACME_CA_BUNDLE` re-points `cafile` for a test CA but there is no way to turn
verification off. `follow_location` is 0, because a JWS signs the URL it is
sent to and following a redirect would present a signature for the wrong one.

## Testing it

A real CA cannot be in the test suite: it needs a network, it rate-limits,
and it cannot be asked to validate a name that resolves to a test box.

- `fpmng-acme-jose.phpt` — the JOSE layer offline. This is where an ACME
  client breaks in ways an end-to-end test misses: `openssl_sign()` returns a
  DER `SEQUENCE{r, s}` and JWS wants raw `r||s`, and
  `openssl_pkey_get_details()` returns EC coordinates as *trimmed* integers,
  so roughly one key in 256 has a coordinate shorter than 32 bytes. Both bugs
  are intermittent, which means they surface weeks later as "the CA rejects
  us sometimes". The test drives enough keys and signatures to make either
  one certain, and compares each JWK coordinate against OpenSSL's own
  fixed-width curve point rather than against a length.
- `fpmng-acme-issue.phpt` — a real order end to end against
  `sapi/fpmng/tests/acme-fake-ca.inc`, a separate CLI process. The fake is
  strict: it verifies every ES256 signature, spends each nonce once, checks
  the signed `url`, hands out a different token per authorization, refuses a
  CSR that does not ask for every ordered name, and **fetches
  `/.well-known/acme-challenge/<token>` over TCP from this build's own
  gateway** and compares it with the key authorization it derives from the
  client's JWK. A client that publishes nothing, or the wrong answer, fails
  there exactly as it would against Let's Encrypt.
- `fpmng-acme-renew-policy.phpt` — the policy, offline. Deliberately under
  the CLI, where the challenge builtins are absent: that is the environment
  the preflight has to describe correctly.
- `fpmng-acme-renew-failure.phpt` — an unreachable CA. Hashes the installed
  files before and after to prove the negative.

Issuance against a live [pebble](https://github.com/letsencrypt/pebble) or
against Let's Encrypt staging stays a **manual** step.

# ACME HTTP-01 challenge — operator and integrator reference

The gateway answers `GET /.well-known/acme-challenge/<token>` itself, from
shared memory, without occupying a worker and without a file on disk.

This is only the challenge half of ACME: obtaining and renewing a
certificate is [issue #49](https://github.com/crazy-goat/php-fpm-ng/issues/49),
and keeping a single renewer is
[issue #47](https://github.com/crazy-goat/php-fpm-ng/issues/47).

## What answers, and where

Both sockets of a `pool.type = http` pool answer the challenge namespace:

- `http.listen`, before static files and before the front controller;
- `http.plain_listen`, the redirect-only companion, *instead of* redirecting.
  The companion is the interesting one: HTTP-01 is plain HTTP by definition,
  and during a fresh bootstrap there is no certificate on `:443` to redirect
  a CA to.

Nothing in the namespace ever reaches a worker. An unknown token is a `404`
from the gateway, a method other than `GET`/`HEAD` is a `405`, and a token
containing a `/` is a `404` — a token is a flat opaque string and is never
turned into a path.

`http.static` has no effect on any of this: a key authorization is not a
file. A file that physically exists at the challenge path under the document
root is never served, whether or not the token is published.

## Publishing a token

A pool that runs a script and serves no request — `pool.type = cron` or
`pool.type = supervisor` — gets three builtins:

```php
fpmng_acme_challenge_set(string $token, string $key_authorization): bool
fpmng_acme_challenge_clear(string $token): bool
fpmng_acme_challenge_list(): array   // tokens only, never key authorizations
```

A published token is immediately visible to every gateway process of every
`http` pool in the same master, because the store is shared memory allocated
before the first fork. That is the point: with `http.gateways > 1` the
process that answers the CA's single request is not the process that
published the token, and the CA gives no retry guarantee.

Request-serving pool types deliberately do not get these builtins, so a
gateway can never execute the ACME client.

```ini
[web]
pool.type = http
http.listen = 0.0.0.0:443
http.plain_listen = 0.0.0.0:80
http.tls_cert = /state/acme/example.com/fullchain.pem
http.tls_key  = /state/acme/example.com/privkey.pem

[acme]
pool.type = cron
cron.schedule = 17 3 * * *
cron.script = /usr/share/php-fpm-ng/acme.php
env[ACME_STATE_DIR] = /state/acme
```

## Limits

- At most 8 tokens may be published at once; a token is at most 127 bytes and
  a key authorization at most 511. A value that does not fit is refused with
  an error in `error_log`, never truncated into an answer no CA would accept.
- A key authorization is never logged, at any level, and
  `fpmng_acme_challenge_list()` deliberately returns tokens only.
- `http.allowed_clients` is not bypassed. A gateway that restricts its
  clients also restricts the CA, which cannot be reached from a fixed address
  range — do not set it on a pool that has to pass HTTP-01.
- The store lives in the master's shared memory, so it does not survive a
  master restart. A renewal interrupted by a restart starts a new order; the
  challenge state was never durable in any implementation, and the CA
  tolerates an unprovisioned token by failing the authorization.

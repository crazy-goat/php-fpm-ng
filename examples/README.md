# Examples

Two tiers (task 035):

- [`combined/`](combined/) -- **Tier 1**: one config, one container, one
  `docker run`, all four pool types (`http`, `cron`, `supervisor`, `status`)
  running at the same time. Start here if you're deciding whether this
  project is worth adopting.
- [`http/`](http/), [`cron/`](cron/), [`supervisor/`](supervisor/),
  [`status/`](status/) -- **Tier 2**: one pool type each, minimal, each
  small enough to read in full. Start here once you know which pool type
  you want and want the smallest config to copy from.

Every example's own README says exactly what was run and observed to verify
it -- not just that the config parses.

## Build the binary once

All five examples run the same `php-fpm-ng` binary; build it once and copy
it next to whichever example's `Dockerfile` you're using. This is the same
recipe `.github/workflows/build-matrix.yml` runs in CI:

```sh
git init php-src && git -C php-src fetch --depth 1 \
  https://github.com/php/php-src.git php-8.5.9 && git -C php-src checkout FETCH_HEAD

./build/prepare.sh "$PWD/php-src"
cd php-src && ./buildconf --force
./configure --disable-all --enable-fpmng --enable-session --with-openssl
make -j"$(nproc)" fpmng

cp sapi/fpmng/php-fpm-ng ../examples/<example>/php-fpm-ng
```

`--with-openssl` builds `ext/openssl` into the CLI (used by our TLS tests,
not by `php-fpm-ng` itself); the gateway's own TLS support only needs
`libssl-dev` at configure time (`fpm_tls_http.c`). Kept here to match CI
exactly rather than re-deriving a minimal flag set.

## Why not `docker/`

`docker/fpm.conf` + `docker/Dockerfile.scratch` predate `pool.type` entirely
(no directive at all -- plain upstream-compatible FastCGI) and don't
demonstrate anything specific to this project; see task 035's Context
section. They still work as a minimal FastCGI-only container (now with the
port mismatch fixed) but are not where you should start looking for how to
configure `http`/`cron`/`supervisor`/`status`.

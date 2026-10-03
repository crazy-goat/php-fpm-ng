# Examples

Everything here runs on `ubuntu:26.04` and the distribution's PHP 8.5
(`libphp8.5-embed`): `php-fpm-ng` links the distribution's `libphp`, so an
image on an older base has no library to run it against. The released packages
contain no PHP and are amd64 only; on another architecture build the binary
yourself (`build/libphp-build.sh`, see [AGENTS.md](../AGENTS.md)).

Two tiers:

- [`combined/`](combined/) -- **Tier 1**: one config, one container, one
  `docker run`, a gateway, a FastCGI pool, cron and a supervisor running at the
  same time, with the operator endpoints on their own ports. Start here if
  you're deciding whether this project is worth adopting.
- [`http/`](http/), [`cron/`](cron/), [`supervisor/`](supervisor/),
  [`status/`](status/) -- **Tier 2**: one thing each, minimal, each small
  enough to read in full. Start here once you know which pool type you want
  and want the smallest config to copy from.
- [`http-direct-worker-mysql/`](http-direct-worker-mysql/) and
  [`http-direct-worker-react/`](http-direct-worker-react/) -- `pool.type =
  http-direct` with a long-lived worker script (the other
  `http-direct-worker*` directories are application code for them).

Every example's own README says exactly what was run and observed to verify
it -- not just that the config parses.

## How the images get the binary

The Tier 1 and Tier 2 images install the **released package**
(`php-fpm-ng_v<version>_php8.5_amd64.deb`, or `php-fpm-ng-tls_...` for
`http/`, which terminates TLS) from the GitHub release, checked against that
release's `SHA256SUMS`. `docker build` in the example directory is the whole
setup. The version and its checksum are two `ARG`s at the top of each
`Dockerfile`.

The `http-direct-worker-*` images take a `php-fpm-ng` next to the example
instead, because their test scripts measure the binary under test; each README
says how to build or fetch it.

## Checked in CI

`build/test-shipped-configs.sh` runs in CI: `static` greps `examples/` and
`docker/` for retired pool types, retired artefacts and pre-26.04 base images;
`images` builds the image of every shipped `*.conf` and runs `php-fpm-ng -t` on
it inside the image. The package-based images carry the released `.deb`, so
`-t` runs there twice: with the released binary, and with the binary the CI
run built mounted over it, so a PR that retires a directive fails here. A new example needs a `Dockerfile` (or the shared
`http-direct-worker-mysql/Dockerfile`) and a `*.conf`, and is picked up by the
second step on its own.

## `docker/`

`docker/fpm.conf` + `docker/Dockerfile` are the smallest container: a gateway
on 9001 in front of one FastCGI pool, serving a mounted `/www`. It is not
`FROM scratch`; a static image is #424 and the single-file path is #425.

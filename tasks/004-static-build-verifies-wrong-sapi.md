# 004 — `build/static-full.sh` verifies the wrong SAPI

**Priority:** high. Cheap to fix, and it invalidates a headline claim.
**Status:** open.

## Context

`README.md` (lines 21-23) advertises, as verified on 2026-09-05:

- a full static musl build with `-static-pie`, including opcache, mbstring,
  curl + OpenSSL, zlib, pdo_mysql, sockets, pcntl, posix
- running in a bare `FROM scratch` image, FPM as PID 1, HTTP 200, 20 MB total

`build/static-full.sh` configures:

    --disable-all --enable-fpm --with-fpm-http ...

and ends with:

    file /build/sapi/fpm/php-fpm
    cp /build/sapi/fpm/php-fpm /out/php-fpm-full

`--with-fpm-http` and `sapi/fpm/` are the **old php-src proof of concept**
(branch `fpm-http-poc` in the php-src fork). They are not `sapi/fpmng`, which is
what this repository actually builds today via `--enable-fpmng`.

The script predates the fpmng SAPI and was never updated. Consequence: **nobody
has ever verified that `sapi/fpmng` builds `-static-pie` into `FROM scratch`.**
The README's headline feature is currently unverified for the code we ship.

## Problem

Make the static build script build and verify the SAPI this repository actually
produces, and find out whether the claim still holds.

## Acceptance criteria

1. The script builds `sapi/fpmng` and the artefact it copies out is the
   `php-fpm-ng` binary.
2. `file` on the artefact confirms it is statically linked (the README already
   warns that plain `-static` silently yields a dynamic binary on Alpine's
   toolchain, which is why `-static-pie` is used — that check must stay).
3. The resulting binary runs in `FROM scratch` (`docker/Dockerfile.scratch`) and
   answers an HTTP request.
4. The outcome is recorded honestly in `README.md`, whichever way it goes:
   - if it works, update the "verified on" date and the image size
   - if it does **not** work, say so and file the blocker as its own task. A
     README claiming an unverified property is worse than one admitting a gap.
5. The extra libraries the current `sapi/fpmng` needs are accounted for. The
   HTTP gateway's optional TLS support needs libevent's OpenSSL glue; Alpine's
   `libevent-static` (2.1.13-r0) was confirmed on 2026-09-06 to contain
   `libevent_openssl.a`, so no new package should be required — but confirm
   rather than assume.

## Explicitly out of scope

- Publishing the image anywhere.
- Shrinking the image or trimming extensions.

## Notes

- Whether the old `--with-fpm-http` path should still be built at all is a real
  question. If the php-src POC branch is now purely historical, the script
  should stop building it; if it is still a comparison baseline, it should build
  **both** and say which is which. Decide and write the decision down.

# Tier 1 image. Expects php-fpm-ng already built next to this file -- see
# examples/README.md "Build the binary once" for the exact build/prepare.sh +
# configure + make invocation (same one CI runs in .github/workflows/build-matrix.yml).
#
# Not `FROM scratch` like docker/Dockerfile.scratch: that image needs a
# static musl binary, which task 004 (open) has not yet verified for
# sapi/fpmng. This image ships the dynamically-linked binary CI actually
# builds, so it needs the shared libs php-fpm-ng links against.
FROM ubuntu:24.04

RUN apt-get update && apt-get install -y --no-install-recommends \
      libevent-2.1-7t64 libevent-core-2.1-7t64 libevent-openssl-2.1-7t64 libssl3t64 zlib1g \
    && rm -rf /var/lib/apt/lists/* \
    && useradd -u 10001 -M -s /usr/sbin/nologin app

COPY php-fpm-ng /php-fpm-ng
COPY fpm-ng.conf /fpm-ng.conf
COPY www /www
RUN chown -R app:app /www

USER app
EXPOSE 8080 8081
ENTRYPOINT ["/php-fpm-ng", "-y", "/fpm-ng.conf", "-F"]

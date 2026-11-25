# A minimal container for the released php-fpm-ng package: one gateway on 9001
# in front of one FastCGI pool on 9000, serving /www. Mount the application:
#   docker build -t fpmng docker/
#   docker run --rm -p 9001:9001 -v "$PWD/app:/www:ro" fpmng
# This is not a `FROM scratch` image: the binary runs on the distribution's
# libphp, which the package pulls in (static image: #424; single file: #425).
FROM ubuntu:26.04
SHELL ["/bin/bash", "-o", "pipefail", "-c"]

# The released package, checked against the SHA256SUMS of that release. It has
# no PHP in it: apt pulls libphp8.5-embed (the distribution's PHP 8.5) and
# libevent in as dependencies, which is why this image is ubuntu:26.04 and not
# 24.04 (no libphp8.5-embed there). The release ships amd64 packages only. To
# move to a newer release change both ARGs; the checksum is in that release's
# SHA256SUMS.
ARG PHP_FPM_NG_VERSION=0.17.2
ARG PHP_FPM_NG_SHA256=b466d1c203666ab78b931a6dc1e94a704084da90740fcaa0bf4010bea1ed57b7
ENV DEBIAN_FRONTEND=noninteractive
# An example image; it follows the Ubuntu package repository, so versions are not pinned.
# On Ubuntu 26.04 the distribution's libphp8.5-embed has the engine security fix.
# That fix changes a struct layout that v0.17.0 was built against, so v0.17.0
# is expected to crash (issue #994). On 2026-11-06 the master of v0.17.2 started on the patched
# libphp 8.5.4-0ubuntu1.5 (measured on one host). Rebuilding with
# `docker build --pull --no-cache` installs the current libphp.
# See docs/install.md#engine-security-fixes.
# hadolint ignore=DL3008
RUN apt-get update \
    && apt-get install -y --no-install-recommends ca-certificates curl \
    && curl -fsSL -o /tmp/php-fpm-ng.deb \
        "https://github.com/crazy-goat/php-fpm-ng/releases/download/v${PHP_FPM_NG_VERSION}/php-fpm-ng_v${PHP_FPM_NG_VERSION}_php8.5_amd64.deb" \
    && echo "${PHP_FPM_NG_SHA256}  /tmp/php-fpm-ng.deb" | sha256sum -c - \
    && apt-get install -y --no-install-recommends /tmp/php-fpm-ng.deb \
    && rm -rf /tmp/php-fpm-ng.deb /var/lib/apt/lists/*

# The pool config runs the workers as 65534, so the master stays root (no USER).
RUN mkdir /www
COPY fpm.conf /fpm.conf
EXPOSE 9001
ENTRYPOINT ["/usr/sbin/php-fpm-ng", "-y", "/fpm.conf", "-F"]

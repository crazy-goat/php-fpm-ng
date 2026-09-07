# Expects, next to this file: php-fpm-ng (see examples/README.md "Build the
# binary once") and certs/ (run ./generate-cert.sh first).
FROM ubuntu:24.04

RUN apt-get update && apt-get install -y --no-install-recommends \
      libevent-2.1-7t64 libevent-core-2.1-7t64 libevent-openssl-2.1-7t64 libssl3t64 zlib1g \
    && rm -rf /var/lib/apt/lists/* \
    && useradd -u 10001 -M -s /usr/sbin/nologin app

COPY php-fpm-ng /php-fpm-ng
COPY fpm-ng.conf /fpm-ng.conf
COPY www /www
COPY certs /certs
RUN chown -R app:app /www && chown -R app:app /certs

USER app
EXPOSE 8443
ENTRYPOINT ["/php-fpm-ng", "-y", "/fpm-ng.conf", "-F"]

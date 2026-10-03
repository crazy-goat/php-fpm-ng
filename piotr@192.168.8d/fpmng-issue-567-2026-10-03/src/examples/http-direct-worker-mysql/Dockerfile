# Build context is examples/, not this directory, so the image can pick up the
# Revolt driver from examples/http-direct-worker/ instead of carrying a second
# copy of it (see app.php).
#
# APP_DIR is the only thing that differs between applications: everything is
# copied to a fixed layout inside the image (/app/app plus
# /app/http-direct-worker), so task 075 can reuse this Dockerfile for a
# ReactPHP application by passing APP_DIR alone. That is also why the
# entrypoint can be exec-form with a literal path — an ARG would not expand
# there, and shell form would put a shell between FPM and SIGQUIT.
ARG APP_DIR=http-direct-worker-mysql

# Composer runs in its own image on purpose: the runtime image carries the
# distribution's libphp (no phar, no composer), and the build stays reproducible
# without a PHP on the host.
FROM composer:2 AS deps
ARG APP_DIR
WORKDIR /build
COPY ${APP_DIR}/composer.json ${APP_DIR}/composer.lock ./
RUN composer install --no-dev --no-interaction --no-progress --optimize-autoloader

# ubuntu:26.04, not debian: php-fpm-ng links the distribution's libphp (#419),
# and Debian bookworm has no PHP 8.5 libphp. The binary has to be built on this
# same distribution release (see the README), because libphp8.5-embed is the
# very library it was linked against.
FROM ubuntu:26.04
ARG APP_DIR
ENV DEBIAN_FRONTEND=noninteractive
# libphp8.5-embed brings the PHP it runs on (openssl, filter and ctype
# included, which amphp needs); libevent is the one library build/libphp-build.sh
# links besides libphp (objdump -p: NEEDED libevent-2.1.so.7).
# An example image; it follows the Ubuntu package repository, so versions are not pinned.
# hadolint ignore=DL3008
RUN apt-get update \
	&& apt-get install -y --no-install-recommends libphp8.5-embed libevent-2.1-7t64 ca-certificates \
	&& rm -rf /var/lib/apt/lists/*
COPY ${APP_DIR}/php-fpm-ng /usr/local/bin/php-fpm-ng
# *.php rather than app.php: task 075's application ships its own classes
# (an event loop and a request-queue bridge) next to app.php, and the
# APP_DIR promise above was worth exactly as much as the file list under it.
COPY ${APP_DIR}/*.php ${APP_DIR}/fpm.conf /app/app/
COPY http-direct-worker/FpmngDriver.php http-direct-worker/FpmngServer.php /app/http-direct-worker/
COPY --from=deps /build/vendor /app/app/vendor
EXPOSE 8080
ENTRYPOINT ["/usr/local/bin/php-fpm-ng", "-y", "/app/app/fpm.conf", "-F"]

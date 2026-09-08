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

# Composer runs in its own image on purpose. Our own binary is built with
# --disable-all and therefore has no phar, which is what made task 073 build
# vendor/ on a second machine and copy it in ("PHP's phar extension is
# missing", measured on the test box).
FROM composer:2 AS deps
ARG APP_DIR
WORKDIR /build
COPY ${APP_DIR}/composer.json ${APP_DIR}/composer.lock ./
RUN composer install --no-dev --no-interaction --no-progress --optimize-autoloader

FROM debian:bookworm-slim
ARG APP_DIR
# A static-pie php-fpm-ng (build/static-full.sh) needs none of these; a
# dynamically linked one needs all of them, and the image should work for
# whichever binary the operator drops in.
RUN apt-get update \
	&& apt-get install -y --no-install-recommends libevent-2.1-7 libssl3 ca-certificates \
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

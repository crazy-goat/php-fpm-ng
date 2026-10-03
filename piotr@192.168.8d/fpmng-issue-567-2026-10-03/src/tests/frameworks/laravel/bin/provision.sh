#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
RUN_DIR=${RUN_DIR:-$ROOT/.run}
PHP=${PHP:-php}
COMPOSER_VERSION=${COMPOSER_VERSION:-2.10.3}
COMPOSER_SHA256=${COMPOSER_SHA256:-7a2d379d5b8ffdaa028580ef26494c36d2feef4b178d3dd1473a4dbc5e17c8d6}
COMPOSER_PHAR="$RUN_DIR/composer-$COMPOSER_VERSION.phar"

if ! command -v curl >/dev/null 2>&1; then
    echo "curl is required to provision Composer $COMPOSER_VERSION" >&2
    exit 2
fi

mkdir -p "$RUN_DIR"

if command -v composer >/dev/null 2>&1; then
    composer install --no-dev --no-interaction --prefer-dist --no-progress
    exit 0
fi

if [ ! -f "$COMPOSER_PHAR" ]; then
    curl --fail --silent --show-error --location \
        "https://getcomposer.org/download/$COMPOSER_VERSION/composer.phar" \
        --output "$COMPOSER_PHAR"
fi

actual_sha256=$(sha256sum "$COMPOSER_PHAR" | awk '{print $1}')
if [ "$actual_sha256" != "$COMPOSER_SHA256" ]; then
    echo "Composer $COMPOSER_VERSION checksum mismatch: $actual_sha256" >&2
    exit 2
fi

COMPOSER_ALLOW_SUPERUSER=1 "$PHP" "$COMPOSER_PHAR" install \
    --no-dev --no-interaction --prefer-dist --no-progress

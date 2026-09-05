#!/bin/sh
# Sklada sapi/fpmng/ w drzewie php-src: najpierw stabilne zrodla FPM z upstreamu,
# potem nasze pliki na wierzch. Repo trzyma tylko to, co nasze.
#
#   build/prepare.sh /sciezka/do/php-src
#
set -e
PHPSRC="${1:?podaj sciezke do php-src}"
REPO="$(cd "$(dirname "$0")/.." && pwd)"

[ -d "$PHPSRC/sapi/fpm" ] || { echo "to nie wyglada na php-src: $PHPSRC" >&2; exit 1; }

rm -rf "$PHPSRC/sapi/fpmng"
cp -r "$PHPSRC/sapi/fpm" "$PHPSRC/sapi/fpmng"

# Testy FPM odwoluja sie do binarki php-fpm, nie naszej. Wroca, gdy beda wlasne.
rm -rf "$PHPSRC/sapi/fpmng/tests"

# Nasze pliki nadpisuja upstream.
cp -r "$REPO/sapi/fpmng/." "$PHPSRC/sapi/fpmng/"

echo "sapi/fpmng gotowe. Nasze pliki:"
(cd "$REPO/sapi/fpmng" && find . -type f | sed 's|^\./|  |')

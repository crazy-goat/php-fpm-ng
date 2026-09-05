#!/bin/sh
# Sklada sapi/fpmng/ w drzewie php-src: najpierw stabilne zrodla FPM z upstreamu,
# potem nasze pliki na wierzch. Repo trzyma tylko to, co nasze.
#
#   build/prepare.sh /sciezka/do/php-src
#
# Upstream nie jest modyfikowany — powstaje wylacznie nowy katalog sapi/fpmng/.
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

# Lista zrodel bierze sie z config.m4 TEGO php-src, a nie z naszej kopii —
# inaczej dryfuje przy kazdej zmianie w upstreamie (np. usunieciu events/devpoll.c).
SOURCES=$(sed -n '/PHP_FPM_FILES="/,/^[[:space:]]*"[[:space:]]*$/p' "$PHPSRC/sapi/fpm/config.m4" \
  | grep -oE 'fpm/[A-Za-z0-9_/]+\.c' \
  | sort -u)
[ -n "$SOURCES" ] || { echo "nie udalo sie odczytac listy zrodel z sapi/fpm/config.m4" >&2; exit 1; }

# Nasze wlasne pliki .c dochodza do listy, jesli upstream ich nie ma.
for f in $(cd "$REPO/sapi/fpmng" && find fpm -name '*.c' | sort); do
  echo "$SOURCES" | grep -qx "$f" || SOURCES="$SOURCES
$f"
done

LIST=$(echo "$SOURCES" | sed 's/$/ \\/' | sed 's/^/    /')
awk -v list="$LIST" '{ gsub(/@FPMNG_SOURCES@/, "\n" list "\n  "); print }' \
  "$PHPSRC/sapi/fpmng/config.m4" > "$PHPSRC/sapi/fpmng/config.m4.tmp"
mv "$PHPSRC/sapi/fpmng/config.m4.tmp" "$PHPSRC/sapi/fpmng/config.m4"

echo "sapi/fpmng gotowe."
echo "  zrodel z upstreamu + naszych: $(echo "$SOURCES" | wc -l | tr -d ' ')"
echo "  nasze pliki:"
(cd "$REPO/sapi/fpmng" && find . -type f | sed 's|^\./|    |' | sort)

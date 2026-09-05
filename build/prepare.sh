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

# Latki na pliki poza sapi/ — odstepstwo od "upstream nietkniety", wiec glosno.
# Zasady i terminy waznosci: patches/README.md
PHPVER=$(awk -F'"' '/PHP_VERSION /{print $2}' "$PHPSRC/main/php_version.h" 2>/dev/null)
PHPMINOR=$(echo "$PHPVER" | cut -d. -f1,2)
PATCHED=0
for p in "$REPO"/patches/*.patch; do
  [ -f "$p" ] || continue
  name=$(basename "$p")
  # wariant wersyjny nadpisuje ogolny
  [ -f "$REPO/patches/php-$PHPMINOR/$name" ] && p="$REPO/patches/php-$PHPMINOR/$name"
  if patch -d "$PHPSRC" -p1 -R --dry-run --silent < "$p" >/dev/null 2>&1; then
    # odwrotne nalozenie przechodzi => latka juz siedzi w drzewie
    echo "  ! latka juz nalozona na upstream: $name"
    PATCHED=$((PATCHED + 1))
  elif patch -d "$PHPSRC" -p1 --forward --silent < "$p"; then
    echo "  ! latka nalozona na upstream: $name"
    PATCHED=$((PATCHED + 1))
  else
    echo "BLAD: latka nie naklada sie na PHP $PHPVER: $name" >&2
    echo "      patrz patches/README.md — albo upstream ja zmergowal (usun ja)," >&2
    echo "      albo potrzebny jest wariant patches/php-$PHPMINOR/$name" >&2
    exit 1
  fi
done

echo "sapi/fpmng gotowe."
if [ "$PATCHED" -gt 0 ]; then
  echo "  UWAGA: upstream zostal zmodyfikowany przez $PATCHED latke/i (patrz wyzej)"
else
  echo "  upstream nietkniety — powstal wylacznie sapi/fpmng/"
fi
echo "  zrodel z upstreamu + naszych: $(echo "$SOURCES" | wc -l | tr -d ' ')"
echo "  nasze pliki:"
(cd "$REPO/sapi/fpmng" && find . -type f | sed 's|^\./|    |' | sort)

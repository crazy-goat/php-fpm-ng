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

# MUSI byc PRZED odtworzeniem katalogu: nizej sapi/fpmng jest kasowane i robione
# na nowo z upstreamowego sapi/fpm, w ktorym nie ma blokow PHP_FPMNG_*_FILES.
# Sluzy do wykrycia, czy doszedl albo znikl plik .c — patrz ostrzezenie na koncu.
# Lista jest podzielona na trzy bloki (bazowy, fiber, async) — patrz nizej —
# ale do wykrycia zmiany bierzemy je razem, bo interesuje nas caly zestaw
# plikow niezaleznie od tego, do ktorego bloku trafily.
OLD_SOURCES=""
if [ -f "$PHPSRC/sapi/fpmng/config.m4" ]; then
  # Tylko bloki PHP_FPMNG_*_FILES="..." — w config.m4 sa tez inne wzmianki
  # o plikach (fpm_systemd.c, fpm_trace.c, www.c pod warunkami), ktore nie
  # naleza do zadnej z tych list.
  OLD_SOURCES=$( { \
      sed -n '/PHP_FPMNG_FILES="/,/^[[:space:]]*"[[:space:]]*$/p' \
        "$PHPSRC/sapi/fpmng/config.m4"; \
      sed -n '/PHP_FPMNG_FIBER_FILES="/,/^[[:space:]]*"[[:space:]]*$/p' \
        "$PHPSRC/sapi/fpmng/config.m4"; \
      sed -n '/PHP_FPMNG_ASYNC_FILES="/,/^[[:space:]]*"[[:space:]]*$/p' \
        "$PHPSRC/sapi/fpmng/config.m4"; \
    } | grep -oE 'fpm/[A-Za-z0-9_/]+\.c' | sort -u)
fi

rm -rf "$PHPSRC/sapi/fpmng"
cp -r "$PHPSRC/sapi/fpm" "$PHPSRC/sapi/fpmng"

# Keep the upstream tests in the copied SAPI. The runner supplies the binary path
# through TEST_PHP_FPM_EXECUTABLE, so the tests do not need to be forked here.

# Our files override upstream.
cp -r "$REPO/sapi/fpmng/." "$PHPSRC/sapi/fpmng/"

# Rozszerzenie fpmng_metrics (NOTES 3k): ext/ wykrywany tym samym globem
# co sapi/, wiec tez zero latek na upstream. Katalog naszego repo:
[ -d "$REPO/ext/fpmng_metrics" ] && {
  rm -rf "$PHPSRC/ext/fpmng_metrics"
  cp -r "$REPO/ext/fpmng_metrics" "$PHPSRC/ext/fpmng_metrics"
}

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

# Podzial na trzy grupy: fiber (fiber + cala warstwa coop, uzywana wylacznie
# przez fiber) i async (fpm_pool_async.c) trafiaja pod --enable-fpmng-fiber /
# --enable-fpmng-async (obie domyslnie "no"); reszta jest budowana zawsze.
# Podzial zweryfikowany po referencjach symboli — coop.* nie jest uzywane
# poza fiber, async nie odwoluje sie do coop.
FIBER_PATTERN='^fpm/fpm_pool_fiber(_xport|_flock|_sleep)?\.c$|^fpm/fpm_pool_coop(_ini|_reval|_session|_session_patch|_statics)?\.c$'
ASYNC_PATTERN='^fpm/fpm_pool_async\.c$'

BASE_SOURCES=$(echo "$SOURCES" | grep -Ev "$FIBER_PATTERN" | grep -Ev "$ASYNC_PATTERN")
FIBER_SOURCES=$(echo "$SOURCES" | grep -E "$FIBER_PATTERN")
ASYNC_SOURCES=$(echo "$SOURCES" | grep -E "$ASYNC_PATTERN")

[ -n "$FIBER_SOURCES" ] || { echo "nie znaleziono plikow fiber/coop w liscie zrodel" >&2; exit 1; }
[ -n "$ASYNC_SOURCES" ] || { echo "nie znaleziono fpm_pool_async.c w liscie zrodel" >&2; exit 1; }

BASE_LIST=$(echo "$BASE_SOURCES" | sed 's/$/ \\/' | sed 's/^/    /')
FIBER_LIST=$(echo "$FIBER_SOURCES" | sed 's/$/ \\/' | sed 's/^/    /')
ASYNC_LIST=$(echo "$ASYNC_SOURCES" | sed 's/$/ \\/' | sed 's/^/    /')

awk -v base="$BASE_LIST" -v fiber="$FIBER_LIST" -v async="$ASYNC_LIST" '{
    gsub(/@FPMNG_SOURCES@/, "\n" base "\n  ");
    gsub(/@FPMNG_FIBER_SOURCES@/, "\n" fiber "\n  ");
    gsub(/@FPMNG_ASYNC_SOURCES@/, "\n" async "\n  ");
    print
  }' "$PHPSRC/sapi/fpmng/config.m4" > "$PHPSRC/sapi/fpmng/config.m4.tmp"
mv "$PHPSRC/sapi/fpmng/config.m4.tmp" "$PHPSRC/sapi/fpmng/config.m4"

# Czy lista zrodel sie zmienila wzgledem poprzedniego przebiegu? Jesli tak, to
# istniejacy katalog budowania ma ZAMROZONA liste obiektow w Makefile i nie
# zobaczy nowego pliku. Objawia sie to bledem linkowania PO przekompilowaniu
# wszystkiego — albo, gdy nowy plik nie eksportuje uzywanych symboli, wcale:
# build przechodzi i wychodzi binarka po cichu pozbawiona nowego typu poola.
# Dlatego ostrzegamy glosno i na koncu, zeby nie zjechalo z ekranu.
# Uwaga: ten skrypt to /bin/sh, wiec zadnych <(...) — porownanie idzie przez
# pliki tymczasowe.
SOURCES_CHANGED=""
NEW_SORTED=$(echo "$SOURCES" | sort -u)
if [ -n "$OLD_SOURCES" ] && [ "$OLD_SOURCES" != "$NEW_SORTED" ]; then
  _old=$(mktemp) && _new=$(mktemp)
  printf '%s\n' "$OLD_SOURCES" > "$_old"
  printf '%s\n' "$NEW_SORTED" > "$_new"
  SOURCES_CHANGED=$(diff "$_old" "$_new" | grep '^[<>]' || true)
  rm -f "$_old" "$_new"
fi

# Latki na pliki poza sapi/ — odstepstwo od "upstream nietkniety", wiec glosno.
# Zasady i terminy waznosci: patches/README.md
PHPVER=$(awk -F'"' '/PHP_VERSION /{print $2}' "$PHPSRC/main/php_version.h" 2>/dev/null)
PHPMINOR=$(echo "$PHPVER" | cut -d. -f1,2)
# Latki tworza stos i potrafia dotykac tego samego regionu (0002 i 0003 obie
# siedza przy accept()). Wtedy test "czy juz nalozona" per latka klamie: odwrotny
# dry-run 0002 pada, bo na niej lezy 0003. Decyzja zapada wiec raz, dla calego
# stosu: albo drzewo jest nietkniete i nakladamy wszystko po kolei, albo caly
# stos schodzi odwrotnie z kopii dotknietych plikow (= juz nalozony), albo BLAD.
PATCHES=""
for p in "$REPO"/patches/*.patch; do
  [ -f "$p" ] || continue
  name=$(basename "$p")
  # wariant wersyjny nadpisuje ogolny
  [ -f "$REPO/patches/php-$PHPMINOR/$name" ] && p="$REPO/patches/php-$PHPMINOR/$name"
  PATCHES="$PATCHES $p"
done
PATCHED=0
if [ -n "$PATCHES" ]; then
  FIRST=${PATCHES%% *}; FIRST=${PATCHES# }; FIRST=${FIRST%% *}
  # Kolejnosc ma znaczenie: NAJPIERW proba w przod. Odwrotne nalozenie na
  # nietknietym drzewie tez potrafi zwrocic sukces (BSD patch), wiec test
  # "-R" jako pierwszy dawalby cicho binarke bez latki z komunikatem, ze jest.
  if patch -d "$PHPSRC" -p1 --dry-run --forward --silent < "$FIRST" >/dev/null 2>&1; then
    for p in $PATCHES; do
      name=$(basename "$p")
      if patch -d "$PHPSRC" -p1 --forward --silent < "$p" >/dev/null 2>&1; then
        echo "  ! latka nalozona na upstream: $name"
        PATCHED=$((PATCHED + 1))
      else
        echo "BLAD: latka nie naklada sie na PHP $PHPVER: $name" >&2
        echo "      patrz patches/README.md — albo upstream ja zmergowal (usun ja)," >&2
        echo "      albo potrzebny jest wariant patches/php-$PHPMINOR/$name" >&2
        exit 1
      fi
    done
  else
    TMP=$(mktemp -d)
    # nazwa pliku konczy sie na pierwszym bialym znaku (diff -u dokleja tam date)
    for f in $(cat $PATCHES | sed -n 's|^+++ b/\([^[:space:]]*\).*|\1|p' | sort -u); do
      mkdir -p "$TMP/$(dirname "$f")"
      cp "$PHPSRC/$f" "$TMP/$f"
    done
    REVERSED=""
    for p in $PATCHES; do REVERSED="$p $REVERSED"; done
    OK=1
    for p in $REVERSED; do
      patch -d "$TMP" -p1 -R --forward --silent < "$p" >/dev/null 2>&1 || { OK=0; break; }
    done
    rm -rf "$TMP"
    if [ "$OK" = 1 ]; then
      for p in $PATCHES; do
        echo "  ! latka juz byla nalozona: $(basename "$p")"
        PATCHED=$((PATCHED + 1))
      done
    else
      echo "BLAD: latki nie nakladaja sie na PHP $PHPVER i drzewo nie wyglada na nietkniete" >&2
      echo "      ($(echo $PATCHES | wc -w | tr -d ' ') latek, pierwsza: $(basename "$FIRST"))" >&2
      echo "      patrz patches/README.md — albo upstream ktoras zmergowal (usun ja)," >&2
      echo "      albo potrzebny jest wariant patches/php-$PHPMINOR/<nazwa>, albo drzewo" >&2
      echo "      ma cudze zmiany w tych plikach (git status w $PHPSRC)" >&2
      exit 1
    fi
  fi
fi

echo "sapi/fpmng gotowe."
if [ "$PATCHED" -gt 0 ]; then
  echo "  UWAGA: upstream zostal zmodyfikowany przez $PATCHED latke/i (patrz wyzej)"
else
  echo "  upstream nietkniety — powstal wylacznie sapi/fpmng/"
fi
echo "  zrodel z upstreamu + naszych: $(echo "$SOURCES" | wc -l | tr -d ' ')"
echo "  nasze pliki:"
(cd "$REPO/sapi/fpmng" && find . -type f | sed 's|^\./|    |' | sort)
if [ -d "$REPO/ext/fpmng_metrics" ]; then
  (cd "$REPO/ext/fpmng_metrics" && find . -type f | sed 's|^|    ext/fpmng_metrics/|' | sort)
fi

if [ -n "$SOURCES_CHANGED" ]; then
  echo
  echo "================================================================"
  echo "UWAGA: zmienila sie lista plikow zrodlowych sapi/fpmng:"
  echo "$SOURCES_CHANGED" | sed 's/^/    /'
  echo
  echo "Istniejacy katalog budowania ma zamrozona liste obiektow i tego"
  echo "NIE zobaczy. Zanim zbudujesz, wykonaj:"
  echo
  echo "    cd $PHPSRC && ./buildconf --force"
  echo "    cd <katalog-budowania> && ./config.nice && make"
  echo
  echo "Pominiecie tego konczy sie bledem linkowania — albo, co gorsza,"
  echo "binarka bez nowego kodu, ktora buduje sie bez slowa skargi."
  echo "================================================================"
fi

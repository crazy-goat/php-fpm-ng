#!/bin/sh
# fpm-ng: embed the distribution payload into a finished binary (issue #171).
#
# Every path that produces a php-fpm-ng anyone runs calls this: libphp-build.sh
# (the packages), static-full.sh (the musl binary) and the CI build jobs (the
# binary the .phpt suite tests). A payload that only some builds carry would
# make `cron.script = fpmng-dist://...` work on a developer's machine and fail
# in a container, which is the class of bug the whole mechanism exists to end.
#
# Usage: embed-payload.sh <binary> <php-interpreter>
#   FPMNG_ACME  1 when the binary was built with --enable-fpmng-acme; default 0
#
# WHAT THE PAYLOAD IS, AND WHY THE FLAG GATES IT. Everything the payload has
# ever carried is the ACME client (sapi/fpmng/acme/*.php), and ACME is opt-in
# since issue #281. Embedding it into a binary built without
# --enable-fpmng-acme would put the one facility that talks to a certificate
# authority inside the default build as data, after the C half was kept out of
# it -- so this script embeds nothing there, and fpm_payload_dist_validate()
# refuses `fpmng-dist://acme/...` in that build by name.
#
# Idempotent, which is not a nicety: CI reuses build trees, so a binary that
# `make` did not relink is still there from the previous run, and appending
# unconditionally would add one archive per run until the binary is mostly
# payload. If the newest distribution entry already has the digest the packer
# would produce, this exits without writing.
#
# strip(1) drops appended data -- it is not an ELF section -- so nothing may
# strip after this runs.
set -eu

BIN=${1:?usage: embed-payload.sh <binary> <php-interpreter>}
PHP=${2:?usage: embed-payload.sh <binary> <php-interpreter>}
REPO=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
DIR=$REPO/sapi/fpmng/acme
PREFIX=acme

fail() { echo "embed-payload.sh: FAIL: $*" >&2; exit 1; }

[ -x "$BIN" ] || fail "$BIN is not an executable file"
[ -x "$PHP" ] || fail "$PHP is not a PHP interpreter"
[ -d "$DIR" ] || fail "$DIR does not exist"

FPMNG_ACME=${FPMNG_ACME:-0}
case "$FPMNG_ACME" in
  0|1) ;;
  *) fail "FPMNG_ACME must be 0 or 1, not '$FPMNG_ACME'" ;;
esac
if [ "$FPMNG_ACME" = 0 ]; then
  echo "embed-payload.sh: FPMNG_ACME=0, embedding nothing (the payload is the ACME client; issue #281)"
  exit 0
fi

want=$("$PHP" "$REPO/build/payload-pack.php" digest --dir="$DIR" --prefix="$PREFIX") ||
  fail "the payload digest could not be computed"
# The newest kind=1 entry is the one fpm_payload_find() would use.
have=$("$PHP" "$REPO/build/payload-pack.php" list --binary="$BIN" |
  awk '$1 == "kind=1" { sub(/^sha256=/, "", $4); print $4; exit }')

if [ "$have" = "$want" ]; then
  echo "embed-payload.sh: already embedded (sha256=$want)"
  exit 0
fi

"$PHP" "$REPO/build/payload-pack.php" append --binary="$BIN" \
  --kind=distribution --dir="$DIR" --prefix="$PREFIX" ||
  fail "the distribution payload could not be embedded"

# Read it back with the same walker the C side uses. The append can fail
# silently in exactly one way -- a short write -- and the failure would then
# show up as a PHP parse error at the first ACME order.
got=$("$PHP" "$REPO/build/payload-pack.php" list --binary="$BIN" |
  awk '$1 == "kind=1" { sub(/^sha256=/, "", $4); print $4; exit }')
[ "$got" = "$want" ] ||
  fail "the binary carries no distribution payload with the expected digest after embedding"
echo "embed-payload.sh: embedded sha256=$want into $BIN"

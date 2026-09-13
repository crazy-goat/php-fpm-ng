#!/bin/sh
# Issue #282: --enable-fpmng-http2 and --enable-fpmng-quic are RESERVED NAMES.
# Neither protocol exists in this tree, and configure refuses both rather than
# accepting a flag that switches nothing on -- a binary the operator believes
# speaks HTTP/2 is worse than no flag at all.
#
# This script is the assertion that keeps that true. It exists so that whoever
# implements HTTP/2 or QUIC has to DELETE A FAILING TEST, which is a decision,
# instead of being able to leave the refusal in place and wonder later why
# their flag does nothing.
#
# Usage: build/test-reserved-configure-flags.sh <prepared-php-src>
#   prepared-php-src  a tree with our overlay applied (build/prepare.sh) and
#                     ./configure already generated (./buildconf)
#
# Each flag gets its own configure run in its own scratch directory: the two
# refusals are separate messages about separate decisions, and one run asking
# for both would only ever prove the first of them. The runs are out-of-tree,
# so nothing here disturbs a build the caller may already have in <src>.
set -eu

fail() { echo "test-reserved-configure-flags.sh: FAIL: $*" >&2; exit 1; }

SRC=${1:?usage: build/test-reserved-configure-flags.sh <prepared-php-src>}
SRC=$(cd "$SRC" && pwd) || fail "no prepared php-src tree at $1"
[ -x "$SRC/configure" ] || fail "$SRC has no ./configure: run ./buildconf there first"

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

# What every run below starts from. --disable-all keeps the run short: the
# refusals live in sapi/fpmng/config.m4 and nothing after it has to be reached.
BASE="--disable-all --enable-fpmng"

# refuses <flag> <substring> ...
#
# Asserts that configure FAILS with the flag, and that its last words contain
# every substring given. Both halves matter: an exit status alone would also
# be satisfied by a tree that fails to configure for an unrelated reason, and
# a message alone would be satisfied by a warning.
refuses() {
	flag=$1
	shift
	dir="$TMP/$(echo "$flag" | tr -cd 'a-z0-9')"
	mkdir -p "$dir"
	# $BASE is meant to word-split into separate flags.
	# shellcheck disable=SC2086
	if (cd "$dir" && "$SRC/configure" $BASE "$flag" >configure.log 2>&1); then
		fail "configure ACCEPTED $flag -- the feature is not implemented, so the
  flag must be an error (issue #282). If you are the one implementing it, this
  assertion is what you delete, together with the AC_MSG_ERROR in
  sapi/fpmng/config.m4."
	fi
	for want in "$@"; do
		grep -qF -- "$want" "$dir/configure.log" ||
			fail "configure refused $flag, but not for our reason: its log does not
  mention '$want'. Last lines:
$(tail -5 "$dir/configure.log")"
	done
	echo "test-reserved-configure-flags.sh: $flag is refused, and says why: ok"
}

refuses --enable-fpmng-http2 \
	"HTTP/2 is NOT IMPLEMENTED" \
	"issues/186"
refuses --enable-fpmng-quic \
	"HTTP/3 over QUIC is NOT IMPLEMENTED" \
	"issues/188"

# The names are reserved to be READ, so `./configure --help` has to carry them
# -- with the "NOT IMPLEMENTED" marker and the dependency on --enable-fpmng-tls
# that both will have when they exist (HTTP/2 is negotiated over ALPN, and QUIC
# carries TLS 1.3 inside the transport; there is no plaintext QUIC).
help=$("$SRC/configure" --help 2>&1) || fail "./configure --help failed"
for flag in --enable-fpmng-http2 --enable-fpmng-quic; do
	line=$(echo "$help" | grep -A2 -- "$flag" || true)
	[ -n "$line" ] || fail "./configure --help does not list $flag (issue #282)"
	echo "$line" | grep -q "NOT IMPLEMENTED" ||
		fail "./configure --help lists $flag without the NOT IMPLEMENTED marker:
$line"
	echo "$line" | grep -q -- "--enable-fpmng-tls" ||
		fail "./configure --help lists $flag without its dependency on
  --enable-fpmng-tls: $line"
done
echo "test-reserved-configure-flags.sh: both names are in ./configure --help: ok"

echo "test-reserved-configure-flags.sh: PASS"

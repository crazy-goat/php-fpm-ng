#!/bin/sh
# Assemble the tree the .phpt runners run in, without a php-src checkout
# (issue #423).
#
#   build/phpt-tree.sh <outdir>
#
# Writes:
#   <outdir>/run-tests.php                        upstream's runner
#   <outdir>/sapi/fpmng/tests/                    upstream's sapi/fpm/tests/ (the
#                                                 harness .inc files and the
#                                                 retained upstream .phpt) with
#                                                 this repo's sapi/fpmng/tests/
#                                                 on top
#   <outdir>/sapi/fpmng/acme/                     what fpmng-acme-*.phpt require
#                                                 by relative path
#   <outdir>/ext/standard/tests/misc/browscap.ini what gh12621.phpt reads by
#                                                 relative path
#   <outdir>/FIXTURES                             provenance of the above
#
# It is the layout build/prepare.sh leaves behind for the test side -- the
# upstream tests copied into sapi/fpmng/tests/ and our own laid over them --
# except that every upstream file comes from third_party/php-src/ (issue #421's
# pinned, hash-checked copy) instead of from whichever checkout the caller had.
# build/run-fpmng-phpt.sh and build/run-fpm-phpt.sh take either, so the two can
# be compared with the same runner and the same binary.
#
# The layout stays one directory for both suites on purpose. Our fpmng-*.phpt
# `require_once "tester.inc"` and `include "skipif.inc"` from their own
# directory, and upstream's harness derives paths from __DIR__ (tester.inc
# reads __DIR__/.user.ini and __DIR__/conf.d). The runners tell the two suites
# apart by file name, not by location.
#
# <outdir> is written to by the tests (sockets, configs, run-test-info.php), so
# give it a throwaway directory. An existing one is emptied of the paths above
# first, nothing else is touched.
set -eu

REPO=$(cd "$(dirname "$0")/.." && pwd)
TP=$REPO/third_party/php-src
OUT=${1:?usage: build/phpt-tree.sh <outdir>}

fail() { echo "phpt-tree.sh: FAIL: $*" >&2; exit 1; }

[ -f "$TP/MANIFEST" ] || fail "no third_party/php-src/MANIFEST"
# The fixtures are only as good as the copy they come from; refuse one that was
# edited in place or is out of step with patches/. Same check CI runs.
"$REPO/build/vendor-php-src.sh" check >/dev/null || fail "third_party/php-src does not match its manifest; run build/vendor-php-src.sh check"

for f in run-tests.php sapi/fpm/tests/tester.inc ext/standard/tests/misc/browscap.ini; do
  [ -f "$TP/$f" ] || fail "third_party/php-src/$f is missing"
done
[ -d "$REPO/sapi/fpmng/acme" ] || fail "sapi/fpmng/acme is missing"

mkdir -p "$OUT"
rm -rf "$OUT/sapi/fpmng/tests" "$OUT/sapi/fpmng/acme" "$OUT/ext/standard/tests/misc" "$OUT/run-tests.php" "$OUT/FIXTURES"
mkdir -p "$OUT/sapi/fpmng" "$OUT/ext/standard/tests/misc"

cp "$TP/run-tests.php" "$OUT/run-tests.php"
cp -R "$TP/sapi/fpm/tests" "$OUT/sapi/fpmng/tests"
# Ours last, so a name on both sides can only ever resolve to ours. None does
# today: upstream's names carry no fpmng- prefix and the runners rely on that.
cp -R "$REPO/sapi/fpmng/tests/." "$OUT/sapi/fpmng/tests/"
cp -R "$REPO/sapi/fpmng/acme" "$OUT/sapi/fpmng/acme"
cp "$TP/ext/standard/tests/misc/browscap.ini" "$OUT/ext/standard/tests/misc/browscap.ini"

manifest_sha=$(if command -v sha256sum >/dev/null 2>&1; then sha256sum "$TP/MANIFEST"; else shasum -a 256 "$TP/MANIFEST"; fi | awk '{print $1}')
commit=$(git -C "$REPO" rev-parse --verify -q HEAD 2>/dev/null) || commit=unknown
{
  echo "source=third_party/php-src"
  awk -F '\t' '$1 == "tag" || $1 == "commit" { print "upstream_" $1 "=" $2 }' "$TP/MANIFEST"
  echo "manifest_sha256=$manifest_sha"
  echo "repo_commit=$commit"
} > "$OUT/FIXTURES"

echo "phpt-tree.sh: $(find "$OUT/sapi/fpmng/tests" -maxdepth 1 -name '*.phpt' | wc -l | tr -d ' ') .phpt files in $OUT/sapi/fpmng/tests ($(sed -n 's/^upstream_tag=//p' "$OUT/FIXTURES") fixtures + this repository)"

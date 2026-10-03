#!/bin/sh
# Hermetic check of build/package-gate-compare.sh (issue #678), with a fake
# results.tsv and a fake tests directory: no build, no docker, no binary.
#
#   1. a new test that passes everywhere needs no edit of the expected file;
#   2. a new test that skips fails, and the message names the test and the cell;
#   3. a stale SKIP line (the test passes now) fails, naming the test;
#   4. a FAIL fails; a missing result row fails; a cell glob only applies to the
#      cells it names; an exception without a reason is refused;
#   5. everything skipped (the PASS=0 trap) fails by name.
set -eu

REPO=$(cd "$(dirname "$0")/.." && pwd)
CMP=$REPO/build/package-gate-compare.sh
WORK=$(mktemp -d "${TMPDIR:-/tmp}/package-gate-expected.XXXXXX")
trap 'rm -rf "$WORK"' EXIT

fail() { echo "FAIL: $*" >&2; exit 1; }

mkdir "$WORK/tests"
for n in alpha beta gamma delta; do : > "$WORK/tests/fpmng-$n.phpt"; done
printf 'fpmng-delta.phpt retired\n' > "$WORK/tests/not-run-in-ci.list"
: > "$WORK/tests/fpmng-delta.phpt"

cat > "$WORK/expected.txt" <<'LIST'
# cells        kind  test         reason
deb,apk        SKIP  fpmng-beta   needs posix_kill() (#344)
apk*           SKIP  fpmng-gamma  no openssl in the Alpine pool
*              XFAIL fpmng-alpha  known defect
LIST

# tsv CELL-less builder: tsv <file> <name:category>...
tsv() {
  f=$1; shift
  printf 'test\tcategory\traw_status\n' > "$f"
  for r in "$@"; do
    printf 'sapi/fpmng/tests/%s.phpt\t%s\tX\n' "${r%%:*}" "${r#*:}" >> "$f"
  done
}

run() { "$CMP" "$1" "$WORK/expected.txt" "$2" "$WORK/tests" 2>&1; }

# 1. the exact expected picture passes, on each cell.
tsv "$WORK/ok-deb" fpmng-alpha:XFAIL fpmng-beta:SKIP fpmng-gamma:PASS
run deb "$WORK/ok-deb" >/dev/null || fail "the expected picture on deb was refused"
tsv "$WORK/ok-debtls" fpmng-alpha:XFAIL fpmng-beta:PASS fpmng-gamma:PASS
run deb-tls "$WORK/ok-debtls" >/dev/null || fail "the expected picture on deb-tls was refused"
tsv "$WORK/ok-apk" fpmng-alpha:XFAIL fpmng-beta:SKIP fpmng-gamma:SKIP
run apk "$WORK/ok-apk" >/dev/null || fail "the expected picture on apk was refused"
tsv "$WORK/ok-apktls" fpmng-alpha:XFAIL fpmng-beta:PASS fpmng-gamma:SKIP
run apk-tls "$WORK/ok-apktls" >/dev/null || fail "the expected picture on apk-tls was refused"

# 1b. a new passing test: one more owned file, one more PASS row, no list edit.
: > "$WORK/tests/fpmng-epsilon.phpt"
tsv "$WORK/new-pass" fpmng-alpha:XFAIL fpmng-beta:SKIP fpmng-gamma:PASS fpmng-epsilon:PASS
run deb "$WORK/new-pass" >/dev/null || fail "a new passing test needed an edit of the expected file"

# 2. a new test that skips.
tsv "$WORK/new-skip" fpmng-alpha:XFAIL fpmng-beta:SKIP fpmng-gamma:PASS fpmng-epsilon:SKIP
out=$(run deb "$WORK/new-skip") && fail "a new unlisted skip passed"
echo "$out" | grep -q 'unexpected SKIP: fpmng-epsilon on deb' || fail "the unexpected skip was not named: $out"

# 3. a stale SKIP entry.
tsv "$WORK/stale" fpmng-alpha:XFAIL fpmng-beta:PASS fpmng-gamma:PASS fpmng-epsilon:PASS
out=$(run deb "$WORK/stale") && fail "a stale SKIP entry passed"
echo "$out" | grep -q 'expected SKIP but PASS: fpmng-beta on deb' || fail "the stale entry was not named: $out"

# 4a. a FAIL.
tsv "$WORK/failing" fpmng-alpha:XFAIL fpmng-beta:SKIP fpmng-gamma:FAIL/ERROR fpmng-epsilon:PASS
out=$(run deb "$WORK/failing") && fail "a FAIL passed"
echo "$out" | grep -q 'unexpected FAIL: fpmng-gamma' || fail "the FAIL was not named: $out"

# 4b. a test with no result row.
tsv "$WORK/short" fpmng-alpha:XFAIL fpmng-beta:SKIP fpmng-gamma:PASS
out=$(run deb "$WORK/short") && fail "a missing result row passed"
echo "$out" | grep -q 'scored 3 tests but the tree owns 4' || fail "the missing row was not reported: $out"

# 4c. an XFAIL that started passing.
tsv "$WORK/xpass" fpmng-alpha:PASS fpmng-beta:SKIP fpmng-gamma:PASS fpmng-epsilon:PASS
out=$(run deb "$WORK/xpass") && fail "a stale XFAIL entry passed"
echo "$out" | grep -q 'expected XFAIL but PASS: fpmng-alpha' || fail "the stale XFAIL was not named: $out"

# 4d. an exception without a reason is refused.
printf 'deb SKIP fpmng-beta\n' > "$WORK/noreason.txt"
"$CMP" deb "$WORK/noreason.txt" "$WORK/new-pass" "$WORK/tests" >/dev/null 2>&1 && fail "an exception with no reason was accepted"

# 5. the PASS=0 trap: everything skipped.
tsv "$WORK/allskip" fpmng-alpha:SKIP fpmng-beta:SKIP fpmng-gamma:SKIP fpmng-epsilon:SKIP
out=$(run deb "$WORK/allskip") && fail "an all-skipped run passed"
echo "$out" | grep -q 'unexpected SKIP: fpmng-epsilon' || fail "the all-skip run was not named: $out"

echo "ok: package-gate-compare.sh scores by name (new pass, new skip, stale skip, FAIL, missing row, no reason, all skipped)"

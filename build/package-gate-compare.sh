#!/bin/sh
# The package gate's scoring (issue #678): compare one cell's per-test results
# against the named exceptions in build/package-gate-expected.txt, instead of
# against pinned counts.
#
# Usage: build/package-gate-compare.sh <cell> <expected-file> <results.tsv> <tests-dir>
#   cell           deb, deb-tls, apk or apk-tls
#   expected-file  build/package-gate-expected.txt
#   results.tsv    the per-test file build/run-fpmng-phpt.sh writes
#   tests-dir      the tree's sapi/fpmng/tests, to count the owned tests
#
# Expected-file lines are "<cell-globs> <SKIP|XFAIL> <test-name> <reason>", the
# globs comma separated and matched against the cell name with case(1) rules.
# The reason is mandatory, so an exception cannot outlive its justification.
#
# The cell passes when: every owned test has a result row (TOTAL is the number
# of owned tests, never a pinned number); nothing FAILed and everything was
# measured; the tests that SKIP are exactly the cell's expected SKIPs; the
# tests that XFAIL are exactly its expected XFAILs. PASS is then TOTAL minus the
# named exceptions by construction, which keeps the PASS=0 trap closed: a run
# in which php-fpm cannot start skips everything and fails here by name.
set -eu

fail() { echo "package-gate-compare.sh: FAIL: $*" >&2; exit 1; }

[ $# -eq 4 ] || fail "usage: build/package-gate-compare.sh <cell> <expected-file> <results.tsv> <tests-dir>"
CELL=$1; EXPECTED=$2; TSV=$3; TESTS=$4
[ -f "$EXPECTED" ] || fail "no expected-results file: $EXPECTED"
[ -f "$TSV" ] || fail "no results file: $TSV"
[ -d "$TESTS" ] || fail "no tests directory: $TESTS"

WORK=$(mktemp -d "${TMPDIR:-/tmp}/package-gate-compare.XXXXXX")
trap 'rm -rf "$WORK"' EXIT

# --- the cell's exceptions, as "KIND name" lines ------------------------------
: > "$WORK/expect"
# No pathname expansion while the glob fields are split: a bare * is a cell glob.
set -f
while IFS= read -r line || [ -n "$line" ]; do
    case "$line" in ''|'#'*) continue ;; esac
    # shellcheck disable=SC2086 # splitting on blanks is the point
    set -- $line
    [ $# -ge 4 ] || fail "$EXPECTED: want '<cells> <SKIP|XFAIL> <test> <reason>', got: $line"
    globs=$1; kind=$2; name=$3
    case "$kind" in SKIP|XFAIL) ;; *) fail "$EXPECTED: kind must be SKIP or XFAIL, got '$kind' for $name" ;; esac
    old_ifs=$IFS; IFS=,
    # shellcheck disable=SC2086
    set -- $globs
    IFS=$old_ifs
    for g in "$@"; do
        # shellcheck disable=SC2254 # the glob is the pattern on purpose
        case "$CELL" in $g) printf '%s %s\n' "$kind" "${name%.phpt}" >> "$WORK/expect"; break ;; esac
    done
done < "$EXPECTED"
set +f
LC_ALL=C sort "$WORK/expect" > "$WORK/expect.sorted"
dups=$(uniq -d "$WORK/expect.sorted" | tr '\n' ';')
[ -z "$dups" ] || fail "$EXPECTED lists the same exception twice for $CELL: $dups"

# --- what the run produced ----------------------------------------------------
awk -F '\t' 'NR > 1 { n = split($1, a, "/"); name = a[n]; sub(/\.phpt$/, "", name); print $2 "\t" name }' \
    "$TSV" | LC_ALL=C sort > "$WORK/got"
ROWS=$(awk 'END {print NR + 0}' "$WORK/got")

# --- TOTAL: the owned tests in the tree the package was built from -----------
OWNED=0
for f in "$TESTS"/fpmng-*.phpt; do
    [ -f "$f" ] || continue
    base=$(basename "$f")
    # Exact match on the first field: the name is data, not a regex (its dot).
    if [ -f "$TESTS/not-run-in-ci.list" ] &&
        awk -v b="$base" '$1 == b { found = 1 } END { exit !found }' "$TESTS/not-run-in-ci.list"; then
        continue
    fi
    OWNED=$((OWNED + 1))
done
[ "$OWNED" -gt 0 ] || fail "no owned fpmng-*.phpt tests under $TESTS"

problems=
note() { problems="$problems
  $*"; }

[ "$ROWS" -eq "$OWNED" ] || note "the run scored $ROWS tests but the tree owns $OWNED"

awk -F '\t' '$1 == "FAIL/ERROR" {print $2}' "$WORK/got" | while read -r n; do echo "FAIL $n"; done > "$WORK/p.fail"
awk -F '\t' '$1 == "NOT MEASURED" {print $2}' "$WORK/got" | while read -r n; do echo "NOT-MEASURED $n"; done > "$WORK/p.nm"
while read -r l; do [ -z "$l" ] || note "unexpected FAIL: ${l#FAIL } on $CELL"; done < "$WORK/p.fail"
while read -r l; do [ -z "$l" ] || note "not measured: ${l#NOT-MEASURED } on $CELL"; done < "$WORK/p.nm"

for kind in SKIP XFAIL; do
    awk -F '\t' -v k="$kind" '$1 == k {print $2}' "$WORK/got" | LC_ALL=C sort > "$WORK/got.$kind"
    awk -v k="$kind" '$1 == k {print $2}' "$WORK/expect.sorted" | LC_ALL=C sort > "$WORK/exp.$kind"
    LC_ALL=C comm -23 "$WORK/got.$kind" "$WORK/exp.$kind" | while read -r n; do
        echo "unexpected $kind: $n on $CELL (add a '$kind' line with a reason to $EXPECTED, or fix the test)"
    done > "$WORK/p.un.$kind"
    LC_ALL=C comm -13 "$WORK/got.$kind" "$WORK/exp.$kind" | while read -r n; do
        cat=$(awk -F '\t' -v n="$n" '$2 == n {print $1}' "$WORK/got")
        echo "expected $kind but ${cat:-no result}: $n on $CELL (stale line in $EXPECTED)"
    done > "$WORK/p.stale.$kind"
    while IFS= read -r l; do [ -z "$l" ] || note "$l"; done < "$WORK/p.un.$kind"
    while IFS= read -r l; do [ -z "$l" ] || note "$l"; done < "$WORK/p.stale.$kind"
done

[ -z "$problems" ] || fail "the packaged binary's results do not match the expectations for $CELL:$problems"

NSKIP=$(awk 'END {print NR + 0}' "$WORK/got.SKIP")
NXFAIL=$(awk 'END {print NR + 0}' "$WORK/got.XFAIL")
echo "package-gate-compare.sh: ok ($CELL: $OWNED owned, $((OWNED - NSKIP - NXFAIL)) pass, $NSKIP skip, $NXFAIL xfail, all exceptions named)"

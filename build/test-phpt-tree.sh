#!/bin/sh
# Hermetic checks for the php-src-free test tree (issue #423): no php-src, no
# build, no real binary.
#
#   1. build/phpt-tree.sh assembles the layout the runners expect from
#      third_party/php-src/ and this repository alone;
#   2. it refuses a vendored fixture that was edited in place;
#   3. the runners will not start a test run when the harness would launch a
#      different FPM binary than the one they were given (the trap being an
#      installed php-fpm8.5 picked up by tester.inc's fallbacks), and say which
#      binary the harness resolved when it does match.
#
# The runners are driven with shell-script stand-ins for the CLI and the FPM
# binary; their preflight is what is under test, not run-tests.php.
set -eu

REPO=$(cd "$(dirname "$0")/.." && pwd)
WORK=$(mktemp -d "${TMPDIR:-/tmp}/fpmng-phpt-tree.XXXXXX")
trap 'rm -rf "$WORK"' EXIT
unset TEST_PHP_EXECUTABLE TEST_PHP_FPM_EXECUTABLE

fail() { echo "FAIL: $*" >&2; exit 1; }

# --- 1. assembly -------------------------------------------------------------
"$REPO/build/phpt-tree.sh" "$WORK/tree" >/dev/null
T=$WORK/tree
for f in run-tests.php FIXTURES ext/standard/tests/misc/browscap.ini \
         sapi/fpmng/acme/state.php sapi/fpmng/tests/tester.inc sapi/fpmng/tests/skipif.inc \
         sapi/fpmng/tests/gh12621.phpt sapi/fpmng/tests/fpmng-http-direct.phpt \
         sapi/fpmng/tests/upstream-deviations.list; do
  [ -f "$T/$f" ] || fail "assembled tree lacks $f"
done
want=$(awk -F '\t' '$1 ~ /^sapi\/fpm\/tests\/.*\.phpt$/' "$REPO/third_party/php-src/MANIFEST" | wc -l | tr -d ' ')
got=$(cd "$T/sapi/fpmng/tests" && find . -maxdepth 1 -name '*.phpt' ! -name 'fpmng-*' | grep -c . || true)
[ "$want" = "$got" ] || fail "tree has $got upstream .phpt files, the manifest lists $want"
grep -qx 'upstream_tag=php-8.5.9' "$T/FIXTURES" || fail "FIXTURES does not name the pinned tag"
# What a prepared tree would hold and this one must not: any php-src source.
for f in main Zend sapi/fpm TSRM configure.ac; do
  [ ! -e "$T/$f" ] || fail "assembled tree contains $f"
done
echo "ok: tree assembled ($got upstream + owned .phpt, no php-src)"

# --- 2. an edited fixture is refused ----------------------------------------
FAKE=$WORK/repo
mkdir -p "$FAKE/sapi/fpmng"
cp -R "$REPO/build" "$REPO/third_party" "$REPO/patches" "$FAKE/"
cp -R "$REPO/sapi/fpmng/tests" "$REPO/sapi/fpmng/acme" "$REPO/sapi/fpmng/fixtures" "$FAKE/sapi/fpmng/"
echo '// edited' >> "$FAKE/third_party/php-src/run-tests.php"
if "$FAKE/build/phpt-tree.sh" "$WORK/tree2" >"$WORK/out2.txt" 2>&1; then
  fail "phpt-tree.sh accepted an edited run-tests.php"
fi
grep -q 'edited in place: third_party/php-src/run-tests.php' "$WORK/out2.txt" || fail "no 'edited in place' diagnosis: $(cat "$WORK/out2.txt")"
echo "ok: an edited fixture is refused"

# --- 2b. a manifest whose upstream and vendored hashes differ is refused ----
# Main carries no php-src patch, so a vendored copy that is not the upstream
# bytes (a patch smuggled in with a re-import) must fail the check.
cp "$REPO/third_party/php-src/MANIFEST" "$FAKE/third_party/php-src/MANIFEST"
cp "$REPO/third_party/php-src/run-tests.php" "$FAKE/third_party/php-src/run-tests.php"
awk -F '\t' -v OFS='\t' '$1 == "main/fastcgi.h" { $3 = "0000000000000000000000000000000000000000000000000000000000000000" } { print }' \
  "$REPO/third_party/php-src/MANIFEST" > "$FAKE/third_party/php-src/MANIFEST"
if "$FAKE/build/vendor-php-src.sh" check >"$WORK/out2b.txt" 2>&1; then
  fail "vendor-php-src.sh check accepted a non-pristine manifest entry"
fi
grep -q 'not pristine: third_party/php-src/main/fastcgi.h' "$WORK/out2b.txt" || fail "no 'not pristine' diagnosis: $(cat "$WORK/out2b.txt")"
echo "ok: a non-pristine vendored file is refused"

# --- 3. the harness must launch the binary it was given ---------------------
FPM=$WORK/given/php-fpm-ng
OTHER=$WORK/installed/php-fpm8.5
mkdir -p "$WORK/given" "$WORK/installed"
for b in "$FPM" "$OTHER"; do
  printf '#!/bin/sh\n# php-fpm-ng pool.type fpmng_ stand-in\necho "PHP 8.5.0 (fpm-fcgi)"\n' > "$b"
  chmod +x "$b"
done
CLI=$WORK/given/php
# -v for the version line; -n -r is the runner asking tester.inc where it would
# start FPM, answered from $STAND_IN_RESOLVES.
cat > "$CLI" <<'CLIEOF'
#!/bin/sh
case "$1" in
  -v) echo "PHP 8.5.0 (cli)" ;;
  -n) printf '%s' "$STAND_IN_RESOLVES" ;;
esac
CLIEOF
chmod +x "$CLI"

for runner in run-fpmng-phpt.sh run-fpm-phpt.sh; do
  rm -rf "$WORK/res"
  set +e
  STAND_IN_RESOLVES=$OTHER TEST_PHP_EXECUTABLE=$CLI TEST_PHP_FPM_EXECUTABLE=$FPM \
    "$REPO/build/$runner" - "$WORK/res" >"$WORK/out3.txt" 2>&1
  status=$?
  set -e
  [ "$status" != 0 ] || fail "$runner went ahead although tester.inc resolves to another FPM"
  grep -q "tester.inc would start '$OTHER', not the requested $(realpath "$FPM")" "$WORK/out3.txt" \
    || fail "$runner: no diagnosis of the mismatched FPM: $(cat "$WORK/out3.txt")"
  grep -qx 'measurement_status=NOT MEASURED' "$WORK/res/summary.txt" || fail "$runner: a mismatch is not NOT MEASURED"

  rm -rf "$WORK/res"
  set +e
  STAND_IN_RESOLVES=$(realpath "$FPM") TEST_PHP_EXECUTABLE=$CLI TEST_PHP_FPM_EXECUTABLE=$FPM \
    "$REPO/build/$runner" - "$WORK/res" >"$WORK/out4.txt" 2>&1
  set -e
  grep -q "^tester_resolves_fpm_to=$(realpath "$FPM")\$" "$WORK/res/metadata.txt" \
    || fail "$runner: metadata.txt does not record what the harness resolved: $(cat "$WORK/res/metadata.txt")"
  grep -q '^fixtures_begin$' "$WORK/res/metadata.txt" && grep -q '^upstream_tag=php-8.5.9$' "$WORK/res/metadata.txt" \
    || fail "$runner: metadata.txt does not record the fixture provenance"
  echo "ok: $runner refuses a harness that would start another FPM and records the resolved binary"
done

# --- 4. a tree can be made safe for run-tests.php -j (issue #394) ------------
P=$WORK/ptree
"$REPO/build/phpt-tree.sh" "$P" >/dev/null
"$REPO/build/phpt-parallel.sh" "$P" || fail "phpt-parallel.sh failed on an assembled tree"
cp "$P/sapi/fpmng/tests/tester.inc" "$WORK/tester.once"
cp "$P/run-tests.php" "$WORK/run-tests.once"
"$REPO/build/phpt-parallel.sh" "$P" || fail "phpt-parallel.sh is not idempotent"
cmp -s "$WORK/tester.once" "$P/sapi/fpmng/tests/tester.inc" && cmp -s "$WORK/run-tests.once" "$P/run-tests.php" \
  || fail "a second phpt-parallel.sh run changed the tree"
grep -q 'TEST_PHP_WORKER' "$P/sapi/fpmng/tests/tester.inc" || fail "tester.inc ignores TEST_PHP_WORKER"
grep -q 'TEST_PHP_WORKER' "$P/run-tests.php" || fail "run-tests.php does not pass TEST_PHP_WORKER to the tests"
[ ! -e "$P/sapi/fpmng/tests/CONFLICTS" ] || fail "the dir-wide CONFLICTS file is still there"
grep -qx 'operator-default-listener' "$P/sapi/fpmng/tests/fpmng-http-gateway.phpt" || fail "a gateway test has no conflict key"
if grep -q '^--CONFLICTS--' "$P/sapi/fpmng/tests/fpmng-http-direct.phpt"; then fail "a non-gateway test got a conflict key"; fi
# The serial run must stay what it was: worker 0 / unset allocates 9008 first.
grep -q '9000 + PHP_INT_SIZE - 1 + \$worker \* 200' "$P/sapi/fpmng/tests/tester.inc" || fail "tester.inc port base is not 9000 + PHP_INT_SIZE - 1 + 200 * worker"
# Issue #567: signalling pid 0 would take the whole process group, runner included.
grep -q 'refusing to send SIG' "$P/sapi/fpmng/tests/tester.inc" || fail "tester.inc still signals a pid below 2"
# The bundle itself is untouched.
(cd "$REPO" && ./build/vendor-php-src.sh check >/dev/null) || fail "the pinned bundle was modified"
echo "ok: phpt-parallel.sh is idempotent, adds worker port blocks and conflict keys, leaves the bundle alone"

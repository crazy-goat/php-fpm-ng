#!/bin/sh
# The reserved port lane of build/run-fpmng-phpt.sh (issue #625).
#
#   build/test-phpt-port-lane.sh
#
# A lane run is supposed to keep every listener of the suite inside the 1000
# ports it reserved, so that two lanes on one host cannot meet. Three things
# can break that, and one of each is checked here:
#
#   1. the fixed-port family of the http-direct tests grew past the bounds the
#      runner translated (the runner would put part of it outside the lane, and
#      no run would notice until something met it);
#   2. a test binds a TCP port by a route the lane does not move -- a literal
#      that is neither a Tester address ({{ADDR...}}) nor a member of the
#      family (the historical failure mode: a number typed into a config);
#   3. the runner's lane arithmetic is wrong, or its refusals stopped working.
#
# Hermetic: no build, no binary, no PHP. The tree is assembled from
# third_party/php-src/ (build/phpt-tree.sh) and the runner is driven with
# shell-script stand-ins for the CLI and the FPM binary, exactly as
# build/test-phpt-tree.sh does; the stand-in CLI runs no test, so the run
# reports NOT MEASURED and the failure floor is what stops it.
set -eu

REPO=$(cd "$(dirname "$0")/.." && pwd)
RUNNER=$REPO/build/run-fpmng-phpt.sh
TESTS=$REPO/sapi/fpmng/tests
WORK=$(mktemp -d "${TMPDIR:-/tmp}/fpmng-phpt-port-lane.XXXXXX")
trap 'rm -rf "$WORK"' EXIT

# The preflight must be what stops these runs, so an inherited TEST_PHP_* from a
# real phpt run in the same shell cannot carry them past it.
unset TEST_PHP_EXECUTABLE TEST_PHP_FPM_EXECUTABLE FPMNG_PHPT_PORT_BASE FPMNG_PHPT_PORT_SHIFT

fail() { printf 'FAIL: %s\n' "$*" >&2; exit 1; }

# The runner's own layout constants, read out of it rather than repeated here:
# a check that carried its own copy would pass when the runner changed.
lane_const() {
    value=$(sed -n "s/^$1=\(.*\)$/\1/p" "$RUNNER" | head -1)
    case "$value" in
        ''|*[!0-9]*) fail "the runner has no readable $1 constant" ;;
    esac
    printf '%s\n' "$value"
}
LANE_SIZE=$(lane_const LANE_SIZE)
TESTER_BLOCK_SIZE=$(lane_const TESTER_BLOCK_SIZE)
ORIGIN=$(lane_const HTTP_DIRECT_PORT_ORIGIN)
FAMILY_END=$(lane_const HTTP_DIRECT_PORT_FAMILY_END)
FAMILY_OFFSET=$(lane_const LANE_FAMILY_OFFSET)

# --- 1. the family bounds, measured from the tests themselves ----------------
#
# The two ways an http-direct test names a fixed port, both of which read the
# per-run shift: a literal default behind an FPMNG_*_PORT override, and a
# literal base assigned to a variable. An offset added to either (+12, or a
# $base + 4) moves the top of the family up, so it is followed one level.
#
# Derived by awk because that is what the checks CI job has; php would do it in
# a third of the lines. The program lives in a file because the no-step case
# below runs the same program.
cat > "$WORK/family.awk" <<'AWK'
    FNR == 1 { delete base }
    /FPMNG_PHPT_PORT_SHIFT/ {
        line = $0
        # literal default of an override, getenv(...) ?: 28054, and an offset
        # applied to the whole expression, ...) + 12
        while (match(line, /[0-9][0-9][0-9][0-9][0-9]/)) {
            # Save the outer match before the inner one below overwrites
            # RSTART/RLENGTH: with the inner offsets, a failed inner match
            # (RSTART=0, RLENGTH=-1) made substr(line, -1) return the whole
            # line and the loop never advanced (issue #625 review, round 2).
            mstart = RSTART
            mlen = RLENGTH
            n = substr(line, mstart, mlen) + 0
            rest = substr(line, mstart + mlen)
            off = 0
            if (match(rest, /^[ \t]*\)?[ \t]*\+[ \t]*[0-9]+/)) {
                # "+ 200 * (int) getenv('TEST_PHP_WORKER')" is the per-worker
                # step, not an offset: a serial lane has no worker.
                after = substr(rest, RSTART + RLENGTH)
                if (after !~ /^[ \t]*\*/) {
                    t = substr(rest, RSTART, RLENGTH)
                    sub(/^[^0-9]*/, "", t)
                    off = t + 0
                }
            }
            if (lo == 0 || n < lo) lo = n
            if (n + off > hi) hi = n + off
            line = substr(line, mstart + mlen)
        }
        # the variable the literal was assigned to, for the rule below
        if (match($0, /\$[A-Za-z_][A-Za-z_0-9]*[ \t]*=[^=]*/)) {
            v = $0
            sub(/^.*\$/, "", v)
            sub(/[ \t]*=.*$/, "", v)
            if (match($0, /[0-9][0-9][0-9][0-9][0-9]/)) {
                base[v] = substr($0, RSTART, RLENGTH) + 0
            }
        }
    }
    # $port = $base + 4, where $base took a family literal in this file
    match($0, /\$[A-Za-z_][A-Za-z_0-9]*[ \t]*=[ \t]*(\(int\)[ \t]*)?\$[A-Za-z_][A-Za-z_0-9]*[ \t]*\+[ \t]*[0-9]+/) {
        rhs = $0
        sub(/^.*\$[A-Za-z_][A-Za-z_0-9]*[ \t]*=[ \t]*(\(int\)[ \t]*)?\$/, "", rhs)
        var = rhs
        sub(/[ \t]*\+.*$/, "", var)
        add = rhs
        sub(/^.*\+[ \t]*/, "", add)
        if (var in base) {
            n = base[var] + add + 0
            if (lo == 0 || n < lo) lo = n
            if (n > hi) hi = n
        }
    }
    END { printf "%d %d\n", lo, hi }
AWK
measured=$(awk -f "$WORK/family.awk" "$TESTS"/fpmng-*.phpt "$TESTS"/*.inc)

measured_lo=$(printf '%s\n' "$measured" | awk '{print $1}')
measured_hi=$(printf '%s\n' "$measured" | awk '{print $2}')
[ -n "$measured_lo" ] && [ "$measured_lo" != 0 ] || fail "no fixed-port family was found in $TESTS"
printf 'ok: the http-direct family measured over %s fpmng-*.phpt is %s..%s\n' \
    "$(find "$TESTS" -maxdepth 1 -name 'fpmng-*.phpt' | wc -l | tr -d ' ')" "$measured_lo" "$measured_hi"
[ "$measured_lo" = "$ORIGIN" ] ||
    fail "the runner translates the family from HTTP_DIRECT_PORT_ORIGIN=$ORIGIN but the tests' lowest fixed port is $measured_lo (update the constant, or the test that moved)"
[ "$measured_hi" = "$FAMILY_END" ] ||
    fail "the runner declares the family to end at HTTP_DIRECT_PORT_FAMILY_END=$FAMILY_END but the tests' highest fixed port is $measured_hi (update the constant, or the test that moved)"

# The family must fit the lane after the Tester's block, or the runner's own
# consistency guard would have stopped the run -- checked here so the failure
# names the layout instead of the run.
[ "$((FAMILY_OFFSET + FAMILY_END - ORIGIN + 1))" -le "$((LANE_SIZE - TESTER_BLOCK_SIZE))" ] ||
    fail "the measured family does not fit a $LANE_SIZE-port lane after a $TESTER_BLOCK_SIZE-port Tester block"

# The no-step shape. A family literal with no "+ <digits>" after it -- here
# 29000 + (int) getenv(...) -- used to hang this measurement forever: the inner
# match() of the offset probe failed, RSTART/RLENGTH became 0/-1, and
# substr(line, -1) returned the whole line so the while never advanced. It must
# terminate and report the literal (issue #625 review, round 2).
cat > "$WORK/nostep.inc" <<'EOF'
$port = (int) (getenv('FPMNG_SCRATCH_PORT') ?: 29000 + (int) getenv('FPMNG_PHPT_PORT_SHIFT'));
EOF
nostep=$(awk -f "$WORK/family.awk" "$WORK/nostep.inc")
[ "$nostep" = "29000 29000" ] ||
    fail "the family measurement does not terminate on a literal with no +N after it (got '$nostep')"
echo "ok: the family measurement terminates on a literal with no +N after it"

# --- 2. no test binds a TCP port the lane cannot move ------------------------
#
# A listening address in a test's configuration is either a Tester address
# ({{ADDR...}}, which becomes a port of the run's own block) or a $variable the
# test computed from one of the two families. A bare literal in the value is a
# number nothing moves, and that is the shape checked here: the directive, the
# address, and the port as the last thing on the line.
#
# What this leaves alone on purpose: a literal inside a quoted PHP string, which
# is an argument rather than a configuration value -- the 127.0.0.1:1 and
# 127.0.0.1:8080 that fpmng-http-direct-config.phpt and
# fpmng-config-rejected-directives.phpt offer to a pool type that refuses them,
# the 127.0.0.1:9001 fpmng-http-direct-config.phpt accepts and never starts, and
# the http.route[] targets at 29040 fpmng-http-route-invalid.phpt accepts and
# never starts. None of them can bind, and that is not an assumption: -t returns
# from fpm_conf_init_main() (sapi/fpmng/fpm/fpm_conf.c:2743) before
# fpm_unix_init_main() (sapi/fpmng/fpm/fpm.c:81) in the startup chain gets to
# open a socket.
#
# Unix socket paths are not ports and are left out of it.
grep -nE '^[^;]*\b(listen|plain_listen|status_listen|metrics_listen)\s*=\s*(127\.0\.0\.1|0\.0\.0\.0|localhost|\[::1\]|\[::\]):[0-9]+$' \
    "$TESTS"/fpmng-*.phpt "$TESTS"/*.inc > "$WORK/literal-listeners.txt" || true
if [ -s "$WORK/literal-listeners.txt" ]; then
    cat "$WORK/literal-listeners.txt" >&2
    fail "a test binds a literal TCP listen address; use {{ADDR[...]}} or an FPMNG_*_PORT default so the lane can move it"
fi
echo "ok: every TCP listen address in the owned tests comes from the Tester or from the translated family"

# --- 3. the runner's lane arithmetic and its refusals ------------------------
FPM=$WORK/given/php-fpm-ng
CLI=$WORK/given/php
mkdir -p "$WORK/given"
printf '#!/bin/sh\n# php-fpm-ng pool.type fpmng_ stand-in\necho "PHP 8.5.0 (fpm-fcgi)"\n' > "$FPM"
chmod +x "$FPM"
cat > "$CLI" <<'CLIEOF'
#!/bin/sh
# Records its own argv, so a check can see what the runner asked run-tests.php
# to do -- in particular whether it passed -j, which the placeholder itself
# never does. It also records the TMPDIR it was started with: the runner hands
# its environment to run-tests.php, which hands it to every test, so the last
# invocation's TMPDIR is the TMPDIR the tests are started with (issue #625
# review, round 2). The write overwrites, so the last invocation wins.
if [ -n "$ARGV_LOG" ]; then printf '%s\n' "$@" > "$ARGV_LOG"; fi
if [ -n "$ENV_LOG" ]; then printf 'TMPDIR=%s\n' "${TMPDIR-}" > "$ENV_LOG"; fi
case "$1" in
  -v) echo "PHP 8.5.0 (cli)" ;;
  -n) printf '%s' "$STAND_IN_RESOLVES" ;;
esac
CLIEOF
chmod +x "$CLI"
# tester.inc resolves its own paths, and on a box where the temporary directory
# is a symlink (/var -> /private/var on macOS) an unresolved path is not what
# it reports back.
RESOLVED_FPM=$(realpath "$FPM")

# A lane run: the stand-in CLI runs no test, so the run stops on the PASS floor
# with every test NOT MEASURED. What is under test is what it printed and
# recorded, which is the whole lane: bounds, serial run, derived shift.
lane_run() { # <base>
    base=$1
    rm -rf "$WORK/res" "$WORK/argv.txt" "$WORK/env.txt"
    set +e
    STAND_IN_RESOLVES="$RESOLVED_FPM" FPMNG_PHPT_PORT_BASE="$base" ARGV_LOG="$WORK/argv.txt" ENV_LOG="$WORK/env.txt" \
    TEST_PHP_EXECUTABLE="$CLI" TEST_PHP_FPM_EXECUTABLE="$FPM" \
        "$RUNNER" - "$WORK/res" > "$WORK/out.txt" 2>&1
    status=$?
    set -e
    printf '%s\n' "$status" > "$WORK/status.txt"
}

expect_metadata() {
    grep -qx "$1" "$WORK/res/metadata.txt" || {
        printf 'FAIL: %s not reported in metadata.txt\n' "$1" >&2
        cat "$WORK/res/metadata.txt" >&2
        exit 1
    }
}
expect_output() {
    grep -qF "$1" "$WORK/out.txt" || {
        printf 'FAIL: the runner did not print: %s\n' "$1" >&2
        cat "$WORK/out.txt" >&2
        exit 1
    }
}
BASE=21000
SHIFT=$((BASE + FAMILY_OFFSET - ORIGIN))
lane_run "$BASE"
[ "$(cat "$WORK/status.txt")" != 0 ] || fail 'a lane run of a placeholder tree reported success'
expect_metadata "port_lane=$BASE-$((BASE + LANE_SIZE - 1))"
expect_metadata "port_tester_block=$((BASE + 1))-$((BASE + TESTER_BLOCK_SIZE))"
expect_metadata "port_operator=$((BASE + TESTER_BLOCK_SIZE - 1))"
expect_metadata "port_http_direct_family=$((BASE + FAMILY_OFFSET))-$((BASE + FAMILY_OFFSET + FAMILY_END - ORIGIN))"
expect_metadata "port_shift=$SHIFT"
expect_output "Port lane: $BASE-$((BASE + LANE_SIZE - 1))"
# Serial: the run must not have passed -j, or one lane would want one 200-port
# block per worker and leave the lane. argv.txt is also the proof the run got
# as far as invoking run-tests.php at all.
[ -f "$WORK/argv.txt" ] || { printf 'FAIL: the lane run never reached run-tests.php\n' >&2; cat "$WORK/out.txt" >&2; exit 1; }
if grep -q -- '^-j' "$WORK/argv.txt"; then
    printf 'FAIL: a lane run passed -j to run-tests.php\n' >&2
    exit 1
fi
echo "ok: FPMNG_PHPT_PORT_BASE=$BASE runs serial inside $BASE-$((BASE + LANE_SIZE - 1)) with shift $SHIFT"

# The bounds are inside the lane and inside the ephemeral range, for every base
# the runner accepts: the arithmetic is the runner's, so redoing it here would
# only test this script.
base=1024
while [ "$base" -le 31000 ]; do
    low=$base
    high=$((base + LANE_SIZE - 1))
    family_top=$((base + FAMILY_OFFSET + FAMILY_END - ORIGIN))
    [ "$high" -lt 32768 ] || { [ "$base" -gt 31000 ] && break; }
    [ "$low" -ge 1024 ] || fail "base $base is below 1024"
    [ "$family_top" -le "$high" ] || fail "base $base puts the family top ($family_top) outside the lane ($high)"
    [ "$((base + TESTER_BLOCK_SIZE))" -le "$((base + FAMILY_OFFSET - 1))" ] || fail "base $base overlaps the Tester block with the family"
    base=$((base + 976))
done
echo "ok: the lane layout holds for every base from 1024 to 31000"

# Refusals. A lane is one decision about where the ports are; each of these
# would otherwise be a second, silent decision. They all have to stop at the
# preflight, so each is checked for a NOT MEASURED summary as well as for the
# message.
refuse() { # <expected text> <results dir> <output file> [VAR=value...]
    expect=$1
    res=$2
    out=$3
    shift 3
    rm -rf "$res"
    set +e
    env STAND_IN_RESOLVES="$RESOLVED_FPM" TEST_PHP_EXECUTABLE="$CLI" \
        TEST_PHP_FPM_EXECUTABLE="$FPM" "$@" \
        "$RUNNER" - "$res" > "$out" 2>&1
    set -e
    grep -qF "$expect" "$out" || {
        printf 'FAIL: the runner did not refuse: %s\n' "$expect" >&2
        cat "$out" >&2
        exit 1
    }
    grep -qx 'measurement_status=NOT MEASURED' "$res/summary.txt" || {
        printf 'FAIL: a refused lane run is not NOT MEASURED\n' >&2
        cat "$res/summary.txt" >&2
        exit 1
    }
}

refuse 'set either FPMNG_PHPT_PORT_BASE' "$WORK/res2" "$WORK/out2.txt" \
    FPMNG_PHPT_PORT_BASE=$BASE FPMNG_PHPT_PORT_SHIFT=0
echo "ok: a base and a shift together are refused"

refuse 'wants a 200-port block per worker and does not fit' "$WORK/res3" "$WORK/out3.txt" \
    FPMNG_PHPT_PORT_BASE=$BASE TEST_FPM_JOBS=4
echo "ok: a lane run with TEST_FPM_JOBS above one is refused"

refuse 'must be a non-negative integer' "$WORK/res4" "$WORK/out4.txt" FPMNG_PHPT_PORT_BASE=abc
refuse 'must be at least 1024' "$WORK/res5" "$WORK/out5.txt" FPMNG_PHPT_PORT_BASE=80
refuse 'must end below the ephemeral range' "$WORK/res6" "$WORK/out6.txt" FPMNG_PHPT_PORT_BASE=40000
echo "ok: a base below 1024, a non-numeric base, and a base reaching the ephemeral range are refused"

# Without a base nothing changed: the historical ports and the automatic shift,
# and -j again, because the lane is what serialises a run.
rm -rf "$WORK/res7" "$WORK/argv.txt"
set +e
STAND_IN_RESOLVES="$RESOLVED_FPM" ARGV_LOG="$WORK/argv.txt" \
TEST_PHP_EXECUTABLE="$CLI" TEST_PHP_FPM_EXECUTABLE="$FPM" \
    "$RUNNER" - "$WORK/res7" > "$WORK/out7.txt" 2>&1
set -e
grep -qx 'port_lane=none' "$WORK/res7/metadata.txt" ||
    fail 'a run without a base reports a lane'
grep -qE '^port_shift=[0-9]+$' "$WORK/res7/metadata.txt" ||
    fail 'a run without a base does not report a non-negative shift'
grep -qF 'Port shift:' "$WORK/out7.txt" ||
    fail 'a run without a base does not print the shift it chose'
grep -q -- '^-j' "$WORK/argv.txt" ||
    fail 'a run without a base is serial too; it should have passed -j'
echo "ok: without a base the run keeps the historical ports, the automatic shift and -j"

# The lane's private temporary root (issue #625). It has to be inside the results
# directory, so two lanes of the same tree cannot meet in /tmp, and it has to be
# the TMPDIR the tests are started with -- run-tests.php hands its own environment
# to every test, so exporting it in the runner is what reaches them.
lane_run "$BASE"
# The runner resolves the results directory, and on a box where the temporary
# directory is a symlink (/var -> /private/var on macOS) that is a different
# spelling of the same place.
expect_metadata "tmp_root=$(realpath "$WORK/res")/.tmp"
grep -qx "TMPDIR=$(realpath "$WORK/res")/.tmp" "$WORK/env.txt" ||
    fail 'the runner did not hand the tests a TMPDIR inside its results directory'
[ -d "$WORK/res/.tmp" ] && fail 'the lane left its temporary root behind after the run'
echo "ok: a lane run hands its tests a temporary root inside its results directory, and removes it again"

rm -rf "$WORK/res7" "$WORK/argv.txt" "$WORK/env.txt"
set +e
STAND_IN_RESOLVES="$RESOLVED_FPM" ARGV_LOG="$WORK/argv.txt" ENV_LOG="$WORK/env.txt" \
TEST_PHP_EXECUTABLE="$CLI" TEST_PHP_FPM_EXECUTABLE="$FPM" \
    "$RUNNER" - "$WORK/res7" > "$WORK/out7.txt" 2>&1
set -e
grep -qx 'port_lane=none' "$WORK/res7/metadata.txt" ||
    fail 'a run without a base reports a lane'
grep -qE '^port_shift=[0-9]+$' "$WORK/res7/metadata.txt" ||
    fail 'a run without a base does not report a non-negative shift'
grep -qF 'Port shift:' "$WORK/out7.txt" ||
    fail 'a run without a base does not print the shift it chose'
grep -q -- '^-j' "$WORK/argv.txt" ||
    fail 'a run without a base is serial too; it should have passed -j'
grep -qx "TMPDIR=$(realpath "$WORK/res7")/.tmp" "$WORK/env.txt" &&
    fail 'a run without a base moved TMPDIR into its results directory'
echo "ok: without a base the run keeps the historical ports, the automatic shift, -j and the system temporary directory"

echo "all port-lane checks passed"
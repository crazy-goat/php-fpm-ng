#!/bin/sh
# Task 074: measure amphp/mysql on a worker-mode HTTP-direct pool, using the
# Docker Compose example under examples/http-direct-worker-mysql.
#
# What this measures that build/test-http-direct-amphp.sh does not: Amp\delay()
# only exercises FPMNG_WORKER_TIMER, while a MySQL query is a TCP socket, so it
# needs an asynchronous connect (FPMNG_WORKER_WRITE, which no other test
# registers) and protocol reads on a socket rather than on the notify pipe. The
# TLS route additionally probes whether a stream that buffers above the
# descriptor breaks a watcher armed on the raw fd — the open question in
# fpmng_worker_event_create().
#
# Usage:
#   ./build/test-http-direct-worker-mysql.sh /path/to/php-fpm-ng
#
# Environment:
#   FPMNG_MYSQL_PORT      host port for the app container (default 28088)
#   FPMNG_MYSQL_N         concurrent SELECT SLEEP(1) requests (default 8)
#   FPMNG_MYSQL_PROJECT   Compose project name (default: unique per run)
#   FPMNG_MYSQL_KEEP      1 = leave the stack running for poking at by hand
#
# Exits 0 with SKIP where Docker or the Compose plugin is unavailable: the
# example pulls images and needs a daemon, so it cannot be a hard requirement.
set -eu

REPO="$(cd "$(dirname "$0")/.." && pwd)"
EXAMPLE="$REPO/examples/http-direct-worker-mysql"
# Deliberately not compose.yaml's own default of 28078, and not the fixed
# `name:` in compose.yaml either. The box is shared (workflow.md), and a fixed
# project name means `down --volumes` in this script's exit trap would delete
# the stack and volume of a hand-started example, or of a second concurrent run
# of this harness, rather than its own.
PORT="${FPMNG_MYSQL_PORT:-28088}"
N="${FPMNG_MYSQL_N:-8}"
PROJECT="${FPMNG_MYSQL_PROJECT:-fpmng-worker-mysql-harness-$$}"
# One second of SLEEP plus connect, TLS handshake and container scheduling. The
# claim under test is "about a second, not N seconds", so the budget only has
# to separate those two, not to be tight.
BUDGET=5

say() {
	echo "test-http-direct-worker-mysql.sh: $*"
}

fail() {
	say "FAIL: $*" >&2
	exit 1
}

skip() {
	say "SKIP: $*"
	exit 0
}

[ "$#" -eq 1 ] || fail "usage: $0 /path/to/php-fpm-ng"
BINARY="$1"
[ -x "$BINARY" ] || fail "not executable: $BINARY"

command -v docker >/dev/null 2>&1 || skip "docker not found in PATH"
docker compose version >/dev/null 2>&1 || skip "the docker compose plugin is not available"
command -v curl >/dev/null 2>&1 || fail "curl is required"
command -v python3 >/dev/null 2>&1 || fail "python3 is required (JSON assertions)"

# Confirm the binary under test before measuring anything: this project has
# already drawn a false conclusion from measuring someone else's build.
strings "$BINARY" 2>/dev/null | grep -q "php-fpm-ng/http-direct-worker" ||
	fail "$BINARY has no http-direct worker executor (built without sapi/fpmng/fpm/fpm_http_direct_worker.c?)"

# A function rather than a string: a repo path containing a space would word-split
# and silently target a different project directory or compose file.
compose() {
	docker compose -p "$PROJECT" --project-directory "$EXAMPLE" \
		-f "$EXAMPLE/compose.yaml" "$@"
}
STARTED=0
COPIED=0

cleanup() {
	status=$?
	set +e
	if [ "$STARTED" -eq 1 ] && [ "${FPMNG_MYSQL_KEEP:-0}" != 1 ]; then
		# --volumes, so the demo database goes away with the demo. Scoped to
		# this run's own project name, so it cannot reach anyone else's stack.
		compose down --volumes --remove-orphans >/dev/null 2>&1
	fi
	# Only if this run created it. README.md tells the operator to put their
	# built binary at exactly this path, and an unconditional rm here deleted
	# it on every SKIP path — before the cp had even run.
	[ "$COPIED" -eq 1 ] && rm -f "$EXAMPLE/php-fpm-ng"
	exit $status
}
trap cleanup EXIT
# $? is 0 inside a signal trap, so Ctrl-C would otherwise exit 0.
trap 'exit 130' INT
trap 'exit 143' TERM

# The binary has to be inside the build context; examples/*/.gitignore keeps it
# from being committed.
# Canonicalised rather than `test -ef`, which POSIX test does not have.
BINARY_ABS="$(cd "$(dirname "$BINARY")" && pwd)/$(basename "$BINARY")"
if [ "$BINARY_ABS" = "$EXAMPLE/php-fpm-ng" ]; then
	say "using the binary already in the build context"
else
	cp "$BINARY" "$EXAMPLE/php-fpm-ng"
	COPIED=1
fi
chmod +x "$EXAMPLE/php-fpm-ng"

say "building the app image and starting MySQL 8.4 (compose project $PROJECT)"
STARTED=1
# The client pool must be able to hold N at once, or the pool queues and the
# overlap assertion fails for a client-side reason.
APP_PORT="$PORT" MYSQL_POOL_MAX="$N" compose up -d --build --wait >/dev/null 2>&1 ||
	fail "docker compose up failed; rerun without redirection to see why"

say "waiting for the worker to answer on 127.0.0.1:$PORT"
i=0
while :; do
	hello=$(curl -fsS --max-time 2 "http://127.0.0.1:$PORT/" 2>/dev/null || true)
	case "$hello" in
		"hello world from pid "*) break ;;
	esac
	[ "$i" -lt 60 ] || fail "the worker never answered; container log:
$(compose logs --no-color app 2>&1 | tail -20)"
	i=$((i + 1))
	sleep 1
done
say "hello-world: ok ($(printf '%s' "$hello" | tr -d '\n'))"

# Each request gets its own curl so the N really are in flight at once.
measure() {
	route=$1
	label=$2
	out=$(mktemp -d)
	start=$(date +%s)
	pids=
	j=0
	while [ "$j" -lt "$N" ]; do
		curl -sS --max-time 30 "http://127.0.0.1:$PORT/$route?id=$j" -o "$out/$j" 2>/dev/null &
		pids="$pids $!"
		j=$((j + 1))
	done
	for pid in $pids; do
		wait "$pid" || true
	done
	elapsed=$(( $(date +%s) - start ))
	verdict=$(N="$N" ELAPSED="$elapsed" BUDGET="$BUDGET" LABEL="$label" python3 - "$out"/* <<'PY'
import json, os, sys

rows, errors = [], []
for path in sys.argv[1:]:
    with open(path) as handle:
        raw = handle.read()
    try:
        rows.append(json.loads(raw))
    except ValueError:
        errors.append("not JSON: " + raw[:200].replace("\n", " "))

n, elapsed, budget = int(os.environ["N"]), int(os.environ["ELAPSED"]), int(os.environ["BUDGET"])
for row in rows:
    if "error" in row:
        errors.append(row["error"])
if errors:
    print("ERROR " + errors[0])
elif len(rows) != n:
    print("ERROR only %d of %d responses came back" % (len(rows), n))
elif len({row["pid"] for row in rows}) != 1:
    print("ERROR more than one worker served the batch: %s" % sorted({r["pid"] for r in rows}))
elif elapsed > budget:
    print("ERROR %d requests took %ds, budget %ds (serialized?)" % (n, elapsed, budget))
elif os.environ["LABEL"] == "tls" and not all(row.get("cipher") for row in rows):
    # amphp masks CLIENT_SSL off without an error when the server does not
    # advertise it (vendor/amphp/mysql/src/Internal/ConnectionProcessor.php:1556-1572),
    # so without this the TLS route could be plaintext and every other
    # assertion here would still pass. Ssl_cipher is the server's own view of
    # the session that ran the sleep.
    print("ERROR /mysql-tls ran in plaintext: Ssl_cipher empty on %d of %d sessions"
          % (sum(1 for row in rows if not row.get("cipher")), len(rows)))
elif os.environ["LABEL"] == "plain" and any(row.get("cipher") for row in rows):
    print("ERROR /mysql was encrypted (Ssl_cipher %r); the two routes are not distinct"
          % sorted({row.get("cipher") for row in rows})[0])
else:
    # The load-bearing assertion, as in fpmng-http-direct-worker.phpt: the last
    # query to start did so before the first one finished, proven from the
    # worker's own clock rather than from wall time.
    last_start, first_end = max(r["t0"] for r in rows), min(r["t1"] for r in rows)
    if last_start >= first_end:
        print("ERROR serialized: last start %.6f >= first end %.6f" % (last_start, first_end))
    else:
        cipher = rows[0].get("cipher") or "none"
        print("OK %d requests in %ds on pid %d; t0 spread %.1fms, overlap %.3fs, cipher %s" % (
            n, elapsed, rows[0]["pid"],
            (max(r["t0"] for r in rows) - min(r["t0"] for r in rows)) * 1000,
            first_end - last_start, cipher))
PY
	)
	rm -rf "$out"
	echo "$verdict"
}

verdict=$(measure mysql plain)
case "$verdict" in
	OK*) say "concurrent-mysql: ok (${verdict#OK })" ;;
	*) fail "concurrent SELECT SLEEP(1) over plain TCP: ${verdict#ERROR }" ;;
esac

# TLS is a measurement, not a requirement: task 074 accepts either result as
# long as it is recorded. A failure here is reported and the harness still
# passes, because the plain-socket claim above is the acceptance criterion.
verdict=$(measure mysql-tls tls)
case "$verdict" in
	OK*) say "concurrent-mysql-tls: ok (${verdict#OK })" ;;
	*) say "concurrent-mysql-tls: NOT WORKING (${verdict#ERROR })"
	   say "  ^ recorded, not fatal: the watcher is armed on the raw fd from"
	   say "    php_stream_cast(), so a TLS stream can buffer above it."
	   say "    Consequence: this harness is not a regression gate for the TLS"
	   say "    claim in tasks/done/074-*.md — read this line, do not read PASS." ;;
esac

# A worker that wedged would still have answered above, so check the log for
# the diagnostics this executor emits when it gives up on a request. Written as
# an if, because `grep && fail` would exit under set -e on the passing path.
if diagnostics=$(compose logs --no-color app 2>&1 | grep -E "unanswered|returned without being asked|SIGSEGV|signal 11"); then
	fail "the worker logged a problem:
$diagnostics"
fi

say "PASS"

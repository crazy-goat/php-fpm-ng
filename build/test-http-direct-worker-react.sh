#!/bin/sh
# Task 075: measure ReactPHP on a worker-mode HTTP-direct pool, using the
# Docker Compose example under examples/http-direct-worker-react.
#
# What this measures that build/test-http-direct-worker-mysql.sh does not: that
# harness drives the worker's primitives through Revolt, which is the only
# consumer they have ever had. This one drives them through
# React\EventLoop\LoopInterface, an unrelated twelve-method contract, with an
# application written in promises and no fibers anywhere. Three assertions:
#
#   concurrent-mysql  N concurrent SELECT SLEEP(1) through react/mysql overlap
#                     inside one worker: asynchronous connect plus socket reads.
#   future-tick       K futureTick() callbacks drain on the next iteration and
#                     not when a pending 10 s timer is due. No Revolt analogue,
#                     so nothing before this task tested it.
#   strand            ReactPHP's own read path at a 1 KiB chunk, below one
#                     16 KiB TLS record, against an unthrottled body: the
#                     narrow case task 079 fixed, which a throttled origin
#                     cannot reach. The one gate here that fails if the loop
#                     stops looking inside userland read buffers.
#   tls               An HTTPS body far above ReactPHP's 64 KiB read chunk,
#                     rate-limited to about a second by the origin. ReactPHP
#                     reads once per readable event with no speculative read
#                     (react/stream, DuplexResourceStream::handleData()), which
#                     is the pattern task 074 recorded as one that "would still
#                     hang" on a watcher armed on the raw fd from
#                     php_stream_cast().
#
# Usage:
#   ./build/test-http-direct-worker-react.sh /path/to/php-fpm-ng
#
# Environment:
#   FPMNG_REACT_PORT      host port for the app container (default 28098)
#   FPMNG_REACT_N         concurrent SELECT SLEEP(1) requests (default 8)
#   FPMNG_REACT_TLS_N     concurrent HTTPS requests (default 4)
#   FPMNG_REACT_TICKS     futureTick callbacks to drain (default 5000)
#   FPMNG_REACT_CHUNK     read chunk for the /strand probe, bytes (default 1024)
#   FPMNG_REACT_PROJECT   Compose project name (default: unique per run)
#   FPMNG_REACT_KEEP      1 = leave the stack running for poking at by hand
#
# Exits 0 with SKIP where Docker or the Compose plugin is unavailable: the
# example pulls images and needs a daemon, so it cannot be a hard requirement.
set -eu

REPO="$(cd "$(dirname "$0")/.." && pwd)"
EXAMPLE="$REPO/examples/http-direct-worker-react"
# Deliberately not compose.yaml's own default of 28068, and not the fixed
# `name:` in compose.yaml either. The box is shared (workflow.md), and a fixed
# project name means `down --volumes` in this script's exit trap would delete
# the stack and volume of a hand-started example, or of a second concurrent run
# of this harness, rather than its own.
PORT="${FPMNG_REACT_PORT:-28098}"
N="${FPMNG_REACT_N:-8}"
TLS_N="${FPMNG_REACT_TLS_N:-4}"

# Rejected here rather than guarded at the assertion: with one request there is
# nothing to overlap, and a `n > 1` guard around max(t0) < min(t1) would let
# FPMNG_REACT_N=1 print PASS without ever evaluating the criterion this harness
# exists to check.
for v in "$N" "$TLS_N"; do
	case "$v" in
		''|*[!0-9]*) echo "test-http-direct-worker-react.sh: request counts must be numeric, got '$v'" >&2; exit 2 ;;
	esac
	[ "$v" -ge 2 ] || { echo "test-http-direct-worker-react.sh: request counts must be >= 2 to prove concurrency, got $v" >&2; exit 2; }
done
TICKS="${FPMNG_REACT_TICKS:-5000}"
CHUNK="${FPMNG_REACT_CHUNK:-1024}"
PROJECT="${FPMNG_REACT_PROJECT:-fpmng-worker-react-harness-$$}"
# One second of SLEEP plus connect and container scheduling. The claim under
# test is "about a second, not N seconds", so the budget only has to separate
# those two, not to be tight.
BUDGET=5
# The origin serves 1 MiB at 1 MiB/s, so one request is about a second; the
# same reasoning as BUDGET, with room for the TLS handshakes.
TLS_BUDGET=8

say() {
	echo "test-http-direct-worker-react.sh: $*"
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

# A function rather than a string: a repo path containing a space would
# word-split and silently target a different project directory or compose file.
compose() {
	docker compose -p "$PROJECT" --project-directory "$EXAMPLE" \
		-f "$EXAMPLE/compose.yaml" "$@"
}
STARTED=0
COPIED=0

cleanup() {
	status=$?
	set +e
	if [ "$STARTED" -eq 1 ] && [ "${FPMNG_REACT_KEEP:-0}" != 1 ]; then
		# --volumes, so the demo database goes away with the demo. Scoped to
		# this run's own project name, so it cannot reach anyone else's stack.
		compose down --volumes --remove-orphans >/dev/null 2>&1
	fi
	# Only if this run created it. README.md tells the operator to put their
	# built binary at exactly this path, and an unconditional rm here would
	# delete it on every SKIP path — before the cp had even run.
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

say "building the app and origin images and starting MySQL 8.4 (compose project $PROJECT)"
STARTED=1
APP_PORT="$PORT" compose up -d --build --wait >/dev/null 2>&1 ||
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
# $1 route (with query string), $2 how many, $3 budget in seconds, $4 label
measure() {
	route=$1
	count=$2
	budget=$3
	label=$4
	out=$(mktemp -d)
	start=$(date +%s)
	pids=
	j=0
	while [ "$j" -lt "$count" ]; do
		case "$route" in
			*\?*) url="http://127.0.0.1:$PORT/$route&id=$j" ;;
			*) url="http://127.0.0.1:$PORT/$route?id=$j" ;;
		esac
		curl -sS --max-time 60 "$url" -o "$out/$j" 2>/dev/null &
		pids="$pids $!"
		j=$((j + 1))
	done
	for pid in $pids; do
		wait "$pid" || true
	done
	elapsed=$(( $(date +%s) - start ))
	verdict=$(N="$count" ELAPSED="$elapsed" BUDGET="$budget" LABEL="$label" python3 - "$out"/* <<'PY'
import json, os, sys

rows, errors = [], []
for path in sys.argv[1:]:
    with open(path) as handle:
        raw = handle.read()
    try:
        rows.append(json.loads(raw))
    except ValueError:
        errors.append("not JSON: " + raw[:200].replace("\n", " "))

n = int(os.environ["N"])
elapsed, budget, label = int(os.environ["ELAPSED"]), int(os.environ["BUDGET"]), os.environ["LABEL"]
for row in rows:
    if row.get("error"):
        errors.append(row["error"])
if errors:
    print("ERROR " + errors[0])
elif len(rows) != n:
    print("ERROR only %d of %d responses came back" % (len(rows), n))
elif len({row["pid"] for row in rows}) != 1:
    print("ERROR more than one worker served the batch: %s" % sorted({r["pid"] for r in rows}))
elif elapsed > budget:
    print("ERROR %d requests took %ds, budget %ds (serialized?)" % (n, elapsed, budget))
elif label == "tls" and not all(row.get("complete") for row in rows):
    print("ERROR incomplete TLS bodies: %s" % [(r["bytes"], r["length"]) for r in rows])
else:
    spread = (max(r["t0"] for r in rows) - min(r["t0"] for r in rows)) * 1000
    last_start, first_end = max(r["t0"] for r in rows), min(r["t1"] for r in rows)
    extra = ""
    if label == "tls":
        extra = ", bytes %d in %d reads (min across the batch: %d reads)" % (
            rows[0]["bytes"], rows[0]["reads"], min(r["reads"] for r in rows))
        # A body of 1 MiB collected in one or two reads would mean the origin
        # was not throttling, and the buffered-stream question would not have
        # been asked at all.
        if min(r["reads"] for r in rows) < 4:
            print("ERROR the body arrived in %d reads; the origin is not throttling, "
                  "so this run does not probe a buffered TLS stream"
                  % min(r["reads"] for r in rows))
            raise SystemExit(0)
    if last_start >= first_end:
        # The load-bearing assertion, as in fpmng-http-direct-worker.phpt: the
        # last request to start did so before the first one finished, proven
        # from the worker's own clock rather than from wall time.
        print("ERROR serialized: last start %.6f >= first end %.6f" % (last_start, first_end))
    else:
        print("OK %d requests in %ds on pid %d; t0 spread %.1fms, overlap %.3fs%s" % (
            n, elapsed, rows[0]["pid"], spread, first_end - last_start, extra))
PY
	)
	rm -rf "$out"
	echo "$verdict"
}

verdict=$(measure mysql "$N" "$BUDGET" plain)
case "$verdict" in
	OK*) say "concurrent-mysql: ok (${verdict#OK })" ;;
	*) fail "concurrent SELECT SLEEP(1) through react/mysql: ${verdict#ERROR }" ;;
esac

# One request, so the number that matters is its own duration: the route arms a
# 10 s timer before queueing the ticks, so a loop that blocked in libevent
# instead of draining its queue answers in about 10 s, not in milliseconds.
say "draining $TICKS futureTick callbacks"
ticks_json=$(curl -sS --max-time 60 "http://127.0.0.1:$PORT/ticks?n=$TICKS" 2>/dev/null || true)
verdict=$(printf '%s' "$ticks_json" | TICKS="$TICKS" python3 -c '
import json, os, sys

raw = sys.stdin.read()
try:
    row = json.loads(raw)
except ValueError:
    print("ERROR not JSON: " + raw[:200].replace("\n", " "))
    raise SystemExit(0)
if row.get("error"):
    print("ERROR " + row["error"])
elif row.get("ticks") != int(os.environ["TICKS"]):
    print("ERROR asked for %s ticks, route ran %s" % (os.environ["TICKS"], row.get("ticks")))
else:
    took = row["t1"] - row["t0"]
    # Half the blocker is the only threshold that matters: below it the queue
    # was drained without asking libevent to sleep, at or above it the loop
    # waited for the timer.
    if took >= row["blocker"] / 2:
        print("ERROR %d ticks took %.3fs against a %.1fs blocker: the tick queue "
              "was not drained before the loop blocked" % (row["ticks"], took, row["blocker"]))
    else:
        print("OK %d ticks in %.1fms on pid %d, blocker %.1fs never fired" % (
            row["ticks"], took * 1000, row["pid"], row["blocker"]))
')
case "$verdict" in
	OK*) say "future-tick: ok (${verdict#OK })" ;;
	*) fail "futureTick(): ${verdict#ERROR }" ;;
esac

# Still a measurement rather than a requirement, but for a different reason
# than in task 075: what made it unreliable then — TLS bytes stranded above the
# descriptor — is gated by tls-strand below since task 079. All that can fail
# here now is the concurrency budget on a loaded box.
verdict=$(measure tls "$TLS_N" "$TLS_BUDGET" tls)
case "$verdict" in
	OK*) say "concurrent-tls: ok (${verdict#OK })" ;;
	*) say "concurrent-tls: NOT WORKING (${verdict#ERROR })"
	   say "  ^ recorded, not fatal, because the failure this route used to"
	   say "    expose is now gated by tls-strand below: since task 079 the"
	   say "    worker loop re-casts every watched stream before it sleeps, so"
	   say "    TLS bytes cannot strand above the descriptor any more. What is"
	   say "    left here is a concurrency budget, which a loaded box can miss"
	   say "    without anything being wrong. Read tls-strand for the claim." ;;
esac

# The narrow case, and a hard gate since task 079. One request: what matters is
# not concurrency but whether a client that cannot drain a TLS record per
# readable event still finishes the body. Before 079 it could not, and this
# probe only recorded that; fpm_worker_activate_buffered() now re-casts each
# watched stream once per loop iteration and activates the watcher while PHP's
# read buffer is non-empty, so stranding is a regression rather than a
# known limitation.
# Prints one verdict line and nothing else: its caller captures stdout, so a
# progress message from in here would be read as the verdict.
strand() {
	keep=$1
	printf '%s' "$(curl -sS --max-time 60 "http://127.0.0.1:$PORT/strand?chunk=$CHUNK&keep=$keep&id=0" 2>/dev/null || true)" | python3 -c '
import json, sys

raw = sys.stdin.read()
try:
    row = json.loads(raw)
except ValueError:
    print("ERROR not JSON: " + raw[:200].replace("\n", " "))
    raise SystemExit(0)
if row.get("error"):
    print("ERROR " + row["error"])
elif row["body"] < 0:
    print("ERROR no Content-Length from the origin after %d bytes" % row["bytes"])
elif row["bytes"] - row["overhead"] < row["body"]:
    print("ERROR %d of %d body bytes in %d reads of %d"
          % (row["bytes"] - row["overhead"], row["body"], row["reads"], row["chunk"]))
elif row["reads"] * row["chunk"] < row["body"]:
    # Sanity, not TLS: if the body arrived in fewer reads than its size allows
    # at this chunk, the chunk size did not apply and the probe proved nothing.
    print("ERROR %d bytes in only %d reads of %d; the chunk size did not apply"
          % (row["bytes"], row["reads"], row["chunk"]))
else:
    print("OK %d body bytes in %d reads of %d on pid %d in %.3fs" % (
        row["body"], row["reads"], row["chunk"], row["pid"], row["t1"] - row["t0"]))
'
}

# Both cases must pass. The first could never strand and is here to show why:
# 1 MiB keeps the kernel receive buffer non-empty, so the descriptor stays
# readable no matter how slowly the client reads. The second is the narrow case
# task 079 fixed — 8 KiB on a connection the origin keeps open, so there is
# neither more data nor a FIN to make the descriptor readable again, and the
# only thing that can finish the body is the loop looking inside the stream's
# userland buffer. Measured before the fix, at this 1 KiB chunk: 769 of 8192
# body bytes after 1 read (task 075, done, has the full table; see docs/task-archive.md).
for keep in 0 1; do
	say "probing for a stranded TLS buffer at a ${CHUNK}-byte read chunk (keep-alive=$keep)"
	verdict=$(strand "$keep")
	case "$verdict" in
		OK*) say "tls-strand(keep-alive=$keep): ok (${verdict#OK })" ;;
		# The pre-079 symptom, so say what it means rather than only that it
		# failed: the loop stopped looking inside userland read buffers, and
		# every TLS consumer that reads once per readable event silently hangs
		# on a body it will never finish.
		*) fail "tls-strand(keep-alive=$keep): STRANDED (${verdict#ERROR }); task 079 regressed — the worker loop is no longer activating read watchers for bytes sitting in a stream's userland buffer" ;;
	esac
done

# A worker that wedged would still have answered above, so check the log for
# the diagnostics this executor emits when it gives up on a request. Written as
# an if, because `grep && fail` would exit under set -e on the passing path.
# Collected before grepping: piping `compose logs` straight into grep would let
# a failure of the log command itself read as "no diagnostics found".
worker_log=$(mktemp)
compose logs --no-color app >"$worker_log" 2>&1 || fail "could not read the worker log"
if diagnostics=$(grep -E "unanswered|returned without being asked|SIGSEGV|signal 11|tick queue was not drained" "$worker_log"); then
	rm -f "$worker_log"
	fail "the worker logged a problem:
$diagnostics"
fi
rm -f "$worker_log"

say "PASS"

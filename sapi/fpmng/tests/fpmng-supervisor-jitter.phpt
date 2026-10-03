--TEST--
fpm-ng: supervisor.start_jitter/supervisor.restart_jitter spread cold starts and
restarts within bounds, without breaking restart_max/backoff accounting (issue #323)
--SKIPIF--
<?php include "skipif.inc"; ?>
--ENV--
FPMNG_DEBUG_CLOCK_RATE=10
--FILE--
<?php

require_once "tester.inc";
require_once "fpmng-operator.inc";

/* FPMNG_DEBUG_CLOCK_RATE above (issue #398). Every time this test asserts on is
 * one the MASTER measured: the pool's status page reports last_start, which a
 * supervisor child stamps with FPM_NOW() -- the scaled clock -- when an
 * iteration begins. The iteration script records no time at all. It used to
 * write microtime(true) and the test compared the deltas to the jitter bounds,
 * a real reading against a virtual bound, which is why it could not take a
 * rate (docs/fpmng-phpt.md, "The virtual clock") and cost 25 s. Every deadline
 * below stays in REAL seconds and stays generous, so a binary built without
 * --enable-fpmng-debug-clock ignores the rate and the test still passes at
 * real speed.
 *
 * last_start is whole seconds of the scaled clock, so every figure below has
 * one second of quantisation, and the bounds allow for it.
 *
 * Four pools, one FPM instance, run concurrently so this test pays FPM's
 * startup/shutdown cost once:
 *
 *   ref    -- supervisor.restart = never, script exits 0. Starts at once, with
 *             no jitter: its last_start is the master's own "pool start" on
 *             the scaled clock, the zero the cold offsets are measured from
 *             (the test process has no way to read that clock).
 *   cold   -- supervisor.processes = 4, supervisor.start_jitter = 9, restart =
 *             on-failure, script always exits 1. restart = never is
 *             deliberately NOT used here: shared->terminal (the "stop
 *             restarting this pool" flag apply_policy sets) is pool-wide, not
 *             per-copy, so the instant the fastest of the 4 copies finished
 *             its one-and-only run, the other 3 -- some of them possibly
 *             still waiting out their own start_jitter delay -- would see
 *             shared->terminal already set and park without ever running
 *             their script at all (see fpm_pool_supervisor_child_main()'s
 *             top-of-loop check). restart = on-failure with an always-failing
 *             script keeps every copy retrying instead, so terminal is never
 *             set pool-wide and all 4 copies get to run. restart_delay is
 *             600 so that, once the first copy fails, no copy starts a SECOND
 *             time inside the test: last_start is one pool-wide stamp, and the
 *             distinct values it takes while the 4 copies cold-start are the
 *             4 first-run seconds, polled fast enough (every 25 ms of real
 *             time, against a one-second resolution) not to miss one.
 *             Checks: all 4 copies eventually start, each one's start falls
 *             inside [0, start_jitter] of ref's plus generous slack, and they
 *             do not all land on the same second (the regression this test
 *             exists for: a rand()/srand() implementation would give every
 *             child forked from the same master the identical "random" delay
 *             -- see fpm_pool_supervisor_jitter()'s comment).
 *   rjsec  -- supervisor.restart_jitter = 6 (plain seconds), restart_delay =
 *             restart_delay_max = 2 (fixed, no exponential growth to
 *             untangle), restart = always, script always fails. Checks: the
 *             gap between consecutive last_start values is always about
 *             >= restart_delay and never exceeds restart_delay +
 *             restart_jitter (plus slack), and is not ALWAYS the same across 6
 *             consecutive gaps -- not 3, because an integer jitter drawn from
 *             7 possible values landing on the same one 2 times running is a
 *             1-in-49 event, too likely for a gate that runs on every PR; 6
 *             identical draws in a row is ~1-in-160000.
 *   rjpct  -- same as rjsec but supervisor.restart_jitter = "50%" and
 *             restart_delay = restart_delay_max = 4, to exercise the
 *             percentage form: bound becomes [4, 4 + 4*50% ] = [4, 6]. Only
 *             the bound is asserted here (3 gaps), not variation -- the
 *             narrower 3-value range (0, 1, 2) makes a variation check on
 *             this few draws flaky for the same reason rjsec needed more.
 *
 * None of this waits on cron's once-a-minute schedule tick, so the whole test
 * fits comfortably inside build/run-fpmng-phpt.sh's 120s TEST_FPM_TIMEOUT
 * (matched by CI's build-matrix.yml) even at rate 1. */

$work = sys_get_temp_dir() . '/fpmng-sup-jitter-' . getmypid();
@mkdir($work, 0700, true);

$coldLog = "$work/cold-runs.log";
@unlink($coldLog);

/* The cold pool only needs to say "I started": one APPENDED pid line per run,
 * LOCK_EX so four concurrent copies never interleave a partial line. It is
 * how the test knows all 4 first starts have happened, and carries no time.
 *
 * Each copy then WAITS until all 4 lines are there before it exits 1. Without
 * that, a copy that drew a start delay of 0 could run and fail while a sibling
 * had not yet reached the top of its loop; the failure sets the pool-wide
 * next_allowed_start (restart_delay = 600 below), and that sibling would then
 * wait out the 600 seconds instead of its start_jitter draw -- seen as one run
 * in four on the test box. The 30 s wait is real time and only ever a bound on
 * a hang. */
file_put_contents("$work/cold.php", <<<PHP
<?php
error_reporting(0);
@file_put_contents('{$coldLog}', getmypid() . "\\n", FILE_APPEND | LOCK_EX);
for (\$i = 0; \$i < 600; \$i++) {
    \$lines = @file('{$coldLog}', FILE_IGNORE_NEW_LINES | FILE_SKIP_EMPTY_LINES);
    if (\$lines !== false && count(\$lines) >= 4) {
        break;
    }
    usleep(50000);
}
exit(1);
PHP);
file_put_contents("$work/ref.php", "<?php\nexit(0);\n");
file_put_contents("$work/fail.php", "<?php\nexit(1);\n");

/* 10 possible integer draws (0..9), not 4 (0..3): with only 4 copies and 4
 * possible values, all 4 landing on the same draw is a 1-in-64 event -- too
 * likely for a check that runs on every PR (the same standard applied to
 * rjsec's variation check below). With 10 values the same coincidence is
 * 1-in-1000. */
$startJitter = 9;
$rjSecDelay = 2;
$rjSecJitter = 6;
$rjPctDelay = 4;
$rjPctPercent = 50;

$cfg = <<<EOT
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
daemonize = no

[ref]
pool.type = supervisor
supervisor.script = $work/ref.php
supervisor.processes = 1
supervisor.restart = never
operator.status_listen = {{ADDR[operator]}}
operator.status_path = /ref-status

[cold]
pool.type = supervisor
supervisor.script = $work/cold.php
supervisor.processes = 4
supervisor.restart = on-failure
supervisor.restart_delay = 600
supervisor.restart_delay_max = 600
supervisor.start_jitter = $startJitter
operator.status_listen = {{ADDR[operator]}}
operator.status_path = /cold-status

[rjsec]
pool.type = supervisor
supervisor.script = $work/fail.php
supervisor.processes = 1
supervisor.restart = always
supervisor.restart_delay = $rjSecDelay
supervisor.restart_delay_max = $rjSecDelay
supervisor.restart_max = 0
supervisor.restart_jitter = $rjSecJitter
operator.status_listen = {{ADDR[operator]}}
operator.status_path = /rjsec-status

[rjpct]
pool.type = supervisor
supervisor.script = $work/fail.php
supervisor.processes = 1
supervisor.restart = always
supervisor.restart_delay = $rjPctDelay
supervisor.restart_delay_max = $rjPctDelay
supervisor.restart_max = 0
supervisor.restart_jitter = {$rjPctPercent}%
operator.status_listen = {{ADDR[operator]}}
operator.status_path = /rjpct-status
EOT;

function poolStatus(string $operator, string $pool): array
{
    $json = json_decode(fpmng_operator_body($operator, "/$pool-status"), true, flags: JSON_THROW_ON_ERROR);
    return $json['pools'][0];
}

function coldRunCount(string $log): int
{
    return is_file($log) ? count(file($log, FILE_IGNORE_NEW_LINES | FILE_SKIP_EMPTY_LINES)) : 0;
}

/* Consecutive differences of an ordered list of start stamps. */
function gaps(array $starts): array
{
    $out = [];
    for ($i = 1; $i < count($starts); $i++) {
        $out[] = $starts[$i] - $starts[$i - 1];
    }
    return $out;
}

/* Appends $start to $seen when it is a new, later stamp: the pool's last_start
 * only ever moves forward, and every start of a one-process pool whose
 * restart_delay is at least 2 lands on a different second. */
function recordStart(array &$seen, mixed $start): bool
{
    if (is_int($start) && $start > 0 && ($seen === [] || $start > end($seen))) {
        $seen[] = $start;
        return true;
    }
    return false;
}

$tester = new FPM\Tester($cfg, '<?php');
try {
    $tester->start(extraArgs: ['-R'], forceStderr: true, daemonize: false);
    $tester->expectLogStartNotices();

    $operator = $tester->getListen('{{ADDR[operator]}}');

    /* One loop reads every pool's master-reported last_start, so the pools
     * are observed concurrently exactly as they run. Real-second deadline. */
    $slack = 6.0; /* master ready -> fork -> PHP request startup; scaled by the rate, hence generous */
    $refStart = 0;
    $coldStarts = [];
    $coldDone = false;
    $rjSecStarts = [];
    $rjSecFailures = null;
    $rjPctStarts = [];
    $deadline = time() + 80;
    do {
        /* Read the marker BEFORE the status: a copy writes shared->last_start
         * before its script runs, so once the marker shows 4 runs, the status
         * read just below already includes all 4 stamps. */
        $coldComplete = coldRunCount($coldLog) >= 4;
        if ($refStart === 0) {
            $refStart = poolStatus($operator, 'ref')['last_start'] ?? 0;
        }
        if (!$coldDone) {
            $start = poolStatus($operator, 'cold')['last_start'] ?? 0;
            if (is_int($start) && $start > 0 && !in_array($start, $coldStarts, true)) {
                $coldStarts[] = $start;
            }
            $coldDone = $coldComplete;
        }
        if (count($rjSecStarts) < 7) {
            $status = poolStatus($operator, 'rjsec');
            if (recordStart($rjSecStarts, $status['last_start'] ?? 0) && count($rjSecStarts) === 7) {
                $rjSecFailures = $status['consecutive_failures'];
            }
        }
        if (count($rjPctStarts) < 4) {
            recordStart($rjPctStarts, poolStatus($operator, 'rjpct')['last_start'] ?? 0);
        }
        if ($refStart > 0 && $coldDone && count($rjSecStarts) >= 7 && count($rjPctStarts) >= 4) {
            break;
        }
        usleep(25000);
    } while (time() < $deadline);

    /* --- cold: supervisor.start_jitter --- */
    if (!$coldDone || $refStart === 0) {
        echo "FAIL: cold pool only produced " . coldRunCount($coldLog) . "/4 cold starts (ref start $refStart)\n";
        $tester->close(true);
        exit(1);
    }
    $offsets = array_map(static fn ($t) => $t - $refStart, $coldStarts);
    /* -1: ref and the cold copies are forked a few ms apart, which can straddle
     * a second boundary of the whole-second stamp. */
    if (min($offsets) < -1 || max($offsets) > $startJitter + $slack) {
        echo "FAIL: cold start offset(s) outside [-1, start_jitter + slack]: " . implode(',', $offsets) . "\n";
        $tester->close(true);
        exit(1);
    }
    if (count($coldStarts) < 2) {
        /* The regression this guards: a rand()/srand()-based implementation
         * seeds once, in whichever process (often the master) calls it
         * first, and every child forked afterwards inherits that exact
         * sequence position -- all 4 copies would then start in the same
         * second regardless of start_jitter. */
        echo "FAIL: cold starts did not spread out (all in second " . $coldStarts[0] . ")\n";
        $tester->close(true);
        exit(1);
    }
    echo "cold start bounded and spread: ok\n";

    /* --- rjsec: supervisor.restart_jitter = 6 (seconds) ---
     * 7 runs (6 gaps), not 4 (3 gaps): with an integer jitter drawn from 7
     * possible values [0, 6], a correct implementation still has a 1-in-49
     * chance of drawing the same value on 2 consecutive gaps out of only 3 --
     * a flaky false failure, not a bug. 6 gaps drawn independently all landing
     * on the same value has probability 1/7^5 (~0.006%), which is the bound
     * this test actually wants to catch (the jitter being inert, e.g. a
     * rand()/srand() collapse across processes -- moot here since this pool
     * has a single process, but the same collapse would also make one
     * process's own successive draws identical). */
    if (count($rjSecStarts) < 7) {
        echo "FAIL: rjsec pool only produced " . count($rjSecStarts) . "/7 runs\n";
        $tester->close(true);
        exit(1);
    }
    $rjSecGaps = gaps($rjSecStarts);
    /* One whole second of quantisation each way (both ends are floored), plus
     * the slack on the upper side for fork/startup cost. */
    if (min($rjSecGaps) < $rjSecDelay - 1 || max($rjSecGaps) > $rjSecDelay + $rjSecJitter + 1 + $slack) {
        echo "FAIL: rjsec gaps out of bounds [" . ($rjSecDelay - 1) . ", " . ($rjSecDelay + $rjSecJitter + 1 + $slack) .
            "]: " . implode(',', $rjSecGaps) . "\n";
        $tester->close(true);
        exit(1);
    }
    if (max($rjSecGaps) === min($rjSecGaps)) {
        echo "FAIL: rjsec gaps never varied -- restart_jitter appears inert: " . implode(',', $rjSecGaps) . "\n";
        $tester->close(true);
        exit(1);
    }
    echo "restart_jitter (seconds) bounded and varying: ok\n";

    /* restart_max/backoff accounting: every run here is a real failure (exit
     * 1), and nothing about jitter should change how failures are counted --
     * it only changes the delay applied between them. Read in the same status
     * response that showed the 7th start, so the pool has not run on since. */
    if ($rjSecFailures < 6 || $rjSecFailures > 8) {
        echo "FAIL: consecutive_failures ($rjSecFailures) does not track " .
            "the observed run count (7)\n";
        $tester->close(true);
        exit(1);
    }
    echo "restart_max/backoff accounting unaffected: ok\n";

    /* --- rjpct: supervisor.restart_jitter = 50% of restart_delay --- */
    if (count($rjPctStarts) < 4) {
        echo "FAIL: rjpct pool only produced " . count($rjPctStarts) . "/4 runs\n";
        $tester->close(true);
        exit(1);
    }
    $rjPctGaps = gaps($rjPctStarts);
    $pctJitterMax = (int) floor($rjPctDelay * $rjPctPercent / 100);
    if (min($rjPctGaps) < $rjPctDelay - 1 || max($rjPctGaps) > $rjPctDelay + $pctJitterMax + 1 + $slack) {
        echo "FAIL: rjpct gaps out of bounds [" . ($rjPctDelay - 1) . ", " . ($rjPctDelay + $pctJitterMax + 1 + $slack) .
            "]: " . implode(',', $rjPctGaps) . "\n";
        $tester->close(true);
        exit(1);
    }
    echo "restart_jitter (percentage) bounded: ok\n";

    echo "Done\n";
} finally {
    $tester->terminate();
    $tester->close();
    @unlink($coldLog);
    @unlink("$work/cold.php");
    @unlink("$work/ref.php");
    @unlink("$work/fail.php");
    @rmdir($work);
}
?>
--EXPECT--
cold start bounded and spread: ok
restart_jitter (seconds) bounded and varying: ok
restart_max/backoff accounting unaffected: ok
restart_jitter (percentage) bounded: ok
Done
--CLEAN--
<?php
require_once "tester.inc";
FPM\Tester::clean();
/* Whatever the run above leaked: start() failing or an expectation throwing
 * skips the cleanup in --FILE--, and these directories are named after a pid
 * this process does not know. */
$stale = time() - 300;
foreach (glob(sys_get_temp_dir() . '/fpmng-sup-jitter-*') as $dir) {
    if (@filemtime($dir) > $stale) {
        continue;
    }
    foreach (glob("$dir/*") as $file) {
        @unlink($file);
    }
    @rmdir($dir);
}
?>

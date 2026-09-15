--TEST--
fpm-ng: supervisor.start_jitter/supervisor.restart_jitter spread cold starts and
restarts within bounds, without breaking restart_max/backoff accounting (issue #323)
--SKIPIF--
<?php include "skipif.inc"; ?>
--FILE--
<?php

require_once "tester.inc";
require_once "fpmng-operator.inc";

/* Three pools, one FPM instance, run concurrently so this test pays FPM's
 * startup/shutdown cost once:
 *
 *   cold   -- supervisor.processes = 4, supervisor.start_jitter = 3, restart =
 *             on-failure, script always exits 1. restart = never is
 *             deliberately NOT used here: shared->terminal (the "stop
 *             restarting this pool" flag apply_policy sets) is pool-wide, not
 *             per-copy, so the instant the fastest of the 4 copies finished
 *             its one-and-only run, the other 3 -- some of them possibly
 *             still waiting out their own start_jitter delay -- would see
 *             shared->terminal already set and park without ever running
 *             their script at all (see fpm_pool_supervisor_child_main()'s
 *             top-of-loop check). restart = on-failure with an always-failing
 *             script keeps every copy retrying forever instead, so terminal
 *             is never set pool-wide and all 4 copies get to run. Only the
 *             FIRST run of each copy is start_jitter's concern, so only the
 *             first logged line per pid is used below. Checks: all 4 copies
 *             eventually start, each one's start falls inside [0,
 *             start_jitter] plus generous scheduling slack, and they do not
 *             all land on the same instant (the regression this test exists
 *             for: a rand()/srand() implementation would give every child
 *             forked from the same master the identical "random" delay --
 *             see fpm_pool_supervisor_jitter()'s comment).
 *   rjsec  -- supervisor.restart_jitter = 6 (plain seconds), restart_delay =
 *             restart_delay_max = 2 (fixed, no exponential growth to
 *             untangle), restart = always, script always fails. Checks: the
 *             gap between consecutive runs is always >= restart_delay and
 *             never exceeds restart_delay + restart_jitter (plus slack), and
 *             is not ALWAYS exactly restart_delay (which would mean the
 *             jitter never fired) across 6 consecutive gaps -- not 3, because
 *             an integer jitter drawn from 7 possible values landing on the
 *             same one 2 times running is a 1-in-49 event, too likely for a
 *             gate that runs on every PR; 6 identical draws in a row is
 *             ~1-in-160000.
 *   rjpct  -- same as rjsec but supervisor.restart_jitter = "50%" and
 *             restart_delay = restart_delay_max = 4, to exercise the
 *             percentage form: bound becomes [4, 4 + 4*50% ] = [4, 6]. Only
 *             the bound is asserted here (3 runs), not variation -- the
 *             narrower 3-value range (0, 1, 2) makes a variation check on
 *             this few draws flaky for the same reason rjsec needed more.
 *
 * None of this waits on cron's once-a-minute schedule tick, so the whole test
 * fits comfortably inside build/run-fpmng-phpt.sh's 120s TEST_FPM_TIMEOUT
 * (matched by CI's build-matrix.yml) with room to spare: the slowest pool
 * (rjsec) needs at most 6 gaps of a ~2-8s backoff, i.e. well under a minute. */

$work = sys_get_temp_dir() . '/fpmng-sup-jitter-' . getmypid();
@mkdir($work, 0700, true);

$logs = [
    'cold' => "$work/cold-runs.log",
    'rjsec' => "$work/rjsec-runs.log",
    'rjpct' => "$work/rjpct-runs.log",
];
foreach ($logs as $log) {
    @unlink($log);
}

/* One APPENDED line per run, "<pid> <microtime>\n" -- append-only so a script
 * still running when the test reads the file can never look like zero runs
 * (see fpmng-supervisor-restart.phpt's comment on why FILE_APPEND matters
 * here), and LOCK_EX so four concurrent 'cold' copies never interleave a
 * partial line into each other's. */
$markerScript = static fn (string $log, int $exitCode) => <<<PHP
<?php
error_reporting(0);
@file_put_contents('{$log}', getmypid() . ' ' . microtime(true) . "\n", FILE_APPEND | LOCK_EX);
exit({$exitCode});
PHP;

file_put_contents("$work/cold.php", $markerScript($logs['cold'], 1));
file_put_contents("$work/rjsec.php", $markerScript($logs['rjsec'], 1));
file_put_contents("$work/rjpct.php", $markerScript($logs['rjpct'], 1));

$startJitter = 3;
$rjSecDelay = 2;
$rjSecJitter = 6;
$rjPctDelay = 4;
$rjPctPercent = 50;

$cfg = <<<EOT
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
daemonize = no

[cold]
pool.type = supervisor
supervisor.script = $work/cold.php
supervisor.processes = 4
supervisor.restart = on-failure
supervisor.start_jitter = $startJitter

[rjsec]
pool.type = supervisor
supervisor.script = $work/rjsec.php
supervisor.processes = 1
supervisor.restart = always
supervisor.restart_delay = $rjSecDelay
supervisor.restart_delay_max = $rjSecDelay
supervisor.restart_max = 0
supervisor.restart_jitter = $rjSecJitter
pm.status_listen = {{ADDR[operator]}}
pm.status_path = /rjsec-status

[rjpct]
pool.type = supervisor
supervisor.script = $work/rjpct.php
supervisor.processes = 1
supervisor.restart = always
supervisor.restart_delay = $rjPctDelay
supervisor.restart_delay_max = $rjPctDelay
supervisor.restart_max = 0
supervisor.restart_jitter = {$rjPctPercent}%
EOT;

function readRuns(string $log): array
{
    if (!is_file($log)) {
        return [];
    }
    $out = [];
    foreach (file($log, FILE_IGNORE_NEW_LINES | FILE_SKIP_EMPTY_LINES) as $line) {
        [$pid, $t] = explode(' ', trim($line), 2);
        $out[] = ['pid' => (int) $pid, 't' => (float) $t];
    }
    return $out;
}

function waitForRuns(string $log, int $count, int $timeoutSeconds): array
{
    $deadline = microtime(true) + $timeoutSeconds;
    do {
        $runs = readRuns($log);
        if (count($runs) >= $count) {
            return $runs;
        }
        usleep(200000);
    } while (microtime(true) < $deadline);
    return $runs;
}

/* Only the first logged run of each pid matters for the cold-start check --
 * with restart = on-failure and an always-failing script, each of the 4
 * 'cold' copies keeps retrying (with its own restart_delay backoff) after
 * that first run, so the log keeps growing per pid; the offset that
 * start_jitter controls is only the first one. */
function firstRunByPid(array $runs): array
{
    $out = [];
    foreach ($runs as $r) {
        if (!isset($out[$r['pid']])) {
            $out[$r['pid']] = $r;
        }
    }
    return array_values($out);
}

function waitForDistinctPids(string $log, int $count, int $timeoutSeconds): array
{
    $deadline = microtime(true) + $timeoutSeconds;
    do {
        $runs = firstRunByPid(readRuns($log));
        if (count($runs) >= $count) {
            return $runs;
        }
        usleep(200000);
    } while (microtime(true) < $deadline);
    return $runs;
}

/* Consecutive gaps between run timestamps of the SAME single-process pool. */
function gaps(array $runs): array
{
    $out = [];
    for ($i = 1; $i < count($runs); $i++) {
        $out[] = $runs[$i]['t'] - $runs[$i - 1]['t'];
    }
    return $out;
}

$tester = new FPM\Tester($cfg, '<?php');
try {
    $tester->start(extraArgs: ['-R'], forceStderr: true, daemonize: false);
    $tester->expectLogStartNotices();
    $t0 = microtime(true);

    $operator = $tester->getListen('{{ADDR[operator]}}');

    /* --- cold: supervisor.start_jitter --- */
    $slack = 6.0; /* master ready -> fork -> PHP request startup, on a loaded runner */
    $coldRuns = waitForDistinctPids($logs['cold'], 4, 30);
    if (count($coldRuns) < 4) {
        echo "FAIL: cold pool only produced " . count($coldRuns) . "/4 distinct cold starts\n";
        $tester->close(true);
        exit(1);
    }
    $offsets = array_map(static fn ($r) => $r['t'] - $t0, $coldRuns);
    $tooLate = array_filter($offsets, static fn ($o) => $o > $startJitter + $slack);
    if ($tooLate) {
        echo "FAIL: cold start offset(s) exceeded start_jitter + slack: " . implode(',', $tooLate) . "\n";
        $tester->close(true);
        exit(1);
    }
    $spread = max($offsets) - min($offsets);
    if ($spread < 0.05) {
        /* The regression this guards: a rand()/srand()-based implementation
         * seeds once, in whichever process (often the master) calls it
         * first, and every child forked afterwards inherits that exact
         * sequence position -- all 4 copies would then land within
         * milliseconds of each other regardless of start_jitter. */
        echo "FAIL: cold starts did not spread out (max-min offset = $spread s)\n";
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
    $rjSecRuns = waitForRuns($logs['rjsec'], 7, 60);
    if (count($rjSecRuns) < 7) {
        echo "FAIL: rjsec pool only produced " . count($rjSecRuns) . "/7 runs\n";
        $tester->close(true);
        exit(1);
    }
    $rjSecGaps = gaps($rjSecRuns);
    $minGap = min($rjSecGaps);
    $maxGap = max($rjSecGaps);
    /* Lower bound has a little slack subtracted for scheduling jitter of the
     * harness itself, not of the code under test; upper bound adds the same
     * slack used above for fork/startup cost. */
    if ($minGap < $rjSecDelay - 0.5 || $maxGap > $rjSecDelay + $rjSecJitter + $slack) {
        echo "FAIL: rjsec gaps out of bounds [" . ($rjSecDelay - 0.5) . ", " . ($rjSecDelay + $rjSecJitter + $slack) .
            "]: " . implode(',', $rjSecGaps) . "\n";
        $tester->close(true);
        exit(1);
    }
    if (max($rjSecGaps) - min($rjSecGaps) < 0.05) {
        echo "FAIL: rjsec gaps never varied -- restart_jitter appears inert: " . implode(',', $rjSecGaps) . "\n";
        $tester->close(true);
        exit(1);
    }
    echo "restart_jitter (seconds) bounded and varying: ok\n";

    /* restart_max/backoff accounting: every run here is a real failure (exit
     * 1), and nothing about jitter should change how failures are counted --
     * it only changes the delay applied between them. */
    $json = json_decode(fpmng_operator_body($operator, '/rjsec-status'), true, flags: JSON_THROW_ON_ERROR);
    $pool = $json['pools'][0];
    $observed = count($rjSecRuns);
    if ($pool['consecutive_failures'] < $observed - 1 || $pool['consecutive_failures'] > $observed + 1) {
        echo "FAIL: consecutive_failures ($pool[consecutive_failures]) does not track " .
            "the observed run count ($observed)\n";
        $tester->close(true);
        exit(1);
    }
    echo "restart_max/backoff accounting unaffected: ok\n";

    /* --- rjpct: supervisor.restart_jitter = 50% of restart_delay --- */
    $rjPctRuns = waitForRuns($logs['rjpct'], 4, 60);
    if (count($rjPctRuns) < 4) {
        echo "FAIL: rjpct pool only produced " . count($rjPctRuns) . "/4 runs\n";
        $tester->close(true);
        exit(1);
    }
    $rjPctGaps = gaps($rjPctRuns);
    $pctJitterMax = (int) floor($rjPctDelay * $rjPctPercent / 100);
    if (min($rjPctGaps) < $rjPctDelay - 0.5 || max($rjPctGaps) > $rjPctDelay + $pctJitterMax + $slack) {
        echo "FAIL: rjpct gaps out of bounds [" . ($rjPctDelay - 0.5) . ", " . ($rjPctDelay + $pctJitterMax + $slack) .
            "]: " . implode(',', $rjPctGaps) . "\n";
        $tester->close(true);
        exit(1);
    }
    echo "restart_jitter (percentage) bounded: ok\n";

    echo "Done\n";
} finally {
    $tester->terminate();
    $tester->close();
    foreach ($logs as $log) {
        @unlink($log);
    }
    @unlink("$work/cold.php");
    @unlink("$work/rjsec.php");
    @unlink("$work/rjpct.php");
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

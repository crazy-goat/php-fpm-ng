--TEST--
fpm-ng: cron.jitter delays an already-due run within bounds, cron.jitter_mode = stable is deterministic across runs (issue #322)
--SKIPIF--
<?php include "skipif.inc"; ?>
--ENV--
FPMNG_DEBUG_CLOCK_RATE=10
--FILE--
<?php

require_once "tester.inc";
require_once "fpmng-operator.inc";

/* FPMNG_DEBUG_CLOCK_RATE above (issue #398). Every figure this test asserts on
 * is one the MASTER measured: the pool's status page reports last_start, which
 * the cron child stamps with FPM_NOW() -- the scaled clock -- when it begins
 * the run, and next_run, which the master computes on the same clock. The job
 * script itself records nothing. It used to write gmdate('s') and the test
 * compared that to cron.jitter, a real reading against a virtual bound, which
 * is why this test could not take a rate (docs/fpmng-phpt.md, "The virtual
 * clock") and was the longest single test in the suite at 67 s. Every deadline
 * below stays in REAL seconds and stays generous, so a binary built without
 * --enable-fpmng-debug-clock ignores the rate and the test still passes, one
 * real minute at a time.
 *
 * Cron's schedule granularity is 1 minute, so the test gets exactly ONE
 * schedule tick; every check below fits inside it. last_start is whole
 * seconds on a clock whose tick is a minute boundary, so last_start % 60 is
 * the jitter offset (plus a fraction of a second of start-up latency, which
 * the floor to whole seconds mostly absorbs).
 *
 * Random-mode regression check (three pools, one tick): rand1/rand2/rand3
 * share a schedule and cron.jitter but have different names, so they fire on
 * the SAME tick and each independently samples cron.jitter_mode = random.
 * Bugbot's Finding 1 was a libc rand()/srand() state that is process-wide and
 * SURVIVES fork() -- every child forked after the state was last advanced
 * produces the identical "random" delay. Three sibling pools forked from the
 * same master within the same second are exactly the scenario that bug
 * collapsed. What is read here is each CHILD's own start stamp, not an
 * estimate the master computed for it, so a child that drew the same delay as
 * its siblings shows up as the same offset: this checks the offsets are not
 * all identical, without needing a second tick.
 *
 * Stable-mode regression check (one pool, one tick, one extra HTTP call): the
 * "deterministic across runs" claim really means "a pure function of the
 * pool's name, with nothing else -- not pid, not the clock -- feeding it".
 * That is exactly the property a shared master-vs-child code path can get
 * wrong. This compares the CHILD's recorded start offset against the status
 * page's (also jitter-adjusted) next_run, which the MASTER computes -- two
 * different processes computing the same pool's stable delay, which must match
 * if it really only depends on the name. A leaked pid/clock in the stable
 * branch would make them differ by up to cron.jitter. */
$work = sys_get_temp_dir() . '/fpmng-cron-jitter-' . getmypid();
@mkdir($work, 0700, true);
$pools = ['rand1', 'rand2', 'rand3', 'stablejob'];

foreach ($pools as $pool) {
    file_put_contents("$work/$pool-job.php", "<?php\n");
}

/* cron.jitter = 20s on a once-a-minute schedule: comfortably below the 60s
 * interval (docs/cron.md warns against jitter comparable to the interval),
 * and wide enough that a delay of 0 on every sample would be suspicious
 * rather than plausible bad luck. */
$jitter = 20;

$cfg = <<<EOT
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
daemonize = no
[rand1]
pool.type = cron
cron.schedule = * * * * *
cron.script = $work/rand1-job.php
cron.jitter = $jitter
cron.jitter_mode = random
operator.status_listen = {{ADDR[operator]}}
operator.status_path = /rand1-status
[rand2]
pool.type = cron
cron.schedule = * * * * *
cron.script = $work/rand2-job.php
cron.jitter = $jitter
cron.jitter_mode = random
operator.status_listen = {{ADDR[operator]}}
operator.status_path = /rand2-status
[rand3]
pool.type = cron
cron.schedule = * * * * *
cron.script = $work/rand3-job.php
cron.jitter = $jitter
cron.jitter_mode = random
operator.status_listen = {{ADDR[operator]}}
operator.status_path = /rand3-status
[stablejob]
pool.type = cron
cron.schedule = * * * * *
cron.script = $work/stablejob-job.php
cron.jitter = $jitter
cron.jitter_mode = stable
operator.status_listen = {{ADDR[operator]}}
operator.status_path = /stablejob-status
EOT;

function poolStatus(string $operator, string $pool): array
{
    $json = json_decode(fpmng_operator_body($operator, "/$pool-status"), true, flags: JSON_THROW_ON_ERROR);
    return $json['pools'][0];
}

/* The first non-zero last_start of every pool, as the master reports it.
 * Polled fast and recorded the moment it first appears: at a high rate the
 * next tick is only seconds of real time away, and a second run would
 * overwrite the stamp this test wants. */
function firstStarts(string $operator, array $pools, int $timeoutSeconds): array
{
    $seen = [];
    $deadline = time() + $timeoutSeconds;
    do {
        foreach ($pools as $pool) {
            if (isset($seen[$pool])) {
                continue;
            }
            $start = poolStatus($operator, $pool)['last_start'] ?? 0;
            if (is_int($start) && $start > 0) {
                $seen[$pool] = $start;
            }
        }
        if (count($seen) === count($pools)) {
            break;
        }
        usleep(50000);
    } while (time() < $deadline);
    return $seen;
}

$tester = new FPM\Tester($cfg, '<?php');
try {
    $tester->start(extraArgs: ['-R'], forceStderr: true, daemonize: false);
    $tester->expectLogStartNotices();

    $operator = $tester->getListen('{{ADDR[operator]}}');

    /* One full minute (the tick every pool here waits for) plus slack, in REAL
     * seconds, for a binary without the virtual clock. Still comfortably under
     * run-fpmng-phpt.sh's per-attempt TEST_FPM_TIMEOUT (120s), leaving
     * headroom for FPM startup/shutdown. */
    $starts = firstStarts($operator, $pools, 100);
    if (count($starts) !== count($pools)) {
        echo "FAIL: not every pool completed its run within 100 seconds (" . json_encode($starts) . ")\n";
        exit(1);
    }

    $randSample = [];
    foreach (['rand1', 'rand2', 'rand3'] as $pool) {
        $randSample[] = $starts[$pool] % 60;
    }

    $withinBound = true;
    foreach ($randSample as $sec) {
        /* Fires at second 0 of the minute, then jitter in [0, cron.jitter];
         * a few seconds of slack absorb scheduling/process-start latency,
         * which a scaled clock multiplies, not jitter itself. */
        if ($sec > $jitter + 5) {
            $withinBound = false;
        }
    }
    if (!$withinBound) {
        echo "FAIL: random jitter exceeded bound (" . implode(',', $randSample) . ")\n";
        exit(1);
    }
    $allIdentical = (count(array_unique($randSample)) === 1);
    echo $allIdentical
        ? "FAIL: random jitter collapsed to one value across sibling pools (" . implode(',', $randSample) . ")\n"
        : "random jitter bounded: ok\n";

    $stableSec = $starts['stablejob'] % 60;
    if ($stableSec > $jitter + 5) {
        echo "FAIL: stable jitter exceeded bound ($stableSec)\n";
        exit(1);
    }

    /* Compare the CHILD's own recorded offset against the MASTER's estimate
     * for the SAME pool, queried right after this run: next_run is always
     * base schedule tick (a multiple of 60, cron's minute granularity) plus
     * jitter, and cron.jitter_mode = stable depends only on the pool's name
     * -- never on which tick it is. So next_run % 60 recovers the master's
     * jitter offset for stablejob without needing to know (or race against)
     * which minute boundary next_run actually targets. The child stamped its
     * start after the sleep, so it can be one whole second later than the
     * exact offset, never earlier. */
    $nextRun = poolStatus($operator, 'stablejob')['next_run'] ?? null;
    if (!is_int($nextRun)) {
        echo "FAIL: status page has no numeric next_run: " . var_export($nextRun, true) . "\n";
        exit(1);
    }
    $masterOffset = $nextRun % 60;
    if ($stableSec < $masterOffset || $stableSec > $masterOffset + 1) {
        echo "FAIL: stable offsets differ between child ($stableSec) and master ($masterOffset)\n";
        exit(1);
    }
    echo "stable jitter deterministic: ok\n";

    echo "Done\n";
} finally {
    $tester->terminate();
    $tester->close();
    foreach ($pools as $pool) {
        @unlink("$work/$pool-job.php");
    }
    @rmdir($work);
}
?>
--EXPECT--
random jitter bounded: ok
stable jitter deterministic: ok
Done
--CLEAN--
<?php
require_once "tester.inc";
FPM\Tester::clean();
?>

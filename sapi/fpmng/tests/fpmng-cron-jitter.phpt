--TEST--
fpm-ng: cron.jitter delays an already-due run within bounds, cron.jitter_mode = stable is deterministic across runs (issue #322)
--SKIPIF--
<?php include "skipif.inc"; ?>
--FILE--
<?php

require_once "tester.inc";
require_once "fpmng-operator.inc";

/* This test has one hard constraint: build/run-fpmng-phpt.sh runs every .phpt
 * under a 120s TEST_FPM_TIMEOUT (matched by CI's build-matrix.yml), and
 * cron's schedule granularity is 1 minute -- so this test gets exactly ONE
 * schedule tick to work with, not two. Every check below is designed to fit
 * inside that single tick.
 *
 * Random-mode regression check (three pools, one tick): rand1/rand2/rand3
 * share a schedule and cron.jitter but have different names, so they fire on
 * the SAME tick and each independently samples cron.jitter_mode = random.
 * Bugbot's Finding 1 was a libc rand()/srand() state that is process-wide and
 * SURVIVES fork() -- every child forked after the state was last advanced
 * produces the identical "random" delay. Three sibling pools forked from the
 * same master within the same second are exactly the scenario that bug
 * collapsed: this checks their offsets are not all identical, without
 * needing a second tick.
 *
 * Stable-mode regression check (one pool, one tick, one extra HTTP call): the
 * "deterministic across runs" claim really means "a pure function of the
 * pool's name, with nothing else -- not pid, not the clock -- feeding it".
 * That is exactly the property a shared master-vs-child code path can get
 * wrong. Rather than waiting a second tick for a second real run, this
 * queries the pool's status page for its (also jitter-adjusted) next_run
 * right after the run has happened, and checks the MASTER's estimate agrees
 * with the CHILD's own recorded offset -- two different processes computing
 * the same pool's stable delay, which must match if it really only depends
 * on the name. A leaked pid/clock in the stable branch would make them
 * differ. */
$work = sys_get_temp_dir() . '/fpmng-cron-jitter-' . getmypid();
@mkdir($work, 0700, true);
$markers = [
    'rand1' => "$work/rand1-runs.log",
    'rand2' => "$work/rand2-runs.log",
    'rand3' => "$work/rand3-runs.log",
    'stablejob' => "$work/stable-runs.log",
];
foreach ($markers as $marker) {
    @unlink($marker);
}

$jobScript = static fn (string $marker) => <<<PHP
<?php
file_put_contents('{$marker}', gmdate('s') . ' ' . time() . "\n", FILE_APPEND);
PHP;

foreach ($markers as $pool => $marker) {
    file_put_contents("$work/$pool-job.php", $jobScript($marker));
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
[rand2]
pool.type = cron
cron.schedule = * * * * *
cron.script = $work/rand2-job.php
cron.jitter = $jitter
cron.jitter_mode = random
[rand3]
pool.type = cron
cron.schedule = * * * * *
cron.script = $work/rand3-job.php
cron.jitter = $jitter
cron.jitter_mode = random
[stablejob]
pool.type = cron
cron.schedule = * * * * *
cron.script = $work/stablejob-job.php
cron.jitter = $jitter
cron.jitter_mode = stable
pm.status_listen = {{ADDR[operator]}}
pm.status_path = /stablejob-status
EOT;

function readSeconds(string $marker): array
{
    if (!is_file($marker)) {
        return [];
    }
    $out = [];
    foreach (file($marker, FILE_IGNORE_NEW_LINES | FILE_SKIP_EMPTY_LINES) as $line) {
        [$sec] = explode(' ', trim($line), 2);
        $out[] = (int) $sec;
    }
    return $out;
}

function waitForAll(array $markers, int $timeoutSeconds): bool
{
    $deadline = time() + $timeoutSeconds;
    do {
        $ready = true;
        foreach ($markers as $marker) {
            if (count(readSeconds($marker)) < 1) {
                $ready = false;
                break;
            }
        }
        if ($ready) {
            return true;
        }
        usleep(200000);
    } while (time() < $deadline);
    return false;
}

$tester = new FPM\Tester($cfg, '<?php');
try {
    $tester->start(extraArgs: ['-R'], forceStderr: true, daemonize: false);
    $tester->expectLogStartNotices();

    $operator = $tester->getListen('{{ADDR[operator]}}');

    /* One full minute (the tick every pool here waits for) plus slack: more
     * generous than fpmng-cron-schedule.phpt's 75s because this test forks
     * FOUR cron pools off the same tick instead of one, so it has more
     * process-spawn and marker-file I/O to absorb under a loaded runner.
     * Still comfortably under run-fpmng-phpt.sh's per-attempt TEST_FPM_TIMEOUT
     * (120s), leaving headroom for FPM startup/shutdown and the one extra
     * HTTP request against the operator endpoint. */
    if (!waitForAll($markers, 100)) {
        $counts = array_map(static fn ($m) => count(readSeconds($m)), $markers);
        echo "FAIL: not every pool completed its run within 100 seconds (" . json_encode($counts) . ")\n";
        exit(1);
    }

    $r1 = readSeconds($markers['rand1'])[0];
    $r2 = readSeconds($markers['rand2'])[0];
    $r3 = readSeconds($markers['rand3'])[0];
    $randSample = [$r1, $r2, $r3];

    $withinBound = true;
    foreach ($randSample as $sec) {
        /* Fires at second 0 of the minute, then jitter in [0, cron.jitter];
         * a few seconds of slack absorb scheduling/process-start latency,
         * not jitter itself. */
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

    $stableSec = readSeconds($markers['stablejob'])[0];
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
     * which minute boundary next_run actually targets. Both numbers come
     * from the same pool name, so they must be exactly equal. */
    $json = json_decode(fpmng_operator_body($operator, '/stablejob-status'), true, flags: JSON_THROW_ON_ERROR);
    $nextRun = $json['pools'][0]['next_run'] ?? null;
    if (!is_int($nextRun)) {
        echo "FAIL: status page has no numeric next_run: " . var_export($nextRun, true) . "\n";
        exit(1);
    }
    $masterOffset = $nextRun % 60;
    if ($masterOffset !== $stableSec) {
        echo "FAIL: stable offsets differ between child ($stableSec) and master ($masterOffset)\n";
        exit(1);
    }
    echo "stable jitter deterministic: ok\n";

    echo "Done\n";
} finally {
    $tester->terminate();
    $tester->close();
    foreach ($markers as $pool => $marker) {
        @unlink($marker);
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

--TEST--
fpm-ng: cron.jitter delays an already-due run within bounds, cron.jitter_mode = stable is deterministic across runs (issue #322)
--SKIPIF--
<?php include "skipif.inc"; ?>
--FILE--
<?php

require_once "tester.inc";

/* Both pools share the schedule that fires most often (every minute) so the
 * test does not wait longer than it has to; both jobs just record their own
 * start second so this test can check BOUNDS, never an exact wall-clock
 * second — an exact assertion would make this flaky on a loaded CI runner,
 * exactly the trap workflow.md's "Evidence" section warns about. */
$work = sys_get_temp_dir() . '/fpmng-cron-jitter-' . getmypid();
@mkdir($work, 0700, true);
$randMarker = "$work/rand-runs.log";
$stableMarker = "$work/stable-runs.log";
@unlink($randMarker);
@unlink($stableMarker);

$jobScript = static fn (string $marker) => <<<PHP
<?php
file_put_contents('{$marker}', gmdate('s') . ' ' . time() . "\n", FILE_APPEND);
PHP;

file_put_contents("$work/rand-job.php", $jobScript($randMarker));
file_put_contents("$work/stable-job.php", $jobScript($stableMarker));

/* cron.jitter = 20s on a once-a-minute schedule: comfortably below the
 * 60s interval (docs/cron.md warns against jitter comparable to the
 * interval), and wide enough that a delay of 0 on every sample would be
 * suspicious rather than plausible bad luck. */
$jitter = 20;

$cfg = <<<EOT
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
daemonize = no
[randjob]
pool.type = cron
cron.schedule = * * * * *
cron.script = $work/rand-job.php
cron.jitter = $jitter
cron.jitter_mode = random
[stablejob]
pool.type = cron
cron.schedule = * * * * *
cron.script = $work/stable-job.php
cron.jitter = $jitter
cron.jitter_mode = stable
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

function waitForLines(string $marker, int $count, int $timeoutSeconds): array
{
    $deadline = time() + $timeoutSeconds;
    do {
        $seconds = readSeconds($marker);
        if (count($seconds) >= $count) {
            return $seconds;
        }
        usleep(200000);
    } while (time() < $deadline);
    return readSeconds($marker);
}

$tester = new FPM\Tester($cfg, '<?php');
try {
    $tester->start(extraArgs: ['-R'], forceStderr: true, daemonize: false);
    $tester->expectLogStartNotices();

    /* Same 75s budget as fpmng-cron-schedule.phpt: one full minute plus slack
     * for a runner that is not the only thing on the machine. */
    $randSeconds = waitForLines($randMarker, 1, 75);
    if (count($randSeconds) < 1) {
        echo "FAIL: random-mode cron did not run within 75 seconds\n";
        exit(1);
    }
    $withinBound = true;
    foreach ($randSeconds as $sec) {
        /* Fires at second 0 of the minute, then jitter in [0, cron.jitter];
         * a few seconds of slack absorb scheduling/process-start latency,
         * not jitter itself. */
        if ($sec > $jitter + 5) {
            $withinBound = false;
        }
    }
    echo $withinBound ? "random jitter bounded: ok\n" : "FAIL: random jitter exceeded bound ($randSeconds[0]s)\n";

    /* Two runs of the SAME stable pool: cron.jitter_mode = stable derives its
     * delay only from the pool name, never from randomness or from when the
     * run happened, so consecutive runs must land on the same second. */
    $stableSeconds = waitForLines($stableMarker, 2, 140);
    if (count($stableSeconds) < 2) {
        echo "FAIL: stable-mode cron did not complete two runs in time\n";
        exit(1);
    }
    $first = $stableSeconds[0];
    $second = $stableSeconds[1];
    if ($first > $jitter + 5 || $second > $jitter + 5) {
        echo "FAIL: stable jitter exceeded bound ($first, $second)\n";
        exit(1);
    }
    echo ($first === $second) ? "stable jitter deterministic: ok\n" : "FAIL: stable offsets differ ($first != $second)\n";

    echo "Done\n";
} finally {
    $tester->terminate();
    $tester->close();
    @unlink($randMarker);
    @unlink($stableMarker);
    @unlink("$work/rand-job.php");
    @unlink("$work/stable-job.php");
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

--TEST--
fpm-ng: a cron pool counts its runs without any fpm_metric_* call (issue #277)
--SKIPIF--
<?php include "skipif.inc"; ?>
--ENV--
FPMNG_DEBUG_CLOCK_RATE=10
--FILE--
<?php

/* FPMNG_DEBUG_CLOCK_RATE above (issue #396): this test waits for one
 * "* * * * *" tick, and how long that takes is a lottery on where in the minute
 * the test happened to start. Run 35274092140 shows both ends of it -- the same
 * test, passing, took 2.0 s in the canonical suite and 40.1 s in the fiber one.
 * At rate 10 the whole lottery shrinks to 0-6 real seconds, which takes the
 * variance out of the suite's duration as well as the mean.
 *
 * Safe here because every assertion reads a figure the MASTER produced: the
 * fpmng_pool_runs_total series and the status page's runs count. Nothing
 * compares a timestamp, so there is no real-versus-virtual reading to get
 * wrong -- see the virtual clock section of docs/fpmng-phpt.md.
 *
 * The 75-second deadline below stays in REAL seconds, so a binary built without
 * --enable-fpmng-debug-clock ignores the variable and this test still passes. */
require_once "tester.inc";
require_once "fpmng-operator.inc";

/* The cron leg of issue #277, kept apart from fpmng-baseline-counters.phpt
 * because the shortest schedule cron has fires once a minute: this test spends
 * most of its time waiting for the top of the next one, and the other two
 * baseline counters should not wait with it.
 *
 * The job script registers no series. "Did the job run at all, and how many
 * times" is answered by the SAPI, which is the point: a cron script that ends
 * with exit() has nowhere to put a metric of its own that survives it. */
$work = sys_get_temp_dir() . '/fpmng-baseline-cron-' . getmypid();
@mkdir($work, 0700, true);
file_put_contents("$work/job.php", '<?php');

$cfg = <<<EOT
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
daemonize = no

[tick]
pool.type = cron
cron.schedule = * * * * *
cron.script = $work/job.php
pm.metrics_listen = {{ADDR[operator]}}
pm.metrics_path = /tick-metrics
pm.status_listen = {{ADDR[operator]}}
pm.status_path = /tick-status
EOT;

function series(string $body, string $name, string $pool): ?float
{
    $pattern = '/^' . preg_quote($name, '/') . '\{pool="' . preg_quote($pool, '/') . '"\} (\S+)$/m';
    return preg_match($pattern, $body, $m) ? (float) $m[1] : null;
}

$tester = new FPM\Tester($cfg, '<?php');
try {
    $tester->start(extraArgs: ['-R'], forceStderr: true, daemonize: false);
    $tester->expectLogStartNotices();

    $operator = $tester->getListen('{{ADDR[operator]}}');

    /* Zero before the first run, and present: a counter that only appears once
     * it has moved cannot be alerted on, because "no series" and "no runs" look
     * the same to the scraper. */
    $runs = series(fpmng_operator_body($operator, '/tick-metrics'), 'fpmng_pool_runs_total', 'tick');
    if ($runs !== 0.0) {
        throw new RuntimeException('runs before the first schedule: ' . var_export($runs, true));
    }
    echo "zero before the first run: ok\n";

    /* The same 75 seconds fpmng-cron-schedule.phpt waits: one full minute plus
     * the slack for a runner that is not the only thing on its machine. */
    $deadline = time() + 75;
    do {
        usleep(200000);
        $runs = series(fpmng_operator_body($operator, '/tick-metrics'), 'fpmng_pool_runs_total', 'tick');
    } while (!$runs && time() < $deadline);

    if (!$runs) {
        throw new RuntimeException('cron did not run within 75 seconds');
    }
    echo "counted after the first run: ok\n";

    $json = json_decode(fpmng_operator_body($operator, '/tick-status'), true, flags: JSON_THROW_ON_ERROR);
    $pool = $json['pools'][0];
    if (($pool['name'] ?? null) !== 'tick' || ($pool['runs'] ?? null) < 1) {
        throw new RuntimeException('status page disagrees: ' . var_export($pool, true));
    }
    echo "status page agrees: ok\n";

    echo "Done\n";
} finally {
    $tester->terminate();
    $tester->close();
    @unlink("$work/job.php");
    @rmdir($work);
}
?>
--EXPECT--
zero before the first run: ok
counted after the first run: ok
status page agrees: ok
Done
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>

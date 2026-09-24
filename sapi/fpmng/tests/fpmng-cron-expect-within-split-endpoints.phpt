--TEST--
fpm-ng: split status and metrics scrapes do not log a second cron stale warning (issue #358)
--SKIPIF--
<?php include "skipif.inc"; ?>
--ENV--
FPMNG_DEBUG_CLOCK_RATE=60
TEST_TIMEOUT=60
--FILE--
<?php
require_once "tester.inc";
require_once "fpmng-operator.inc";

$work = sys_get_temp_dir() . '/fpmng-cron-stale-split-' . getmypid();
@mkdir($work, 0700, true);
$marker = "$work/runs.log";
$script = <<<PHP
<?php
file_put_contents('{$marker}', "run\\n", FILE_APPEND);
sleep(10);
PHP;
file_put_contents("$work/job.php", $script);

$cfg = <<<EOT
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
daemonize = no
log_level = notice
[tick]
pool.type = cron
cron.schedule = * * * * *
cron.script = $work/job.php
cron.expect_within = 1
operator.status_listen = {{ADDR[status]}}
operator.status_path = /tick-status
operator.metrics_listen = {{ADDR[metrics]}}
operator.metrics_path = /tick-metrics
EOT;

$tester = new FPM\Tester($cfg, $script);
try {
    /* Wait for both deterministic operator ports to be released by prior tests. */
    foreach (['status', 'metrics'] as $listener) {
        [$host, $port] = explode(':', $tester->getListen("{{ADDR[$listener]}}"));
        $deadline = time() + 30;
        do {
            $probe = @stream_socket_server("tcp://$host:$port", $errno, $errstr);
            if ($probe !== false) {
                fclose($probe);
                break;
            }
            usleep(200000);
        } while (time() < $deadline);
    }

    $tester->start(extraArgs: ['-R'], forceStderr: true, daemonize: false);
    $tester->expectLogStartNotices();
    $status = $tester->getListen('{{ADDR[status]}}');
    $metrics = $tester->getListen('{{ADDR[metrics]}}');

    /* No status or metrics request has happened yet: the master timer itself
     * must produce the first warning. */
    $warning = '/WARNING: .*\[pool tick\] cron: stale -- the schedule\'s next run after the last one was '
        . 'due at \d+, and it is now more than cron\.expect_within = 1s past that/';
    $tester->expectLogPattern($warning, false, 8);
    echo "master timer warns before a scrape: ok\n";

    for ($i = 0; $i < 8; $i++) {
        $row = json_decode(fpmng_operator_body($status, '/tick-status'), true, flags: JSON_THROW_ON_ERROR)['pools'][0];
        $metricsBody = fpmng_operator_body($metrics, '/tick-metrics');
        if (empty($row['stale']) || !str_contains($metricsBody, 'fpmng_pool_stale{pool="tick"} 1')) {
            throw new RuntimeException('split endpoint did not render stale state: '
                . var_export($row, true) . "\n" . $metricsBody);
        }
    }
    echo "status and metrics both report stale: ok\n";

    /* The two endpoint children can render concurrently, but only the master
     * timer owns the shared warning latch, so these reads cannot log duplicates. */
    $tester->expectNoLogPattern($warning, false, null, 1500000);
    if (substr_count((string) @file_get_contents($marker), "run\n") !== 1) {
        throw new RuntimeException('operator scrapes or timer started another cron run');
    }
    echo "split scrapes do not duplicate warning or start a run: ok\n";
    echo "Done\n";
} finally {
    $tester->terminate();
    $tester->close();
    @unlink($marker);
    @unlink("$work/job.php");
    @rmdir($work);
}
?>
--EXPECT--
master timer warns before a scrape: ok
status and metrics both report stale: ok
split scrapes do not duplicate warning or start a run: ok
Done
--CLEAN--
<?php
require_once "tester.inc";
FPM\Tester::clean();
$stale = time() - 300;
foreach (glob(sys_get_temp_dir() . '/fpmng-cron-stale-split-*') as $dir) {
    if (@filemtime($dir) > $stale) {
        continue;
    }
    foreach (glob("$dir/*") as $file) {
        @unlink($file);
    }
    @rmdir($dir);
}
?>

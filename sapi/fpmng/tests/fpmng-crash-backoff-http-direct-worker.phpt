--TEST--
fpm-ng: a worker-mode http-direct child that exits during boot is a fast failure, the pool gives up once and then waits out the capped delay (issue #727)
--SKIPIF--
<?php
include "fpmng-skipif.inc";
if (PHP_OS_FAMILY !== 'Linux') {
    die('skip the test reads the process table with ps -eo and expects the Linux process title');
}
?>
--FILE--
<?php

require_once "tester.inc";
require_once "fpmng-operator.inc";
require_once "fpmng-saturation.inc";

/* The worker script exits before it enters its event loop. Each child then dies
 * during boot, without a request: that is the case of issue #727. The pool gives
 * up after three fast failures. pool.executor = worker refuses operator.status_path
 * (docs/http-direct.md), so the state is read from operator.metrics_path only. */
$root = sys_get_temp_dir() . '/fpmng-crash-backoff-worker-' . getmypid();
@mkdir($root, 0700, true);
file_put_contents("$root/worker.php", "<?php\nexit(3);\n");

$cfg = <<<EOT
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
log_level = notice

[work]
listen = {{ADDR[work]}}
pool.type = http-direct
pool.executor = worker
pm = static
pm.max_children = 1
pm.max_consecutive_failures = 3
php_admin_value[max_execution_time] = 0
chdir = $root
http.front_controller = /worker.php
operator.metrics_listen = {{ADDR[operator]}}
operator.metrics_path = /metrics/work
EOT;

$tester = new FPM\Tester($cfg, '<?php echo "unused";');
try {
    @unlink($tester->getPrefixedFile(FPM\Tester::FILE_EXT_LOG_ERR));
    $tester->start([], false);
    $tester->switchLogSource('{{FILE:LOG}}');
    $tester->expectLogStartNotices();

    $operator = $tester->getListen('{{ADDR[operator]}}');
    $metrics = static fn(): string => fpmng_operator_body($operator, '/metrics/work');
    fpmng_saturation_wait(static function () use ($metrics): bool {
        return fpmng_saturation_value($metrics(), 'fpmng_pool_crash_gave_up{pool="work"}') === 1.0;
    }, 'the work pool to give up after boot failures', $metrics);
    $body = $metrics();
    fpmng_saturation_check(fpmng_saturation_value($body, 'fpmng_pool_consecutive_crashes{pool="work"}') === 3.0,
        "work pool does not count three boot failures:\n$body");
    fpmng_saturation_check((fpmng_saturation_value($body, 'fpmng_pool_respawn_delay_ms{pool="work"}') ?? 0) >= 30000,
        "work pool does not wait the capped delay:\n$body");
    echo "boot-failures-give-up: ok\n";

    /* The wait after the give-up is 30-60 s, so the pool starts no child in the
     * next seconds: the count of boot failures stays at three. */
    usleep(2000000);
    $log = (string) file_get_contents($tester->getPrefixedFile(FPM\Tester::FILE_EXT_LOG_ERR));
    fpmng_saturation_check(preg_match_all('/\[pool work\] child \d+ exited after /', $log) === 3,
        "work pool started children during the wait:\n$log");
    fpmng_saturation_check(substr_count($log, '[pool work] gave up after') === 1,
        "work pool does not log exactly one ALERT:\n$log");
    fpmng_saturation_check(preg_match('/ALERT.*\[pool work\] gave up after 3 children in a row/', $log) === 1,
        "the give-up line is not an ALERT:\n$log");
    echo "no-respawn-loop-during-the-wait: ok\n";
    echo "Done\n";
} finally {
    $tester->terminate();
    $tester->close();
    @unlink("$root/worker.php");
    @rmdir($root);
}
?>
--EXPECT--
boot-failures-give-up: ok
no-respawn-loop-during-the-wait: ok
Done

--TEST--
fpm-ng: a pool spared by a selective reload keeps its saturation counters (issue #644)
--SKIPIF--
<?php
include "fpmng-skipif.inc";
if (PHP_OS_FAMILY !== 'Linux') {
    die('skip the listen queue is read with TCP_INFO, which only Linux builds sample');
}
?>
--FILE--
<?php

require_once "tester.inc";
require_once "fpmng-operator.inc";
require_once "fpmng-saturation.inc";

/* Issue #330 spares an unchanged pool across the master's execvp(). Its
 * scoreboard lives in a memfd that the new master maps again (issue #537), so
 * the high-water marks and the max_children_reached count must carry on from
 * the old values rather than start at zero.
 *
 * Pool [a] is spared, pool [b] changes. [a] saturates first: one busy child,
 * one connection queued. Both connections are closed before the reload, so the
 * values read before it are final. After the reload they must not go back. */
$root = sys_get_temp_dir() . '/fpmng-sat-reload-' . getmypid();
@mkdir($root, 0700, true);
file_put_contents("$root/sleep.php", '<?php sleep(5); echo "done";');

$cfgBefore = <<<EOT
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
log_level = notice
reload.selective = yes

[a]
listen = {{ADDR[a]}}
chdir = $root
pm = dynamic
pm.max_children = 1
pm.start_servers = 1
pm.min_spare_servers = 1
pm.max_spare_servers = 1
operator.metrics_listen = {{ADDR[operator]}}
operator.metrics_path = /metrics/a

[b]
listen = {{ADDR[b]}}
chdir = $root
pm = static
pm.max_children = 1
env[GENERATION] = 1
EOT;
/* [a] is identical in both files; only [b] differs. */
$cfgAfter = str_replace('env[GENERATION] = 1', 'env[GENERATION] = 2', $cfgBefore);

$tester = new FPM\Tester($cfgBefore, '<?php');
$conns = [];
try {
    @unlink($tester->getPrefixedFile(FPM\Tester::FILE_EXT_LOG_ERR));
    $tester->start([], false);
    $tester->switchLogSource('{{FILE:LOG}}');
    $tester->expectLogStartNotices();

    $operator = $tester->getListen('{{ADDR[operator]}}');
    $a = $tester->getListen('{{ADDR[a]}}');

    $series = static function () use ($operator): array {
        $body = fpmng_operator_body($operator, '/metrics/a');
        return [
            'max_children_reached' => fpmng_saturation_value($body, 'fpmng_pool_max_children_reached_total{pool="a"}') ?? 0,
            'listen_queue_max' => fpmng_saturation_value($body, 'fpmng_pool_listen_queue_max{pool="a"}') ?? 0,
        ];
    };

    $conns[] = fpmng_saturation_fcgi_start($a, "$root/sleep.php");
    $conns[] = fpmng_saturation_connect($a);
    fpmng_saturation_wait(static function () use ($series): bool {
        $v = $series();
        return $v['max_children_reached'] >= 1 && $v['listen_queue_max'] >= 1;
    }, 'pool a to saturate before the reload', static fn(): string => fpmng_operator_body($operator, '/metrics/a'));
    foreach ($conns as $conn) {
        fclose($conn);
    }
    $conns = [];
    /* The child finishes its script before the reload, so the spared child is
     * idle when the old master hands over. */
    fpmng_saturation_wait(static function () use ($operator): bool {
        return (fpmng_saturation_value(fpmng_operator_body($operator, '/metrics/a'), 'fpmng_pool_workers_active{pool="a"}') ?? 1) === 0.0;
    }, 'pool a to go idle before the reload');
    $before = $series();
    fpmng_saturation_check($before['max_children_reached'] >= 1 && $before['listen_queue_max'] >= 1,
        'before the reload: ' . var_export($before, true));

    $tester->reload($cfgAfter);
    $tester->expectLogReloadingNotices(0);

    /* Without this the test could pass for the wrong reason: pool a must really
     * have been spared. */
    $log = (string) @file_get_contents($tester->getPrefixedFile(FPM\Tester::FILE_EXT_LOG_ERR));
    fpmng_saturation_check(preg_match('/\[pool a\][^\n]*config unchanged -- sparing/', $log) === 1, 'pool a was not spared');

    /* The series of the spared pool must still be there, and not below what
     * the old master had counted. */
    $after = $series();
    fpmng_saturation_check($after['max_children_reached'] >= $before['max_children_reached'],
        'max_children_reached went back across the reload: ' . var_export($after, true));
    fpmng_saturation_check($after['listen_queue_max'] >= $before['listen_queue_max'],
        'listen_queue_max went back across the reload: ' . var_export($after, true));
    echo "saturation counters continue across the reload: ok\n";
    echo "Done\n";
} finally {
    foreach ($conns as $conn) {
        if (is_resource($conn)) {
            fclose($conn);
        }
    }
    $tester->terminate();
    $tester->close();
    @unlink("$root/sleep.php");
    @rmdir($root);
}
?>
--EXPECT--
saturation counters continue across the reload: ok
Done
--CLEAN--
<?php
require_once "tester.inc";
FPM\Tester::clean();
$stale = time() - 300;
foreach (glob(sys_get_temp_dir() . '/fpmng-sat-reload-*') as $dir) {
    if (@filemtime($dir) > $stale) {
        continue;
    }
    foreach (glob("$dir/*") as $file) {
        @unlink($file);
    }
    @rmdir($dir);
}
?>

--TEST--
fpm-ng: a pool spared by a selective reload keeps reporting application metrics (issue #384)
--XFAIL--
Issue #537: a pool spared by a selective reload loses its application metrics and its scoreboard. Delete this section when #537 is fixed.
--SKIPIF--
<?php
include "fpmng-skipif.inc";
fpmng_skip_if_pool_type_unsupported('http-direct');
?>
--FILE--
<?php

require_once "tester.inc";
require_once "fpmng-operator.inc";

/* Issue #330 spares an unchanged pool's children across the master's execvp().
 * The metrics region is anonymous shared memory the NEW master allocates
 * (fpm_metrics_init_main()), and a child binds to its slot once, at start. So
 * the question this test asks is whether a spared child keeps writing into a
 * region the new master's operator endpoint still renders.
 *
 * Pool [a] is spared (its section is byte-identical across the reload), pool
 * [b] changes. pm.max_children = 1 keeps [a] on one worker for the whole test,
 * so its counter is a running total and "continues from N" is checkable.
 * http-direct because both builds serve it. */
$root = sys_get_temp_dir() . '/fpmng-reload-sel-metrics-' . getmypid();
@mkdir($root, 0700, true);

file_put_contents("$root/a.php", <<<'PHP'
<?php
fpm_metric_register('a_hits', 'counter', 'a hits');
fpm_metric_inc('a_hits', 1.0);
echo getmypid();
PHP);
file_put_contents("$root/b.php", '<?php echo "b";');

$cfgBefore = <<<EOT
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
log_level = notice
reload.selective = yes

[a]
listen = {{ADDR[a]}}
chdir = $root
pm = static
pm.max_children = 1
pool.type = http-direct
http.front_controller = /a.php
operator.status_listen = {{ADDR[operator]}}
operator.status_path = /a-status
operator.metrics_listen = {{ADDR[operator]}}
operator.metrics_path = /a-metrics

[b]
listen = {{ADDR[b]}}
chdir = $root
pm = static
pm.max_children = 1
pool.type = http-direct
http.front_controller = /b.php
env[GENERATION] = 1
EOT;
/* [a] is identical in both files; only [b] differs. */
$cfgAfter = str_replace('env[GENERATION] = 1', 'env[GENERATION] = 2', $cfgBefore);

function hit(string $addr): string
{
    $fp = stream_socket_client("tcp://$addr", $errno, $error, 5);
    if (!$fp) {
        throw new RuntimeException("connect $addr: $error");
    }
    stream_set_timeout($fp, 5);
    fwrite($fp, "GET / HTTP/1.1\r\nHost: test\r\nConnection: close\r\n\r\n");
    $raw = (string) stream_get_contents($fp);
    fclose($fp);
    return explode("\r\n\r\n", $raw, 2)[1] ?? '';
}

function check(bool $condition, string $message): void
{
    if (!$condition) {
        throw new RuntimeException($message);
    }
}

function counter(string $operator): ?float
{
    $body = fpmng_operator_body($operator, '/a-metrics');
    return preg_match('/^a_hits\{pool="a"\} (\S+)$/m', $body, $m) ? (float) $m[1] : null;
}

$tester = new FPM\Tester($cfgBefore, '<?php');
try {
    @unlink($tester->getPrefixedFile(FPM\Tester::FILE_EXT_LOG_ERR));
    $tester->start([], false);
    $tester->switchLogSource('{{FILE:LOG}}');
    $tester->expectLogStartNotices();

    $operator = $tester->getListen('{{ADDR[operator]}}');
    $a = $tester->getListen('{{ADDR[a]}}');

    $pidBefore = hit($a);
    hit($a);
    hit($a);
    $before = counter($operator);
    check($before === 3.0, 'before the reload: ' . var_export($before, true));

    $tester->reload($cfgAfter);
    $tester->expectLogReloadingNotices(0);

    /* Without these two the test could pass or fail for the wrong reason: the
     * pool must really have been spared, and by the same worker. */
    $log = (string) @file_get_contents($tester->getPrefixedFile(FPM\Tester::FILE_EXT_LOG_ERR));
    check(preg_match('/\[pool a\][^\n]*config unchanged -- sparing/', $log) === 1, 'pool a was not spared');
    check(hit($a) === $pidBefore, 'pool a answered from a different worker after the reload');

    /* hit() above was the fourth request. */
    $after = counter($operator);
    check($after === 4.0, 'after the reload the series is ' . var_export($after, true) . ', expected 4');
    echo "metrics continue: ok\n";

    /* The scoreboard is shared memory too. */
    $status = fpmng_operator_body($operator, '/a-status');
    check(preg_match('/^total processes:\s+1$/m', $status) === 1, "status lost the spared worker:\n$status");
    echo "status keeps the worker: ok\n";
    echo "Done\n";
} finally {
    $tester->terminate();
    $tester->close();
    @unlink("$root/a.php");
    @unlink("$root/b.php");
    @rmdir($root);
}
?>
--EXPECT--
metrics continue: ok
status keeps the worker: ok
Done
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>

--TEST--
fpm-ng: a worker holds no memfd descriptors with reload.selective = yes, before and after a sparing reload (issue #691)
--SKIPIF--
<?php
include "fpmng-skipif.inc";
fpmng_skip_if_pool_type_unsupported('http-direct');
if (!is_dir('/proc/self/fd')) {
    die("skip needs /proc to list the worker's descriptors");
}
?>
--FILE--
<?php

require_once "tester.inc";
require_once "fpmng-operator.inc";

/* Issue #691: with reload.selective = yes the scoreboards and the metrics
 * region are memfd-backed with MFD_CLOEXEC, which closes across exec but not
 * across fork -- so every forked worker kept the descriptors open and a PHP
 * script could see and resize them through /proc/self/fd. The child now
 * closes them right after fork (fpm_reload_shm_child_init(), next to
 * fpm_metrics_child_init()), keeping the MAP_SHARED mappings, which is all
 * it ever uses.
 *
 * One http-direct pool, pm.max_children = 1: the front controller counts the
 * worker's own memfd descriptors and bumps a metric, so the test pins both
 * halves of the fix -- no descriptors, and the mappings (metrics, scoreboard)
 * still shared with the master. Reloading the byte-identical configuration
 * spares the pool, so the second half also proves the adopted worker (an old
 * process, whose descriptors were closed at its own fork) still serves and
 * still writes into the region the new master renders. */
$root = sys_get_temp_dir() . '/fpmng-reload-sel-no-memfd-' . getmypid();
@mkdir($root, 0700, true);

file_put_contents("$root/a.php", <<<'PHP'
<?php
fpm_metric_register('a_hits', 'counter', 'a hits');
fpm_metric_inc('a_hits', 1.0);
$memfds = 0;
foreach (scandir('/proc/self/fd') as $fd) {
    if ($fd === '.' || $fd === '..') {
        continue;
    }
    $target = @readlink("/proc/self/fd/$fd");
    if (is_string($target) && str_starts_with($target, 'memfd:')) {
        $memfds++;
    }
}
echo 'pid ' . getmypid() . ' memfds ' . $memfds;
PHP);

$cfg = <<<EOT
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
EOT;

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

function workerMemfds(string $body): int
{
    check(preg_match('/^pid (\d+) memfds (\d+)$/', trim($body), $m) === 1, 'unexpected body: ' . var_export($body, true));
    return (int) $m[2];
}

function workerPid(string $body): int
{
    preg_match('/^pid (\d+) memfds (\d+)$/', trim($body), $m);
    return (int) $m[1];
}

function counter(string $operator): ?float
{
    $body = fpmng_operator_body($operator, '/a-metrics');
    return preg_match('/^a_hits\{pool="a"\} (\S+)$/m', $body, $m) ? (float) $m[1] : null;
}

$tester = new FPM\Tester($cfg, '<?php');
try {
    @unlink($tester->getPrefixedFile(FPM\Tester::FILE_EXT_LOG_ERR));
    $tester->start([], false);
    $tester->switchLogSource('{{FILE:LOG}}');
    $tester->expectLogStartNotices();

    $operator = $tester->getListen('{{ADDR[operator]}}');
    $a = $tester->getListen('{{ADDR[a]}}');

    $pidBefore = workerPid(hit($a));
    check(workerMemfds(hit($a)) === 0, 'the worker holds memfd descriptors');
    echo "no memfds on start: ok\n";

    /* Byte-identical configuration: the reload spares the pool instead of
     * restarting it. */
    $tester->reload($cfg);
    $tester->expectLogReloadingNotices(0);

    $log = (string) @file_get_contents($tester->getPrefixedFile(FPM\Tester::FILE_EXT_LOG_ERR));
    check(preg_match('/\[pool a\][^\n]*config unchanged -- sparing/', $log) === 1, 'pool a was not spared');

    $after = hit($a);
    check(workerPid($after) === $pidBefore, 'pool a answered from a different worker after the reload');
    check(workerMemfds($after) === 0, 'the spared worker holds memfd descriptors');
    echo "no memfds after a sparing reload: ok\n";

    /* The hit() above was the fourth request overall; the counter reaching 4
     * through the new master's endpoint proves the spared worker still
     * writes into the region the new master renders, i.e. closing the
     * descriptors did not take the mappings with them. */
    $n = counter($operator);
    check($n === 4.0, 'after the reload the series is ' . var_export($n, true) . ', expected 4');
    echo "metrics continue: ok\n";

    $status = fpmng_operator_body($operator, '/a-status');
    check(preg_match('/^total processes:\s+1$/m', $status) === 1, "status lost the spared worker:\n$status");
    echo "status keeps the worker: ok\n";
    echo "Done\n";
} finally {
    $tester->terminate();
    $tester->close();
    @unlink("$root/a.php");
    @rmdir($root);
}
?>
--EXPECT--
no memfds on start: ok
no memfds after a sparing reload: ok
metrics continue: ok
status keeps the worker: ok
Done
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>

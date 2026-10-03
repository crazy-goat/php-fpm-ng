--TEST--
fpm-ng: a replaced pool of unchanged size does not take over its old metrics slot while a pool is spared (issue #537)
--SKIPIF--
<?php
include "fpmng-skipif.inc";
fpmng_skip_if_pool_type_unsupported('http-direct');
?>
--FILE--
<?php

require_once "tester.inc";
require_once "fpmng-operator.inc";

/* Pool [z] comes first and is changed without changing its size, [a] after
 * it is spared. [z]'s previous workers may still write to the old slot for a
 * while (a #329 survivor), so the new [z] must not be given that slot: it is
 * placed after [a]'s. Both pools must keep exact counters either way: [a]
 * continues, [z] restarts from zero. */
$root = sys_get_temp_dir() . '/fpmng-reload-sel-fresh-' . getmypid();
@mkdir($root, 0700, true);

file_put_contents("$root/a.php", <<<'PHP'
<?php
fpm_metric_register('a_hits', 'counter', 'a hits');
fpm_metric_inc('a_hits', 1.0);
echo getmypid();
PHP);
file_put_contents("$root/z.php", <<<'PHP'
<?php
fpm_metric_register('z_hits', 'counter', 'z hits');
fpm_metric_inc('z_hits', 1.0);
echo getmypid();
PHP);

$cfgBefore = <<<EOT
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
log_level = notice
reload.selective = yes

[z]
listen = {{ADDR[z]}}
chdir = $root
pm = static
pm.max_children = 1
pool.type = http-direct
http.front_controller = /z.php
operator.metrics_listen = {{ADDR[operatorz]}}
operator.metrics_path = /z-metrics

[a]
listen = {{ADDR[a]}}
chdir = $root
pm = static
pm.max_children = 1
pool.type = http-direct
http.front_controller = /a.php
operator.metrics_listen = {{ADDR[operatora]}}
operator.metrics_path = /a-metrics
EOT;
$cfgAfter = str_replace('[z]
listen = {{ADDR[z]}}
chdir = ' . $root . '
pm = static
pm.max_children = 1', '[z]
listen = {{ADDR[z]}}
chdir = ' . $root . '
pm = static
pm.max_requests = 1000
pm.max_children = 1', $cfgBefore);

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

function counter(string $operator, string $path, string $pool, string $name): ?float
{
    $body = fpmng_operator_body($operator, $path);
    return preg_match('/^' . $name . '\{pool="' . $pool . '"\} (\S+)$/m', $body, $m) ? (float) $m[1] : null;
}

$tester = new FPM\Tester($cfgBefore, '<?php');
try {
    @unlink($tester->getPrefixedFile(FPM\Tester::FILE_EXT_LOG_ERR));
    $tester->start([], false);
    $tester->switchLogSource('{{FILE:LOG}}');
    $tester->expectLogStartNotices();

    $opA = $tester->getListen('{{ADDR[operatora]}}');
    $opZ = $tester->getListen('{{ADDR[operatorz]}}');
    $a = $tester->getListen('{{ADDR[a]}}');
    $z = $tester->getListen('{{ADDR[z]}}');

    $pidBefore = hit($a);
    hit($a);
    hit($z);
    hit($z);
    check(counter($opA, '/a-metrics', 'a', 'a_hits') === 2.0, 'before: a');
    check(counter($opZ, '/z-metrics', 'z', 'z_hits') === 2.0, 'before: z');

    $tester->reload($cfgAfter);
    $tester->expectLogReloadingNotices(0);

    $log = (string) @file_get_contents($tester->getPrefixedFile(FPM\Tester::FILE_EXT_LOG_ERR));
    check(preg_match('/\[pool a\][^\n]*config unchanged -- sparing/', $log) === 1, 'pool a was not spared');
    check(hit($a) === $pidBefore, 'pool a answered from a different worker after the reload');

    $after = counter($opA, '/a-metrics', 'a', 'a_hits');
    check($after === 3.0, 'after the reload a_hits is ' . var_export($after, true) . ', expected 3');
    echo "spared pool continues: ok\n";

    hit($z);
    $zAfter = counter($opZ, '/z-metrics', 'z', 'z_hits');
    check($zAfter === 1.0, 'after the reload z_hits is ' . var_export($zAfter, true) . ', expected 1');
    echo "changed pool restarts from zero: ok\n";
    echo "Done\n";
} finally {
    $tester->terminate();
    $tester->close();
    @unlink("$root/a.php");
    @unlink("$root/z.php");
    @rmdir($root);
}
?>
--EXPECT--
spared pool continues: ok
changed pool restarts from zero: ok
Done
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>

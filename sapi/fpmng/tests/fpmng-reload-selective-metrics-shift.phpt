--TEST--
fpm-ng: a spared pool keeps its metrics slots when an earlier pool grows across a selective reload (issue #537)
--SKIPIF--
<?php
include "fpmng-skipif.inc";
fpmng_skip_if_pool_type_unsupported('http-direct');
?>
--FILE--
<?php

require_once "tester.inc";
require_once "fpmng-operator.inc";

/* The metrics slot a worker writes to is bound once, when it starts, from its
 * pool's base. Pool [z] comes first and grows from one worker to two while
 * [a], after it, is spared: a base computed as "sum of the earlier pools'
 * pm.max_children" would now point [a]'s series at slots its worker never
 * wrote to. [z] is changed, so its workers restart and its counter starts
 * from zero; [a]'s must continue. */
$root = sys_get_temp_dir() . '/fpmng-reload-sel-shift-' . getmypid();
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
pm.max_children = 2', $cfgBefore);

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

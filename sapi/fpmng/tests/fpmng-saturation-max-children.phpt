--TEST--
fpm-ng: a saturated fastcgi or classic pool counts max_children_reached and shows its listen queue (issue #644)
--SKIPIF--
<?php
include "fpmng-skipif.inc";
fpmng_skip_if_pool_type_unsupported('http-direct');
if (PHP_OS_FAMILY !== 'Linux') {
    die('skip the listen queue is read with TCP_INFO, which only Linux builds sample');
}
?>
--FILE--
<?php

require_once "tester.inc";
require_once "fpmng-operator.inc";
require_once "fpmng-saturation.inc";

/* pool.type = http-direct enforces pm = static in production (see
 * fpm_http_direct_validate_common()), so a classic pool never reaches
 * pm.max_children in a shipped build and never reports max_children_reached.
 * This test asks the binary to relax that one check, as
 * fpmng-http-direct-scale-down-drain.phpt does, so the counter's move can be
 * seen on the classic path too. The master code that counts is the same for
 * every type. */
putenv('FPMNG_TEST_ALLOW_NONSTATIC_DIRECT=1');

/* Four pools, one operator listener.
 *
 * fcgi and classic are dynamic with pm.max_children = 1 and no spare child:
 * a busy child with a second connection queued is the case upstream counts as
 * "reached pm.max_children". fixed is static, so the same queue appears but
 * max_children_reached must not. unix listens on a unix socket, which has no
 * TCP_INFO queue, so no listen queue series may appear for it. */
$root = sys_get_temp_dir() . '/fpmng-sat-max-' . getmypid();
@mkdir($root, 0700, true);
file_put_contents("$root/sleep.php", '<?php sleep(5); echo "done";');

$cfg = <<<EOT
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
log_level = notice

[fcgi]
listen = {{ADDR[fcgi]}}
chdir = $root
pm = dynamic
pm.max_children = 1
pm.start_servers = 1
pm.min_spare_servers = 1
pm.max_spare_servers = 1
operator.metrics_listen = {{ADDR[operator]}}
operator.metrics_path = /metrics/fcgi
operator.status_listen = {{ADDR[operator]}}
operator.status_path = /fcgi-status

[fixed]
listen = {{ADDR[fixed]}}
chdir = $root
pm = static
pm.max_children = 1
operator.metrics_listen = {{ADDR[operator]}}
operator.metrics_path = /metrics/fixed

[classic]
listen = {{ADDR[classic]}}
pool.type = http-direct
http.front_controller = /sleep.php
chdir = $root
pm = dynamic
pm.max_children = 1
pm.start_servers = 1
pm.min_spare_servers = 1
pm.max_spare_servers = 1
operator.metrics_listen = {{ADDR[operator]}}
operator.metrics_path = /metrics/classic
operator.status_listen = {{ADDR[operator]}}
operator.status_path = /classic-status

[unix]
listen = $root/unix.sock
chdir = $root
pm = static
pm.max_children = 1
operator.metrics_listen = {{ADDR[operator]}}
operator.metrics_path = /metrics/unix
EOT;

$tester = new FPM\Tester($cfg, '<?php');
$conns = [];
try {
    @unlink($tester->getPrefixedFile(FPM\Tester::FILE_EXT_LOG_ERR));
    $tester->start([], false);
    $tester->switchLogSource('{{FILE:LOG}}');
    $tester->expectLogStartNotices();

    $operator = $tester->getListen('{{ADDR[operator]}}');
    $fcgi = $tester->getListen('{{ADDR[fcgi]}}');
    $fixed = $tester->getListen('{{ADDR[fixed]}}');
    $classic = $tester->getListen('{{ADDR[classic]}}');

    /* fastcgi, dynamic: the child accepts a connection and waits for its
     * request headers, so it is not idle; the second connection waits in the
     * backlog. */
    $metricsOf = static function (string $pool) use ($operator): string {
        return fpmng_operator_body($operator, "/metrics/$pool");
    };
    $conns[] = fpmng_saturation_fcgi_start($fcgi, "$root/sleep.php");
    $conns[] = fpmng_saturation_connect($fcgi);
    fpmng_saturation_wait(static function () use ($metricsOf): bool {
        $body = $metricsOf('fcgi');
        return (fpmng_saturation_value($body, 'fpmng_pool_max_children_reached_total{pool="fcgi"}') ?? 0) >= 1
            && (fpmng_saturation_value($body, 'fpmng_pool_listen_queue_max{pool="fcgi"}') ?? 0) >= 1;
    }, 'the dynamic fastcgi pool to saturate', static fn(): string => $metricsOf('fcgi'));

    $body = $metricsOf('fcgi');
    fpmng_saturation_check((fpmng_saturation_value($body, 'fpmng_pool_listen_queue_length{pool="fcgi"}') ?? 0) > 0,
        "listen backlog not reported:\n$body");
    fpmng_saturation_check(str_contains($body, 'fpmng_pool_listen_queue{pool="fcgi"} '),
        "listen queue not reported:\n$body");

    $status = json_decode(fpmng_operator_body($operator, '/fcgi-status'), true, 512, JSON_THROW_ON_ERROR);
    $row = $status['pools'][0];
    fpmng_saturation_check(($row['listen_queue_max'] ?? 0) >= 1 && isset($row['listen_queue_length'], $row['max_children_reached']),
        'fastcgi status JSON lacks the saturation keys: ' . var_export($row, true));
    echo "dynamic fastcgi pool counts max_children_reached and listen queue: ok\n";

    /* classic: the child runs a request that sleeps, and the second connection
     * waits behind it. */
    $conns[] = fpmng_saturation_connect($classic, "GET / HTTP/1.1\r\nHost: test\r\nConnection: close\r\n\r\n");
    $conns[] = fpmng_saturation_connect($classic);
    fpmng_saturation_wait(static function () use ($metricsOf): bool {
        $body = $metricsOf('classic');
        return (fpmng_saturation_value($body, 'fpmng_pool_max_children_reached_total{pool="classic"}') ?? 0) >= 1
            && (fpmng_saturation_value($body, 'fpmng_pool_listen_queue_max{pool="classic"}') ?? 0) >= 1;
    }, 'the dynamic classic pool to saturate', static fn(): string => $metricsOf('classic'));

    $text = fpmng_operator_body($operator, '/classic-status');
    fpmng_saturation_check(preg_match('/^listen queue:\s+\d+$/m', $text) === 1
        && preg_match('/^max listen queue:\s+[1-9]\d*$/m', $text) === 1,
        "classic status text lacks the listen queue lines:\n$text");
    $status = json_decode(fpmng_operator_body($operator, '/classic-status?json'), true, 512, JSON_THROW_ON_ERROR);
    fpmng_saturation_check(isset($status['listen queue'], $status['max listen queue'], $status['listen queue length'])
        && $status['max listen queue'] >= 1,
        'classic status JSON lacks the listen queue keys: ' . var_export($status, true));
    echo "classic pool counts max_children_reached and shows its listen queue: ok\n";

    /* fixed: the queue is measured, pm.max_children is not counted. Give the
     * heartbeat a few seconds so a wrong counter would have shown up. */
    $conns[] = fpmng_saturation_fcgi_start($fixed, "$root/sleep.php");
    $conns[] = fpmng_saturation_connect($fixed);
    fpmng_saturation_wait(static function () use ($metricsOf): bool {
        return (fpmng_saturation_value($metricsOf('fixed'), 'fpmng_pool_listen_queue_max{pool="fixed"}') ?? 0) >= 1;
    }, 'the static pool to show a queue', static fn(): string => $metricsOf('fixed'));
    $body = $metricsOf('fixed');
    fpmng_saturation_check(!str_contains($body, 'max_children_reached_total{pool="fixed"}'),
        "static pool reports max_children_reached:\n$body");
    echo "static pool shows a listen queue and no max_children_reached: ok\n";

    /* unix: no TCP_INFO queue, so the listen queue series are left out. */
    $body = $metricsOf('unix');
    fpmng_saturation_check(str_contains($body, 'fpmng_pool_info{pool="unix",type="fastcgi"} 1'),
        "unix pool missing from metrics:\n$body");
    /* The HELP and TYPE lines name the series on every page; only a sample line
     * (name followed by a brace) would mean the pool reports it. */
    fpmng_saturation_check(!str_contains($body, 'fpmng_pool_listen_queue{')
        && !str_contains($body, 'fpmng_pool_max_children_reached_total{'),
        "unix pool reports saturation series:\n$body");
    echo "unix listener reports no listen queue: ok\n";
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
    @unlink("$root/unix.sock");
    @rmdir($root);
}
?>
--EXPECT--
dynamic fastcgi pool counts max_children_reached and listen queue: ok
classic pool counts max_children_reached and shows its listen queue: ok
static pool shows a listen queue and no max_children_reached: ok
unix listener reports no listen queue: ok
Done
--CLEAN--
<?php
require_once "tester.inc";
FPM\Tester::clean();
$stale = time() - 300;
foreach (glob(sys_get_temp_dir() . '/fpmng-sat-max-*') as $dir) {
    if (@filemtime($dir) > $stale) {
        continue;
    }
    foreach (glob("$dir/*") as $file) {
        @unlink($file);
    }
    @rmdir($dir);
}
?>

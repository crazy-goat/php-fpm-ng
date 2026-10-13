--TEST--
fpm-ng: slow requests are counted on fastcgi and classic pools that set request_slowlog_timeout (issue #644)
--SKIPIF--
<?php
include "fpmng-skipif.inc";
fpmng_skip_if_pool_type_unsupported('http-direct');
?>
--FILE--
<?php

require_once "tester.inc";
require_once "fpmng-operator.inc";
require_once "fpmng-saturation.inc";

/* request_slowlog_timeout is the only switch for the slow-request count, so
 * two pools carry it (fastcgi and classic) and one pool does not. The quiet
 * fastcgi pool runs the same slow script and must not show the series at all:
 * a counter that is always zero would say "no slow requests" where the honest
 * answer is "not measured". */
$root = sys_get_temp_dir() . '/fpmng-sat-slow-' . getmypid();
@mkdir($root, 0700, true);
file_put_contents("$root/slow.php", '<?php sleep(3); echo "done";');

$cfg = <<<EOT
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
log_level = notice

[slowfcgi]
listen = {{ADDR[slowfcgi]}}
chdir = $root
pm = static
pm.max_children = 1
request_slowlog_timeout = 1s
slowlog = $root/slowfcgi.log
operator.metrics_listen = {{ADDR[operator]}}
operator.metrics_path = /metrics/slowfcgi

[slowclassic]
listen = {{ADDR[slowclassic]}}
pool.type = http-direct
http.front_controller = /slow.php
chdir = $root
pm = static
pm.max_children = 1
request_slowlog_timeout = 1s
slowlog = $root/slowclassic.log
operator.metrics_listen = {{ADDR[operator]}}
operator.metrics_path = /metrics/slowclassic

[quiet]
listen = {{ADDR[quiet]}}
chdir = $root
pm = static
pm.max_children = 1
operator.metrics_listen = {{ADDR[operator]}}
operator.metrics_path = /metrics/quiet
EOT;

$tester = new FPM\Tester($cfg, '<?php');
try {
    @unlink($tester->getPrefixedFile(FPM\Tester::FILE_EXT_LOG_ERR));
    $tester->start([], false);
    $tester->switchLogSource('{{FILE:LOG}}');
    $tester->expectLogStartNotices();

    $operator = $tester->getListen('{{ADDR[operator]}}');
    $slowFcgi = $tester->getListen('{{ADDR[slowfcgi]}}');
    $slowClassic = $tester->getListen('{{ADDR[slowclassic]}}');
    $quiet = $tester->getListen('{{ADDR[quiet]}}');

    $slowCount = static function (string $pool) use ($operator): ?float {
        $body = fpmng_operator_body($operator, "/metrics/$pool");
        return fpmng_saturation_value($body, "fpmng_pool_slow_requests_total{pool=\"$pool\"}");
    };

    /* Each request runs three seconds against a one-second slowlog timeout.
     * The count moves while the request still runs, so it is read after the
     * answer and polled for a little longer. */
    $tester->request(address: $slowFcgi, scriptFilename: "$root/slow.php", scriptName: '/slow.php')
        ->expectBody('done', skipHeadersCheck: true);
    fpmng_saturation_wait(static fn(): bool => ($slowCount('slowfcgi') ?? 0) >= 1, 'the fastcgi slow request to count');
    fpmng_saturation_check($slowCount('slowfcgi') === 1.0, 'fastcgi slow requests: ' . var_export($slowCount('slowfcgi'), true));
    echo "fastcgi pool counts a slow request: ok\n";

    fpmng_saturation_check(str_contains(fpmng_saturation_get($slowClassic, '/'), 'done'), 'classic request did not answer');
    fpmng_saturation_wait(static fn(): bool => ($slowCount('slowclassic') ?? 0) >= 1, 'the classic slow request to count');
    fpmng_saturation_check($slowCount('slowclassic') === 1.0, 'classic slow requests: ' . var_export($slowCount('slowclassic'), true));
    echo "classic pool counts a slow request: ok\n";

    /* quiet runs the same slow script but has no request_slowlog_timeout. */
    $tester->request(address: $quiet, scriptFilename: "$root/slow.php", scriptName: '/slow.php')
        ->expectBody('done', skipHeadersCheck: true);
    $body = fpmng_operator_body($operator, '/metrics/quiet');
    fpmng_saturation_check(!str_contains($body, 'slow_requests_total{pool="quiet"}'),
        "pool without request_slowlog_timeout reports slow requests:\n$body");
    echo "pool without request_slowlog_timeout reports no slow requests: ok\n";
    echo "Done\n";
} finally {
    $tester->terminate();
    $tester->close();
    @unlink("$root/slow.php");
    @unlink("$root/slowfcgi.log");
    @unlink("$root/slowclassic.log");
    @rmdir($root);
}
?>
--EXPECT--
fastcgi pool counts a slow request: ok
classic pool counts a slow request: ok
pool without request_slowlog_timeout reports no slow requests: ok
Done
--CLEAN--
<?php
require_once "tester.inc";
FPM\Tester::clean();
$stale = time() - 300;
foreach (glob(sys_get_temp_dir() . '/fpmng-sat-slow-*') as $dir) {
    if (@filemtime($dir) > $stale) {
        continue;
    }
    foreach (glob("$dir/*") as $file) {
        @unlink($file);
    }
    @rmdir($dir);
}
?>

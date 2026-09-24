--TEST--
fpm-ng: fastcgi pools expose their own application metrics on a shared operator listener (issue #383)
--SKIPIF--
<?php include "skipif.inc"; ?>
--FILE--
<?php
require_once "tester.inc";
require_once "fpmng-operator.inc";

$work = sys_get_temp_dir() . '/fpmng-fastcgi-operator-' . getmypid();
@mkdir($work, 0700, true);
$script = <<<'PHP'
<?php
$target = getenv('TARGET');
fpm_metric_register('fastcgi_probe_total', 'counter', 'FastCGI application probe');
fpm_metric_inc('fastcgi_probe_total', 1, ['target' => $target]);
echo "ran:$target";
PHP;
file_put_contents("$work/metric.php", $script);

$cfg = <<<EOT
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
[app]
listen = {{ADDR[app]}}
pm = static
pm.max_children = 1
chdir = $work
env[TARGET] = app
pm.status_path = /fpm-status
operator.metrics_listen = {{ADDR[operator]}}
operator.metrics_path = /metrics/app
[api]
listen = {{ADDR[api]}}
pm = static
pm.max_children = 1
chdir = $work
env[TARGET] = api
operator.metrics_listen = {{ADDR[operator]}}
operator.metrics_path = /metrics/api
EOT;

$tester = new FPM\Tester($cfg, $script);
try {
    $tester->start();
    $tester->expectLogStartNotices();

    $app = $tester->getListen('{{ADDR[app]}}');
    $api = $tester->getListen('{{ADDR[api]}}');
    $operator = $tester->getListen('{{ADDR[operator]}}');
    $tester->request(address: $app, scriptFilename: "$work/metric.php", scriptName: '/metric.php')
        ->expectBody('ran:app', skipHeadersCheck: true);
    $tester->request(address: $api, scriptFilename: "$work/metric.php", scriptName: '/metric.php')
        ->expectBody('ran:api', skipHeadersCheck: true);

    foreach (['app', 'api'] as $pool) {
        $metrics = fpmng_operator_body($operator, "/metrics/$pool");
        foreach ([
            "fpmng_pool_info{pool=\"$pool\",type=\"fastcgi\"} 1",
            "fastcgi_probe_total{pool=\"$pool\",target=\"$pool\"} 1",
        ] as $needle) {
            if (!str_contains($metrics, $needle)) {
                throw new RuntimeException("$pool metrics missing $needle: $metrics");
            }
        }
    }
    echo "two fastcgi pools expose their own metrics: ok\n";

    /* operator.* does not take over upstream's distinct pm.status_path on the
     * same fastcgi pool. The endpoint requests above used a separate socket. */
    $legacy = $tester->request(address: $app, uri: '/fpm-status',
        scriptFilename: "$work/metric.php", scriptName: '/fpm-status')->getBody('text/plain');
    if (!is_string($legacy) || !str_contains($legacy, 'pool:                 app')) {
        throw new RuntimeException('pm.status_path did not remain on the FastCGI socket: '
            . var_export($legacy, true));
    }
    echo "upstream pm.status_path remains on FastCGI: ok\n";
    echo "Done\n";
} finally {
    $tester->terminate();
    $tester->close();
    @unlink("$work/metric.php");
    @rmdir($work);
}
?>
--EXPECT--
two fastcgi pools expose their own metrics: ok
upstream pm.status_path remains on FastCGI: ok
Done
--CLEAN--
<?php
require_once "tester.inc";
FPM\Tester::clean();
$stale = time() - 300;
foreach (glob(sys_get_temp_dir() . '/fpmng-fastcgi-operator-*') as $dir) {
    if (@filemtime($dir) > $stale) {
        continue;
    }
    foreach (glob("$dir/*") as $file) {
        @unlink($file);
    }
    @rmdir($dir);
}
?>

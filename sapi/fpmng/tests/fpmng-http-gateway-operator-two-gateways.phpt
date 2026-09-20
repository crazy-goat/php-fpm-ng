--TEST--
fpm-ng: several gateways with http.operator = yes each expose the whole set under their own base (issue #389)
--SKIPIF--
<?php
include "fpmng-skipif.inc";
fpmng_skip_if_pool_type_unsupported('gateway');
?>
--FILE--
<?php

require_once "tester.inc";
require_once "fpmng-operator.inc";

/* Issue #389, acceptance criterion 5. Two gateways, both http.operator = yes,
 * different bases: gw1 keeps the default /metrics, gw2 sets /m. Both forward
 * the SAME exposed pool (app) to the SAME operator listener, each under its own
 * base, and each serves its own page at its own bare base. There is no
 * per-gateway pool list: the map is built from the pool that exposed itself. */

$root = sys_get_temp_dir() . '/fpmng-gw-operator-two-' . getmypid();
@mkdir($root, 0700, true);
file_put_contents($root . '/front.php', '<?php echo "php:" . $_SERVER["REQUEST_URI"];');

$cfg = <<<EOT
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}

[gw1]
pool.type = gateway
listen = {{ADDR[http1]}}
chdir = $root
http.gateways = 1
http.route[app] = /
http.operator = yes
http.operator_allowed_clients = 127.0.0.1
operator.metrics_listen = {{ADDR[op1]}}
operator.status_listen = {{ADDR[op1]}}

[gw2]
pool.type = gateway
listen = {{ADDR[http2]}}
chdir = $root
http.gateways = 1
http.route[app] = /
http.operator = yes
http.operator_allowed_clients = 127.0.0.1
operator.metrics_listen = {{ADDR[op2]}}
operator.metrics_path = /m
operator.status_listen = {{ADDR[op2]}}

[app]
pool.type = http-direct
listen = {{ADDR[app]}}
pm = static
pm.max_children = 2
chdir = $root
http.front_controller = /front.php
operator.metrics_listen = {{ADDR[operator]}}
operator.metrics = on
EOT;

function gatewayGet(string $url): array
{
    $ctx = stream_context_create(['http' => ['timeout' => 10, 'ignore_errors' => true]]);
    $body = @file_get_contents($url, false, $ctx);
    $headers = $http_response_header ?? [];
    $status = 0;
    foreach ($headers as $h) {
        if (preg_match('#^HTTP/\S+\s+(\d+)#', $h, $m)) {
            $status = (int) $m[1];
        }
    }
    return [$status, $body === false ? '' : $body];
}

$tester = new FPM\Tester($cfg, '<?php echo "unused";');
try {
    $tester->start();
    $tester->expectLogStartNotices();

    $http1 = $tester->getAddr('ipv4', '[http1]');
    $http2 = $tester->getAddr('ipv4', '[http2]');
    $operator = $tester->getListen('{{ADDR[operator]}}');
    $local = fpmng_operator_body($operator, '/metrics/app');

    [$status, $body] = gatewayGet("http://$http1/metrics/app");
    if ($status !== 200 || $body !== $local) {
        throw new RuntimeException("gw1 /metrics/app did not forward app's page: $status\n$body");
    }
    echo "gw1: /metrics/app\n";

    [$status, $body] = gatewayGet("http://$http2/m/app");
    if ($status !== 200 || $body !== $local) {
        throw new RuntimeException("gw2 /m/app did not forward app's page: $status\n$body");
    }
    echo "gw2: /m/app\n";

    /* Each gateway's own page is at its own bare base and carries its own name,
     * so the two are not the same page. */
    [$status, $body] = gatewayGet("http://$http1/metrics");
    if ($status !== 200 || !str_contains($body, 'pool="gw1"')) {
        throw new RuntimeException("gw1's own page is not at /metrics: $status\n$body");
    }
    [$status, $body] = gatewayGet("http://$http2/m");
    if ($status !== 200 || !str_contains($body, 'pool="gw2"')) {
        throw new RuntimeException("gw2's own page is not at /m: $status\n$body");
    }
    echo "own pages: at each bare base\n";

    echo "Done\n";
} finally {
    $tester->terminate();
    $tester->close();
    @unlink($root . '/front.php');
    @rmdir($root);
}
?>
--EXPECT--
gw1: /metrics/app
gw2: /m/app
own pages: at each bare base
Done
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>

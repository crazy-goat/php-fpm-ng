--TEST--
fpm-ng: http.operator exact matches only -- a miss is a local 404 from the gateway, never a forward (issue #389)
--SKIPIF--
<?php
include "fpmng-skipif.inc";
fpmng_skip_if_pool_type_unsupported('gateway');
?>
--FILE--
<?php

require_once "tester.inc";

/* Issue #389, acceptance criterion 2. Anything under the base that is not in
 * the config-time map is a local 404 and is NEVER forwarded. That is a
 * guarantee, not an optimisation: the operator listener's own 404 lists every
 * path it knows (fpm_operator_http.c), which on loopback is a convenience and
 * on a public port would enumerate the pools.
 *
 * The proof of "no forward" available from outside is the 404 BODY: the
 * gateway's own evhttp 404 carries no path list, while the operator listener's
 * 404 starts with "not found: ..." and then "known paths: ...". If the miss
 * reached the operator listener, the body would carry that list. The test
 * asserts both sides of that distinction. */

$root = sys_get_temp_dir() . '/fpmng-gw-operator-miss-' . getmypid();
@mkdir($root, 0700, true);
file_put_contents($root . '/front.php', '<?php echo "php:" . $_SERVER["REQUEST_URI"];');

$cfg = <<<EOT
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}

[gw]
pool.type = gateway
listen = {{ADDR[http]}}
chdir = $root
http.gateways = 1
http.route[app] = /
http.operator = yes
http.operator_allowed_clients = 127.0.0.1
operator.metrics_listen = {{ADDR[operator]}}

[app]
pool.type = http-direct
listen = {{ADDR[app]}}
pm = static
pm.max_children = 2
chdir = $root
http.front_controller = /front.php
operator.metrics_listen = {{ADDR[operator]}}
operator.metrics_path = /_m
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

function operatorGet(string $addr, string $path): array
{
    $fp = @stream_socket_client("tcp://$addr", $errno, $error, 5);
    if (!$fp) {
        throw new RuntimeException("connect $addr: $error");
    }
    stream_set_timeout($fp, 5);
    fwrite($fp, "GET $path HTTP/1.1\r\nHost: operator\r\nConnection: close\r\n\r\n");
    $raw = (string) stream_get_contents($fp);
    fclose($fp);
    $split = explode("\r\n\r\n", $raw, 2);
    if (count($split) !== 2 || !preg_match('#^HTTP/1\.1 (\d+)#', $split[0], $m)) {
        throw new RuntimeException("bad response for $path: " . var_export($raw, true));
    }
    return [(int) $m[1], $split[1]];
}

$tester = new FPM\Tester($cfg, '<?php echo "unused";');
try {
    $tester->start();
    $tester->expectLogStartNotices();

    $http = $tester->getAddr('ipv4', '[http]');
    $operator = $tester->getListen('{{ADDR[operator]}}');

    /* The exposed path works, so the map is not empty. */
    [$status, $body] = gatewayGet("http://$http/metrics/app");
    if ($status !== 200 || !str_contains($body, 'pool="app"')) {
        throw new RuntimeException("the exposed path did not work: $status\n$body");
    }
    echo "exposed path: ok\n";

    /* The exact miss: 404, answered locally. */
    [$status, $body] = gatewayGet("http://$http/metrics/nope");
    if ($status !== 404) {
        throw new RuntimeException("the miss answered $status, not 404:\n$body");
    }
    if (str_contains($body, 'known paths:') || str_contains($body, '/_m')) {
        throw new RuntimeException("the miss was forwarded to the operator listener:\n$body");
    }
    echo "miss: local 404, no forward\n";

    /* The operator listener's own 404 DOES carry the list -- so the assertion
     * above tells the two apart rather than passing by accident. */
    [$status, $body] = operatorGet($operator, '/metrics/nope');
    if ($status !== 404 || !str_contains($body, 'known paths:')) {
        throw new RuntimeException("the operator listener's 404 lost its known-paths body: $status\n$body");
    }
    echo "operator 404: lists paths (distinguishable)\n";

    echo "Done\n";
} finally {
    $tester->terminate();
    $tester->close();
    @unlink($root . '/front.php');
    @rmdir($root);
}
?>
--EXPECT--
exposed path: ok
miss: local 404, no forward
operator 404: lists paths (distinguishable)
Done
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>

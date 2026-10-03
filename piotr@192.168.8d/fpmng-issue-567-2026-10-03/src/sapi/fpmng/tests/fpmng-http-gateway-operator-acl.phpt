--TEST--
fpm-ng: http.operator_allowed_clients is checked before the map lookup -- 403 for an existing and a non-existing pool path alike (issue #389)
--SKIPIF--
<?php
include "fpmng-skipif.inc";
fpmng_skip_if_pool_type_unsupported('gateway');
?>
--FILE--
<?php

require_once "tester.inc";

/* Issue #389, acceptance criterion 3. http.operator_allowed_clients is its own
 * ACL, separate from http.allowed_clients, and it is evaluated BEFORE the map
 * lookup: a stranger gets the same 403 for /metrics/app (a pool that exists)
 * and /metrics/nope (one that does not). Answering 404 for the second would be
 * an oracle telling them which pool names are configured.
 *
 * The test's peer is 127.0.0.1, so 10.9.9.9 denies it -- the ACL is the only
 * thing refusing these requests. Normal routed traffic ("/") is untouched. */

$root = sys_get_temp_dir() . '/fpmng-gw-operator-acl-' . getmypid();
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
http.operator_allowed_clients = 10.9.9.9
operator.metrics_listen = {{ADDR[operator]}}

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
    $http = $tester->getAddr('ipv4', '[http]');

    foreach (['/metrics/app' => 'an existing pool', '/metrics/nope' => 'a non-existing pool', '/metrics' => 'the gateway itself'] as $path => $what) {
        [$status, $body] = gatewayGet("http://$http$path");
        if ($status !== 403) {
            throw new RuntimeException("$path ($what) answered $status, expected 403:\n$body");
        }
        echo "$path: 403\n";
    }

    /* The operator ACL does not gate the ordinary routed traffic: "/" still
     * reaches the pool. */
    [$status, $body] = gatewayGet("http://$http/anything?x=1");
    if ($status !== 200 || !str_contains($body, 'php:/anything')) {
        throw new RuntimeException("routed traffic was affected by the operator ACL: $status\n$body");
    }
    echo "routed traffic: unaffected\n";

    echo "Done\n";
} finally {
    $tester->terminate();
    $tester->close();
    @unlink($root . '/front.php');
    @rmdir($root);
}
?>
--EXPECT--
/metrics/app: 403
/metrics/nope: 403
/metrics: 403
routed traffic: unaffected
Done
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>

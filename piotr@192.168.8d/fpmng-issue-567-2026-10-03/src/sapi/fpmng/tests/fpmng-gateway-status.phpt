--TEST--
fpm-ng: the gateway's own /status is JSON with one row per target (issue #390)
--SKIPIF--
<?php
include "fpmng-skipif.inc";
fpmng_skip_if_pool_type_unsupported('gateway');
?>
--FILE--
<?php

require_once "tester.inc";
require_once "fpmng-operator.inc";

/* Issue #390, acceptance criterion 3. The gateway's /status on its own
 * operator.status_path is the same numbers as /metrics, one row per target, in
 * the generic {"pools":[...]} shape a client already parses. The layout is
 * #targets + 2 target rows (the routed pools, "operator", "-") plus the pool
 * row carrying connections_open/ping_total; a ping is counted both there and on
 * the metrics page. */

$docroot = sys_get_temp_dir() . '/fpmng-gw-status-' . getmypid();
@mkdir($docroot, 0700, true);
file_put_contents($docroot . '/index.php', '<?php echo "ok";');

$cfg = <<<EOT
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
[gw]
pool.type = gateway
listen = {{ADDR[http]}}
chdir = $docroot
http.gateways = 1
http.front_controller = /index.php
http.route[app] = /api
http.route[web] = /
ping.path = /ping
ping.response = pong
operator.metrics_listen = {{ADDR[operator]}}
operator.status_listen = {{ADDR[operator]}}
[app]
pool.type = fastcgi
listen = {{ADDR[app]}}
pm = static
pm.max_children = 2
[web]
pool.type = fastcgi
listen = {{ADDR[web]}}
chdir = $docroot
pm = static
pm.max_children = 2
EOT;

function fetch(string $url): array
{
    $ctx = stream_context_create(['http' => ['timeout' => 10, 'ignore_errors' => true]]);
    $body = @file_get_contents($url, false, $ctx);
    $status = 0;
    foreach ($http_response_header ?? [] as $h) {
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
    $operator = $tester->getListen('{{ADDR[operator]}}');

    [$pingStatus, $pingBody] = fetch("http://$http/ping");
    if ($pingStatus !== 200 || trim($pingBody) !== 'pong') {
        echo "FAIL: ping answered status=$pingStatus body=$pingBody\n";
        exit(1);
    }

    [$status, $body] = fetch("http://$http/api/x");
    if ($status !== 200) {
        echo "FAIL: routed /api/x answered $status\n$body\n";
        exit(1);
    }

    $raw = fpmng_operator_body($operator, '/status');
    $decoded = json_decode($raw, true, flags: JSON_THROW_ON_ERROR);
    if (!isset($decoded['pools']) || !is_array($decoded['pools'])) {
        echo "FAIL: /status is not {\"pools\":[...]}: $raw\n";
        exit(1);
    }

    $poolRow = null;
    $targets = [];
    foreach ($decoded['pools'] as $row) {
        if (array_key_exists('target', $row)) {
            $targets[$row['target']] = $row;
        } else {
            $poolRow = $row;
        }
    }

    if ($poolRow === null || ($poolRow['name'] ?? null) !== 'gw' || ($poolRow['type'] ?? null) !== 'gateway') {
        echo "FAIL: no pool row for gw: $raw\n";
        exit(1);
    }
    if (($poolRow['ping_total'] ?? null) !== 1 || !array_key_exists('connections_open', $poolRow)) {
        echo "FAIL: pool row ping_total/connections_open wrong: $raw\n";
        exit(1);
    }
    echo "pool-row: ok\n";

    foreach (['app', 'web', 'operator', '-'] as $label) {
        if (!isset($targets[$label])) {
            echo "FAIL: no status row for target=\"$label\": $raw\n";
            exit(1);
        }
        if (!array_key_exists('requests', $targets[$label]) || !array_key_exists('rejected_503', $targets[$label])
            || !array_key_exists('upstreams_used', $targets[$label])) {
            echo "FAIL: target=\"$label\" row is missing numbers: $raw\n";
            exit(1);
        }
    }
    if ((int) $targets['app']['requests'] !== 1) {
        echo "FAIL: target=\"app\" requests should be 1: $raw\n";
        exit(1);
    }
    if ((int) $targets['-']['requests'] !== 1) {
        echo "FAIL: target=\"-\" requests should count the ping (1): $raw\n";
        exit(1);
    }
    echo "targets: app web operator -\n";

    echo "Done\n";
} finally {
    $tester->terminate();
    $tester->expectLogTerminatingNotices();
    $tester->close();
    @unlink($docroot . '/index.php');
    @rmdir($docroot);
}
?>
--EXPECT--
pool-row: ok
targets: app web operator -
Done
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>

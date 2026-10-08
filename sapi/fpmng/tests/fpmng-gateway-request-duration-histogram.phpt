--TEST--
fpm-ng: a gateway's per-target request-duration histogram counts every answered request in its row (issue #652)
--SKIPIF--
<?php
include "fpmng-skipif.inc";
fpmng_skip_if_pool_type_unsupported('gateway');
?>
--FILE--
<?php

require_once "tester.inc";
require_once "fpmng-operator.inc";

/* Issue #652. One gateway with one routed target, "web". Three requests go to
 * it: two fast ones and one that the target holds for 300 ms. A local 404 (a
 * miss under the operator base) is answered by the gateway itself and lands in
 * the "-" row. One operator page is forwarded and lands in the "operator" row.
 *
 * The histogram of a row must agree with that row's requests_total, must be
 * cumulative, and must put the slow request above the 0.25 s bucket. */

$docroot = sys_get_temp_dir() . '/fpmng-gw-duration-' . getmypid();
@mkdir($docroot, 0700, true);
file_put_contents($docroot . '/index.php', '<?php
if (str_starts_with($_SERVER["REQUEST_URI"], "/slow")) {
    usleep(300000);
}
echo "ok:" . $_SERVER["REQUEST_URI"];');

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
http.route[web] = /
http.operator = yes
http.operator_allowed_clients = 127.0.0.1
operator.metrics_listen = {{ADDR[operator]}}
operator.status_listen = {{ADDR[operator]}}
[web]
pool.type = http-direct
listen = {{ADDR[web]}}
pm = static
pm.max_children = 2
chdir = $docroot
http.front_controller = /index.php
operator.metrics_listen = {{ADDR[operator]}}
operator.metrics = on
EOT;

function request(string $url): array
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

function total(string $metrics, string $name, string $target): ?int
{
    $re = '/^' . preg_quote($name, '/') . '\{pool="gw",target="' . preg_quote($target, '/') . '"\} (\d+)$/m';
    return preg_match($re, $metrics, $m) ? (int) $m[1] : null;
}

/* The row's histogram: [le => cumulative count] in file order, the sum in
 * microseconds (the renderer prints it as seconds with six decimals), and the
 * _count. */
function histogram(string $metrics, string $target): array
{
    $name = 'fpmng_gateway_request_duration_seconds';
    $t = preg_quote($target, '/');
    $buckets = [];
    if (preg_match_all('/^' . $name . '_bucket\{pool="gw",target="' . $t . '",le="([^"]+)"\} (\d+)$/m', $metrics, $m, PREG_SET_ORDER)) {
        foreach ($m as $row) {
            $buckets[] = [$row[1], (int) $row[2]];
        }
    }
    $sum = null;
    if (preg_match('/^' . $name . '_sum\{pool="gw",target="' . $t . '"\} (\d+)\.(\d{6})$/m', $metrics, $s)) {
        $sum = (int) $s[1] * 1000000 + (int) $s[2];
    }
    $count = preg_match('/^' . $name . '_count\{pool="gw",target="' . $t . '"\} (\d+)$/m', $metrics, $c)
        ? (int) $c[1] : null;
    return ['buckets' => $buckets, 'sum_us' => $sum, 'count' => $count];
}

$tester = new FPM\Tester($cfg, '<?php echo "unused";');
try {
    $tester->start();
    $tester->expectLogStartNotices();

    $http = $tester->getAddr('ipv4', '[http]');
    $operator = $tester->getListen('{{ADDR[operator]}}');

    foreach (['/', '/'] as $path) {
        [$status] = request("http://$http$path");
        if ($status !== 200) {
            echo "FAIL: $path answered status=$status\n";
            exit(1);
        }
    }
    [$status] = request("http://$http/slow/1");
    if ($status !== 200) {
        echo "FAIL: /slow/1 answered status=$status\n";
        exit(1);
    }
    [$status] = request("http://$http/metrics/nope");
    if ($status !== 404) {
        echo "FAIL: /metrics/nope answered status=$status, want the gateway's own 404\n";
        exit(1);
    }
    [$status, $body] = request("http://$http/metrics/web");
    if ($status !== 200 || !str_contains($body, 'fpmng_')) {
        echo "FAIL: forwarded /metrics/web answered status=$status\n";
        exit(1);
    }
    echo "served: 2 fast, 1 slow, 1 local, 1 forwarded\n";

    $metrics = fpmng_operator_body($operator, '/metrics');

    if (!str_contains($metrics, "# TYPE fpmng_gateway_request_duration_seconds histogram\n")) {
        echo "FAIL: no HELP/TYPE histogram header\n$metrics\n";
        exit(1);
    }

    /* Web: 3 requests, so the histogram count equals requests_total, and the
     * +Inf bucket equals the count. */
    $web = histogram($metrics, 'web');
    $requests = total($metrics, 'fpmng_gateway_requests_total', 'web');
    if ($requests !== 3 || $web['count'] !== 3) {
        echo "FAIL: web requests_total=" . var_export($requests, true) . " histogram count=" . var_export($web['count'], true) . " (want 3)\n$metrics\n";
        exit(1);
    }
    if (count($web['buckets']) !== 12 || end($web['buckets'])[0] !== '+Inf' || end($web['buckets'])[1] !== 3) {
        echo "FAIL: web buckets are not 11 finite plus +Inf=3\n$metrics\n";
        exit(1);
    }
    echo "web: count=3, +Inf=3\n";

    /* Cumulative and non-decreasing, in the order the renderer prints them. */
    $prev = 0;
    foreach ($web['buckets'] as [$le, $n]) {
        if ($n < $prev) {
            echo "FAIL: web bucket le=$le ($n) is below the bucket before it ($prev)\n$metrics\n";
            exit(1);
        }
        $prev = $n;
    }
    echo "web: cumulative\n";

    /* The 0.25 s bucket must not hold the slow request, which took at least
     * 0.3 s: only the two fast ones are at or below it. */
    $le025 = null;
    foreach ($web['buckets'] as [$le, $n]) {
        if ($le === '0.25') {
            $le025 = $n;
        }
    }
    if ($le025 !== 2) {
        echo "FAIL: web le=0.25 holds " . var_export($le025, true) . " requests, want 2 (the slow one is above 0.25 s)\n$metrics\n";
        exit(1);
    }
    echo "web: le=0.25 holds the 2 fast requests\n";

    /* The sum covers the 300 ms the target held the slow request. */
    if ($web['sum_us'] === null || $web['sum_us'] < 300000) {
        echo "FAIL: web sum " . var_export($web['sum_us'], true) . " us, want at least 300000\n$metrics\n";
        exit(1);
    }
    echo "web: sum >= 0.3 s\n";

    /* The gateway's own answer: one local 404 in the "-" row. */
    $local = histogram($metrics, '-');
    if ($local['count'] !== 1 || total($metrics, 'fpmng_gateway_requests_total', '-') !== 1) {
        echo "FAIL: '-' row count=" . var_export($local['count'], true) . ", want 1\n$metrics\n";
        exit(1);
    }
    echo "local: count=1\n";

    /* The forwarded operator page: one request in the "operator" row. */
    $forwarded = histogram($metrics, 'operator');
    if ($forwarded['count'] !== 1 || total($metrics, 'fpmng_gateway_requests_total', 'operator') !== 1) {
        echo "FAIL: 'operator' row count=" . var_export($forwarded['count'], true) . ", want 1\n$metrics\n";
        exit(1);
    }
    echo "operator: count=1\n";

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
served: 2 fast, 1 slow, 1 local, 1 forwarded
web: count=3, +Inf=3
web: cumulative
web: le=0.25 holds the 2 fast requests
web: sum >= 0.3 s
local: count=1
operator: count=1
Done
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>

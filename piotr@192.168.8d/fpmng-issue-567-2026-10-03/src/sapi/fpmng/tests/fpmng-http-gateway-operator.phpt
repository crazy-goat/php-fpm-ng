--TEST--
fpm-ng: http.operator = yes forwards every exposed pool's operator page through the gateway's public port (issue #389)
--SKIPIF--
<?php
include "fpmng-skipif.inc";
fpmng_skip_if_pool_type_unsupported('gateway');
?>
--FILE--
<?php

require_once "tester.inc";
require_once "fpmng-operator.inc";

/* Issue #389, acceptance criterion 1. One gateway, three pools that expose
 * themselves in three different ways, sharing ONE operator listener:
 *
 *   app  (http-direct) operator.metrics = on        -> local /metrics/app
 *   api  (http-direct) operator.metrics_path = /_m  -> local /_m
 *   tick (cron)        operator.status  = on        -> local /status/tick
 *
 * Through the gateway each is reached at the SAME public URL regardless of what
 * the pool declared locally: /metrics/app, /metrics/api, /status/tick. The
 * gateway's own page sits at the bare base (/metrics). FastCGI pools can now
 * also opt into their own operator listener (#383); this test keeps the two
 * HTTP-direct pools because it specifically exercises "explicit local path"
 * (api) next to "derived same-as-gateway path" (app). The FastCGI listener
 * path and upstream pm.status_path separation are covered by
 * fpmng-fastcgi-operator-metrics.phpt. */

$root = sys_get_temp_dir() . '/fpmng-gw-operator-' . getmypid();
@mkdir($root, 0700, true);
file_put_contents($root . '/front.php', '<?php echo "php:" . $_SERVER["REQUEST_URI"];');
file_put_contents($root . '/loop.php', '<?php while (true) { sleep(1); }');

$cfg = <<<EOT
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}

[gw]
pool.type = gateway
listen = {{ADDR[http]}}
chdir = $root
http.gateways = 1
http.access_log = {{FILE:LOG:ACC}}
access.suppress_path[] = /metrics/api
http.route[app] = /
http.route[api] = /api
http.operator = yes
http.operator_allowed_clients = 127.0.0.1
operator.metrics_listen = {{ADDR[operator]}}
operator.status_listen = {{ADDR[operator]}}

[app]
pool.type = http-direct
listen = {{ADDR[app]}}
pm = static
pm.max_children = 2
chdir = $root
http.front_controller = /front.php
operator.metrics_listen = {{ADDR[operator]}}
operator.metrics = on
operator.status_listen = {{ADDR[operator]}}
operator.status = on

[api]
pool.type = http-direct
listen = {{ADDR[api]}}
pm = static
pm.max_children = 2
chdir = $root
http.front_controller = /front.php
operator.metrics_listen = {{ADDR[operator]}}
operator.metrics_path = /_m

[tick]
pool.type = cron
cron.schedule = @hourly
cron.script = $root/loop.php
operator.status_listen = {{ADDR[operator]}}
operator.status = on
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

/* gatewayGet() plus the Content-Type, for the query-variant check below. */
function gatewayFetch(string $url): array
{
    $ctx = stream_context_create(['http' => ['timeout' => 10, 'ignore_errors' => true]]);
    $body = @file_get_contents($url, false, $ctx);
    $headers = $http_response_header ?? [];
    $status = 0;
    $type = '';
    foreach ($headers as $h) {
        if (preg_match('#^HTTP/\S+\s+(\d+)#', $h, $m)) {
            $status = (int) $m[1];
        }
        if (stripos($h, 'Content-Type:') === 0) {
            $type = trim(substr($h, strlen('Content-Type:')));
        }
    }
    return [$status, $type, $body === false ? '' : $body];
}

/* Issue #390: reading the gateway's OWN page through the gateway is the one
 * case where the two bodies cannot be byte-identical. That request is itself a
 * connection to the gateway, so at render time `fpmng_gateway_connections_open`
 * counts it, and the forward to the operator listener holds that listener's
 * upstream open, so `fpmng_gateway_upstreams_used{target="operator"}` is 1
 * rather than 0. Both are live gauges about the very connection the scrape is
 * happening over; normalise those sample lines and the rest must still match.
 * A pool's OWN page (app, api) is rendered by the operator child and reads none
 * of the gateway's segment, so it is compared byte for byte. */
function stripLiveConnectionGauges(string $body): string
{
    $out = [];
    foreach (explode("\n", $body) as $line) {
        if (preg_match('/^fpmng_gateway_(connections_open|upstreams_used)\{/', $line)) {
            continue;
        }
        $out[] = $line;
    }
    return implode("\n", $out);
}

function expectSame(string $what, string $gatewayUrl, string $operatorAddr, string $localPath,
    bool $ownGatewayPage = false): void
{
    [$status, $public] = gatewayGet($gatewayUrl);
    if ($status !== 200) {
        throw new RuntimeException("$what: gateway answered $status\n$public");
    }
    $local = fpmng_operator_body($operatorAddr, $localPath);
    if ($ownGatewayPage) {
        $public = stripLiveConnectionGauges($public);
        $local = stripLiveConnectionGauges($local);
    }
    if ($public !== $local) {
        throw new RuntimeException("$what: gateway body differs from the operator page\n" .
            "--- gateway ---\n$public\n--- operator ---\n$local");
    }
    echo "$what: same body\n";
}

$tester = new FPM\Tester($cfg, '<?php echo "unused";');
try {
    $tester->start();
    $tester->expectLogStartNotices();

    $http = $tester->getAddr('ipv4', '[http]');
    $operator = $tester->getListen('{{ADDR[operator]}}');

    /* The pool's local path is /metrics/app (operator.metrics = on), so the two
     * URLs coincide -- the derived form. */
    expectSame('app', "http://$http/metrics/app", $operator, '/metrics/app');

    /* The pool's local path is /_m and the gateway still serves it at
     * /metrics/api: the whole point of the map. */
    expectSame('api', "http://$http/metrics/api", $operator, '/_m');

    /* The gateway's own page is in the map too, at the bare base. */
    expectSame('gateway own metrics', "http://$http/metrics", $operator, '/metrics', true);

    /* Status follows the same rule; its JSON carries a couple of values that
     * move on their own (times), so compare the identifying fields rather than
     * the bytes. */
    [$status, $body] = gatewayGet("http://$http/status/tick");
    if ($status !== 200) {
        throw new RuntimeException("tick status through the gateway answered $status\n$body");
    }
    $decoded = json_decode($body, true, flags: JSON_THROW_ON_ERROR);
    if (($decoded['pools'][0]['name'] ?? null) !== 'tick' || ($decoded['pools'][0]['type'] ?? null) !== 'cron') {
        throw new RuntimeException("tick status through the gateway is not tick's:\n$body");
    }
    [$status, $body] = gatewayGet("http://$http/status");
    if ($status !== 200 || !str_contains($body, '"pools"')) {
        throw new RuntimeException("the gateway's own status page is not at /status: $status\n$body");
    }
    echo "status: forwarded and own\n";

    /* The query string survives the rewrite exactly once. app's http-direct
     * status page selects its JSON variant with ?json; before the fix the
     * rewritten request line carried the query twice, the operator listener's
     * flag matcher missed it, and the JSON variant was silently the text page.
     * Content type is the stable signal (the bodies carry a moving uptime). */
    [$status, $type, $body] = gatewayFetch("http://$http/status/app?json");
    if ($status !== 200 || !str_contains($type, 'application/json') || !str_starts_with(ltrim($body), '{')) {
        throw new RuntimeException("?json through the gateway was not honoured: status=$status type=$type\n$body");
    }
    [$status, $type] = gatewayFetch("http://$http/status/app");
    if ($status !== 200 || !str_contains($type, 'text/plain')) {
        throw new RuntimeException("the unqueried status page should be text/plain: status=$status type=$type");
    }
    echo "query variant: ?json honoured\n";

    /* Precedence: the operator namespace is checked before http.route[], so
     * even though "/" is routed to app, /metrics is not. */
    echo "precedence: ok\n";

    /* Access log (issue #341's field, issue #389): forwarded operator requests
     * appear with target=operator, and access.suppress_path[] applies to them
     * like to any other gateway request (the suppressed /metrics/api has no
     * line). */
    $accessLog = $tester->getPrefixedFile(FPM\Tester::FILE_EXT_LOG_ACC);
    $deadline = microtime(true) + 10;
    $content = '';
    do {
        $content = (string) @file_get_contents($accessLog);
        if (str_contains($content, '/metrics/app')) {
            break;
        }
        usleep(100000);
    } while (microtime(true) < $deadline);
    $appLine = null;
    foreach (explode("\n", $content) as $line) {
        if (str_contains($line, 'GET /metrics/app')) {
            $appLine = $line;
        }
    }
    if ($appLine === null || !preg_match('/ target=operator$/', $appLine)) {
        throw new RuntimeException("no target=operator line for /metrics/app:\n$content");
    }
    if (str_contains($content, '/metrics/api')) {
        throw new RuntimeException("access.suppress_path[] did not suppress /metrics/api:\n$content");
    }
    echo "access-log: target=operator, suppressed\n";

    echo "Done\n";
} finally {
    $tester->terminate();
    $tester->close();
    @unlink($root . '/front.php');
    @unlink($root . '/loop.php');
    @rmdir($root);
}
?>
--EXPECT--
app: same body
api: same body
gateway own metrics: same body
status: forwarded and own
query variant: ?json honoured
precedence: ok
access-log: target=operator, suppressed
Done
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>

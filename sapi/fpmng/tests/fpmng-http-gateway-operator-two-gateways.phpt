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

/* Issue #743: app's page cannot be compared byte for byte across two reads,
 * because two of its lines are live.
 *
 * WHICH lines. fpm_operator_pages.c:242-243 prints fpmng_pool_workers_idle and
 * fpmng_pool_workers_active for every serves_requests pool -- app is
 * http-direct, fpm_pool_type.c:404 -- from row->idle/row->active, which
 * fpm_operator_pages.c:130-136 read out of app's own scoreboard with
 * fpm_scoreboard_copy(). A value in that scoreboard is a moment-in-time fact
 * about the pool, not a property of the page, so two reads taken milliseconds
 * apart may disagree on it.
 *
 * WHY they moved here. A child counts itself idle only once it reaches its
 * accept loop: fpm_http_direct.c:2548 calls fpm_request_accepting_ex(false),
 * which is the idle++ at fpm_request.c:69, and that call sits behind the whole
 * per-child startup -- script resolution, the event base and the evhttp object,
 * the TLS attach, the signal handlers, zend_signal_init(), the SAPI surgery, the
 * per-child user_ini, ops and access-log init (fpm_http_direct.c:2423-2547).
 * expectLogStartNotices() returns on the MASTER's own "ready to handle
 * connections" NOTICE, which third_party/php-src/sapi/fpm/fpm/fpm_events.c:374
 * logs on entering the master's event loop, and
 * third_party/php-src/sapi/fpm/tests/logtool.inc:465-474 is what matches it.
 * With pm = static the master has forked pm.max_children = 2 children by that
 * line, but nothing has waited for either of them to get past 2548. So the
 * first scrape can read idle 0 and the next one idle 2, which is what CI run
 * 37151225422 read.
 *
 * The scrape is not what moves them. The page is rendered by the operator
 * endpoint's OWN single child -- fpm_operator_endpoint.c:216 allocates the
 * internal "__operator <address>" pool, pm_max_children = 1 at :89 -- which only
 * reads app's scoreboard; it is not one of app's children and never touches
 * app's listening socket. It is the children settling, not the measurement.
 *
 * Deliberately NOT normalised, because none of them moves between the two reads
 * and each is what makes a wrong page still fail:
 *  - fpmng_pool_requests_total (fpm_operator_pages.c:245-247, the scoreboard's
 *    requests at :141) moves only when a child starts reading a request's
 *    headers (fpm_request.c:118). Every URL below is in the operator namespace,
 *    which the operator child answers, so nothing ever reaches app's listening
 *    socket and the counter is 0 in both reads.
 *  - fpmng_pool_info is the literal 1 at fpm_operator_pages.c:235.
 * There is no fpmng_pool_worker_* series here either: the base http-direct type
 * -- the classic executor this pool gets by default -- sets no .live_gauges
 * (fpm_pool_type.c:392-428), only the worker executor variant does (:269), so
 * fpm_operator_pages.c:186-193 emits no live[] series. Switching app to
 * pool.executor = worker puts fpmng_pool_worker_pending and
 * fpmng_pool_worker_watchers on this page, and both move.
 *
 * The sample lines are dropped, not blanked: a line present on one side and
 * absent on the other is still a difference, so a gateway that served a page
 * without these gauges does not pass.
 */
function stripLiveWorkerGauges(string $body): string
{
    $out = [];
    foreach (explode("\n", $body) as $line) {
        if (preg_match('/^fpmng_pool_workers_(idle|active)\{/', $line)) {
            continue;
        }
        $out[] = $line;
    }
    return implode("\n", $out);
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
    if ($status !== 200 || stripLiveWorkerGauges($body) !== stripLiveWorkerGauges($local)) {
        throw new RuntimeException("gw1 /metrics/app did not forward app's page: $status\n" .
            "--- gateway ---\n$body\n--- operator ---\n$local");
    }
    echo "gw1: /metrics/app\n";

    [$status, $body] = gatewayGet("http://$http2/m/app");
    if ($status !== 200 || stripLiveWorkerGauges($body) !== stripLiveWorkerGauges($local)) {
        throw new RuntimeException("gw2 /m/app did not forward app's page: $status\n" .
            "--- gateway ---\n$body\n--- operator ---\n$local");
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

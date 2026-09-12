--TEST--
fpm-ng: the per-pool operator endpoint serves each pool on its own listener (issue #274)
--SKIPIF--
<?php include "skipif.inc"; ?>
--FILE--
<?php

require_once "tester.inc";

/* One request per connection, Connection: close, because that is what a scrape
 * does and what the operator server answers: it is a sequential server for
 * monitoring, not a keep-alive one. */
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
    if (count($split) !== 2 || !preg_match('#^HTTP/1\.[01] (\d+)#', $split[0], $m)) {
        throw new RuntimeException("bad response for $path: " . var_export($raw, true));
    }
    return [(int) $m[1], $split[0], $split[1]];
}

function expect(string $what, $actual, $expected): void
{
    if ($actual !== $expected) {
        throw new RuntimeException("$what: expected " . var_export($expected, true) .
            ', got ' . var_export($actual, true));
    }
}

$root = sys_get_temp_dir() . '/fpmng-operator-endpoint-' . getmypid();
@mkdir($root, 0700, true);
file_put_contents($root . '/front.php', '<?php echo "php:" . $_SERVER["REQUEST_URI"];');
/* The supervised script has to stay alive for the pool to have a state worth
 * reporting, and has to notice SIGTERM so the shutdown notices the harness
 * matches are the ordinary ones. */
file_put_contents($root . '/loop.php', '<?php while (true) { sleep(1); }');

/* Three pools of three types share ONE operator listener: that is the shape the
 * design has to survive -- one internal child, one socket, four routes, each
 * reporting on the pool that configured it. `tick` also takes both formats from
 * the same address, which the collision rule allows because the paths differ.
 *
 * The cron schedule is @hourly so the job never fires during the test: what is
 * under test is the endpoint, not the scheduler.
 *
 * http-direct appears here with a metrics path only. Its status page has not
 * moved onto this listener -- it is rendered inside the child that answers and
 * reports per-child rows (issue #64), which is #275's subject -- so
 * pm.status_path still means the page on the pool's own listener there, and
 * fpmng-http-direct-operator.phpt is where that one is tested. */
$cfg = <<<EOT
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}

[web]
listen = {{ADDR}}
pool.type = http-direct
pm = static
pm.max_children = 2
chdir = $root
http.front_controller = /front.php
pm.metrics_listen = {{ADDR[operator]}}
pm.metrics_path = /web-metrics

[tick]
pool.type = cron
cron.schedule = @hourly
cron.script = $root/loop.php
pm.status_listen = {{ADDR[operator]}}
pm.status_path = /tick-status
pm.metrics_listen = {{ADDR[operator]}}
pm.metrics_path = /tick-metrics

[sup]
pool.type = supervisor
supervisor.script = $root/loop.php
supervisor.processes = 1
pm.status_listen = {{ADDR[operator]}}
pm.status_path = /sup-status
EOT;

$tester = new FPM\Tester($cfg, '<?php');
try {
    $tester->start();
    $tester->expectLogStartNotices();

    $public = $tester->getListen('{{ADDR}}');
    $operator = $tester->getListen('{{ADDR[operator]}}');

    /* A pool that runs no requests: JSON, one row, and it is this pool's. The
     * row shape is decided by the type rather than by the path asked for. */
    [$status, $head, $body] = operatorGet($operator, '/tick-status');
    expect('tick status code', $status, 200);
    if (!str_contains($head, 'application/json')) {
        throw new RuntimeException("tick status content-type:\n$head");
    }
    $decoded = json_decode($body, true, flags: JSON_THROW_ON_ERROR);
    expect('tick status rows', count($decoded['pools']), 1);
    expect('tick status pool', $decoded['pools'][0]['name'], 'tick');
    expect('tick status type', $decoded['pools'][0]['type'], 'cron');
    expect('tick status serves', $decoded['pools'][0]['serves_requests'], false);
    if (!isset($decoded['pools'][0]['state'])) {
        throw new RuntimeException("tick status has no state: $body");
    }
    echo "cron status: ok\n";

    /* A second type on the same socket, reporting only itself: the filter is
     * per route, not per listener. */
    [$status, , $body] = operatorGet($operator, '/sup-status');
    expect('sup status code', $status, 200);
    $decoded = json_decode($body, true, flags: JSON_THROW_ON_ERROR);
    expect('sup status rows', count($decoded['pools']), 1);
    expect('sup status pool', $decoded['pools'][0]['name'], 'sup');
    expect('sup status type', $decoded['pools'][0]['type'], 'supervisor');
    echo "supervisor status: ok\n";

    /* Prometheus on its own path, and the per-pool filter holds for it too: a
     * scrape of one pool's metrics path must not carry another pool. */
    [$status, $head, $body] = operatorGet($operator, '/tick-metrics');
    expect('tick metrics code', $status, 200);
    if (!str_contains($head, 'text/plain')) {
        throw new RuntimeException("tick metrics content-type:\n$head");
    }
    if (!str_contains($body, 'pool="tick"')) {
        throw new RuntimeException("tick metrics has no row for its own pool:\n$body");
    }
    foreach (['pool="web"', 'pool="sup"'] as $foreign) {
        if (str_contains($body, $foreign)) {
            throw new RuntimeException("tick metrics carries $foreign:\n$body");
        }
    }
    echo "cron metrics: ok\n";

    /* The type that does serve requests, reporting on itself from a process
     * that is not one of its children. */
    [$status, , $body] = operatorGet($operator, '/web-metrics');
    expect('web metrics code', $status, 200);
    if (!str_contains($body, 'pool="web"')) {
        throw new RuntimeException("web metrics has no row for its own pool:\n$body");
    }
    if (str_contains($body, 'pool="tick"')) {
        throw new RuntimeException("web metrics carries another pool:\n$body");
    }
    echo "http-direct metrics: ok\n";

    /* A path nobody registered is a 404 that says what this listener does
     * answer -- the four routes above, on one socket. */
    [$status, , $body] = operatorGet($operator, '/nothing-here');
    expect('unknown path code', $status, 404);
    foreach (['/web-metrics', '/tick-status', '/tick-metrics', '/sup-status'] as $path) {
        if (!str_contains($body, $path)) {
            throw new RuntimeException("404 does not list $path:\n$body");
        }
    }
    echo "unknown path: ok\n";

    /* Issue #274's second acceptance criterion, for the path that did move: the
     * public listener of an http-direct pool does not answer it. A request for
     * it reaches the front controller like any other URL, which is the point --
     * the endpoint is not reachable from wherever the site is reachable from. */
    $fp = stream_socket_client("tcp://$public", $errno, $error, 5);
    if (!$fp) {
        throw new RuntimeException("connect $public: $error");
    }
    stream_set_timeout($fp, 5);
    fwrite($fp, "GET /web-metrics HTTP/1.1\r\nHost: test\r\nConnection: close\r\n\r\n");
    $raw = (string) stream_get_contents($fp);
    fclose($fp);
    if (!str_contains($raw, 'php:/web-metrics')) {
        throw new RuntimeException("public listener did not reach the front controller:\n$raw");
    }
    echo "public listener: ok\n";

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
cron status: ok
supervisor status: ok
cron metrics: ok
http-direct metrics: ok
unknown path: ok
public listener: ok
Done
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>

--TEST--
fpm-ng: a gateway killed with a connection open loses its gauges, not just its slot (issue #390)
--SKIPIF--
<?php
include "fpmng-skipif.inc";
fpmng_skip_if_pool_type_unsupported('gateway');
?>
--FILE--
<?php

require_once "tester.inc";
require_once "fpmng-operator.inc";

/* Issue #390 review. connections_open and the per-target upstreams_used are
 * "currently open" gauges. In a segment that deliberately survives a gateway
 * respawn, a single pool-wide gauge can only leak: a process killed with
 * connections open runs no close callback, so nothing ever decrements it and
 * repeated crashes accumulate. The gauges are therefore per gateway process,
 * summed by the renderer, and the master zeroes a dead process's block on exit.
 *
 * This holds a connection open (a request whose script sleeps), SIGKILLs the
 * one gateway process holding it, and asserts the sums drop back to 0 AND that
 * the shared admission budget came back (with pm.max_children = 1 a fresh
 * request succeeds only if the dead process's reservation was returned). The
 * monotonic accepted-request counter must still survive, because the segment
 * belongs to the pool. */

$docroot = sys_get_temp_dir() . '/fpmng-gw-gauge-' . getmypid();
@mkdir($docroot, 0700, true);
file_put_contents($docroot . '/index.php',
    '<?php if (str_contains($_SERVER["REQUEST_URI"] ?? "", "/hold")) { sleep(3); } echo "ok";');

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
http.route[app] = /
operator.metrics_listen = {{ADDR[operator]}}
[app]
pool.type = fastcgi
listen = {{ADDR[app]}}
pm = static
pm.max_children = 1
EOT;

function fetch(string $url, float $timeout = 10.0): array
{
    $ctx = stream_context_create(['http' => ['timeout' => $timeout, 'ignore_errors' => true]]);
    $body = @file_get_contents($url, false, $ctx);
    $status = 0;
    foreach ($http_response_header ?? [] as $h) {
        if (preg_match('#^HTTP/\S+\s+(\d+)#', $h, $m)) {
            $status = (int) $m[1];
        }
    }
    return [$status, $body === false ? '' : $body];
}

function metric(string $metrics, string $re): ?int
{
    return preg_match($re, $metrics, $m) ? (int) $m[1] : null;
}

/* The gateway processes of THIS master (ppid = its pid from the pid file); a
 * bare args match would find another php-fpm-ng instance's "gw" pool. */
function gatewayPids(int $masterPid, ?int $except = null): array
{
    $pids = [];
    foreach (explode("\n", (string) shell_exec('ps -eo pid,ppid,args 2>/dev/null')) as $line) {
        if (preg_match('/^\s*(\d+)\s+(\d+)\s+(.*)$/', $line, $m)
            && (int) $m[2] === $masterPid && str_contains($m[3], 'http gateway gw')
            && (int) $m[1] !== $except) {
            $pids[] = (int) $m[1];
        }
    }
    return $pids;
}

$tester = new FPM\Tester($cfg, '<?php echo "unused";');
$hold = null;
try {
    $tester->start();
    $tester->expectLogStartNotices();

    $http = $tester->getAddr('ipv4', '[http]');
    $operator = $tester->getListen('{{ADDR[operator]}}');
    [$host, $port] = explode(':', $http);

    /* Hold a connection open, and wait until both gauges can see it: one client
     * connection and one upstream held to app. */
    $hold = fsockopen($host, (int) $port, $errno, $errstr, 5);
    if (!$hold) {
        echo "FAIL: connect for /hold: $errstr ($errno)\n";
        exit(1);
    }
    fwrite($hold, "GET /hold HTTP/1.1\r\nHost: $host\r\nConnection: close\r\n\r\n");

    $connRe = '/fpmng_gateway_connections_open\{pool="gw"\} (\d+)/';
    $upRe = '/fpmng_gateway_upstreams_used\{pool="gw",target="app"\} (\d+)/';
    $conn = $up = null;
    $deadline = microtime(true) + 10;
    do {
        $metrics = fpmng_operator_body($operator, '/metrics');
        $conn = metric($metrics, $connRe);
        $up = metric($metrics, $upRe);
        if ($conn === 1 && $up === 1) {
            break;
        }
        usleep(100000);
    } while (microtime(true) < $deadline);
    if ($conn !== 1 || $up !== 1) {
        echo "FAIL: the held connection is not visible: connections_open=" .
            var_export($conn, true) . " upstreams_used=" . var_export($up, true) . "\n$metrics\n";
        exit(1);
    }
    echo "held: connections_open=1 upstreams_used=1\n";

    $masterPid = $tester->getPid();
    $own = gatewayPids($masterPid);
    if (count($own) !== 1) {
        echo "FAIL: expected exactly one gateway process, found " . count($own) . "\n";
        exit(1);
    }
    shell_exec('kill -9 ' . $own[0]);

    /* The respawn and the reconciliation are both the master's exit path, so
     * wait for a new pid first, then for the gauges to drop. */
    $deadline = microtime(true) + 15;
    do {
        if (gatewayPids($masterPid, $own[0])) {
            break;
        }
        usleep(100000);
    } while (microtime(true) < $deadline);

    $conn = $up = $total = null;
    $deadline = microtime(true) + 15;
    do {
        $metrics = fpmng_operator_body($operator, '/metrics');
        $conn = metric($metrics, $connRe);
        $up = metric($metrics, $upRe);
        $total = metric($metrics, '/fpmng_gateway_requests_total\{pool="gw",target="app"\} (\d+)/');
        if ($conn === 0 && $up === 0) {
            break;
        }
        usleep(100000);
    } while (microtime(true) < $deadline);

    fclose($hold);
    $hold = null;

    if ($conn !== 0 || $up !== 0) {
        echo "FAIL: the killed gateway leaked its gauges: connections_open=" .
            var_export($conn, true) . " upstreams_used=" . var_export($up, true) . "\n$metrics\n";
        exit(1);
    }
    echo "after kill: connections_open=0 upstreams_used=0\n";

    if ($total !== 1) {
        echo "FAIL: the monotonic accepted-request counter did not survive the kill: " .
            var_export($total, true) . "\n$metrics\n";
        exit(1);
    }
    echo "monotonic counter survived: app=$total\n";

    /* The shared admission budget came back with it: with pm.max_children = 1 a
     * fresh request only succeeds if the dead process's reservation was
     * returned (otherwise the target counts as full and answers 503). The app
     * worker may still be finishing /hold's sleep, so allow it to. */
    [$status, $body] = fetch("http://$http/plain", 15.0);
    if ($status !== 200) {
        echo "FAIL: the budget did not come back; a fresh request answered $status\n$body\n";
        exit(1);
    }
    echo "budget returned: fresh request 200\n";

    echo "Done\n";
} finally {
    if (is_resource($hold)) {
        fclose($hold);
    }
    $tester->terminate();
    $tester->expectLogTerminatingNotices();
    $tester->close();
    @unlink($docroot . '/index.php');
    @rmdir($docroot);
}
?>
--EXPECT--
held: connections_open=1 upstreams_used=1
after kill: connections_open=0 upstreams_used=0
monotonic counter survived: app=1
budget returned: fresh request 200
Done
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>

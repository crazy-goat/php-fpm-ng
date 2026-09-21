--TEST--
fpm-ng: a gateway's own counters live in shared memory and survive a respawned gateway process (issue #390)
--SKIPIF--
<?php
include "fpmng-skipif.inc";
fpmng_skip_if_pool_type_unsupported('gateway');
?>
--FILE--
<?php

require_once "tester.inc";
require_once "fpmng-operator.inc";

/* Issue #390, acceptance criterion 1. One gateway routing to two http-direct
 * pools (the two types that can expose an operator page today, #383 pending),
 * both of which expose themselves so the /metrics index has two pools to list.
 *
 * N requests go to /api (target app), M to / (target web). The gateway's own
 * baseline counter is fpmng_pool_requests_total{pool="gw"} -- the type's
 * `requests` counter, now read from the segment instead of the scoreboard no
 * gateway child bumped. The per-target series are the #341 family, moved into
 * that segment. */

$docroot = sys_get_temp_dir() . '/fpmng-gw-counters-' . getmypid();
@mkdir($docroot, 0700, true);
file_put_contents($docroot . '/index.php', '<?php echo "ok:" . $_SERVER["REQUEST_URI"];');

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
http.operator = yes
http.operator_allowed_clients = 127.0.0.1
operator.metrics_listen = {{ADDR[operator]}}
operator.status_listen = {{ADDR[operator]}}
[app]
pool.type = http-direct
listen = {{ADDR[app]}}
pm = static
pm.max_children = 2
chdir = $docroot
http.front_controller = /index.php
operator.metrics_listen = {{ADDR[operator]}}
operator.metrics = on
operator.status_listen = {{ADDR[operator]}}
operator.status = on
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

function series(string $metrics, string $name, string $target): ?int
{
    $re = '/' . preg_quote($name, '/') . '\{pool="gw",target="' . preg_quote($target, '/') . '"\} (\d+)/';
    return preg_match($re, $metrics, $m) ? (int) $m[1] : null;
}

function total(string $metrics, string $name): ?int
{
    return preg_match('/' . preg_quote($name, '/') . '\{pool="gw"\} (\d+)/', $metrics, $m) ? (int) $m[1] : null;
}

$tester = new FPM\Tester($cfg, '<?php echo "unused";');
try {
    $tester->start();
    $tester->expectLogStartNotices();

    $http = $tester->getAddr('ipv4', '[http]');
    $operator = $tester->getListen('{{ADDR[operator]}}');

    $N = 3;
    $M = 2;
    for ($i = 0; $i < $N; $i++) {
        [$status, $body] = request("http://$http/api/r$i");
        if ($status !== 200 || !str_contains($body, '/api/r')) {
            echo "FAIL: /api/r$i answered status=$status body=$body\n";
            exit(1);
        }
    }
    for ($i = 0; $i < $M; $i++) {
        [$status] = request("http://$http/");
        if ($status !== 200) {
            echo "FAIL: / answered status=$status\n";
            exit(1);
        }
    }
    echo "served: $N app, $M web\n";

    $metrics = fpmng_operator_body($operator, '/metrics');

    $baseline = total($metrics, 'fpmng_pool_requests_total');
    if ($baseline !== $N + $M) {
        echo "FAIL: baseline requests total should be " . ($N + $M) . ", got " . var_export($baseline, true) . "\n$metrics\n";
        exit(1);
    }
    echo "baseline: " . ($N + $M) . "\n";

    $app = series($metrics, 'fpmng_gateway_requests_total', 'app');
    $web = series($metrics, 'fpmng_gateway_requests_total', 'web');
    if ($app !== $N || $web !== $M) {
        echo "FAIL: per-target requests app=$app (want $N) web=$web (want $M)\n$metrics\n";
        exit(1);
    }
    echo "per-target: app=$N web=$M\n";

    /* The index: one line per pool the gateway forwards for. */
    if (!preg_match('/fpmng_gateway_exposed_pool\{pool="app",metrics="([^"]*)",status="([^"]*)"\} 1/', $metrics, $m)
        || $m[1] !== '/metrics/app' || $m[2] !== '/status/app') {
        echo "FAIL: no exposed_pool line for app (or wrong paths)\n$metrics\n";
        exit(1);
    }
    if (!preg_match('/fpmng_gateway_exposed_pool\{pool="web",metrics="([^"]*)",status="([^"]*)"\} 1/', $metrics, $m)
        || $m[1] !== '/metrics/web' || $m[2] !== '') {
        echo "FAIL: no exposed_pool line for web (web exposes metrics only)\n$metrics\n";
        exit(1);
    }
    echo "index: app and web listed\n";

    /* The pool-wide numbers, and target="-" / "operator" when nothing has gone
     * to them: both requests above were routed, so the local bucket is zero and
     * no operator page was forwarded. The connections_open gauge must come back
     * to zero once the test's client connections have closed -- a value stuck
     * above zero is the leak the connection-lifetime bookkeeping exists to
     * prevent, so it is worth waiting for rather than reading once. */
    $ping = total($metrics, 'fpmng_gateway_ping_total');
    $local = series($metrics, 'fpmng_gateway_requests_total', '-');
    $operatorRow = series($metrics, 'fpmng_gateway_requests_total', 'operator');
    if ($ping !== 0 || $local !== 0 || $operatorRow !== 0) {
        echo "FAIL: idle buckets not zero: ping=" . var_export($ping, true) .
            " local=" . var_export($local, true) . " operator=" . var_export($operatorRow, true) . "\n$metrics\n";
        exit(1);
    }
    $open = null;
    $deadline = microtime(true) + 10;
    do {
        $metrics = fpmng_operator_body($operator, '/metrics');
        $open = total($metrics, 'fpmng_gateway_connections_open');
        if ($open === 0) {
            break;
        }
        usleep(100000);
    } while (microtime(true) < $deadline);
    if ($open !== 0) {
        echo "FAIL: connections_open did not return to 0, got " . var_export($open, true) . "\n$metrics\n";
        exit(1);
    }
    echo "pool-wide: ping=0 local=0 operator=0 connections_open=0\n";

    /* Kill the gateway process and let the master respawn it. The segment
     * belongs to the pool, so the counters must not reset. Discovery is scoped
     * to THIS master's children (ppid == the tester's pid, read from its pid
     * file): another php-fpm-ng instance on the box may run a pool named "gw"
     * too, and a bare args match would kill -- and then mistake the respawn of
     * -- that one, leaving this instance's own gateway untouched. */
    $masterPid = $tester->getPid();
    $gatewayPids = function (int $parent, ?int $except = null) : array {
        $pids = [];
        foreach (explode("\n", (string) shell_exec('ps -eo pid,ppid,args 2>/dev/null')) as $line) {
            if (preg_match('/^\s*(\d+)\s+(\d+)\s+(.*)$/', $line, $m)
                && (int) $m[2] === $parent && str_contains($m[3], 'http gateway gw')
                && (int) $m[1] !== $except) {
                $pids[] = (int) $m[1];
            }
        }
        return $pids;
    };
    $own = $gatewayPids($masterPid);
    if (count($own) !== 1) {
        echo "FAIL: expected exactly one gateway process for gw, found " . count($own) . "\n";
        exit(1);
    }
    $killed = $own[0];
    shell_exec("kill -9 $killed");

    $logFile = $tester->getPrefixedFile(FPM\Tester::FILE_EXT_LOG_ERR);
    $deadline = microtime(true) + 15;
    do {
        if (str_contains((string) @file_get_contents($logFile), 'respawning')) {
            break;
        }
        usleep(100000);
    } while (microtime(true) < $deadline);
    /* Wait for the respawned process to exist (a different pid). */
    $newPid = null;
    $deadline = microtime(true) + 15;
    do {
        $new = $gatewayPids($masterPid, $killed);
        if ($new) {
            $newPid = $new[0];
            break;
        }
        usleep(100000);
    } while (microtime(true) < $deadline);
    if ($newPid === null) {
        echo "FAIL: the master did not respawn the gateway\n";
        exit(1);
    }
    echo "gateway killed and respawned\n";

    /* One more request, then the counters still carry the earlier traffic. */
    [$status] = request("http://$http/api/after");
    if ($status !== 200) {
        echo "FAIL: /api/after answered status=$status\n";
        exit(1);
    }

    $metrics = fpmng_operator_body($operator, '/metrics');
    $app = series($metrics, 'fpmng_gateway_requests_total', 'app');
    $web = series($metrics, 'fpmng_gateway_requests_total', 'web');
    $baseline = total($metrics, 'fpmng_pool_requests_total');
    if ($app !== $N + 1 || $web !== $M || $baseline !== $N + $M + 1) {
        echo "FAIL: after respawn expected app=" . ($N + 1) . " web=$M total=" . ($N + $M + 1) .
            ", got app=" . var_export($app, true) . " web=" . var_export($web, true) .
            " total=" . var_export($baseline, true) . "\n$metrics\n";
        exit(1);
    }
    echo "after respawn: app=" . ($N + 1) . ", total=$baseline\n";

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
served: 3 app, 2 web
baseline: 5
per-target: app=3 web=2
index: app and web listed
pool-wide: ping=0 local=0 operator=0 connections_open=0
gateway killed and respawned
after respawn: app=4, total=6
Done
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>

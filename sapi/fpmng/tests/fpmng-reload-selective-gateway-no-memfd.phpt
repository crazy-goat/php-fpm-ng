--TEST--
fpm-ng: a gateway process, first spawn and respawn, holds no memfd descriptors with reload.selective = yes (issue #841)
--SKIPIF--
<?php
include "fpmng-skipif.inc";
fpmng_skip_if_pool_type_unsupported('gateway');
if (!is_dir('/proc/self/fd')) {
    die("skip needs /proc to list the gateway's descriptors");
}
?>
--FILE--
<?php

require_once "tester.inc";
require_once "fpmng-operator.inc";

/* Issue #841: fpm_http_gateway_spawn() forks the gateway straight from the
 * master, so it never went through fpm_reload_shm_child_init(). Issue #691
 * closed the memfd descriptors in the pm children only, and the gateway kept
 * the scoreboard and metrics descriptors for its whole life. A gateway
 * compromised through its HTTP parser then had fd-level read, write and
 * resize access to both regions. The gateway child now closes them right
 * after fork, keeping the MAP_SHARED mappings it uses.
 *
 * The test checks the first gateway process and a respawned one (SIGKILL,
 * as in fpmng-gateway-counters-respawn-gauge.phpt). Each descriptor count is
 * taken only after a request has been served, because the gateway closes the
 * descriptors before it starts serving: an earlier count could race the
 * child init. The last checks prove that the respawned gateway still serves
 * requests and that the operator status page and metrics still render. */

$docroot = sys_get_temp_dir() . '/fpmng-gw-no-memfd-' . getmypid();
@mkdir($docroot, 0700, true);
file_put_contents($docroot . '/index.php', '<?php echo "ok";');

$cfg = <<<EOT
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
reload.selective = yes
[gw]
pool.type = gateway
listen = {{ADDR[http]}}
chdir = $docroot
http.gateways = 1
http.front_controller = /index.php
http.route[app] = /
operator.status_listen = {{ADDR[operator]}}
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

function memfdCount(int $pid): int
{
    $fds = @scandir("/proc/$pid/fd");
    if ($fds === false) {
        throw new RuntimeException("cannot list /proc/$pid/fd");
    }
    $memfds = 0;
    foreach ($fds as $fd) {
        $target = @readlink("/proc/$pid/fd/$fd");
        if (is_string($target) && str_starts_with($target, '/memfd:fpmng-shm')) {
            $memfds++;
        }
    }
    return $memfds;
}

function check(bool $condition, string $message): void
{
    if (!$condition) {
        throw new RuntimeException($message);
    }
}

$tester = new FPM\Tester($cfg, '<?php echo "unused";');
try {
    $tester->start();
    $tester->expectLogStartNotices();

    $http = $tester->getAddr('ipv4', '[http]');
    $operator = $tester->getListen('{{ADDR[operator]}}');
    $masterPid = $tester->getPid();

    $first = gatewayPids($masterPid);
    check(count($first) === 1, 'expected one gateway process, found ' . count($first));
    [$status, $body] = fetch("http://$http/");
    check($status === 200 && $body === 'ok', "first gateway: status $status body " . var_export($body, true));
    $n = memfdCount($first[0]);
    check($n === 0, "the first gateway holds $n memfd descriptors");
    echo "no memfds on first spawn: ok\n";

    shell_exec('kill -9 ' . $first[0]);

    $respawned = [];
    $deadline = microtime(true) + 15;
    do {
        $respawned = gatewayPids($masterPid, $first[0]);
        if ($respawned) {
            break;
        }
        usleep(100000);
    } while (microtime(true) < $deadline);
    check(count($respawned) === 1, 'no respawned gateway process');

    [$status, $body] = fetch("http://$http/");
    check($status === 200 && $body === 'ok', "respawned gateway: status $status body " . var_export($body, true));
    $n = memfdCount($respawned[0]);
    check($n === 0, "the respawned gateway holds $n memfd descriptors");
    echo "no memfds after respawn: ok\n";

    $status = fpmng_operator_body($operator, '/status');
    $decoded = json_decode($status, true);
    check(is_array($decoded) && isset($decoded['pools']) && is_array($decoded['pools']), "operator status does not render:\n$status");
    $metrics = fpmng_operator_body($operator, '/metrics');
    check(str_contains($metrics, 'fpmng_gateway_'), "operator metrics do not render:\n$metrics");
    echo "operator status and metrics render after respawn: ok\n";
    echo "Done\n";
} finally {
    $tester->terminate();
    $tester->close();
    @unlink("$docroot/index.php");
    @rmdir($docroot);
}
?>
--EXPECT--
no memfds on first spawn: ok
no memfds after respawn: ok
operator status and metrics render after respawn: ok
Done
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>

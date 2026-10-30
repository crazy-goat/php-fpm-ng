--TEST--
fpm-ng: during the soft window of a stop and of a reload, http.ready_path answers the probe 503 "draining" and the application 200 on NEW connections, and the gateway exits without a respawn (issue #646)
--SKIPIF--
<?php
include "fpmng-skipif.inc";
if (PHP_OS_FAMILY !== 'Linux') die('skip requires Linux /proc');
fpmng_skip_if_pool_type_unsupported('gateway');
?>
--FILE--
<?php

require_once "tester.inc";

/* Issue #646. At the start of a stop or a reload the master sends SIGUSR1 to
 * every gateway of a pool with http.ready_path. The gateway keeps its listeners
 * for the whole window (process_control_timeout less a margin) and answers the
 * probe 503 "draining", so a probe that opens a NEW connection during the window
 * gets an answer, not a refusal. The children of the pool the gateway routes to
 * are held back for the same window, so the application still answers 200 on a
 * NEW connection. When the gateway exits, the children get their signal. The
 * gateway that ends its window is logged as a NOTICE and is not respawned. */

$root = sys_get_temp_dir() . '/fpmng-gw-ready-drain-' . getmypid();
@mkdir($root, 0700, true);
file_put_contents("$root/app.php", <<<'PHP'
<?php
echo 'app';
PHP);

$cfg = <<<EOT
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
log_level = notice
process_control_timeout = 10
[gw]
pool.type = gateway
listen = {{ADDR[http]}}
chdir = $root
http.gateways = 1
http.ready_path = /ready
http.route[app] = /
[app]
pool.type = fastcgi
listen = {{ADDR}}
chdir = $root
pm = static
pm.max_children = 2
EOT;

/* The gateway process whose parent is $master, via the setproctitle line
 * fpm_http_gateway_run() installs. 0 when none exists. */
function gateway_pid(int $master): int
{
    foreach (glob('/proc/[0-9]*/stat') as $stat) {
        $raw = @file_get_contents($stat);
        if ($raw === false || !preg_match('/^(\d+) \((.*)\) \S (\d+) /s', $raw, $m)) {
            continue;
        }
        if ((int) $m[3] !== $master) {
            continue;
        }
        $cmd = @file_get_contents(dirname($stat) . '/cmdline');
        if ($cmd !== false && str_contains($cmd, 'http gateway gw')) {
            return (int) $m[1];
        }
    }
    return 0;
}

function read_all($fp, int $seconds): string
{
    stream_set_timeout($fp, $seconds);
    $out = '';
    while (!feof($fp)) {
        $chunk = fread($fp, 65536);
        if ($chunk === false || $chunk === '') {
            $meta = stream_get_meta_data($fp);
            if ($chunk === false || $meta['timed_out'] || feof($fp)) {
                break;
            }
            continue;
        }
        $out .= $chunk;
    }
    return $out;
}

/* One request on a fresh connection, closed by the client after the reply. */
function get_once(string $http, string $path): string
{
    $fp = @stream_socket_client("tcp://$http", $errno, $error, 5);
    if (!$fp) {
        throw new RuntimeException("connect: $error");
    }
    fwrite($fp, "GET $path HTTP/1.1\r\nHost: test\r\nConnection: close\r\n\r\n");
    $r = read_all($fp, 5);
    fclose($fp);
    return $r;
}

/* False once the process has exited or is a zombie. */
function process_alive(int $pid): bool
{
    $raw = @file_get_contents("/proc/$pid/stat");
    return $raw !== false && !preg_match('/\) [ZX] /', $raw);
}

/* The start lines, the pid file and the gateway process do not appear at
 * the same moment, and the log may still hold the start lines of an earlier
 * master. Wait until the pid file names a master with a gateway child. */
function wait_for_gateway(): array
{
    $pidFile = preg_replace('/\.php$/', '.pid', __FILE__);
    $deadline = microtime(true) + 20;
    do {
        $master = is_file($pidFile) ? (int) @file_get_contents($pidFile) : 0;
        $gateway = $master > 0 ? gateway_pid($master) : 0;
        if ($gateway > 0) {
            return [$master, $gateway];
        }
        usleep(50000);
    } while (microtime(true) < $deadline);
    throw new RuntimeException('the gateway process did not start');
}

/* The listener is bound a moment after the gateway process exists. */
function wait_for_listener(string $http): void
{
    $deadline = microtime(true) + 10;
    do {
        $fp = @stream_socket_client("tcp://$http", $errno, $error, 1);
        if ($fp) {
            fclose($fp);
            return;
        }
        usleep(50000);
    } while (microtime(true) < $deadline);
    throw new RuntimeException("no listener on $http: $error");
}

/* One stop or one reload: the master gets $signal, the window opens at once,
 * and a NEW connection made inside the window is answered 503 "draining".
 * The test waits for the window to close before it ends the master, so the
 * next case never starts while this master still holds the pid file. */
function window_probe(string $cfg, string $signal, string $label): void
{
    $tester = new FPM\Tester($cfg, '<?php');
    try {
        $tester->start([], false);
        $tester->switchLogSource('{{FILE:LOG}}');
        $tester->expectLogStartNotices();
        $http = $tester->getAddr('ipv4', '[http]');
        [$master, $gateway] = wait_for_gateway();
        wait_for_listener($http);

        $deadline = microtime(true) + 20;
        do {
            $r = get_once($http, '/ready');
        } while (!str_starts_with($r, 'HTTP/1.1 200') && microtime(true) < $deadline);
        if (!str_starts_with($r, 'HTTP/1.1 200')) {
            throw new RuntimeException("the probe is not 200 ready before the $label\n$r");
        }
        echo "ready before the $label: ok\n";

        /* The children exit within a moment of the signal; the window is still
         * open after one second, because it lasts process_control_timeout. */
        $tester->signal($signal, $master);
        usleep(1000000);

        $r = get_once($http, '/ready');
        if (!str_starts_with($r, 'HTTP/1.1 503') || !str_ends_with($r, 'draining')) {
            throw new RuntimeException("a new connection during the $label window is not 503 draining\n$r");
        }
        echo "503 draining for a new connection during the $label window: ok\n";

        /* The children are held for the window, so the application answers. */
        $app = get_once($http, '/app.php');
        /* The reply is chunked: the body is one chunk holding "app". */
        if (!str_starts_with($app, 'HTTP/1.1 200') || !str_contains($app, "\r\napp\r\n")) {
            throw new RuntimeException("an application request during the $label window is not 200 app\n$app");
        }
        echo "application 200 for a new connection during the $label window: ok\n";

        /* The old gateway exits when the window closes. A stop then ends the
         * master. A reload execs a new master under the same pid, so it stays. */
        $deadline = microtime(true) + 30;
        while (process_alive($gateway) && microtime(true) < $deadline) {
            usleep(100000);
        }
        if (process_alive($gateway)) {
            throw new RuntimeException("the gateway is still alive after the $label window");
        }
        /* The window's end is a NOTICE, not a respawn WARNING (issue #646). */
        $tester->expectLogNotice('http gateway \d+ \(pid \d+\) finished its drain during a stop or reload, not respawned', 'gw');
        if ($signal === 'QUIT') {
            while (process_alive($master) && microtime(true) < $deadline) {
                usleep(100000);
            }
        }
    } finally {
        $tester->terminate();
        $tester->close();
    }
}

try {
    window_probe($cfg, 'QUIT', 'stop');
    window_probe($cfg, 'USR2', 'reload');
    echo "Done\n";
} finally {
    @unlink("$root/app.php");
    @rmdir($root);
}
?>
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>
--EXPECT--
ready before the stop: ok
503 draining for a new connection during the stop window: ok
application 200 for a new connection during the stop window: ok
ready before the reload: ok
503 draining for a new connection during the reload window: ok
application 200 for a new connection during the reload window: ok
Done

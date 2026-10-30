--TEST--
fpm-ng: during the window of a stop, http.ready_path keeps the application answering for an idle ondemand target (the master forks its child in the window) and for a request that is in flight when the window ends (issue #646)
--SKIPIF--
<?php
include "fpmng-skipif.inc";
if (PHP_OS_FAMILY !== 'Linux') die('skip requires Linux /proc');
fpmng_skip_if_pool_type_unsupported('gateway');
?>
--FILE--
<?php

require_once "tester.inc";

/* Issue #646, review of the readiness probe. Two cases, both on a stop with
 * process_control_timeout = 4, so the window lasts 3.9 s.
 *
 * 1. An ondemand target with no child at the stop. The master used to end at
 *    once when no child ran, and an ondemand pool never forks in a stop, so a
 *    new application connection in the window got no answer. Now the master
 *    waits for the window and the ondemand pool forks for it.
 * 2. A static target with a request that starts inside the window and runs
 *    past its end and past the escalation. The gateway's hard drain gets its
 *    own deadline after the window, so the request is answered. A zero-length
 *    drain cut it off. The loop in slow.php does not stop on a signal: sleep()
 *    returns early when the child gets the release signal at the escalation. */

$root = sys_get_temp_dir() . '/fpmng-gw-ready-window-' . getmypid();
@mkdir($root, 0700, true);
file_put_contents("$root/app.php", <<<'PHP'
<?php
echo 'app';
PHP);
file_put_contents("$root/slow.php", <<<'PHP'
<?php
$end = microtime(true) + 3;
while (microtime(true) < $end) {
    usleep(50000);
}
echo 'slow';
PHP);

function config(string $pm): string
{
    global $root;
    return <<<EOT
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
log_level = notice
process_control_timeout = 4
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
pm = $pm
pm.max_children = 2
EOT;
}

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
function get_once(string $http, string $path, int $seconds = 5): string
{
    $fp = @stream_socket_client("tcp://$http", $errno, $error, $seconds);
    if (!$fp) {
        throw new RuntimeException("connect: $error");
    }
    fwrite($fp, "GET $path HTTP/1.1\r\nHost: test\r\nConnection: close\r\n\r\n");
    $r = read_all($fp, $seconds);
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

/* Waits until the probe answers 200, so the pool is known to serve. */
function wait_for_ready(string $http): void
{
    $deadline = microtime(true) + 20;
    do {
        $r = get_once($http, '/ready');
    } while (!str_starts_with($r, 'HTTP/1.1 200') && microtime(true) < $deadline);
    if (!str_starts_with($r, 'HTTP/1.1 200')) {
        throw new RuntimeException("the probe is not 200 ready before the stop\n$r");
    }
}

/* Waits until the gateway and the master have exited, then checks the log. */
function wait_for_exit(int $master, int $gateway): void
{
    $deadline = microtime(true) + 30;
    while (process_alive($gateway) && microtime(true) < $deadline) {
        usleep(100000);
    }
    if (process_alive($gateway)) {
        throw new RuntimeException('the gateway is still alive after the stop window');
    }
    while (process_alive($master) && microtime(true) < $deadline) {
        usleep(100000);
    }
    if (process_alive($master)) {
        throw new RuntimeException('the master is still alive after the stop');
    }
}

/* Case 1: an idle ondemand target. Its pool has no child when the stop starts. */
function ondemand_window(): void
{
    $tester = new FPM\Tester(config('ondemand'), '<?php');
    try {
        $tester->start([], false);
        $tester->switchLogSource('{{FILE:LOG}}');
        $tester->expectLogStartNotices();
        $http = $tester->getAddr('ipv4', '[http]');
        [$master, $gateway] = wait_for_gateway();
        wait_for_listener($http);
        /* An ondemand target counts as serving at once. */
        wait_for_ready($http);
        echo "ready before the stop with an idle ondemand target: ok\n";

        $tester->signal('QUIT', $master);
        usleep(1000000);

        $r = get_once($http, '/ready');
        if (!str_starts_with($r, 'HTTP/1.1 503') || !str_ends_with($r, 'draining')) {
            throw new RuntimeException("a new connection during the stop window is not 503 draining\n$r");
        }
        echo "503 draining for a new connection with an ondemand target: ok\n";

        /* No child existed at the stop. The window forks one for this request. */
        $app = get_once($http, '/app.php', 8);
        if (!str_starts_with($app, 'HTTP/1.1 200') || !str_contains($app, "\r\napp\r\n")) {
            throw new RuntimeException("an application request to an ondemand target in the window is not 200 app\n$app");
        }
        echo "application 200 for an ondemand target in the stop window: ok\n";

        wait_for_exit($master, $gateway);
        $tester->expectLogNotice('http gateway \d+ \(pid \d+\) finished its drain during a stop or reload, not respawned', 'gw');
    } finally {
        $tester->terminate();
        $tester->close();
    }
}

/* Case 2: a static target, and a request that starts in the window and ends
 * after the escalation. The request starts at about 2 s, the window ends at
 * about 3.9 s, the escalation is at 4 s, and the request ends at about 5 s. */
function inflight_window(): void
{
    $tester = new FPM\Tester(config('static'), '<?php');
    try {
        $tester->start([], false);
        $tester->switchLogSource('{{FILE:LOG}}');
        $tester->expectLogStartNotices();
        $http = $tester->getAddr('ipv4', '[http]');
        [$master, $gateway] = wait_for_gateway();
        wait_for_listener($http);
        wait_for_ready($http);
        echo "ready before the stop with a static target: ok\n";

        $tester->signal('QUIT', $master);
        usleep(2000000);

        /* Still inside the window: the connection is accepted and forwarded. */
        $fp = @stream_socket_client("tcp://$http", $errno, $error, 5);
        if (!$fp) {
            throw new RuntimeException("connect in the stop window: $error");
        }
        fwrite($fp, "GET /slow.php HTTP/1.1\r\nHost: test\r\nConnection: close\r\n\r\n");
        $r = read_all($fp, 15);
        fclose($fp);
        if (!str_starts_with($r, 'HTTP/1.1 200') || !str_contains($r, "\r\nslow\r\n")) {
            throw new RuntimeException("a request in flight at the end of the window is not 200 slow\n$r");
        }
        echo "request in flight at the end of the stop window: 200 slow\n";

        wait_for_exit($master, $gateway);
        $tester->expectLogNotice('http gateway \(pid \d+\) drained and is exiting', 'gw');
    } finally {
        $tester->terminate();
        $tester->close();
    }
}

try {
    ondemand_window();
    inflight_window();
    echo "Done\n";
} finally {
    @unlink("$root/app.php");
    @unlink("$root/slow.php");
    @rmdir($root);
}
?>
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>
--EXPECT--
ready before the stop with an idle ondemand target: ok
503 draining for a new connection with an ondemand target: ok
application 200 for an ondemand target in the stop window: ok
ready before the stop with a static target: ok
request in flight at the end of the stop window: 200 slow
Done

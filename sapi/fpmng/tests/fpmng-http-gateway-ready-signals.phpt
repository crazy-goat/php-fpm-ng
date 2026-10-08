--TEST--
fpm-ng: a SIGTERM stop keeps http.ready_path at 503 and the application answering during the window, cuts a request still running at the release (502); a second stop signal during the window is not held (issue #646)
--SKIPIF--
<?php
include "fpmng-skipif.inc";
if (PHP_OS_FAMILY !== 'Linux') die('skip requires Linux /proc');
fpmng_skip_if_pool_type_unsupported('gateway');
?>
--FILE--
<?php

require_once "tester.inc";

/* Issue #646, review of the drain signals. Both cases use process_control_timeout
 * = 4, so the window lasts 3.9 s and the release of the held children is at 4 s.
 *
 * 1. A SIGTERM stop. SIGTERM is the default stop signal of Docker and of the
 *    kubelet. The window works for it: the probe answers 503 draining and the
 *    application answers, because the children are held. The release then sends
 *    the held signal to the children, and SIGTERM ends a request that is still
 *    running. The client gets 502 at about 4 s. SIGQUIT is the signal that lets
 *    that request finish; see docs/gateway.md.
 * 2. A SIGQUIT stop, then a SIGTERM during the window. The SIGTERM overrides the
 *    stop, and it is not held: the children get it at once, so a request that
 *    runs at that moment gets 502 at about 2.5 s, not at the release at 4 s. */

$root = sys_get_temp_dir() . '/fpmng-gw-ready-signals-' . getmypid();
@mkdir($root, 0700, true);
file_put_contents("$root/app.php", <<<'PHP'
<?php
echo 'app';
PHP);
file_put_contents("$root/slow.php", <<<'PHP'
<?php
$end = microtime(true) + 8;
while (microtime(true) < $end) {
    usleep(50000);
}
echo 'slow';
PHP);

function config(): string
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
pm = static
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

/* Waits until the pid file names a master with a gateway child. */
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

/* Waits until the gateway and the master have exited. */
function wait_for_exit(int $master, int $gateway): void
{
    $deadline = microtime(true) + 30;
    while (process_alive($gateway) && microtime(true) < $deadline) {
        usleep(100000);
    }
    if (process_alive($gateway)) {
        throw new RuntimeException('the gateway is still alive after the stop');
    }
    while (process_alive($master) && microtime(true) < $deadline) {
        usleep(100000);
    }
    if (process_alive($master)) {
        throw new RuntimeException('the master is still alive after the stop');
    }
}

/* Case 1: a SIGTERM stop. The probe and the application answer in the window,
 * and a request still running at the release is cut with 502. */
function sigterm_stop(): void
{
    $tester = new FPM\Tester(config(), '<?php');
    try {
        $tester->start([], false);
        $tester->switchLogSource('{{FILE:LOG}}');
        $tester->expectLogStartNotices();
        $http = $tester->getAddr('ipv4', '[http]');
        [$master, $gateway] = wait_for_gateway();
        wait_for_listener($http);
        wait_for_ready($http);
        echo "ready before the stop: ok\n";

        $tester->signal('TERM', $master);
        usleep(1000000);

        $r = get_once($http, '/ready');
        if (!str_starts_with($r, 'HTTP/1.1 503') || !str_ends_with($r, 'draining')) {
            throw new RuntimeException("a new connection during a SIGTERM stop window is not 503 draining\n$r");
        }
        echo "503 draining for a new connection during a SIGTERM stop window: ok\n";

        $app = get_once($http, '/app.php', 8);
        if (!str_starts_with($app, 'HTTP/1.1 200') || !str_contains($app, "\r\napp\r\n")) {
            throw new RuntimeException("an application request during a SIGTERM stop window is not 200 app\n$app");
        }
        echo "application 200 during a SIGTERM stop window: ok\n";

        /* Starts at about 2 s and runs past the release at 4 s. */
        usleep(1000000);
        $fp = @stream_socket_client("tcp://$http", $errno, $error, 5);
        if (!$fp) {
            throw new RuntimeException("connect for the slow request: $error");
        }
        fwrite($fp, "GET /slow.php HTTP/1.1\r\nHost: test\r\nConnection: close\r\n\r\n");
        $r = read_all($fp, 15);
        fclose($fp);
        if (!str_starts_with($r, 'HTTP/1.1 502')) {
            throw new RuntimeException("a request still running at the SIGTERM release is not 502\n$r");
        }
        echo "request still running at the SIGTERM release: 502\n";

        wait_for_exit($master, $gateway);
    } finally {
        $tester->terminate();
        $tester->close();
    }
}

/* Case 2: a SIGQUIT stop, then a SIGTERM at 2.5 s. The request that runs at
 * that moment gets 502 at once, well before the release at 4 s. */
function overridden_stop(): void
{
    $tester = new FPM\Tester(config(), '<?php');
    try {
        $tester->start([], false);
        $tester->switchLogSource('{{FILE:LOG}}');
        $tester->expectLogStartNotices();
        $http = $tester->getAddr('ipv4', '[http]');
        [$master, $gateway] = wait_for_gateway();
        wait_for_listener($http);
        wait_for_ready($http);
        echo "ready before the stop: ok\n";

        $stopAt = microtime(true);
        $tester->signal('QUIT', $master);
        usleep(2000000);

        $fp = @stream_socket_client("tcp://$http", $errno, $error, 5);
        if (!$fp) {
            throw new RuntimeException("connect for the slow request: $error");
        }
        fwrite($fp, "GET /slow.php HTTP/1.1\r\nHost: test\r\nConnection: close\r\n\r\n");
        usleep(500000);
        $tester->signal('TERM', $master);
        $r = read_all($fp, 15);
        $answeredAfter = microtime(true) - $stopAt;
        fclose($fp);

        if (!str_starts_with($r, 'HTTP/1.1 502')) {
            throw new RuntimeException("a request running at the overriding SIGTERM is not 502\n$r");
        }
        if ($answeredAfter >= 3.5) {
            throw new RuntimeException(sprintf("the overriding SIGTERM was held back: 502 after %.1f s, the release is at 4 s", $answeredAfter));
        }
        echo "overriding SIGTERM is not held: 502 before the release: ok\n";

        wait_for_exit($master, $gateway);
    } finally {
        $tester->terminate();
        $tester->close();
    }
}

try {
    sigterm_stop();
    overridden_stop();
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
ready before the stop: ok
503 draining for a new connection during a SIGTERM stop window: ok
application 200 during a SIGTERM stop window: ok
request still running at the SIGTERM release: 502
ready before the stop: ok
overriding SIGTERM is not held: 502 before the release: ok
Done

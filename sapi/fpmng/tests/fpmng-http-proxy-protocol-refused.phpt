--TEST--
fpm-ng: refused PROXY connections are closed and released, so they stay out of connections_open (issue #651)
--SKIPIF--
<?php
include "fpmng-skipif.inc";
if (PHP_OS_FAMILY !== 'Linux') die('skip requires Linux /proc');
fpmng_skip_if_pool_type_unsupported('gateway');
?>
--FILE--
<?php

require_once "tester.inc";
require_once "fpmng-operator.inc";

/* Issue #651, review R1. A request on a connection whose PROXY header was
 * refused (untrusted source, or no header from a trusted one) was never
 * answered and never closed: evhttp had already dispatched it, the connection
 * stayed, and connections_open kept counting it. Each case below sends N such
 * requests, then waits for connections_open to return to 0 and checks that the
 * gateway's fd count did not grow by N. Before the fix connections_open stays
 * at N and the test fails. A trusted request afterwards must still be
 * answered. */
$docroot = sys_get_temp_dir() . '/fpmng-proxy-refused-' . getmypid();
@mkdir($docroot, 0700, true);
file_put_contents($docroot . '/index.php', <<<'PHP'
<?php
echo 'remote=' . ($_SERVER['REMOTE_ADDR'] ?? 'none') . "\n";
PHP);

$config = <<<EOT
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
[gw]
pool.type = gateway
listen = {{ADDR[http]}}
chdir = $docroot
http.gateways = 1
http.front_controller = /index.php
http.proxy_protocol = yes
http.proxy_protocol_trusted = 127.0.0.1
http.route[web] = /
operator.status_listen = {{ADDR[operator]}}
[web]
pool.type = fastcgi
listen = {{ADDR}}
chdir = $docroot
pm = static
pm.max_children = 2
EOT;

const GET = "GET /index.php HTTP/1.1\r\nHost: test\r\nConnection: close\r\n\r\n";
const REFUSED = 20;

function exchange(string $addr, string $bytes, string $bindto = ''): string
{
    $ctx = stream_context_create(['socket' => $bindto === '' ? [] : ['bindto' => $bindto]]);
    $fp = @stream_socket_client("tcp://$addr", $errno, $errstr, 5, STREAM_CLIENT_CONNECT, $ctx);
    if ($fp === false) {
        return "CONNECT FAILED: $errstr";
    }
    stream_set_timeout($fp, 5);
    fwrite($fp, $bytes);
    $resp = '';
    while (!feof($fp)) {
        $chunk = fread($fp, 8192);
        if ($chunk === false || $chunk === '') {
            break;
        }
        $resp .= $chunk;
    }
    fclose($fp);
    return $resp;
}

function remoteOf(string $resp): ?string
{
    return preg_match('/remote=(\S+)/', $resp, $m) === 1 ? $m[1] : null;
}

/* The gateway process whose parent is $master, via the setproctitle line
 * fpm_http_gateway_run() installs (as in fpmng-http-gateway-abort-release.phpt). */
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

function fd_count(int $pid): int
{
    return count(glob("/proc/$pid/fd/*"));
}

/* A leaked connection costs one fd, so REFUSED leaks would grow the count by
 * REFUSED. Half of that leaves room for the gateway's own fds (fastcgi). */
function check_fds(int $gw, int $base, string $what): void
{
    $grow = fd_count($gw) - $base;
    if ($grow > REFUSED / 2) {
        echo "FAIL: $what grew the gateway fd count by $grow\n";
        exit(1);
    }
}

function connections_open(string $operator): int
{
    $decoded = json_decode(fpmng_operator_body($operator, '/status'), true, flags: JSON_THROW_ON_ERROR);
    foreach ($decoded['pools'] as $row) {
        if (!array_key_exists('target', $row) && ($row['name'] ?? '') === 'gw') {
            return (int) $row['connections_open'];
        }
    }
    echo "FAIL: no pool row for gw\n";
    exit(1);
}

/* Waits for connections_open to reach 0 (the close callback has run). */
function wait_idle(string $operator): void
{
    for ($i = 0; $i < 100; $i++) {
        if (connections_open($operator) === 0) {
            return;
        }
        usleep(100000);
    }
    echo 'FAIL: connections_open is ' . connections_open($operator) . " after 10 s, refused connections were not released\n";
    exit(1);
}

$v1 = "PROXY TCP4 192.0.2.10 198.51.100.1 56324 443\r\n";

$tester = new FPM\Tester($config, '<?php echo "unused";');
try {
    $tester->start();
    $tester->expectLogStartNotices();
    $addr = $tester->getAddr('ipv4', '[http]');
    $operator = $tester->getListen('{{ADDR[operator]}}');
    $gw = gateway_pid($tester->getPid());
    if ($gw === 0) {
        echo "FAIL: the gateway process was not found\n";
        exit(1);
    }
    wait_idle($operator);
    $fdBase = fd_count($gw);

    /* An untrusted source with a valid header: the request is refused. */
    for ($i = 0; $i < REFUSED; $i++) {
        $r = exchange($addr, $v1 . GET, '127.0.0.2:0');
        if (str_contains($r, 'remote=')) {
            echo "FAIL: untrusted source reached the target: $r\n";
            exit(1);
        }
    }
    wait_idle($operator);
    check_fds($gw, $fdBase, 'untrusted requests');
    echo "untrusted requests are released: ok\n";

    /* An untrusted source that sends a plain GET with no header. evhttp
     * dispatches this one (a PROXY line from an untrusted source is refused by
     * evhttp's own request-line parser), so this is the shape that leaked one
     * fd per request before the fix (review of #651, measured on the test box). */
    for ($i = 0; $i < REFUSED; $i++) {
        $r = exchange($addr, GET, '127.0.0.2:0');
        if (str_contains($r, 'remote=')) {
            echo "FAIL: untrusted plain request reached the target: $r\n";
            exit(1);
        }
    }
    wait_idle($operator);
    check_fds($gw, $fdBase, 'untrusted plain requests');
    echo "untrusted plain requests are released: ok\n";

    /* A trusted source without a header: the request is refused. */
    for ($i = 0; $i < REFUSED; $i++) {
        $r = exchange($addr, GET);
        if (str_contains($r, 'remote=')) {
            echo "FAIL: request without a header reached the target: $r\n";
            exit(1);
        }
    }
    wait_idle($operator);
    check_fds($gw, $fdBase, 'requests without a header');
    echo "requests without a header are released: ok\n";

    /* The gateway still serves a trusted request with a header. */
    $r = exchange($addr, $v1 . GET);
    if (remoteOf($r) !== '192.0.2.10') {
        echo "FAIL: trusted request after the refusals was not answered: $r\n";
        exit(1);
    }
    echo "trusted request after the refusals is answered: ok\n";
} finally {
    $tester->terminate();
    $tester->close();
}

@unlink("$docroot/index.php");
@rmdir($docroot);
echo "Done\n";
?>
--EXPECT--
untrusted requests are released: ok
untrusted plain requests are released: ok
requests without a header are released: ok
trusted request after the refusals is answered: ok
Done
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>

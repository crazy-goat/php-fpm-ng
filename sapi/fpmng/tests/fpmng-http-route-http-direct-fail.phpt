--TEST--
fpm-ng: the http-direct target's failure matrix through the gateway — 502 dead worker, 503 full, 501 Upgrade (issue #344)
--SKIPIF--
<?php
include "fpmng-skipif.inc";
fpmng_skip_if_pool_type_unsupported('gateway');
fpmng_skip_if_pool_type_unsupported('http-direct');
if (!function_exists('posix_kill')) {
    die("skip ext/posix is required to kill a worker mid-request");
}
?>
--FILE--
<?php

require_once "tester.inc";

function check(bool $condition, string $message): void
{
    if (!$condition) throw new RuntimeException($message);
}

/* Issue #344, acceptance criterion 4: a target the gateway cannot get an
 * answer from behaves exactly as #340 specifies for FastCGI targets — a
 * worker that dies mid-request is the #118 forensic case (502, log names the
 * target), a full target answers 503 + Retry-After, and an Upgrade request is
 * answered 501 rather than silently stripped. Criterion 5 is the first
 * assertion: [direct] listens on a unix socket. */

$root = sys_get_temp_dir() . '/fpmng-route-direct-fail-' . getmypid();
@mkdir($root, 0700, true);
$script = '/front-' . getmypid() . '.php';
$sock = "$root/direct.sock";
file_put_contents($root . $script, <<<'PHP'
<?php
header('Content-Type: text/plain');
if (($_GET['mode'] ?? '') === 'die') {
    /* Not a clean end: the connection goes away with nothing written, which
     * is the "closed without one byte of a reply" forensic case (#118). */
    posix_kill(getmypid(), SIGKILL);
}
/* Long enough to hold the target's single budget slot while the second
 * request of the saturation pair is refused. */
if (($_GET['mode'] ?? '') === 'slow') { usleep(700000); }
echo 'direct';
PHP);

$config = <<<EOT
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
[gw]
pool.type = gateway
listen = {{ADDR[http]}}
chdir = $root
http.gateways = 1
http.front_controller = $script
http.route[direct] = /direct
http.route[web] = /
[web]
pool.type = fastcgi
listen = {{ADDR}}
chdir = $root
pm = static
pm.max_children = 1

[direct]
listen = $sock
pm = static
pm.max_children = 1
pool.type = http-direct
chdir = $root
http.front_controller = $script
EOT;

function fetchStatus(string $url, array $headers = []): array
{
    $ctx = stream_context_create(['http' => ['timeout' => 5, 'ignore_errors' => true,
        'header' => implode("\r\n", $headers)]]);
    $body = @file_get_contents($url, false, $ctx);
    return [$http_response_header ?? [], $body];
}

$tester = new FPM\Tester($config, '<?php echo "unused";');
try {
    $tester->start();
    $tester->expectLogStartNotices();
    $http = $tester->getAddr('ipv4', '[http]');

    /* Criterion 5: the unix-socket route works. */
    [$head, $body] = fetchStatus("http://$http/direct/index.php");
    check(trim((string) $head[0]) === 'HTTP/1.1 200 OK' && $body === 'direct',
        'unix-socket target: ' . var_export($head[0] ?? null, true) . ' / ' . var_export($body, true));
    echo "unix-socket-target: ok\n";

    /* Target cannot answer: the worker dies with the request in flight —
     * 502, and the log names the target's address. */
    [$head] = fetchStatus("http://$http/direct/index.php?mode=die");
    check(str_starts_with((string) $head[0], 'HTTP/1.1 502'), 'dead worker: ' . var_export($head[0] ?? null, true));
    $tester->expectLogPattern("/closed .*after the complete request was written.*upstream|upstream '$sock'/", false, null, 5000000);
    echo "no-answer-502: ok\n";

    /* Upgrade: answered 501 by the transport, not stripped and forwarded. */
    [$head] = fetchStatus("http://$http/direct/index.php",
        ['Upgrade: websocket', 'Connection: Upgrade', 'Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==']);
    check(str_starts_with((string) $head[0], 'HTTP/1.1 501'), 'upgrade: ' . var_export($head[0] ?? null, true));
    echo "upgrade-501: ok\n";
} finally {
    $tester->terminate();
    $tester->expectLogTerminatingNotices();
    $tester->close();
    @unlink($root . $script);
    @unlink($sock);
    @rmdir($root);
}

/* Saturation needs a second run of its own: the first request must still be
 * in flight when the second arrives. */
$config2 = <<<EOT
[global]
error_log = {{FILE:LOG2}}
pid = {{FILE:PID2}}
[gw]
pool.type = gateway
listen = {{ADDR[http]}}
chdir = $root
http.gateways = 1
http.front_controller = $script
http.route[direct] = /direct
http.route[web] = /
[web]
pool.type = fastcgi
listen = {{ADDR}}
chdir = $root
pm = static
pm.max_children = 1

[direct]
listen = {{ADDR[direct]}}
pm = static
pm.max_children = 1
pool.type = http-direct
chdir = $root
http.front_controller = $script
EOT;

function connect(string $addr)
{
    $fp = stream_socket_client("tcp://$addr", $errno, $error, 5);
    if (!$fp) throw new RuntimeException("connect $addr: $error");
    stream_set_timeout($fp, 10);
    return $fp;
}

$tester = new FPM\Tester($config2, '<?php echo "unused";');
try {
    $tester->start();
    $tester->expectLogStartNotices();
    $http = $tester->getAddr('ipv4', '[http]');

    /* Target full: pm.max_children = 1 is also the gateway's connection
     * budget for this target, so a held request makes the next one 503. */
    $slow = connect($http);
    fwrite($slow, "GET /direct/index.php?mode=slow HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n");
    usleep(100000);    /* the slow request is dispatched and holding the slot */

    [$head] = fetchStatus("http://$http/direct/index.php");
    check(str_starts_with((string) $head[0], 'HTTP/1.1 503'), 'target full: ' . var_export($head[0] ?? null, true));
    $retry = null;
    foreach ($head as $h) {
        if (stripos($h, 'Retry-After:') === 0) {
            $retry = trim(substr($h, 12));
        }
    }
    check($retry === '1', 'Retry-After: ' . var_export($retry, true));
    echo "target-full-503-retry-after: ok\n";

    /* Read the held response so the child does not die mid-write. */
    while (($line = fgets($slow)) !== false) {
    }
    fclose($slow);
} finally {
    $tester->terminate();
    $tester->expectLogTerminatingNotices();
    $tester->close();
    @unlink($root . $script);
    @rmdir($root);
}
echo "Done\n";
?>
--EXPECT--
unix-socket-target: ok
no-answer-502: ok
upgrade-501: ok
target-full-503-retry-after: ok
Done
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>

--TEST--
fpm-ng: HTTP gateway cuts a connect towards a target that never completes it (http.upstream_connect_timeout) and answers 504 (issue #716)
--SKIPIF--
<?php
include "fpmng-skipif.inc";
fpmng_skip_if_pool_type_unsupported('gateway');
/* The connect case needs the kernel to DROP the SYN of a connect to a listener
 * whose accept queue is full, not refuse it: only then does the connect stay
 * pending and the gateway's deadline is what ends it. tcp_abort_on_overflow = 1
 * answers with a RST instead, which is an immediate ECONNREFUSED and a 502, not
 * a 504. Measured on the build container: the default is 0. */
$abort = @file_get_contents('/proc/sys/net/ipv4/tcp_abort_on_overflow');
if ($abort !== false && trim($abort) !== '0') {
    die('skip tcp_abort_on_overflow = ' . trim($abort)
        . ': a full accept queue refuses a connect instead of leaving it pending');
}
?>
--FILE--
<?php

require_once "tester.inc";

/* Issue #716, the connect half: fpm_http_transport_connect() used to wait for
 * EV_WRITE with no timeout, so a SYN nobody answers held the client connection and
 * one slot of the target's admission budget for as long as it liked.
 *
 * Making a connect hang needs a peer that does not complete the handshake, and a
 * route target has to be a pool of this configuration (fpm_http_route.c refuses a
 * target that is not one), so the pool's own listening socket is what has to stop
 * answering. It stops once its accept queue is full: Linux drops further SYNs
 * rather than refusing them, which leaves the gateway's connect in progress
 * indefinitely. Measured in the build container: net.ipv4.tcp_abort_on_overflow = 0,
 * and a connect to such a listener was still not writable 3 s later.
 *
 * Keeping the queue full needs the target's worker not to accept, and a FastCGI
 * worker blocked reading a request head does not accept. So the worker is given a
 * request it never finishes (one worker, pm.max_children = 1), and the connections
 * opened after it fill a one-deep queue (listen.backlog = 1).
 *
 * Whether the kernel drops or refuses is a sysctl, so --SKIPIF-- refuses a host
 * whose tcp_abort_on_overflow answers with a RST, and the premise is still CHECKED
 * before anything is asserted: a connect of the test's own has to stay pending.
 * A queue that did not fill is a hard failure, not a silent pass. */

const CONNECT_TIMEOUT_MS = 1000;

$docroot = sys_get_temp_dir() . '/fpmng-gw-upstream-connect-timeout';
@mkdir($docroot, 0700, true);
file_put_contents("$docroot/ok.php", '<?php echo "served";');

/* A heredoc interpolates variables, not constants, hence the one variable. */
$connectTimeout = CONNECT_TIMEOUT_MS;

$config = <<<EOT
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
[gw]
pool.type = gateway
listen = {{ADDR[http]}}
chdir = $docroot
http.gateways = 1
http.upstream_connect_timeout = {$connectTimeout}
; Long enough that this case can only end on the connect, never on the read.
http.upstream_read_timeout = 30000
; Short, so the connection kept from the request above is given back before the
; case below opens a new one (otherwise the request is written into the full accept
; queue and waits for a worker instead of connecting).
http.idle_timeout = 200
ping.path = /pingz
http.route[web] = /
[web]
pool.type = fastcgi
listen = {{ADDR[web]}}
listen.backlog = 1
pm = static
pm.max_children = 1
chdir = $docroot
EOT;

$tester = new FPM\Tester($config);
$tester->start();
$tester->expectLogStartNotices();
[$host, $port] = explode(':', $tester->getAddr('ipv4', '[http]'));
$target = $tester->getAddr('ipv4', '[web]');

function gateway_get(string $host, int $port, string $path, float $timeout = 30.0): array
{
    $fp = @fsockopen($host, $port, $errno, $errstr, 5);
    if (!$fp) {
        echo "FAIL: connect: $errstr ($errno)\n";
        exit(1);
    }
    stream_set_timeout($fp, (int) $timeout);
    $started = microtime(true);
    fwrite($fp, "GET $path HTTP/1.1\r\nHost: $host\r\nConnection: close\r\n\r\n");
    $raw = stream_get_contents($fp);
    fclose($fp);

    return ['raw' => $raw, 'ms' => (microtime(true) - $started) * 1000];
}

function status_of(string $raw): int
{
    return preg_match('#^HTTP/1\.[01] (\d{3})#', $raw, $m) ? (int) $m[1] : 0;
}

function check(bool $ok, string $what, array $got = []): void
{
    if ($ok) {
        echo "$what: ok\n";
        return;
    }
    echo "FAIL: $what\n";
    if ($got) {
        echo "took: " . round($got['ms']) . " ms\n";
        echo "response: " . substr(str_replace("\r\n", ' | ', $got['raw']), 0, 300) . "\n";
    }
    exit(1);
}

/* ---- the target, before anything is done to it ---------------------------------- */

$normal = gateway_get($host, (int) $port, '/ok.php');
check(status_of($normal['raw']) === 200 && str_contains($normal['raw'], 'served'), 'normal', $normal);
/* Give the kept connection back (http.idle_timeout above), so the case below has to
 * open one of its own rather than reuse an established one. */
usleep(600000);

/* ---- the premise: a connect to this target can be made to hang ------------------ */

/* A FastCGI request head the worker reads and never finishes answering, so the one
 * worker stays inside the script instead of accepting the next connection. The
 * records are the ones fpmng-fastcgi-tcp-nodelay.phpt spells out. */
$record = function (int $type, string $content): string {
    $pad = (8 - strlen($content) % 8) % 8;
    return pack('CCnnCx', 1, $type, 1, strlen($content), $pad) . $content . str_repeat("\0", $pad);
};
$len = fn (string $s): string => strlen($s) < 128 ? chr(strlen($s)) : pack('N', strlen($s) | 0x80000000);
$pair = fn (string $k, string $v): string => $len($k) . $len($v) . $k . $v;
$params = $pair('SCRIPT_FILENAME', "$docroot/sleeper.php")
    . $pair('REQUEST_METHOD', 'GET') . $pair('REQUEST_URI', '/sleeper.php')
    . $pair('SCRIPT_NAME', '/sleeper.php') . $pair('SERVER_PROTOCOL', 'HTTP/1.1')
    . $pair('GATEWAY_INTERFACE', 'CGI/1.1') . $pair('QUERY_STRING', '')
    . $pair('CONTENT_LENGTH', '0');
$requestHead = $record(1, pack('nCx5', 1, 1)) . $record(4, $params) . $record(4, '') . $record(5, '');
/* Only as long as the case below needs (a 400 ms premise probe and at most the
 * connect timeout): a worker still asleep at teardown is the slowest thing in this
 * test. */
file_put_contents("$docroot/sleeper.php", '<?php usleep(8000000);');

$busy = @stream_socket_client('tcp://' . $target, $errno, $errstr, 5);
if (!$busy) {
    echo "FAIL: cannot open the connection that keeps the worker busy: $errstr ($errno)\n";
    exit(1);
}
fwrite($busy, $requestHead);
usleep(300000);      /* the worker accepts it and blocks on the rest of the request */

/* Connections that fill the one-deep queue; everything past the first has its SYN
 * dropped. They stay open on purpose: a closed connection is a free slot again. */
$open = [];
for ($i = 0; $i < 8; $i++) {
    $fp = @stream_socket_client('tcp://' . $target, $errno, $errstr, 5);
    if ($fp) {
        $open[] = $fp;
    }
}
$probe = @stream_socket_client('tcp://' . $target, $errno, $errstr, 5,
    STREAM_CLIENT_CONNECT | STREAM_CLIENT_ASYNC_CONNECT);
$reproducible = false;
if ($probe) {
    $w = [$probe];
    $r = $e = [];
    $reproducible = @stream_select($r, $w, $e, 0, 400000) === 0;
    $open[] = $probe;
}

if (!$reproducible) {
    /* The accept queue did not fill, so the gateway's connect below would not hang
     * either. Fail loudly rather than pass without exercising the deadline. */
    echo "FAIL: a connect to the full accept queue completed (premise did not hold)\n";
} else {
    /* ---- the case: the gateway's own connect is cut ---------------------------- */

    $cut = gateway_get($host, (int) $port, '/ok.php');
    check(status_of($cut['raw']) === 504, 'connect-cut-504', $cut);
    /* Cut on the deadline: not left hanging, and not by the read timeout above
     * (30000 ms, far beyond the upper bound). The lower bound leaves room for a
     * gateway that cut a little early, the upper one for a loaded machine. */
    check($cut['ms'] >= CONNECT_TIMEOUT_MS * 0.5 && $cut['ms'] < CONNECT_TIMEOUT_MS + 8000,
        'connect-cut-at-the-timeout', $cut);

    $tester->expectLogPattern('/http: connect to upstream .* did not complete within http\.upstream_connect_timeout/');

    /* The gateway still serves, and still answers locally: ping.path needs no
     * target, so this cannot be answered 503 by a budget the timeout failed to
     * give back. */
    $alive = gateway_get($host, (int) $port, '/pingz');
    check(status_of($alive['raw']) === 200 && str_contains($alive['raw'], 'pong'), 'still-serving', $alive);
}

/* A gateway that died handling its own timer would be respawned and would answer
 * the requests above anyway, so "still serving" needs the master's own word too. */
$tester->expectNoLogPattern('/http gateway \d+ \(pid \d+\) (killed by signal|exited with code)/');

foreach ($open as $fp) {
    if (is_resource($fp)) {
        fclose($fp);
    }
}
if (is_resource($busy)) {
    fclose($busy);
}
$tester->terminate();
$tester->close();

?>
--EXPECT--
normal: ok
connect-cut-504: ok
connect-cut-at-the-timeout: ok
still-serving: ok
--CLEAN--
<?php
require_once "tester.inc";
FPM\Tester::clean();

$docroot = sys_get_temp_dir() . '/fpmng-gw-upstream-connect-timeout';
foreach (['ok.php', 'sleeper.php'] as $f) {
    @unlink("$docroot/$f");
}
@rmdir($docroot);
?>
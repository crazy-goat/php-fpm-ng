--TEST--
fpm-ng: direct HTTP does not leave a large response waiting on a delayed ACK (issue #244)
--SKIPIF--
<?php
include "skipif.inc";
?>
--FILE--
<?php
require_once "tester.inc";

// Issue #244. A direct pool accepts on the socket fpm_sockets.c opened, and
// that path never set TCP_NODELAY -- it was written for a FastCGI listener,
// where the peer is a web server on the same host. So every connection a direct
// pool served ran with Nagle on, and any response whose last segment is partial
// sat in the send queue until the client's delayed ACK fired ~40 ms later.
//
// Each response is 200 KiB, which on loopback is a few whole segments with a
// partial one at the end -- the shape Nagle holds until the peer acknowledges
// what came before it.
//
// What is counted is how many responses take longer than a threshold, not how
// long the run took. The stall is a fixed 40 ms kernel timer, so it shows up as
// a cluster at ~42 ms and nothing in between; a slow machine, by contrast,
// makes every response slower and none of them 42 ms. Counting separates those
// two, where a total does not. Measured on the poligon, five runs each:
//
//   with TCP_NODELAY     0 of 40 over the threshold, slowest response 0.52 ms
//   without it          18 to 29 of 40, every one of them 42.0 to 43.0 ms
//
// Hence a tolerance of two: far below anything the bug produces, and two whole
// responses of headroom for a CI runner that hiccups, when the fixed path's
// slowest response is half a millisecond.

$requests = 40;
$bodyBytes = 200 * 1024;
$stallMs = 25;
$stallsAllowed = 2;

$root = __DIR__;
$script = '/fpmng-http-direct-nodelay-front-' . getmypid() . '.php';
file_put_contents($root . $script, <<<PHP
<?php
header('Content-Type: application/octet-stream');
echo str_repeat('x', $bodyBytes);
PHP);

$port = (int) (getenv('FPMNG_DIRECT_TEST_PORT') ?: 28054) + 14;
$addr = "127.0.0.1:$port";
$cfg = <<<CFG
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
[direct]
listen = $addr
pool.type = http-direct
pm = static
pm.max_children = 1
pm.max_requests = 0
chdir = $root
http.front_controller = $script
CFG;

$tester = new FPM\Tester($cfg, '<?php');
try {
    $tester->start();
    $tester->expectLogStartNotices();

    $fp = stream_socket_client("tcp://$addr", $errno, $error, 5);
    if (!$fp) {
        throw new RuntimeException("connect: $error ($errno)");
    }
    stream_set_timeout($fp, 10);

    // One warm-up request outside the measurement: the first response on a
    // fresh connection also pays whatever the pool costs to reach its first
    // php_request_startup(), and that is not what is being measured here.
    $read = function () use ($fp, $bodyBytes): void {
        $head = '';
        while (!str_contains($head, "\r\n\r\n")) {
            $chunk = fgets($fp, 8192);
            if ($chunk === false || $chunk === '') {
                throw new RuntimeException('the connection closed before the headers');
            }
            $head .= $chunk;
        }
        if (!str_contains($head, ' 200 ')) {
            throw new RuntimeException('not a 200: ' . strtok($head, "\r\n"));
        }
        if (!preg_match('/\r\nContent-Length: (\d+)\r\n/i', $head, $m) || (int) $m[1] !== $bodyBytes) {
            throw new RuntimeException('unexpected framing: ' . str_replace("\r\n", ' | ', trim($head)));
        }
        $left = $bodyBytes;
        while ($left > 0) {
            $chunk = fread($fp, min($left, 65536));
            if ($chunk === false || $chunk === '') {
                throw new RuntimeException("the body stopped $left bytes short");
            }
            $left -= strlen($chunk);
        }
    };

    fwrite($fp, "GET / HTTP/1.1\r\nHost: test\r\n\r\n");
    $read();

    $elapsed = [];
    for ($i = 0; $i < $requests; $i++) {
        $started = microtime(true);
        fwrite($fp, "GET / HTTP/1.1\r\nHost: test\r\n\r\n");
        $read();
        $elapsed[] = (microtime(true) - $started) * 1000;
    }
    fclose($fp);

    $stalled = array_values(array_filter($elapsed, fn ($ms) => $ms > $stallMs));
    if (count($stalled) > $stallsAllowed) {
        rsort($elapsed);
        throw new RuntimeException(sprintf(
            '%d of %d responses of %d bytes took over %d ms, which is more than the %d allowed; '
                . 'slowest: %s ms. A cluster around 40 ms is the delayed ACK timer, not the server',
            count($stalled), $requests, $bodyBytes, $stallMs, $stallsAllowed,
            implode(', ', array_map(fn ($ms) => sprintf('%.1f', $ms), array_slice($elapsed, 0, 5)))));
    }
    echo "no stall: ok\n";

    $tester->terminate();
    $tester->expectLogTerminatingNotices();
    $tester->close();
} finally {
    @unlink($root . $script);
}
echo "Done\n";
?>
--EXPECT--
no stall: ok
Done
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>

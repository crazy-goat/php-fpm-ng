--TEST--
fpm-ng: a TCP keep-alive FastCGI client does not wait on a delayed ACK for a large response (issue #590)
--SKIPIF--
<?php
include "skipif.inc";
?>
--FILE--
<?php
require_once "tester.inc";

// Issue #590, the FastCGI twin of fpmng-http-direct-nodelay.phpt (issue #244).
// Upstream main/fastcgi.c intends to set TCP_NODELAY on a FCGI_KEEP_CONN
// connection but never does off Windows (req->tcp is assigned only under
// _WIN32), so a response larger than the 8 KiB output buffer, written in
// several write() calls with a small last one, waits for the peer's delayed ACK
// (Nagle). The fix is the listening_socket_nodelay flag on the fastcgi pool
// type: Linux copies the listener's options onto a connection when the
// handshake completes, so every accepted socket has TCP_NODELAY.
//
// What is counted is how many responses take longer than a threshold, not the
// total: the stall is a fixed ~40 ms kernel timer, and a slow machine makes
// every response slower but none of them 40 ms (same reasoning and tolerance
// as the direct test).

$requests = 40;
$bodyBytes = 200 * 1024;
$stallMs = 25;
$stallsAllowed = 2;

$cfg = <<<CFG
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
[unconfined]
listen = {{ADDR}}
pm = static
pm.max_children = 1
CFG;

// Under the temp dir, not __DIR__: the path is a FastCGI parameter value and a
// long results directory would make it exceed 127 bytes. The encoder below
// handles that too, but a short path keeps the request small. It is removed in
// the finally block below, also when the test throws.
$script = sys_get_temp_dir() . '/fpmng-fastcgi-tcp-nodelay-' . getmypid() . '.php';
file_put_contents($script, "<?php echo str_repeat('x', $bodyBytes);");
chmod($script, 0644);

try {
$tester = new FPM\Tester($cfg, '<?php');
$tester->start();
$tester->expectLogStartNotices();

// A hand-written FastCGI client rather than FPM\Tester::request(): the Tester's
// client did not reproduce the stall on an unfixed binary (the test passed), a
// bare keep-alive client does (41 ms median on the same binary, measured with
// the reproducer that patches/0002 used to carry). Whether the stall shows
// depends on how the client acknowledges, so the client is spelled out here.
$record = function (int $type, string $content): string {
    $pad = (8 - strlen($content) % 8) % 8;
    return pack('CCnnCx', 1, $type, 1, strlen($content), $pad) . $content . str_repeat("\0", $pad);
};
// FastCGI name-value lengths: one byte below 128, else four bytes with the high bit set.
$len = fn (string $s): string => strlen($s) < 128 ? chr(strlen($s)) : pack('N', strlen($s) | 0x80000000);
$pair = fn (string $k, string $v): string => $len($k) . $len($v) . $k . $v;
$params = $pair('SCRIPT_FILENAME', $script)
    . $pair('REQUEST_METHOD', 'GET') . $pair('REQUEST_URI', '/')
    . $pair('SCRIPT_NAME', '/x.php') . $pair('SERVER_PROTOCOL', 'HTTP/1.1')
    . $pair('GATEWAY_INTERFACE', 'CGI/1.1') . $pair('QUERY_STRING', '')
    . $pair('CONTENT_LENGTH', '0');
// FCGI_BEGIN_REQUEST (1), role FCGI_RESPONDER (1), flags FCGI_KEEP_CONN (1).
$request = $record(1, pack('nCx5', 1, 1)) . $record(4, $params) . $record(4, '') . $record(5, '');

$fp = stream_socket_client('tcp://' . $tester->getAddr(), $errno, $error, 5);
if (!$fp) {
    throw new RuntimeException("connect: $error ($errno)");
}
stream_set_timeout($fp, 10);
// Unbuffered, so that each fread() is one recv() of the size asked for, as in
// the reproducer; with PHP's 8 KiB read buffer the stall showed on only about
// half of the runs against an unfixed binary.
stream_set_read_buffer($fp, 0);
$exact = function (int $n) use ($fp): string {
    $got = '';
    while (strlen($got) < $n) {
        $chunk = fread($fp, $n - strlen($got));
        if ($chunk === false || $chunk === '') {
            throw new RuntimeException('the connection closed mid-record');
        }
        $got .= $chunk;
    }
    return $got;
};
$roundTrip = function () use ($fp, $request, $exact): int {
    fwrite($fp, $request);
    $stdout = 0;
    while (true) {
        $h = unpack('Cversion/Ctype/nid/nlen/Cpad', $exact(8));
        $exact($h['len'] + $h['pad']);
        if ($h['type'] === 6) {
            $stdout += $h['len'];
        } elseif ($h['type'] === 3) {
            return $stdout;
        }
    }
};

// Warm-up outside the measurement, on the connection the loop reuses.
$roundTrip();

$elapsed = [];
for ($i = 0; $i < $requests; $i++) {
    $started = microtime(true);
    $roundTrip();
    $elapsed[] = (microtime(true) - $started) * 1000;
}
fclose($fp);

$stalled = array_filter($elapsed, fn ($ms) => $ms > $stallMs);
if (count($stalled) > $stallsAllowed) {
    rsort($elapsed);
    echo sprintf(
        "%d of %d responses of %d bytes took over %d ms (allowed %d); slowest: %s ms\n",
        count($stalled), $requests, $bodyBytes, $stallMs, $stallsAllowed,
        implode(', ', array_map(fn ($ms) => sprintf('%.1f', $ms), array_slice($elapsed, 0, 5)))
    );
} else {
    echo "no stall: ok\n";
}

$tester->terminate();
$tester->expectLogTerminatingNotices();
$tester->close();
} finally {
    @unlink($script);
}
?>
Done
--EXPECT--
no stall: ok
Done
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>

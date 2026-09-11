--TEST--
fpm-ng: HTTP-direct http.stream sends the body as the script produces it
--SKIPIF--
<?php include "skipif.inc"; ?>
--FILE--
<?php
require_once "tester.inc";
$root = __DIR__;
$script = '/fpmng-direct-stream-front-' . getmypid() . '.php';
file_put_contents($root . $script, <<<'PHP'
<?php
/* 16 MiB in 64 KiB pieces: twice FPM_DIRECT_RESPONSE_MAX, so a buffered pool
 * cannot answer it and a streaming one must. Every piece is distinguishable
 * from every other, which is what makes the assertion about order real. */
function pieces(): iterable
{
    for ($i = 0; $i < 256; $i++) {
        yield sprintf('%06d', $i) . str_repeat(chr(65 + $i % 26), 65536 - 6);
    }
}
switch ($_GET['mode'] ?? 'plain') {
    case 'big':
        header('Content-Type: application/octet-stream');
        foreach (pieces() as $piece) { echo $piece; }
        break;
    case 'medium':
        /* Comfortably above FPM_DIRECT_STREAM_CHUNK and below
         * FPM_DIRECT_RESPONSE_MAX: a 1.1 client gets it chunked, a 1.0 client
         * must get it buffered with a Content-Length. */
        for ($i = 0; $i < 32; $i++) { echo str_repeat('M', 65536); }
        break;
    case 'early':
        echo "first\n";
        flush();
        usleep(1000000);
        echo "second\n";
        break;
    case 'stall':
        /* Far more than any socket buffer, so the pump is guaranteed to block
         * against a client that never reads. */
        for ($i = 0; $i < 1024; $i++) { echo str_repeat('S', 65536); }
        break;
    case 'status':
        http_response_code((int) $_GET['code']);
        echo 'must-not-be-sent';
        break;
    default:
        echo 'plain';
}
PHP);
register_shutdown_function(static function () use ($root, $script) { @unlink($root . $script); });

$base = (int) (getenv('FPMNG_DIRECT_TEST_PORT') ?: 28054);
[$plainPort, $streamPort, $stallPort] = [$base + 12, $base + 13, $base + 14];
$cfg = <<<CFG
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
[buffered]
listen = 127.0.0.1:$plainPort
pool.type = http-direct
pm = static
pm.max_children = 1
chdir = $root
http.front_controller = $script
php_admin_value[output_buffering] = 0
[streamed]
listen = 127.0.0.1:$streamPort
pool.type = http-direct
pm = static
pm.max_children = 1
chdir = $root
http.front_controller = $script
http.stream = yes
php_admin_value[output_buffering] = 0
[stalled]
listen = 127.0.0.1:$stallPort
pool.type = http-direct
pm = static
pm.max_children = 1
chdir = $root
http.front_controller = $script
http.stream = yes
http.stream_write_timeout = 1000
catch_workers_output = yes
php_admin_value[output_buffering] = 0
CFG;

function connect(int $port)
{
    $fp = stream_socket_client("tcp://127.0.0.1:$port", $errno, $error, 5);
    if (!$fp) throw new RuntimeException("connect :$port: $error");
    stream_set_timeout($fp, 10);
    return $fp;
}

function readExactly($fp, int $n): string
{
    $out = '';
    while (strlen($out) < $n) {
        $part = fread($fp, $n - strlen($out));
        if ($part === false || $part === '') throw new RuntimeException('short read');
        $out .= $part;
    }
    return $out;
}

/** Status line plus headers, lower-cased names. */
function readHead($fp, int $expected, string $version = '1.1'): array
{
    $line = fgets($fp);
    if (!$line || !str_starts_with($line, "HTTP/$version $expected ")) {
        throw new RuntimeException('bad status line: ' . var_export($line, true));
    }
    $headers = [];
    while (($l = fgets($fp)) !== false && $l !== "\r\n") {
        [$name, $value] = explode(':', $l, 2);
        $headers[strtolower($name)] = trim($value);
    }
    return $headers;
}

/** Decodes one chunked message, so a missing terminator is an error and not a
 *  short body that happens to look complete. */
function readChunked($fp): string
{
    $body = '';
    while (true) {
        $line = fgets($fp);
        if ($line === false || trim($line) === '') throw new RuntimeException('chunked stream ended without a terminator');
        $size = hexdec(trim($line));
        if ($size === 0) { fgets($fp); return $body; }
        $body .= readExactly($fp, $size);
        if (readExactly($fp, 2) !== "\r\n") throw new RuntimeException('chunk not closed by CRLF');
    }
}

function expected16MiB(): string
{
    $out = '';
    for ($i = 0; $i < 256; $i++) $out .= sprintf('%06d', $i) . str_repeat(chr(65 + $i % 26), 65536 - 6);
    return $out;
}

$tester = new FPM\Tester($cfg, '<?php');
try {
    $tester->start();
    $tester->expectLogStartNotices();

    /* 1. Default: unchanged. The oversized response is still refused whole and
     *    an ordinary one still carries a Content-Length. */
    $fp = connect($plainPort);
    fwrite($fp, "GET /?mode=big HTTP/1.1\r\nHost: t\r\n\r\n");
    $headers = readHead($fp, 500);
    if (isset($headers['transfer-encoding'])) throw new RuntimeException('buffered pool used chunked encoding');
    $body = readExactly($fp, (int) $headers['content-length']);
    if (!str_contains($body, 'response body exceeds POC limits')) throw new RuntimeException("buffered body: $body");
    fwrite($fp, "GET / HTTP/1.1\r\nHost: t\r\n\r\n");
    $headers = readHead($fp, 200);
    if (readExactly($fp, (int) $headers['content-length']) !== 'plain') throw new RuntimeException('buffered keep-alive broken');
    fclose($fp);
    echo "buffered-default: unchanged\n";

    /* 2. Streaming: every byte, in order, past the buffered bound. */
    $fp = connect($streamPort);
    fwrite($fp, "GET /?mode=big HTTP/1.1\r\nHost: t\r\n\r\n");
    $headers = readHead($fp, 200);
    if (($headers['transfer-encoding'] ?? '') !== 'chunked') throw new RuntimeException('streamed response is not chunked');
    if (isset($headers['content-length'])) throw new RuntimeException('chunked response carries a Content-Length');
    $body = readChunked($fp);
    $want = expected16MiB();
    if (strlen($body) !== strlen($want)) throw new RuntimeException('streamed ' . strlen($body) . ' of ' . strlen($want));
    if ($body !== $want) throw new RuntimeException('streamed body differs at offset ' . strspn($body ^ $want, "\0"));
    echo "streamed-16MiB: " . strlen($body) . " bytes in order\n";

    /* 3. flush() puts bytes on the wire before the script ends -- the whole
     *    point of the feature, and impossible before issue #56. */
    fwrite($fp, "GET /?mode=early HTTP/1.1\r\nHost: t\r\n\r\n");
    $started = microtime(true);
    readHead($fp, 200);
    $size = hexdec(trim(fgets($fp)));
    $first = readExactly($fp, $size);
    readExactly($fp, 2);
    $firstAt = microtime(true) - $started;
    $rest = readChunked($fp);
    $doneAt = microtime(true) - $started;
    if ($first !== "first\n" || $rest !== "second\n") throw new RuntimeException("early: '$first' / '$rest'");
    if ($firstAt > 0.5 || $doneAt < 0.9) throw new RuntimeException("early: first at {$firstAt}s, done at {$doneAt}s");
    echo "streamed-early: first chunk before the script finished\n";

    /* 4. Bodyless responses keep their framing: streaming declines to start,
     *    so the next request on the same connection is not fed stray bytes. */
    foreach ([204, 205, 304] as $code) {
        fwrite($fp, "GET /?mode=status&code=$code HTTP/1.1\r\nHost: t\r\n\r\n");
        readHead($fp, $code);
    }
    fwrite($fp, "HEAD / HTTP/1.1\r\nHost: t\r\n\r\n");
    readHead($fp, 200);
    fwrite($fp, "GET / HTTP/1.1\r\nHost: t\r\n\r\n");
    $headers = readHead($fp, 200);
    if (readExactly($fp, (int) $headers['content-length']) !== 'plain') throw new RuntimeException('bodyless response corrupted the connection');
    fclose($fp);
    echo "streamed-bodyless: framing intact\n";

    /* 4b. An HTTP/1.0 client has no chunked framing to receive, so streaming
     *     declines and the response is buffered with a Content-Length. Getting
     *     this wrong puts a body after `Content-Length: 0` on a keep-alive
     *     connection, which the next response would be read out of. */
    $fp = connect($streamPort);
    fwrite($fp, "GET /?mode=medium HTTP/1.0\r\nHost: t\r\nConnection: keep-alive\r\n\r\n");
    $headers = readHead($fp, 200, '1.0');
    if (isset($headers['transfer-encoding'])) throw new RuntimeException('HTTP/1.0 client got chunked framing');
    if (($headers['content-length'] ?? null) !== (string) (32 * 65536)) {
        throw new RuntimeException('HTTP/1.0 content-length: ' . var_export($headers['content-length'] ?? null, true));
    }
    $body = readExactly($fp, 32 * 65536);
    if ($body !== str_repeat('M', 32 * 65536)) throw new RuntimeException('HTTP/1.0 body differs');
    if (($headers['connection'] ?? '') === 'keep-alive') {
        /* Only then is a desync observable: the next response must start at the
         * next byte, not somewhere inside the previous body. */
        fwrite($fp, "GET / HTTP/1.0\r\nHost: t\r\nConnection: keep-alive\r\n\r\n");
        $headers = readHead($fp, 200, '1.0');
        if (readExactly($fp, (int) $headers['content-length']) !== 'plain') {
            throw new RuntimeException('HTTP/1.0 keep-alive connection is out of sync');
        }
    }
    fclose($fp);
    echo "streamed-http10: buffered with a content-length\n";

    /* 5. A client that stops reading is dropped after http.stream_write_timeout,
     *    with the message deliberately unterminated. */
    $fp = connect($stallPort);
    stream_set_timeout($fp, 20);
    fwrite($fp, "GET /?mode=stall HTTP/1.1\r\nHost: t\r\n\r\n");
    readHead($fp, 200);
    sleep(3);
    $seen = '';
    while (($part = fread($fp, 1 << 20)) !== false && $part !== '') $seen .= $part;
    if (feof($fp) === false) throw new RuntimeException('the worker kept the stalled connection open');
    if (str_ends_with($seen, "0\r\n\r\n")) throw new RuntimeException('a truncated response was terminated as if complete');
    fclose($fp);
    $tester->expectLogPattern('/the client stopped reading the response/', true);
    echo "streamed-stall: connection dropped, message unterminated\n";
} finally {
    $tester->terminate();
    $tester->close();
}
?>
--EXPECT--
buffered-default: unchanged
streamed-16MiB: 16777216 bytes in order
streamed-early: first chunk before the script finished
streamed-bodyless: framing intact
streamed-http10: buffered with a content-length
streamed-stall: connection dropped, message unterminated
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>

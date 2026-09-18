--TEST--
fpm-ng: HTTP-direct streams a response over TLS, past the buffered bound (issue #195)
--SKIPIF--
<?php
include "skipif.inc";
if (!extension_loaded('openssl')) {
    die('skip requires the openssl extension');
}
if (trim((string) shell_exec('command -v openssl 2>/dev/null')) === '') {
    die('skip requires the openssl CLI to generate a test certificate');
}
/* A build without libevent_openssl/OpenSSL refuses http.tls_cert with this
 * wording, which is what to skip on -- not the absence of ext/openssl, which
 * only the test client needs. Same probe as fpmng-http-direct-tls.phpt. */
$probe = new FPM\Tester(<<<'EOT'
[global]
error_log = {{FILE:LOG}}
[unconfined]
listen = {{ADDR}}
pm = static
pm.max_children = 1
pool.type = http-direct
chdir = /tmp
http.front_controller = /nonexistent-front-controller.php
http.tls_cert = /nonexistent-cert.pem
http.tls_key = /nonexistent-key.pem
EOT, '<?php');
$messages = $probe->testConfig(true, null, false, false);
FPM\Tester::clean();
foreach ((array) $messages as $message) {
    if (str_contains($message, 'built with TLS support')) {
        die('skip php-fpm-ng built without TLS support (configure without --enable-fpmng-tls, issue #280)');
    }
}
?>
--FILE--
<?php
require_once "tester.inc";

/* Issue #195. http.stream used to be refused together with http.tls_cert: the
 * pump writes the connection's output buffer to its own descriptor, and on a
 * TLS connection that buffer holds plaintext. What this test has to show is
 * that the encrypted path is the same feature and not a weaker one -- a body
 * far past the buffered bound arriving byte for byte in order, flush() putting
 * bytes on the wire before the script ends, and a client that stops reading
 * being dropped rather than holding the worker. */
function check(bool $ok, string $message): void
{
    if (!$ok) {
        throw new RuntimeException($message);
    }
}

function run(string $cmd): void
{
    exec($cmd . ' 2>&1', $output, $code);
    if ($code !== 0) {
        throw new RuntimeException("COMMAND FAILED: $cmd\n" . implode("\n", $output));
    }
}

$root = sys_get_temp_dir() . '/fpmng-direct-tls-stream-' . getmypid();
@mkdir($root, 0700, true);
run("openssl req -x509 -newkey rsa:2048 -nodes -days 2 -sha256 "
    . "-subj /CN=stream.test -keyout $root/tls.key -out $root/tls.crt");

file_put_contents("$root/index.php", <<<'PHP'
<?php
/* 16 MiB in 64 KiB pieces, the same shape fpmng-http-direct-streaming.phpt
 * uses over plain HTTP: twice FPM_DIRECT_RESPONSE_MAX, so a buffered pool
 * cannot answer it at all, and every piece is distinguishable from every
 * other, which is what makes the assertion about order mean something. */
switch ($_GET['mode'] ?? 'plain') {
    case 'big':
        header('Content-Type: application/octet-stream');
        for ($i = 0; $i < 256; $i++) {
            echo sprintf('%06d', $i) . str_repeat(chr(65 + $i % 26), 65536 - 6);
        }
        break;
    case 'early':
        /* 300ms, not a full second (issue #399): what the reader downstream
         * has to be able to tell apart is "the first chunk arrived while the
         * script was still running" from "both arrived together at the end",
         * and 300ms is three orders of magnitude above the read jitter that
         * distinction is measured against. */
        echo "first\n";
        flush();
        usleep(300000);
        echo "second\n";
        break;
    case 'respond':
        /* issue #57 on a TLS pool: the response is finished from inside the
         * script, which goes out through the same write step streaming uses. */
        echo "responded\n";
        fpmng_respond();
        usleep(300000);
        break;
    case 'stall':
        /* Far more than any socket buffer or TLS record, so the pump is
         * guaranteed to block against a client that never reads. */
        for ($i = 0; $i < 1024; $i++) { echo str_repeat('S', 65536); }
        break;
    default:
        echo 'plain';
}
PHP);

$base = (int) (getenv('FPMNG_DIRECT_TEST_PORT') ?: 28054);
$streamPort = $base + 35;
$stallPort = $base + 36;
$respondPort = $base + 37;
$cfg = <<<CFG
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
[tlsstream]
listen = 127.0.0.1:$streamPort
pool.type = http-direct
pm = static
pm.max_children = 1
chdir = $root
http.front_controller = /index.php
http.stream = yes
http.tls_cert = $root/tls.crt
http.tls_key = $root/tls.key
php_admin_value[output_buffering] = 0
[tlsrespond]
listen = 127.0.0.1:$respondPort
pool.type = http-direct
pm = static
pm.max_children = 1
chdir = $root
http.front_controller = /index.php
http.tls_cert = $root/tls.crt
http.tls_key = $root/tls.key
php_admin_value[output_buffering] = 0
[tlsstall]
listen = 127.0.0.1:$stallPort
pool.type = http-direct
pm = static
pm.max_children = 1
chdir = $root
http.front_controller = /index.php
http.stream = yes
http.stream_write_timeout = 1000
http.tls_cert = $root/tls.crt
http.tls_key = $root/tls.key
catch_workers_output = yes
php_admin_value[output_buffering] = 0
CFG;

function tlsConnect(int $port)
{
    $context = stream_context_create(['ssl' => [
        'verify_peer' => false,
        'verify_peer_name' => false,
        'SNI_enabled' => false,
    ]]);
    for ($i = 0; $i < 50; $i++) {
        $client = @stream_socket_client("ssl://127.0.0.1:$port", $errno, $error, 5,
            STREAM_CLIENT_CONNECT, $context);
        if ($client) {
            stream_set_timeout($client, 20);
            return $client;
        }
        usleep(100000);
    }
    throw new RuntimeException("connect ssl://127.0.0.1:$port: $error");
}

function readExactly($fp, int $n): string
{
    $out = '';
    while (strlen($out) < $n) {
        $part = fread($fp, $n - strlen($out));
        if ($part === false || $part === '') {
            throw new RuntimeException('short read: ' . strlen($out) . " of $n");
        }
        $out .= $part;
    }
    return $out;
}

function readHead($fp, int $expected): array
{
    $line = fgets($fp);
    if (!$line || !str_starts_with($line, "HTTP/1.1 $expected ")) {
        throw new RuntimeException('bad status line: ' . var_export($line, true));
    }
    $headers = [];
    while (($l = fgets($fp)) !== false && $l !== "\r\n") {
        [$name, $value] = explode(':', $l, 2);
        $headers[strtolower($name)] = trim($value);
    }
    return $headers;
}

/* Decodes one chunked message, so a missing terminator is an error rather than
 * a short body that happens to look complete -- which is exactly what a broken
 * encrypted pump would produce. */
function readChunked($fp): string
{
    $body = '';
    while (true) {
        $line = fgets($fp);
        if ($line === false || trim($line) === '') {
            throw new RuntimeException('chunked stream ended without a terminator');
        }
        $size = hexdec(trim($line));
        if ($size === 0) {
            fgets($fp);
            return $body;
        }
        $body .= readExactly($fp, $size);
        if (readExactly($fp, 2) !== "\r\n") {
            throw new RuntimeException('chunk not closed by CRLF');
        }
    }
}

$tester = new FPM\Tester($cfg, '<?php');
try {
    $tester->start();
    $tester->expectLogStartNotices();

    /* 1. Every byte, in order, past the bound a buffered pool refuses at. */
    $fp = tlsConnect($streamPort);
    fwrite($fp, "GET /?mode=big HTTP/1.1\r\nHost: t\r\n\r\n");
    $headers = readHead($fp, 200);
    check(($headers['transfer-encoding'] ?? '') === 'chunked', 'streamed TLS response is not chunked');
    check(!isset($headers['content-length']), 'chunked response carries a Content-Length');
    $body = readChunked($fp);
    $want = '';
    for ($i = 0; $i < 256; $i++) {
        $want .= sprintf('%06d', $i) . str_repeat(chr(65 + $i % 26), 65536 - 6);
    }
    check(strlen($body) === strlen($want), 'streamed ' . strlen($body) . ' of ' . strlen($want) . ' bytes');
    check($body === $want, 'streamed body differs at offset ' . strspn($body ^ $want, "\0"));
    echo "tls-streamed-16MiB: " . strlen($body) . " bytes in order\n";

    /* 2. And the bytes leave when the script asks, not when the request
     *    callback returns -- the whole point of the feature, and the half that
     *    depends on the pump actually driving SSL_write() rather than leaving
     *    the buffer for the event loop. */
    fwrite($fp, "GET /?mode=early HTTP/1.1\r\nHost: t\r\n\r\n");
    $started = microtime(true);
    readHead($fp, 200);
    $size = hexdec(trim(fgets($fp)));
    $first = readExactly($fp, $size);
    readExactly($fp, 2);
    $firstAt = microtime(true) - $started;
    $rest = readChunked($fp);
    $doneAt = microtime(true) - $started;
    check($first === "first\n" && $rest === "second\n", "early: '$first' / '$rest'");
    /* The interval between the two reads, not $doneAt on its own: see the same
     * change in fpmng-http-direct-streaming.phpt (issue #399). */
    check($firstAt <= 0.5 && $doneAt - $firstAt >= 0.2, "early: first at {$firstAt}s, done at {$doneAt}s");
    fclose($fp);
    echo "tls-streamed-early: first chunk before the script finished\n";

    /* 3. A response finished from inside the script (issue #57) leaves the
     *    request finished, not just delivered. The write step drains the
     *    connection's output buffer itself, and libevent's own writer ends a
     *    successful write by triggering the write callback -- which is how
     *    evhttp runs evhttp_send_done(). A step that drains without that
     *    delivers the bytes and then leaves the request open forever: measured
     *    on the test box against the first version of this code, the client
     *    below got 'responded' and then waited out its own timeout for the
     *    answer to the second request. A buffered pool, because that is where
     *    the buffer is emptied to zero by one call. */
    $fp = tlsConnect($respondPort);
    fwrite($fp, "GET /?mode=respond HTTP/1.1\r\nHost: t\r\n\r\n");
    $headers = readHead($fp, 200);
    check(readExactly($fp, (int) ($headers['content-length'] ?? 0)) === "responded\n",
        'fpmng_respond() over TLS did not deliver its body');
    fwrite($fp, "GET /?mode=plain HTTP/1.1\r\nHost: t\r\n\r\n");
    $headers = readHead($fp, 200);
    check(readExactly($fp, (int) ($headers['content-length'] ?? 0)) === 'plain',
        'the keep-alive connection was not usable after fpmng_respond()');
    fclose($fp);
    echo "tls-respond: request finished, connection still usable\n";

    /* 4. The stalled-client failure mode of #56 holds over TLS too: the worker
     *    is not held by a client that stopped reading, and the truncated
     *    message is left unterminated so the client cannot mistake it for a
     *    complete response. */
    $fp = tlsConnect($stallPort);
    fwrite($fp, "GET /?mode=stall HTTP/1.1\r\nHost: t\r\n\r\n");
    readHead($fp, 200);
    /* 1.5 s against http.stream_write_timeout = 1000 (issue #399): still 50%
     * past the timeout, which is what has to elapse for the drop to be the
     * timeout firing rather than anything else. */
    usleep(1500000);
    $seen = '';
    while (($part = fread($fp, 1 << 20)) !== false && $part !== '') {
        $seen .= $part;
    }
    check(feof($fp), 'the worker kept the stalled TLS connection open');
    check(!str_ends_with($seen, "0\r\n\r\n"), 'a truncated response was terminated as if complete');
    fclose($fp);
    $tester->expectLogPattern('/the client stopped reading the response/', true);
    echo "tls-streamed-stall: connection dropped, message unterminated\n";
} finally {
    $tester->terminate();
    $tester->close();
    @unlink("$root/index.php");
    @unlink("$root/tls.crt");
    @unlink("$root/tls.key");
    @rmdir($root);
}
?>
--EXPECT--
tls-streamed-16MiB: 16777216 bytes in order
tls-streamed-early: first chunk before the script finished
tls-respond: request finished, connection still usable
tls-streamed-stall: connection dropped, message unterminated
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>

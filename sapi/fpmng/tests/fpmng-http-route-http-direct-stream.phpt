--TEST--
fpm-ng: a streamed response from an http-direct target passes through the gateway incrementally (issue #344)
--SKIPIF--
<?php
include "fpmng-skipif.inc";
fpmng_skip_if_pool_type_unsupported('gateway');
fpmng_skip_if_pool_type_unsupported('http-direct');
?>
--FILE--
<?php

require_once "tester.inc";

function check(bool $condition, string $message): void
{
    if (!$condition) throw new RuntimeException($message);
}

/* Issue #344, acceptance criterion 3: the gateway's HTTP/1.1 client transport
 * de-chunks the upstream's streaming response and re-frames it, one
 * fpm_http_stdout() per de-chunked piece -- the same shape as the FastCGI
 * STDOUT path. Asserted the way fpmng-http-direct-streaming.phpt asserts it
 * on the direct pool itself: the interval between the first chunk and the end
 * of the response must contain the script's own pause (issue #399's
 * gap-measurement, not an absolute bound). */

$root = sys_get_temp_dir() . '/fpmng-route-direct-stream-' . getmypid();
@mkdir($root, 0700, true);
$script = '/front-' . getmypid() . '.php';
file_put_contents($root . $script, <<<'PHP'
<?php
switch ($_GET['mode'] ?? 'plain') {
    case 'early':
        header('Content-Type: text/plain');
        echo "first\n";
        flush();
        usleep(300000);
        echo "second\n";
        break;
    case 'big':
        header('Content-Type: application/octet-stream');
        for ($i = 0; $i < 256; $i++) {
            echo sprintf('%06d', $i) . str_repeat(chr(65 + $i % 26), 65530);
            flush();
        }
        break;
    default:
        echo 'plain';
}
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
listen = {{ADDR[direct]}}
pm = static
pm.max_children = 1
pool.type = http-direct
chdir = $root
http.front_controller = $script
http.stream = yes
php_admin_value[output_buffering] = 0
EOT;

function connect(string $addr)
{
    $fp = stream_socket_client("tcp://$addr", $errno, $error, 5);
    if (!$fp) throw new RuntimeException("connect $addr: $error");
    stream_set_timeout($fp, 20);
    return $fp;
}

function readHead($fp): array
{
    $line = fgets($fp);
    if (!$line || !str_starts_with($line, 'HTTP/1.1 200 ')) {
        throw new RuntimeException('bad status line: ' . var_export($line, true));
    }
    $headers = [];
    while (($line = fgets($fp)) !== false && $line !== "\r\n") {
        [$k, $v] = explode(':', $line, 2);
        $headers[strtolower(trim($k))] = trim($v);
    }
    return $headers;
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

function readChunked($fp): string
{
    $out = '';
    while (true) {
        $line = fgets($fp);
        if ($line === false) {
            throw new RuntimeException('connection closed before the terminating chunk');
        }
        $size = hexdec(trim($line));
        if ($size === 0) {
            while (($line = fgets($fp)) !== false && $line !== "\r\n");
            return $out;
        }
        $out .= readExactly($fp, $size);
        readExactly($fp, 2);
    }
}

$tester = new FPM\Tester($config, '<?php echo "unused";');
try {
    $tester->start();
    $tester->expectLogStartNotices();
    $http = $tester->getAddr('ipv4', '[http]');

    /* Incremental: the first chunk must be observable while the script is
     * still paused, and the pause must fall between the two reads. */
    $fp = connect($http);
    fwrite($fp, "GET /direct/index.php?mode=early HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n");
    $started = microtime(true);
    readHead($fp);
    $first = readExactly($fp, hexdec(trim((string) fgets($fp))));
    readExactly($fp, 2);
    $firstAt = microtime(true) - $started;
    $rest = readChunked($fp);
    $doneAt = microtime(true) - $started;
    if ($first !== "first\n" || $rest !== "second\n") {
        throw new RuntimeException("early: " . var_export($first, true) . ' / ' . var_export($rest, true));
    }
    if ($firstAt > 0.5 || $doneAt - $firstAt < 0.2) {
        throw new RuntimeException("early: first at {$firstAt}s, done at {$doneAt}s -- not streamed");
    }
    fclose($fp);
    echo "streamed-through-gateway: ok\n";

    /* Integrity at size: 16 MiB of distinguishable pieces through the
     * de-chunk/re-frame path, byte order included. */
    $fp = connect($http);
    fwrite($fp, "GET /direct/index.php?mode=big HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n");
    readHead($fp);
    $body = readChunked($fp);
    $expected = '';
    for ($i = 0; $i < 256; $i++) {
        $expected .= sprintf('%06d', $i) . str_repeat(chr(65 + $i % 26), 65530);
    }
    check($body === $expected, 'the 16 MiB streamed body differs: ' . strlen($body) . ' bytes');
    fclose($fp);
    echo "streamed-16MiB-intact: " . strlen($body) . " bytes in order\n";
} finally {
    $tester->terminate();
    $tester->close();
    @unlink($root . $script);
    @rmdir($root);
}
echo "Done\n";
?>
--EXPECT--
streamed-through-gateway: ok
streamed-16MiB-intact: 16777216 bytes in order
Done
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>

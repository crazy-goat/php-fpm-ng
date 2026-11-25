--TEST--
fpm-ng: http-direct single-range 206, 416 and If-Range on static files (http.static.ranges, issue #648)
--SKIPIF--
<?php include "skipif.inc"; ?>
--FILE--
<?php
require_once "tester.inc";
$root = sys_get_temp_dir() . '/fpmng-range-' . getmypid();
@mkdir($root);
@mkdir($root . '/assets');
/* 15 bytes: "body{color:red}". Byte 0 is 'b', byte 14 is '}'. */
file_put_contents($root . '/assets/app.css', 'body{color:red}');
file_put_contents($root . '/assets/empty.css', '');
file_put_contents($root . '/assets/app.css.br', 'BR-BODY');
file_put_contents($root . '/front.php', '<?php echo "php:", $_SERVER["REQUEST_URI"];');
$port = 28122 + 200 * (int) getenv('TEST_PHP_WORKER') + (int) getenv('FPMNG_PHPT_PORT_SHIFT');

function config(int $port, string $root, string $extra): string
{
    return <<<CFG
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
[direct]
listen = 127.0.0.1:$port
pool.type = http-direct
pm = static
pm.max_children = 1
chdir = $root
http.front_controller = /front.php
http.static = yes
$extra
CFG;
}

/* One framed reply: status, headers, and a body bounded by Content-Length. A
 * HEAD reply and a 304 have no body on the wire, whatever their length says. */
function fetch($fp, string $path, string $method = 'GET', array $extra = []): array
{
    $request = "$method $path HTTP/1.1\r\nHost: test\r\n";
    foreach ($extra as $name => $value) {
        $request .= "$name: $value\r\n";
    }
    fwrite($fp, $request . "\r\n");
    $line = fgets($fp);
    if (!$line || !preg_match('#^HTTP/1\.1 (\d+) #', $line, $m)) {
        throw new RuntimeException('bad status line: ' . var_export($line, true));
    }
    $status = (int) $m[1];
    $headers = [];
    $length = 0;
    while (($line = fgets($fp)) !== false && $line !== "\r\n") {
        [$name, $value] = explode(':', $line, 2);
        $headers[strtolower($name)] = trim($value);
        if (strcasecmp($name, 'Content-Length') === 0) {
            $length = (int) trim($value);
        }
    }
    $body = '';
    if ($method !== 'HEAD' && $status !== 304) {
        while (strlen($body) < $length) {
            $chunk = fread($fp, $length - strlen($body));
            if ($chunk === false || $chunk === '') {
                throw new RuntimeException('short body');
            }
            $body .= $chunk;
        }
    }
    return [$status, $headers, $body];
}

function expect(string $what, $actual, $expected): void
{
    if ($actual !== $expected) {
        throw new RuntimeException("$what: expected " . var_export($expected, true) .
            ', got ' . var_export($actual, true));
    }
}

function withPool(string $cfg, int $port, callable $body): void
{
    $tester = new FPM\Tester($cfg, '<?php');
    try {
        $tester->start();
        $tester->expectLogStartNotices();
        $fp = stream_socket_client("tcp://127.0.0.1:$port", $errno, $error, 5);
        stream_set_timeout($fp, 5);
        $body($fp);
        fclose($fp);
    } finally {
        $tester->terminate();
        $tester->close();
    }
}

try {
    withPool(config($port, $root, 'http.static.ranges = yes'), $port, function ($fp) {
        [$status, $headers, $body] = fetch($fp, '/assets/app.css');
        expect('whole file status', $status, 200);
        expect('accept-ranges', $headers['accept-ranges'] ?? null, 'bytes');
        expect('whole file body', $body, 'body{color:red}');
        $etag = $headers['etag'];
        $modified = $headers['last-modified'];

        /* Each row: the Range value, the status, the Content-Range (or null), and the body. */
        $cases = [
            ['bytes=0-4',       206, 'bytes 0-4/15',     'body{'],
            ['bytes=-5',        206, 'bytes 10-14/15',   ':red}'],
            ['bytes=10-',       206, 'bytes 10-14/15',   ':red}'],
            ['bytes=-100',      206, 'bytes 0-14/15',    'body{color:red}'],
            ['bytes=0-999',     206, 'bytes 0-14/15',    'body{color:red}'],
            ['bytes=0-99999999999999999999999', 206, 'bytes 0-14/15', 'body{color:red}'],
            ['BYTES=0-4',       206, 'bytes 0-4/15',     'body{'],
            ['bytes=0-0',       206, 'bytes 0-0/15',     'b'],
            ['bytes=-0',        416, 'bytes */15',       ''],
            ['bytes=15-',       416, 'bytes */15',       ''],
            ['bytes=20-30',     416, 'bytes */15',       ''],
            ['bytes=99999999999999999999999-', 416, 'bytes */15', ''],
            ['bytes=0-4,6-8',   200, null,               'body{color:red}'],
            ['bytes=5-2',       200, null,               'body{color:red}'],
            ['bytes=abc',       200, null,               'body{color:red}'],
            ['items=0-4',       200, null,               'body{color:red}'],
        ];
        foreach ($cases as [$range, $want, $crange, $bodyWant]) {
            [$status, $headers, $body] = fetch($fp, '/assets/app.css', 'GET', ['Range' => $range]);
            expect("$range status", $status, $want);
            expect("$range content-range", $headers['content-range'] ?? null, $crange);
            expect("$range body", $body, $bodyWant);
            expect("$range content-length", $headers['content-length'], (string) strlen($bodyWant));
        }
        echo "ranges: ok\n";

        /* HEAD ignores Range (RFC 9110 14.2): a whole-file length, no Content-Range. */
        [$status, $headers, $body] = fetch($fp, '/assets/app.css', 'HEAD', ['Range' => 'bytes=0-4']);
        expect('head range status', $status, 200);
        expect('head range length', $headers['content-length'], '15');
        expect('head range content-range', $headers['content-range'] ?? null, null);
        expect('head range body', $body, '');
        echo "head: ok\n";

        /* If-Range: a matching validator keeps the 206; any other one sends the whole file. */
        [$status, , $body] = fetch($fp, '/assets/app.css', 'GET', ['Range' => 'bytes=0-4', 'If-Range' => $etag]);
        expect('if-range etag match', $status, 206);
        expect('if-range etag match body', $body, 'body{');
        [$status, , $body] = fetch($fp, '/assets/app.css', 'GET', ['Range' => 'bytes=0-4', 'If-Range' => $modified]);
        expect('if-range date match', $status, 206);
        [$status, , $body] = fetch($fp, '/assets/app.css', 'GET', ['Range' => 'bytes=0-4', 'If-Range' => '"stale"']);
        expect('if-range etag mismatch', $status, 200);
        expect('if-range etag mismatch body', $body, 'body{color:red}');
        [$status] = fetch($fp, '/assets/app.css', 'GET', ['Range' => 'bytes=0-4', 'If-Range' => 'W/' . $etag]);
        expect('if-range weak etag never matches', $status, 200);
        [$status] = fetch($fp, '/assets/app.css', 'GET', ['Range' => 'bytes=0-4', 'If-Range' => 'Sun, 01 Jan 2023 00:00:00 GMT']);
        expect('if-range other date', $status, 200);
        echo "if-range: ok\n";

        /* A 304 takes precedence over a Range that would have produced a 206 or a 416. */
        [$status, , $body] = fetch($fp, '/assets/app.css', 'GET', ['Range' => 'bytes=0-4', 'If-None-Match' => $etag]);
        expect('304 before range', $status, 304);
        expect('304 has no body', $body, '');
        [$status] = fetch($fp, '/assets/app.css', 'GET', ['Range' => 'bytes=99-', 'If-None-Match' => $etag]);
        expect('304 before 416', $status, 304);
        echo "precedence: ok\n";

        /* An empty representation has no byte to return, so every Range is unsatisfiable. */
        [$status, $headers, $body] = fetch($fp, '/assets/empty.css');
        expect('empty whole status', $status, 200);
        expect('empty whole length', $headers['content-length'], '0');
        expect('empty whole body', $body, '');
        [$status, $headers] = fetch($fp, '/assets/empty.css', 'GET', ['Range' => 'bytes=0-']);
        expect('empty open-ended', $status, 416);
        expect('empty open-ended content-range', $headers['content-range'], 'bytes */0');
        [$status, $headers] = fetch($fp, '/assets/empty.css', 'GET', ['Range' => 'bytes=-1']);
        expect('empty suffix', $status, 416);
        echo "empty file: ok\n";

        /* No body on the wire for a 416: the next request on the same connection
         * must still parse. */
        [$status] = fetch($fp, '/assets/app.css', 'GET', ['Range' => 'bytes=-0']);
        expect('keep-alive after 416', $status, 416);
        [$status, , $body] = fetch($fp, '/assets/app.css', 'GET', ['Range' => 'bytes=0-4']);
        expect('keep-alive after 416 still serves', $body, 'body{');
        echo "keep-alive: ok\n";
    });

    /* With a sidecar, Range counts the bytes of the coded representation, and
     * the Content-Range names that length: the sidecar is 7 bytes, not 15. */
    withPool(config($port, $root, "http.static.ranges = yes\nhttp.static.precompressed = yes"), $port, function ($fp) {
        [$status, $headers, $body] = fetch($fp, '/assets/app.css', 'GET', ['Accept-Encoding' => 'br', 'Range' => 'bytes=0-2']);
        expect('sidecar range status', $status, 206);
        expect('sidecar range body', $body, 'BR-');
        expect('sidecar range content-range', $headers['content-range'], 'bytes 0-2/7');
        expect('sidecar range coding', $headers['content-encoding'] ?? null, 'br');
        [$status, $headers, $body] = fetch($fp, '/assets/app.css', 'GET', ['Range' => 'bytes=0-2']);
        expect('identity range status', $status, 206);
        expect('identity range body', $body, 'bod');
        expect('identity range content-range', $headers['content-range'], 'bytes 0-2/15');
        echo "sidecar range: ok\n";
    });

    /* http.static.ranges left out: Range is ignored, and no Accept-Ranges is sent. */
    withPool(config($port, $root, ''), $port, function ($fp) {
        [$status, $headers, $body] = fetch($fp, '/assets/app.css', 'GET', ['Range' => 'bytes=0-4']);
        expect('default off status', $status, 200);
        expect('default off body', $body, 'body{color:red}');
        expect('default off accept-ranges', $headers['accept-ranges'] ?? null, null);
        expect('default off content-range', $headers['content-range'] ?? null, null);
        echo "default off: ok\n";
    });
    echo "Done\n";
} finally {
    @unlink($root . '/assets/app.css');
    @unlink($root . '/assets/app.css.br');
    @unlink($root . '/assets/empty.css');
    @unlink($root . '/front.php');
    @rmdir($root . '/assets');
    @rmdir($root);
}
?>
--EXPECT--
ranges: ok
head: ok
if-range: ok
precedence: ok
empty file: ok
keep-alive: ok
sidecar range: ok
default off: ok
Done
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>

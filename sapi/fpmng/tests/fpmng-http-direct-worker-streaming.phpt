--TEST--
fpm-ng: worker.executor streaming responses (fpmng_worker_respond_start/chunk/end, issue #332)
--SKIPIF--
<?php include "skipif.inc"; ?>
--FILE--
<?php
require_once "tester.inc";

$root = sys_get_temp_dir() . '/fpmng-worker-streaming-' . getmypid();
@mkdir($root, 0700, true);

/* /stream sends three chunks through the new builtins, exactly the sequence
 * fpmng_worker_respond() cannot do: a status line before the body is known in
 * full. /stream refuses cleanly on an HTTP/1.0 request (no chunked framing to
 * carry it), which the handler reports back distinguishably instead of
 * silently falling through to a buffered response. */
file_put_contents("$root/worker.php", <<<'PHP'
<?php
$notify = fpmng_worker_notify_stream();

function handle(int $id): void
{
    $env = fpmng_worker_request_env($id);
    $uri = $env['REQUEST_URI'] ?? '/';

    if (str_starts_with($uri, '/stream')) {
        $ok = fpmng_worker_respond_start($id, 200, ['Content-Type' => 'text/plain', 'X-Stream' => 'yes']);
        if (!$ok) {
            fpmng_worker_respond($id, 505, ['X-Refused' => 'start'], 'start-refused');
            return;
        }
        fpmng_worker_respond_chunk($id, 'first-');
        fpmng_worker_respond_chunk($id, 'second-');
        fpmng_worker_respond_chunk($id, 'third');
        fpmng_worker_respond_end($id);
        return;
    }

    if ($uri === '/double-start') {
        $ok1 = fpmng_worker_respond_start($id, 200, []);
        $ok2 = fpmng_worker_respond_start($id, 200, []);
        fpmng_worker_respond_chunk($id, $ok1 && !$ok2 ? 'ok' : 'wrong');
        fpmng_worker_respond_end($id);
        return;
    }

    fpmng_worker_respond($id, 200, ['Content-Type' => 'text/plain'], 'hello from pid ' . getmypid());
}

$watcher = fpmng_worker_event_create(FPMNG_WORKER_READ, $notify, function () use ($notify): void {
    fread($notify, 65536);
    while (($id = fpmng_worker_next_request()) !== null) {
        handle($id);
    }
});
fpmng_worker_event_enable($watcher);

while (!fpmng_worker_may_exit()) {
    fpmng_worker_loop(true);
}
PHP);

$port = (int) (getenv('FPMNG_DIRECT_WORKER_STREAMING_PORT') ?: 28094);
$config = <<<CFG
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
[streamed]
listen = 127.0.0.1:$port
pool.type = http-direct
pool.executor = worker
pm = static
pm.max_children = 1
chdir = $root
http.front_controller = /worker.php
http.read_timeout = 10000
http.max_body = 1M
catch_workers_output = yes
php_admin_value[max_execution_time] = 0
php_admin_value[display_errors] = 0
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

$tester = new FPM\Tester($config, '<?php');
try {
    $tester->start();
    $tester->expectLogStartNotices();

    /* 1. Three chunks, in order, framed as chunked transfer-encoding -- the
     *    thing a one-shot fpmng_worker_respond() cannot produce. */
    $fp = connect($port);
    fwrite($fp, "GET /stream HTTP/1.1\r\nHost: t\r\n\r\n");
    $headers = readHead($fp, 200);
    if (($headers['transfer-encoding'] ?? '') !== 'chunked') {
        throw new RuntimeException('streamed response is not chunked: ' . json_encode($headers));
    }
    if (isset($headers['content-length'])) throw new RuntimeException('chunked response carries a Content-Length');
    if (($headers['x-stream'] ?? '') !== 'yes') throw new RuntimeException('_start dropped a header: ' . json_encode($headers));
    $body = readChunked($fp);
    if ($body !== 'first-second-third') throw new RuntimeException("streamed body: $body");
    echo "streamed-multi-chunk: ok\n";

    /* 2. A second fpmng_worker_respond_start() on the same id fails cleanly
     *    (no second status line), not a crash and not two responses. */
    fwrite($fp, "GET /double-start HTTP/1.1\r\nHost: t\r\n\r\n");
    $headers = readHead($fp, 200);
    $body = readChunked($fp);
    if ($body !== 'ok') throw new RuntimeException("double-start body: $body");
    echo "double-start-refused: ok\n";

    /* The connection survives both streamed exchanges: framing stayed intact. */
    fwrite($fp, "GET / HTTP/1.1\r\nHost: t\r\n\r\n");
    $headers = readHead($fp, 200);
    $plain = readExactly($fp, (int) $headers['content-length']);
    if (!str_starts_with($plain, 'hello from pid ')) throw new RuntimeException("plain body: $plain");
    fclose($fp);
    echo "connection-intact-after-stream: ok\n";

    /* 3. HTTP/1.0 has no chunked framing: _start refuses and the handler's
     *    fallback (505, buffered) is what reaches the wire. */
    $fp = connect($port);
    fwrite($fp, "GET /stream HTTP/1.0\r\nHost: t\r\nConnection: close\r\n\r\n");
    $headers = readHead($fp, 505, '1.0');
    if (($headers['x-refused'] ?? '') !== 'start') throw new RuntimeException('HTTP/1.0 was not refused: ' . json_encode($headers));
    if (isset($headers['transfer-encoding'])) throw new RuntimeException('HTTP/1.0 client got chunked framing');
    fclose($fp);
    echo "http10-start-refused: ok\n";
} finally {
    $tester->terminate();
    $tester->close();
    @unlink("$root/worker.php");
    @rmdir($root);
}
echo "Done\n";
?>
--EXPECT--
streamed-multi-chunk: ok
double-start-refused: ok
connection-intact-after-stream: ok
http10-start-refused: ok
Done
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>

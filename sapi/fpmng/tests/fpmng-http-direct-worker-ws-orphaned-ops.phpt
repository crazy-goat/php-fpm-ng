--TEST--
fpm-ng: the orphaned flag guards every WebSocket stream op during the shutdown-function window (issue #443)
--SKIPIF--
<?php
include "skipif.inc";
?>
--FILE--
<?php

require_once "tester.inc";

function check(bool $condition, string $message): void
{
    if (!$condition) throw new RuntimeException($message);
}

/* Issue #443: the orphaned flag guarded only close. A shutdown function or
 * __destruct holding the stream could fread()/fwrite()/cast() on it during
 * php_request_shutdown() -- AFTER evhttp_free() had freed the hijacked
 * bufferevent. Confirmed: fwrite() reported 4 bytes written through freed
 * memory. Every op now answers "dead stream" when orphaned, and the orphan
 * walk flags eof so feof()/select-liveness report the peer gone. */

$root = sys_get_temp_dir() . '/fpmng-ws-orphaned-ops-' . getmypid();
@mkdir($root, 0700, true);

file_put_contents("$root/worker.php", <<<'PHP'
<?php
$notify = fpmng_worker_notify_stream();
$ws = null;

/* The marker proves what the ops answered during the shutdown window,
 * after evhttp_free(). */
register_shutdown_function(function () use (&$ws): void {
    if ($ws === null) {
        return;
    }
    $read = fread($ws, 8192);
    $written = fwrite($ws, 'xxxx');
    $feof = feof($ws);
    /* Normalise: any failure (false, 0, negative) is "nothing written". */
    file_put_contents(__DIR__ . '/marker.txt',
        'read=' . var_export($read === false ? '' : $read, true)
        . ' written=' . ($written === false || $written <= 0 ? '0' : $written)
        . ' feof=' . var_export($feof, true));
});

$watcher = fpmng_worker_event_create(FPMNG_WORKER_READ, $notify, function () use ($notify, &$ws): void {
    fread($notify, 65536);
    while (($id = fpmng_worker_next_request()) !== null) {
        $env = fpmng_worker_request_env($id);
        $uri = $env['REQUEST_URI'] ?? '/';

        if (str_contains($uri, '/ws')) {
            /* The upgrade completes the request with its own 101. */
            $ws = fpmng_worker_upgrade($id, []);
            continue;
        }
        fpmng_worker_respond($id, 200, ['Content-Type' => 'text/plain'], 'pid ' . getmypid());
    }
});
fpmng_worker_event_enable($watcher);

while (!fpmng_worker_may_exit()) {
    fpmng_worker_loop(true);
}
PHP);

$port = (int) (getenv('FPMNG_DIRECT_WORKER_WS_ORPHAN_PORT') ?: 28147);
$config = <<<CFG
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
[worker]
listen = 127.0.0.1:$port
pool.type = http-direct
pool.executor = worker
pm = static
pm.max_children = 1
pm.max_requests = 3
chdir = $root
http.front_controller = /worker.php
http.read_timeout = 10000
env[USE_ZEND_ALLOC] = 0
env[MALLOC_PERTURB_] = 165
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

function readHead($fp): string
{
    $line = fgets($fp);
    if (!$line || !str_starts_with($line, 'HTTP/')) {
        throw new RuntimeException('bad status line: ' . var_export($line, true));
    }
    $status = trim($line);
    while (($line = fgets($fp)) !== false && $line !== "\r\n") {
    }
    return $status;
}

function probe(int $port): string
{
    $fp = connect($port);
    fwrite($fp, "GET / HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n");
    $status = readHead($fp);
    $body = stream_get_contents($fp);
    fclose($fp);
    check(str_starts_with($status, 'HTTP/1.1 200'), "probe status: $status");
    return trim((string) $body);
}

$tester = new FPM\Tester($config, '<?php echo "unused";');
try {
    $tester->start();
    $tester->expectLogStartNotices();

    /* Request 1: upgrade; the stream is held for the child's whole life. */
    $ws = connect($port);
    fwrite($ws, "GET /ws HTTP/1.1\r\nHost: t\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
        . "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n");
    $status = readHead($ws);
    check(str_starts_with($status, 'HTTP/1.1 101'), "101 expected: $status");

    /* Request 2: the child's pid while the ws is held. */
    $pid0 = probe($port);
    check(str_starts_with($pid0, 'pid '), 'probe body: ' . var_export($pid0, true));

    /* Request 3: pm.max_requests retires the child after it. The shutdown
     * function then touches the stream -- the orphaned window. */
    probe($port);

    /* The successor must answer (a clean recycle), and the marker must show
     * the ops answered "dead stream": nothing read, nothing written (the
     * pre-fix run recorded written=4 -- a write through freed memory), and
     * feof true. */
    $pid1 = probe($port);
    check($pid1 !== $pid0, 'expected the child to retire and a successor to answer, got ' . var_export($pid1, true));

    $marker = @file_get_contents("$root/marker.txt");
    check($marker === "read='' written=0 feof=true",
        'shutdown-window ops answered dead-stream: ' . var_export($marker, true));
    echo "orphaned-ops-dead-stream: ok\n";

    /* The retirement must have been clean -- no signal death in the log. */
    $tester->expectNoLogPattern('exited on signal');
    echo "clean-recycle: ok\n";
    fclose($ws);
} finally {
    $tester->terminate();
    $tester->close();
    @unlink("$root/worker.php");
    @unlink("$root/marker.txt");
    @rmdir($root);
}
echo "Done\n";
?>
--EXPECT--
orphaned-ops-dead-stream: ok
clean-recycle: ok
Done
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>

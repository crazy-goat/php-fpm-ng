--TEST--
fpm-ng: closing upgraded streams releases their connection -- the worker's fd count does not grow with closed streams (issue #472)
--SKIPIF--
<?php
include "skipif.inc";
if (!is_dir('/proc/self/fd')) {
    die("skip needs /proc to count the worker's descriptors");
}
?>
--FILE--
<?php

require_once "tester.inc";

function check(bool $condition, string $message): void
{
    if (!$condition) throw new RuntimeException($message);
}

/* Issue #472: an upgraded connection stays evhttp's for the worker's life, and
 * closing the stream used to shutdown() it without ever freeing it, so a
 * long-lived worker accumulated one inert fd per closed WebSocket. USE_ZEND_ALLOC=0
 * + MALLOC_PERTURB_=165 are kept from the #442 test so a stray write through a
 * freed context or connection is a signal, not a coin flip. */

$root = sys_get_temp_dir() . '/fpmng-ws-close-idle-' . getmypid();
@mkdir($root, 0700, true);

file_put_contents("$root/worker.php", <<<'PHP'
<?php
$notify = fpmng_worker_notify_stream();
$wss = [];

$watcher = fpmng_worker_event_create(FPMNG_WORKER_READ, $notify, function () use ($notify, &$wss): void {
    fread($notify, 65536);
    while (($id = fpmng_worker_next_request()) !== null) {
        $env = fpmng_worker_request_env($id);
        $uri = $env['REQUEST_URI'] ?? '/';

        if (str_contains($uri, '/ws')) {
            $wss[] = fpmng_worker_upgrade($id, []);
            continue;
        }
        if (str_contains($uri, '/close-ws')) {
            $n = count($wss);
            foreach ($wss as $w) { fclose($w); }
            $wss = [];
            fpmng_worker_respond($id, 200, ['Content-Type' => 'text/plain'], 'closed:' . $n);
            continue;
        }
        if (str_contains($uri, '/reap-ws')) {
            /* peer closed first: read EOF then fclose */
            $n = count($wss);
            foreach ($wss as $w) { @fread($w, 10); fclose($w); }
            $wss = [];
            fpmng_worker_respond($id, 200, ['Content-Type' => 'text/plain'], 'reaped:' . $n);
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

$port = (int) (getenv('FPMNG_DIRECT_WORKER_WS_FD_RELEASE_PORT') ?: 28172 + 200 * (int) getenv('TEST_PHP_WORKER') + (int) getenv('FPMNG_PHPT_PORT_SHIFT'));
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

function readHead($fp): array
{
    $line = fgets($fp);
    if (!$line || !str_starts_with($line, 'HTTP/')) {
        throw new RuntimeException('bad status line: ' . var_export($line, true));
    }
    $status = trim($line);
    $headers = [];
    while (($line = fgets($fp)) !== false && $line !== "\r\n") {
        [$k, $v] = explode(':', $line, 2);
        $headers[strtolower(trim($k))] = trim($v);
    }
    return [$status, $headers];
}

function probe(int $port): array
{
    $fp = connect($port);
    fwrite($fp, "GET / HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n");
    [$status, ] = readHead($fp);
    $body = stream_get_contents($fp);
    fclose($fp);
    check(str_starts_with($status, 'HTTP/1.1 200'), "probe status: $status");
    return [trim((string) $body)];
}

$tester = new FPM\Tester($config, '<?php echo "unused";');
try {
    $tester->start();
    $tester->expectLogStartNotices();

    [$pid0] = probe($port);
    $pid = (int) substr($pid0, 4);
    $fds = fn() => count(scandir("/proc/$pid/fd")) - 2;
    $baseline = $fds();

    /* Issue #472: shutdown() ended each closed stream but its fd stayed open
     * until evhttp_free(), one per closed stream (measured 23 -> 323 fds over
     * 300 closes). Both orders matter: the server closing an idle stream, and
     * the peer going first so the server reads EOF before it closes. */
    $N = 40;
    foreach (['close-ws' => false, 'reap-ws' => true] as $mode => $peerFirst) {
        $clients = [];
        for ($i = 0; $i < $N; $i++) {
            $ws = connect($port);
            fwrite($ws, "GET /ws HTTP/1.1\r\nHost: t\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                . "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n");
            readHead($ws);
            $clients[] = $ws;
        }
        check($fds() >= $baseline + $N, "$mode: the streams are not holding descriptors");
        if ($peerFirst) {
            foreach ($clients as $c) fclose($c);
            usleep(300000);
        }
        $fp = connect($port);
        fwrite($fp, "GET /$mode HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n");
        readHead($fp);
        $answer = trim((string) stream_get_contents($fp));
        fclose($fp);
        check(str_ends_with($answer, ":$N"), "$mode answered " . var_export($answer, true));
        if (!$peerFirst) {
            foreach ($clients as $c) fclose($c);
        }
        /* Released within a bounded number of loop iterations: poll briefly
         * instead of sleeping a fixed time. */
        $deadline = microtime(true) + 6;
        while ($fds() > $baseline + 2 && microtime(true) < $deadline) {
            usleep(50000);
        }
        $after = $fds();
        check($after <= $baseline + 2, "$mode: $after descriptors after closing $N streams, baseline $baseline");
        echo "$mode-releases-fds: ok\n";
    }
    [$pid1] = probe($port);
    check($pid1 === $pid0, 'worker did not survive');
    echo "worker-survives: ok\n";
} finally {
    $tester->terminate();
    $tester->close();
    @unlink("$root/worker.php");
    @rmdir($root);
}
echo "Done\n";
?>
--EXPECT--
close-ws-releases-fds: ok
reap-ws-releases-fds: ok
worker-survives: ok
Done
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>

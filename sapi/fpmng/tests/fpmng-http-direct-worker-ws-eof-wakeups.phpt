--TEST--
fpm-ng: worker ws does not busy-loop the read watcher after EOF (issue #460)
--SKIPIF--
<?php include "skipif.inc"; ?>
--FILE--
<?php

require_once "tester.inc";

function check(bool $condition, string $message): void
{
    if (!$condition) throw new RuntimeException($message);
}

/* Issue #460: once EOF is flagged on an upgraded WebSocket stream and userland
 * does not fclose() it, the read watcher used to fire continuously -- a
 * level-triggered busy-loop at 100% CPU -- until the stream was closed, because
 * the read watcher is an EV_PERSIST event bound to a descriptor that is now
 * permanently readable. The worker below records every wakeup and never closes;
 * the client half-closes, waits, and counts. The fix delivers the EOF wakeup
 * once and takes the watcher off its descriptor; the broken code records
 * thousands of wakeups in the same window. */

$root = sys_get_temp_dir() . '/fpmng-ws-eof-' . getmypid();
@mkdir($root, 0700, true);
$wakeups = "$root/wakeups.log";
@unlink($wakeups);

file_put_contents("$root/worker.php", <<<PHP
<?php
\$wakeups = '$wakeups';
\$notify = fpmng_worker_notify_stream();
\$ws = null;
\$wsWatcher = null;

\$watcher = fpmng_worker_event_create(FPMNG_WORKER_READ, \$notify, function () use (\$notify, &\$ws, &\$wsWatcher, \$wakeups): void {
    fread(\$notify, 65536);
    while ((\$id = fpmng_worker_next_request()) !== null) {
        \$ws = fpmng_worker_upgrade(\$id, []);
        \$wsWatcher = fpmng_worker_event_create(FPMNG_WORKER_READ, \$ws, function () use (&\$ws, \$wakeups): void {
            @file_put_contents(\$wakeups, '1', FILE_APPEND);
            if (\$ws === null) {
                return;
            }
            /* Drain whatever is there; on EOF this returns '' but the watcher
             * must not be the reason the loop cannot sleep. */
            while ((\$chunk = fread(\$ws, 8192)) !== false && \$chunk !== '') {
                /* discard */
            }
        });
        fpmng_worker_event_enable(\$wsWatcher);
    }
});
fpmng_worker_event_enable(\$watcher);

while (!fpmng_worker_may_exit() || \$ws !== null) {
    fpmng_worker_loop(true);
}
PHP);

$port = (int) (getenv('FPMNG_DIRECT_WORKER_WS_EOF_PORT') ?: 28147);
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

function readHead($fp): array
{
    $line = fgets($fp);
    if (!$line || !str_starts_with($line, 'HTTP/')) {
        throw new RuntimeException('bad status line: ' . var_export($line, true));
    }
    $status = trim($line);
    while (($line = fgets($fp)) !== false && $line !== "\r\n") {
        /* skip headers */
    }
    return [$status];
}

function wsFrame(string $payload, int $op = 0x1): string
{
    $len = strlen($payload);
    $head = chr($op | 0x80);
    if ($len < 126) {
        $head .= chr(0x80 | $len);
    } else {
        $head .= pack('n', 0x80 | 126) . pack('n', $len);
    }
    $mask = '1234';
    $masked = '';
    for ($i = 0; $i < $len; $i++) {
        $masked .= $payload[$i] ^ $mask[$i % 4];
    }
    return $head . $mask . $masked;
}

$tester = new FPM\Tester($config, '<?php echo "unused";');
try {
    $tester->start();
    $tester->expectLogStartNotices();

    $ws = connect($port);
    fwrite($ws, "GET /ws HTTP/1.1\r\nHost: t\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
        . "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n");
    [$status] = readHead($ws);
    check(str_starts_with($status, 'HTTP/1.1 101'), "101 expected: $status");
    echo "upgrade-101: ok\n";

    /* Let the worker see the frame, then half-close so it flags EOF while the
     * stream stays open. */
    fwrite($ws, wsFrame('x'));
    usleep(200000);
    stream_socket_shutdown($ws, STREAM_SHUT_WR);

    /* Two seconds is ~2600 iterations at the measured ~1300/s; a fixed watcher
     * records one data wakeup plus one EOF wakeup. 100 is a very wide margin
     * between the two. */
    usleep(2000000);
    $count = is_file($wakeups) ? strlen((string) file_get_contents($wakeups)) : 0;
    check($count > 0, 'the read watcher never fired at all');
    check($count < 100, "read watcher busy-looped after EOF: $count wakeups in 2s");
    echo "eof-wakeups-bounded: ok\n";

    fclose($ws);
} finally {
    $tester->terminate();
    $tester->expectLogTerminatingNotices();
    $tester->close();
    @unlink($wakeups);
    @unlink("$root/worker.php");
    @rmdir($root);
}
echo "Done\n";
?>
--EXPECT--
upgrade-101: ok
eof-wakeups-bounded: ok
Done
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>

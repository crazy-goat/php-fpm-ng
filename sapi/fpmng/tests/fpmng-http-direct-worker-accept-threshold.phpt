--TEST--
fpm-ng: worker.accept_threshold stops one worker from swallowing a whole burst (issue #338)
--SKIPIF--
<?php include "skipif.inc"; ?>
--FILE--
<?php
require_once "tester.inc";

/* Issue #338, the worker-executor counterpart of
 * fpmng-http-direct-accept-fairness.phpt (issue #53, classic).
 *
 * Every child of an http-direct pool accepts from the one listening socket the
 * master opened, and libevent's listener_read_cb() accepts until the queue is
 * empty -- so whichever child wakes first takes the whole burst, and keep-alive
 * connections then stay pinned to it. Measured on this executor before
 * worker.accept_threshold existed: two workers out of eight served a
 * 64-connection keep-alive run, the busiest of them 81 % of it
 * (build/benchmark-http-direct-fairness.py --executor worker, numbers in
 * docs/http-direct.md).
 *
 * worker.accept_threshold caps how many connections one worker accepts before
 * it disables its own listener for the cooldown that keeps it out of the accept
 * race. This test asserts the observable consequence: a burst of connections
 * opened before any request is written is served by more than one worker.
 *
 * The bar is deliberately "more than one", not "all four", and two rounds out
 * of three rather than all of them: which child the kernel wakes is not ours to
 * choose, and the directive is a ceiling rather than a scheduler. What it
 * removes is the case where one worker takes everything -- which is exactly
 * what the unset-directive baseline did on every single round of this shape
 * while this test was being written. */

$root = sys_get_temp_dir() . '/fpmng-worker-accept-threshold-' . getmypid();
@mkdir($root, 0700, true);

/* The handler blocks for a little while on purpose: this executor's event loop
 * cannot run while PHP executes, so a worker that hoarded the burst would also
 * serialise it, which is the cost the directive exists to avoid. */
file_put_contents("$root/worker.php", <<<'PHP'
<?php
$notify = fpmng_worker_notify_stream();
$watcher = fpmng_worker_event_create(FPMNG_WORKER_READ, $notify, function () use ($notify): void {
    fread($notify, 65536);
    while (($id = fpmng_worker_next_request()) !== null) {
        usleep(50000);
        fpmng_worker_respond($id, 200, ['Content-Type' => 'text/plain'], (string) getmypid());
    }
});
fpmng_worker_event_enable($watcher);
while (!fpmng_worker_may_exit()) {
    fpmng_worker_loop(true);
}
PHP);

$port = (int) (getenv('FPMNG_DIRECT_WORKER_ACCEPT_THRESHOLD_PORT') ?: 28112);
$config = <<<CFG
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
[worker]
listen = 127.0.0.1:$port
pool.type = http-direct
pool.executor = worker
pm = static
pm.max_children = 4
chdir = $root
http.front_controller = /worker.php
worker.accept_threshold = 1
catch_workers_output = yes
php_admin_value[max_execution_time] = 0
php_admin_value[display_errors] = 0
CFG;

/* One burst: every connection is established before any request is written, so
 * the kernel has all of them queued when the first child wakes. Returns the set
 * of pids that answered. */
function burst(int $port, int $connections): array
{
    $sockets = [];
    for ($i = 0; $i < $connections; $i++) {
        $fp = stream_socket_client("tcp://127.0.0.1:$port", $errno, $error, 5);
        if (!$fp) {
            throw new RuntimeException("connect #$i: $error ($errno)");
        }
        stream_set_blocking($fp, false);
        $sockets[$i] = $fp;
    }
    foreach ($sockets as $fp) {
        fwrite($fp, "GET / HTTP/1.1\r\nHost: test\r\nConnection: close\r\n\r\n");
    }

    $raw = array_fill(0, $connections, '');
    $deadline = microtime(true) + 20;
    while ($sockets && microtime(true) < $deadline) {
        $read = $sockets;
        $write = $except = null;
        if (!stream_select($read, $write, $except, 1)) {
            continue;
        }
        foreach ($read as $fp) {
            $i = array_search($fp, $sockets, true);
            $chunk = fread($fp, 8192);
            if ($chunk !== false && $chunk !== '') {
                $raw[$i] .= $chunk;
            }
            if ($chunk === '' || feof($fp)) {
                fclose($fp);
                unset($sockets[$i]);
            }
        }
    }
    foreach ($sockets as $i => $fp) {
        fclose($fp);
        throw new RuntimeException("response #$i never completed");
    }

    $pids = [];
    foreach ($raw as $i => $text) {
        [$head, $body] = array_pad(explode("\r\n\r\n", $text, 2), 2, '');
        if (!str_contains($head, ' 200 ')) {
            throw new RuntimeException("response #$i is not a 200: " . strtok($head, "\r\n"));
        }
        if ((int) $body <= 0) {
            throw new RuntimeException("response #$i carries no pid");
        }
        $pids[(int) $body] = true;
    }
    return array_keys($pids);
}

$tester = new FPM\Tester($config, '<?php');
try {
    $tester->start();
    $tester->expectLogStartNotices();

    /* Two of three rounds, for the same reason issue #53's own test settles for
     * two of three: which child the kernel wakes is not ours to choose. */
    $rounds = 3;
    $spread = 0;
    $seen = [];
    for ($r = 0; $r < $rounds; $r++) {
        $pids = burst($port, 8);
        $seen[] = count($pids);
        if (count($pids) > 1) {
            $spread++;
        }
    }
    if ($spread < 2) {
        throw new RuntimeException(sprintf(
            'only %d of %d bursts reached more than one worker; workers per round: %s',
            $spread, $rounds, implode(', ', $seen)));
    }
    echo "burst-reaches-more-than-one-worker: ok\n";

    /* The connection pipeline still works end to end with the directive set --
     * the ceiling must not be able to leave a listener disabled for good. */
    $hello = file_get_contents("http://127.0.0.1:$port/");
    if ((int) $hello <= 0) {
        throw new RuntimeException('a plain request after the bursts: ' . var_export($hello, true));
    }
    echo "still-serving-after-the-bursts: ok\n";

    $tester->terminate();
    $tester->expectLogTerminatingNotices();
    $tester->close();
} finally {
    @unlink("$root/worker.php");
    @rmdir($root);
}
echo "Done\n";
?>
--EXPECT--
burst-reaches-more-than-one-worker: ok
still-serving-after-the-bursts: ok
Done
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>

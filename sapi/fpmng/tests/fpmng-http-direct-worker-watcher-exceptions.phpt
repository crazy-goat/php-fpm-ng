--TEST--
fpm-ng: an exception thrown from a watcher callback breaks the loop cleanly and a later loop() call recovers (issue #336)
--SKIPIF--
<?php include "skipif.inc"; ?>
--FILE--
<?php
require_once "tester.inc";

function check(bool $condition, string $message): void
{
    if (!$condition) throw new RuntimeException($message);
}

/* fpm_worker_watcher_fire() calls event_base_loopbreak() when the callback it
 * just ran leaves an exception pending, and fpmng_worker_loop() rethrows it.
 * Untested until now:
 *
 *  (a) that a SUBSEQUENT fpmng_worker_loop() call still works, and that a
 *      sibling watcher activated in the SAME iteration as the one that threw
 *      is not silently lost -- event_base_loopbreak() stops libevent's
 *      current pass without waiting for every already-active callback to run,
 *      so a watcher activated alongside the throwing one fires on the NEXT
 *      loop() call rather than never.
 *
 *  (b) fpmng_worker_event_free() called from INSIDE its own callback for a
 *      STREAM watcher specifically. fpmng-http-direct-worker.phpt's
 *      waitFor() already proves the idiom for a TIMER watcher
 *      ($box->id/fpmng_worker_event_free($box->id) at its own top); this
 *      proves the same self-free pattern for a READ watcher on a real stream,
 *      whose fpm_worker_watcher_dtor() also releases watcher->stream, not
 *      just watcher->callback.
 */
$root = sys_get_temp_dir() . '/fpmng-worker-watcher-exceptions-' . getmypid();
@mkdir($root, 0700, true);

file_put_contents("$root/worker.php", <<<'PHP'
<?php
$notify = fpmng_worker_notify_stream();

function handle(int $id): void
{
    $env = fpmng_worker_request_env($id);
    $uri = $env['REQUEST_URI'] ?? '/';

    if ($uri === '/throw-and-sibling') {
        /* Two one-shot timers armed for the same instant. Both become active
         * in the SAME event_base_loop() pass; the first one's callback
         * throws, which loopbreak()s that pass. The second must not be lost:
         * it stays pending and fires on the loop() call this handler makes
         * right afterwards. */
        $siblingFired = 0;
        $sibling = fpmng_worker_event_create(FPMNG_WORKER_TIMER, null, function () use (&$siblingFired): void {
            $siblingFired++;
        });
        $thrower = fpmng_worker_event_create(FPMNG_WORKER_TIMER, null, function (): void {
            throw new RuntimeException('deliberate watcher exception');
        });
        fpmng_worker_event_enable($sibling, 0.01);
        fpmng_worker_event_enable($thrower, 0.01);

        /* Let both timers become due, then run the loop until the thrower's
         * exception surfaces. */
        usleep(30000);
        $caught = null;
        try {
            fpmng_worker_loop(false);
        } catch (\Throwable $e) {
            $caught = $e->getMessage();
        }
        fpmng_worker_event_free($thrower);

        /* The sibling was activated in the same pass as the thrower but must
         * not have run its callback yet if the pass broke before reaching it,
         * NOR must it have been silently dropped -- one more loop() call
         * (this file's "subsequent call still works" assertion) has to
         * deliver it. */
        $afterThrow = $siblingFired;
        for ($i = 0; $i < 20 && $siblingFired === 0; $i++) {
            fpmng_worker_loop(false);
        }
        fpmng_worker_event_free($sibling);

        fpmng_worker_respond($id, 200, [], json_encode([
            'caught' => $caught,
            'sibling_fired_immediately_after_throw' => $afterThrow,
            'sibling_fired_eventually' => $siblingFired,
        ]));
        return;
    }

    if ($uri === '/self-free-stream') {
        /* The timer idiom (fpmng-http-direct-worker.phpt's waitFor()) frees
         * its own watcher from inside its callback. This is the same idiom
         * for a STREAM (read) watcher: fpm_worker_watcher_dtor() also
         * zval_ptr_dtor()s watcher->stream, which the timer case never
         * exercises because a timer watcher's ->stream is IS_UNDEF. */
        [$a, $b] = stream_socket_pair(STREAM_PF_UNIX, STREAM_SOCK_STREAM, STREAM_IPPROTO_IP);
        stream_set_blocking($a, false);
        stream_set_blocking($b, false);
        fwrite($b, 'x');
        $box = new stdClass();
        $fired = 0;
        $error = null;
        $box->id = fpmng_worker_event_create(FPMNG_WORKER_READ, $a, function () use ($a, $box, &$fired): void {
            $fired++;
            fread($a, 1);
            fpmng_worker_event_free($box->id);
        });
        fpmng_worker_event_enable($box->id);
        try {
            for ($i = 0; $i < 20 && $fired === 0; $i++) {
                fpmng_worker_loop(false);
            }
            /* A few more iterations: if the freed watcher's descriptor were
             * still referenced somewhere, the loop would either crash or spin
             * forever trying to fire it again for the byte already read. */
            for ($i = 0; $i < 20; $i++) {
                fpmng_worker_loop(false);
            }
        } catch (\Throwable $e) {
            $error = $e->getMessage();
        }
        fclose($a);
        fclose($b);
        fpmng_worker_respond($id, 200, [], json_encode(['fired' => $fired, 'error' => $error]));
        return;
    }

    fpmng_worker_respond($id, 200, [], 'hello from pid ' . getmypid());
}

$watcher = fpmng_worker_event_create(FPMNG_WORKER_READ, $notify, function () use ($notify): void {
    fread($notify, 65536);
});
fpmng_worker_event_enable($watcher);

/* handle() runs OUTSIDE the notify watcher's own callback -- same shape as
 * fpmng-http-direct-worker-buffered-streams.phpt -- because both routes below
 * drive fpmng_worker_loop() themselves, which refuses to run while an outer
 * fpmng_worker_loop() call (here, the notify watcher's) is already on the
 * stack. */
while (!fpmng_worker_may_exit()) {
    fpmng_worker_loop(true);
    while (($id = fpmng_worker_next_request()) !== null) {
        handle($id);
    }
}
PHP);

$port = (int) (getenv('FPMNG_DIRECT_WORKER_WATCHER_EXCEPTIONS_PORT') ?: 28103);
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

$tester = new FPM\Tester($config, '<?php');
try {
    $tester->start();
    $tester->expectLogStartNotices();

    $hello = file_get_contents("http://127.0.0.1:$port/");
    check(is_string($hello) && str_starts_with($hello, 'hello from pid '), 'hello: ' . var_export($hello, true));

    $result = json_decode((string) file_get_contents("http://127.0.0.1:$port/throw-and-sibling"), true);
    check(is_array($result), 'throw-and-sibling: not json');
    check($result['caught'] === 'deliberate watcher exception', 'caught: ' . var_export($result['caught'], true));
    check($result['sibling_fired_eventually'] === 1,
        'the sibling watcher activated in the same pass as the thrower was lost: ' . var_export($result, true));
    echo "exception-rethrown-and-sibling-not-lost: ok\n";

    /* A subsequent fpmng_worker_loop() call still works: the request above
     * only completed by making one, and this second, unrelated request
     * proves the loop is not wedged for the rest of the worker's life. */
    $again = file_get_contents("http://127.0.0.1:$port/");
    check($again === $hello, "worker's loop did not recover after the exception: $again vs $hello");
    echo "loop-recovers-after-exception: ok\n";

    $selfFree = json_decode((string) file_get_contents("http://127.0.0.1:$port/self-free-stream"), true);
    check(is_array($selfFree), 'self-free-stream: not json');
    check($selfFree['error'] === null, 'self-free-stream error: ' . var_export($selfFree['error'], true));
    check($selfFree['fired'] === 1, 'self-free-stream fired: ' . var_export($selfFree['fired'], true));
    echo "self-free-stream-watcher: ok\n";

    $again2 = file_get_contents("http://127.0.0.1:$port/");
    check($again2 === $hello, "worker was replaced: $again2 vs $hello");
    echo "worker-persists: ok\n";
} finally {
    $tester->terminate();
    $tester->close();
    @unlink("$root/worker.php");
    @rmdir($root);
}
echo "Done\n";
?>
--EXPECT--
exception-rethrown-and-sibling-not-lost: ok
loop-recovers-after-exception: ok
self-free-stream-watcher: ok
worker-persists: ok
Done
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>

--TEST--
fpm-ng: FPMNG_WORKER_WRITE watchers and fpmng_worker_event_disable() (issue #336)
--SKIPIF--
<?php include "skipif.inc"; ?>
--FILE--
<?php
require_once "tester.inc";

function check(bool $condition, string $message): void
{
    if (!$condition) throw new RuntimeException($message);
}

/* FPMNG_WORKER_WRITE and fpmng_worker_event_disable() are exercised today only
 * by the Docker-based amphp harness (build/test-http-direct-amphp.sh, issue
 * #75), which is not wired into CI. This test is dependency-free: a
 * stream_socket_pair() gives a non-blocking, always-writable local socket to
 * register a write watcher on, with no network and no origin process. */
$root = sys_get_temp_dir() . '/fpmng-worker-write-watcher-' . getmypid();
@mkdir($root, 0700, true);

file_put_contents("$root/worker.php", <<<'PHP'
<?php
/* This script runs as its own process, spawned by FPM -- it does not share
 * scope with the outer test file, so it needs its own copy of check(). */
function check(bool $condition, string $message): void
{
    if (!$condition) throw new RuntimeException($message);
}

$notify = fpmng_worker_notify_stream();

function handle(int $id): void
{
    $env = fpmng_worker_request_env($id);
    $uri = $env['REQUEST_URI'] ?? '/';

    if ($uri === '/write-fires') {
        /* A local socketpair is writable the instant it is created -- nothing
         * else needs to drain it for the write side to be ready. */
        [$a, $b] = stream_socket_pair(STREAM_PF_UNIX, STREAM_SOCK_STREAM, STREAM_IPPROTO_IP);
        stream_set_blocking($a, false);
        stream_set_blocking($b, false);
        $fired = 0;
        $watcher = fpmng_worker_event_create(FPMNG_WORKER_WRITE, $a, function () use (&$fired): void {
            $fired++;
        });
        fpmng_worker_event_enable($watcher);
        for ($i = 0; $i < 20 && $fired === 0; $i++) {
            fpmng_worker_loop(false);
        }
        fpmng_worker_event_free($watcher);
        fclose($a);
        fclose($b);
        fpmng_worker_respond($id, 200, [], json_encode(['fired' => $fired]));
        return;
    }

    if ($uri === '/write-disabled') {
        [$a, $b] = stream_socket_pair(STREAM_PF_UNIX, STREAM_SOCK_STREAM, STREAM_IPPROTO_IP);
        stream_set_blocking($a, false);
        stream_set_blocking($b, false);
        $fired = 0;
        $watcher = fpmng_worker_event_create(FPMNG_WORKER_WRITE, $a, function () use (&$fired): void {
            $fired++;
        });
        /* Never enabled at all: a freshly created watcher must stay silent
         * until fpmng_worker_event_enable() is called, the same contract a
         * disabled one has. */
        for ($i = 0; $i < 20; $i++) {
            fpmng_worker_loop(false);
        }
        $neverEnabled = $fired;

        fpmng_worker_event_enable($watcher);
        for ($i = 0; $i < 20 && $fired === 0; $i++) {
            fpmng_worker_loop(false);
        }
        $afterEnable = $fired;

        /* Disable it, then run several more non-blocking iterations: the
         * always-writable descriptor would fire every single time if disable
         * did not actually stop it. */
        check(fpmng_worker_event_disable($watcher) === true, 'event_disable() did not report success');
        for ($i = 0; $i < 20; $i++) {
            fpmng_worker_loop(false);
        }
        $afterDisable = $fired;

        fpmng_worker_event_free($watcher);
        fclose($a);
        fclose($b);
        fpmng_worker_respond($id, 200, [], json_encode([
            'never_enabled' => $neverEnabled,
            'after_enable' => $afterEnable,
            'after_disable' => $afterDisable,
        ]));
        return;
    }

    if ($uri === '/write-timer-fallback') {
        /* Re-enabling with a timeout: on a descriptor that never becomes
         * writable (both peer fds closed, only the write side kept open by
         * this process holding its reference) the timer path must still fire
         * so a re-armed write watcher cannot wedge a driver forever. A pipe's
         * write end has no "writable" event once the read end is closed and
         * the buffer is full, which needs more setup than this test wants;
         * instead this uses fpmng_worker_event_enable()'s own documented
         * timeout argument on a TIMER watcher registered the same way a write
         * watcher's caller would combine the two -- proving the timeout
         * argument itself reaches libevent and fires when nothing else does. */
        [$a, $b] = stream_socket_pair(STREAM_PF_UNIX, STREAM_SOCK_STREAM, STREAM_IPPROTO_IP);
        stream_set_blocking($a, false);
        stream_set_blocking($b, false);
        $writeFired = 0;
        $timerFired = 0;
        $writeWatcher = fpmng_worker_event_create(FPMNG_WORKER_WRITE, $a, function () use (&$writeFired): void {
            $writeFired++;
        });
        /* Fully drain the socket's send buffer backlog is not needed: this
         * disables the write watcher immediately after its first fire so only
         * the timer below is left racing the clock, proving the timer path
         * fires on its own rather than merely alongside an always-ready
         * write watcher. */
        fpmng_worker_event_enable($writeWatcher);
        fpmng_worker_loop(false);
        fpmng_worker_event_disable($writeWatcher);
        fpmng_worker_event_free($writeWatcher);

        $timerWatcher = fpmng_worker_event_create(FPMNG_WORKER_TIMER, null, function () use (&$timerFired): void {
            $timerFired++;
        });
        fpmng_worker_event_enable($timerWatcher, 0.05);
        for ($i = 0; $i < 100 && $timerFired === 0; $i++) {
            fpmng_worker_loop(true);
        }
        fpmng_worker_event_free($timerWatcher);
        fclose($a);
        fclose($b);
        fpmng_worker_respond($id, 200, [], json_encode([
            'write_fired_before_disable' => $writeFired,
            'timer_fired' => $timerFired,
        ]));
        return;
    }

    fpmng_worker_respond($id, 200, [], 'hello from pid ' . getmypid());
}

$watcher = fpmng_worker_event_create(FPMNG_WORKER_READ, $notify, function () use ($notify): void {
    fread($notify, 65536);
});
fpmng_worker_event_enable($watcher);

/* handle() is called OUTSIDE the notify watcher's callback, once
 * fpmng_worker_loop() has already returned -- same shape as
 * fpmng-http-direct-worker-buffered-streams.phpt. Several handlers here call
 * fpmng_worker_loop() themselves to drive a watcher of their own to
 * completion, which fpmng_worker_loop() refuses to do while ITSELF is already
 * on the call stack (fw.running); calling handle() from inside another
 * watcher's callback would trip that guard on the very first nested call. */
while (!fpmng_worker_may_exit()) {
    fpmng_worker_loop(true);
    while (($id = fpmng_worker_next_request()) !== null) {
        handle($id);
    }
}
PHP);

$port = (int) (getenv('FPMNG_DIRECT_WORKER_WRITE_WATCHER_PORT') ?: 28102);
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
php_admin_value[max_execution_time] = 0
php_admin_value[display_errors] = 0
CFG;

$tester = new FPM\Tester($config, '<?php');
try {
    $tester->start();
    $tester->expectLogStartNotices();

    $hello = file_get_contents("http://127.0.0.1:$port/");
    check(is_string($hello) && str_starts_with($hello, 'hello from pid '), 'hello: ' . var_export($hello, true));

    $fires = json_decode((string) file_get_contents("http://127.0.0.1:$port/write-fires"), true);
    check(is_array($fires) && $fires['fired'] > 0, 'write-fires: ' . var_export($fires, true));
    echo "write-watcher-fires: ok\n";

    $disabled = json_decode((string) file_get_contents("http://127.0.0.1:$port/write-disabled"), true);
    check(is_array($disabled), 'write-disabled: not json');
    check($disabled['never_enabled'] === 0, 'a never-enabled watcher fired: ' . var_export($disabled, true));
    check($disabled['after_enable'] > 0, 'enabling did not make it fire: ' . var_export($disabled, true));
    check($disabled['after_disable'] === $disabled['after_enable'],
        'a disabled write watcher kept firing: ' . var_export($disabled, true));
    echo "write-watcher-disable-stops-it: ok\n";

    $fallback = json_decode((string) file_get_contents("http://127.0.0.1:$port/write-timer-fallback"), true);
    check(is_array($fallback), 'write-timer-fallback: not json');
    check($fallback['write_fired_before_disable'] > 0,
        'the write watcher never fired before being disabled: ' . var_export($fallback, true));
    check($fallback['timer_fired'] === 1,
        'the timer path did not fire once the write watcher was gone: ' . var_export($fallback, true));
    echo "timer-fires-when-write-watcher-disabled: ok\n";

    $again = file_get_contents("http://127.0.0.1:$port/");
    check($again === $hello, "worker was replaced: $again vs $hello");
    echo "worker-persists: ok\n";

    $tester->expectNoLogPattern('/ERROR:/', true);
} finally {
    $tester->terminate();
    $tester->close();
    @unlink("$root/worker.php");
    @rmdir($root);
}
echo "Done\n";
?>
--EXPECT--
write-watcher-fires: ok
write-watcher-disable-stops-it: ok
timer-fires-when-write-watcher-disabled: ok
worker-persists: ok
Done
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>

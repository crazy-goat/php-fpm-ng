--TEST--
fpm-ng: a direct HTTP child out of file descriptors does not spin on accept() and recovers when they are freed (issue #729)
--SKIPIF--
<?php
include "skipif.inc";
if (!is_dir('/proc/self/fd')) {
    die("skip needs /proc");
}
?>
--FILE--
<?php

require_once "tester.inc";

/* Issue #729, the http-direct counterpart of
 * fpmng-gateway-accept-emfile-backoff.phpt (issue #687). Both executors
 * (classic and worker) accept through evhttp_accept_socket_with_handle() on the
 * shared listening socket, so both are run with the same scenario.
 *
 * The global rlimit_files caps every process of the master at 64 descriptors.
 * Idle clients are opened until accept() in the one child fails with EMFILE;
 * the kernel queue keeps the rest, so the listening socket stays readable.
 * Without an evconnlistener error callback libevent only logs each failed
 * accept() and the listener stays readable: a busy loop on one core. The child
 * logs through the master, which drains the pipe, so the unfixed child really
 * spins: measured with the fix reverted, about 200 CPU ticks in 2 s (classic
 * and worker). The CPU check catches that, and the "pausing accept" WARNING
 * comes from the callback.
 *
 * The child is then retired (SIGUSR1) while it is still out of descriptors. A
 * backoff that outlives the listener it was installed on (retiring and
 * stopping delete the listener, the 100 ms resume timer would then enable freed
 * memory) kills the child with SIGSEGV about 100 ms later; the log must show no
 * "exited on signal". Afterwards the clients are closed and a request must be
 * answered by the replacement child. */

$root = sys_get_temp_dir() . '/fpmng-direct-emfile-' . getmypid();
@mkdir($root, 0700, true);
file_put_contents("$root/front.php", '<?php echo "ok";');
file_put_contents("$root/worker.php", <<<'PHP'
<?php
$notify = fpmng_worker_notify_stream();
$watcher = fpmng_worker_event_create(FPMNG_WORKER_READ, $notify, function () use ($notify): void {
    fread($notify, 65536);
    while (($id = fpmng_worker_next_request()) !== null) {
        fpmng_worker_respond($id, 200, ['Content-Type' => 'text/plain'], 'ok');
    }
});
fpmng_worker_event_enable($watcher);
while (!fpmng_worker_may_exit()) {
    fpmng_worker_loop(true);
}
PHP);

function childPid(int $master): int
{
    foreach (glob('/proc/[0-9]*/stat') as $stat) {
        $raw = @file_get_contents($stat);
        if ($raw !== false && preg_match('/^(\d+) \((.*)\) \S (\d+) /s', $raw, $m) && (int) $m[3] === $master) {
            return (int) $m[1];
        }
    }
    return 0;
}

function cpuTicks(int $pid): int
{
    $raw = file_get_contents("/proc/$pid/stat");
    $f = explode(' ', substr($raw, strrpos($raw, ')') + 2));
    return (int) $f[11] + (int) $f[12]; // utime + stime (fields 14 and 15)
}

function fail(string $message): void
{
    throw new RuntimeException($message);
}

function runCase(string $executor, string $front, int $port): void
{
    global $root;

    $cfg = <<<EOT
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
process_control_timeout = 5
rlimit_files = 64
[direct]
pool.type = http-direct
pool.executor = $executor
listen = 127.0.0.1:$port
chdir = $root
pm = static
pm.max_children = 1
http.front_controller = $front
php_admin_value[max_execution_time] = 0
php_admin_value[display_errors] = 0
EOT;

    // Makes a use of freed memory crash instead of passing by luck.
    putenv('MALLOC_PERTURB_=165');
    $tester = new FPM\Tester($cfg, '<?php');
    $clients = [];
    try {
        $tester->start();
        $tester->expectLogStartNotices();

        $child = childPid($tester->getPid());
        if ($child === 0) {
            fail("$executor: child process not found");
        }
        if (!preg_match('/Max open files\s+(\d+)/', file_get_contents("/proc/$child/limits"), $m) || (int) $m[1] !== 64) {
            fail("$executor: child did not get rlimit_files = 64: " . ($m[1] ?? '?'));
        }

        for ($i = 0; $i < 200; $i++) {
            $fp = @fsockopen('127.0.0.1', $port, $errno, $errstr, 2);
            if (!$fp) {
                break;
            }
            $clients[] = $fp;
        }
        usleep(500000);
        $open = count(scandir("/proc/$child/fd")) - 2;
        if ($open < 55) {
            fail("$executor: the child did not run out of descriptors ($open open, " . count($clients) . " clients)");
        }
        echo "$executor exhausted: yes\n";

        $before = cpuTicks($child);
        sleep(2);
        $used = cpuTicks($child) - $before;
        // A spinning accept loop uses about 200 ticks in 2 s; a poll every 100 ms uses a few.
        if ($used > 40) {
            fail("$executor: the child used $used ticks of CPU in 2 s while out of descriptors (busy loop)");
        }
        echo "$executor idle-while-exhausted: ok\n";

        // Fails with a clear message when the error callback is missing.
        $tester->expectLogPattern('/WARNING: \[pool direct\] http: accept\(\) on the http-direct listener failed: .*; pausing accept for 100 ms/', true, 5);
        echo "$executor backoff-logged: ok\n";

        $tester->signal('USR1', $child);
        sleep(1);
        $tester->expectNoLogPattern('/exited on signal/', true, 1);
        echo "$executor retired-without-crash: ok\n";

        foreach ($clients as $fp) {
            fclose($fp);
        }
        $clients = [];

        $ok = '/^HTTP\/1\.1 200 .*\r\n\r\n(2\r\n)?ok/s';
        $body = '';
        for ($try = 0; $try < 20 && !preg_match($ok, $body); $try++) {
            usleep(200000);
            $fp = @fsockopen('127.0.0.1', $port, $errno, $errstr, 2);
            if (!$fp) {
                continue;
            }
            stream_set_timeout($fp, 3);
            fwrite($fp, "GET / HTTP/1.1\r\nHost: test\r\nConnection: close\r\n\r\n");
            $body = stream_get_contents($fp);
            fclose($fp);
        }
        if (!preg_match($ok, $body)) {
            fail("$executor: no answer after the descriptors were freed:\n$body");
        }
        echo "$executor recovered: ok\n";
    } finally {
        foreach ($clients as $fp) {
            fclose($fp);
        }
        $tester->terminate();
        $tester->expectLogTerminatingNotices();
        $tester->close();
    }
}

$base = (int) (getenv('FPMNG_DIRECT_TEST_PORT') ?: 28054 + 200 * (int) getenv('TEST_PHP_WORKER') + (int) getenv('FPMNG_PHPT_PORT_SHIFT'));
try {
    runCase('classic', '/front.php', $base + 90);
    runCase('worker', '/worker.php', $base + 91);
    echo "Done\n";
} finally {
    @unlink("$root/front.php");
    @unlink("$root/worker.php");
    @rmdir($root);
}
?>
--EXPECT--
classic exhausted: yes
classic idle-while-exhausted: ok
classic backoff-logged: ok
classic retired-without-crash: ok
classic recovered: ok
worker exhausted: yes
worker idle-while-exhausted: ok
worker backoff-logged: ok
worker retired-without-crash: ok
worker recovered: ok
Done
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>

--TEST--
fpm-ng: WATCHDOG=1 at half of WATCHDOG_USEC, from the master only, and not for another WATCHDOG_PID (issue #643)
--SKIPIF--
<?php include "fpmng-skipif.inc"; ?>
--FILE--
<?php

require_once "tester.inc";

/* Issue #643: systemd sets WATCHDOG_USEC for a unit with WatchdogSec=, and
 * the master then sends WATCHDOG=1 every half of it (fpm_sd_notify.c,
 * sd_watchdog_enabled(3)). The packaged unit does not set WatchdogSec=, so the
 * default is no pings at all; that default is covered by the lifecycle test,
 * which expects READY, RELOADING, READY, STOPPING and nothing else.
 *
 * Two runs, both with WATCHDOG_USEC=1000000 (one ping every 500 ms):
 *  A. WATCHDOG_PID names a process that is not this master. systemd uses
 *     WATCHDOG_PID for a process that is meant to ping, so this master must
 *     stay silent: no WATCHDOG=1 for two seconds after READY=1.
 *  B. WATCHDOG_PID is not set. The master sends WATCHDOG=1 at once, then READY=1,
 *     then at least three more pings within five seconds. */

$root = sys_get_temp_dir() . '/fpmng-sd-notify-watchdog-' . getmypid();
@mkdir($root, 0700, true);
$log = "$root/error.log";
$conf = "$root/fpm.conf";
$notify = "$root/notify.sock";

function fail(string $what): void
{
    throw new RuntimeException($what);
}

function pid_alive(int $pid): bool
{
    if ($pid <= 1 || !is_dir("/proc/$pid")) {
        return false;
    }
    $stat = @file_get_contents("/proc/$pid/stat");
    $close = $stat === false ? false : strrpos($stat, ')');
    return $close === false || ($stat[$close + 2] ?? '') !== 'Z';
}

/* One datagram from the master, or null when none arrived within $seconds. */
function next_datagram($srv, float $seconds): ?string
{
    $rd = [$srv];
    $wr = null;
    $ex = null;
    $sec = (int) $seconds;
    $usec = (int) (($seconds - $sec) * 1000000);
    if (stream_select($rd, $wr, $ex, $sec, $usec) !== 1) {
        return null;
    }
    $data = stream_socket_recvfrom($srv, 4096);
    return $data === false ? null : $data;
}

function start_master(string $conf, string $notify)
{
    putenv("NOTIFY_SOCKET=$notify");
    $proc = proc_open([FPM\Tester::findExecutable(), '-n', '-y', $conf, '-F'],
        [0 => ['file', '/dev/null', 'r'], 1 => ['file', '/dev/null', 'w'], 2 => ['file', '/dev/null', 'w']], $pipes);
    putenv('NOTIFY_SOCKET=');
    return $proc;
}

/* SIGQUIT, then the datagrams up to STOPPING=1. Pings may still arrive before
 * it: the watchdog keeps running while the master drains its children. Any
 * other datagram is a failure. The master's exit is awaited, and the pings
 * that were queued before it are drained the same way. */
function stop_master($srv, int $pid, string $log): void
{
    exec("kill -QUIT $pid");
    do {
        $m = next_datagram($srv, 30);
        if ($m === null) {
            fail("no STOPPING=1 after SIGQUIT\n" . (string) @file_get_contents($log));
        }
    } while ($m === "WATCHDOG=1\n");
    if ($m !== "STOPPING=1\n") {
        fail('unexpected datagram at SIGQUIT: ' . var_export($m, true));
    }

    $deadline = time() + 60;
    while (time() < $deadline && pid_alive($pid)) {
        usleep(100000);
    }
    if (pid_alive($pid)) {
        fail("master $pid is still running after STOPPING=1");
    }
    while (($m = next_datagram($srv, 1)) !== null) {
        if ($m !== "WATCHDOG=1\n") {
            fail('unexpected datagram after STOPPING=1: ' . var_export($m, true));
        }
    }
}

$tester = new FPM\Tester('[global]', '<?php');
$addr = $tester->getAddr('ipv4', '[a]');

if (FPM\Tester::findExecutable() === false) {
    fail('cannot find the php-fpm-ng binary to test');
}

file_put_contents($conf, <<<CFG
[global]
error_log = $log
pid = $root/fpm.pid
log_level = notice
[a]
listen = $addr
pool.type = http-direct
pm = static
pm.max_children = 1
chdir = $root
http.front_controller = /app.php
CFG);
file_put_contents("$root/app.php", "<?php echo 'ok';");

$srv = @stream_socket_server("udg://$notify", $errno, $errstr, STREAM_SERVER_BIND);
if ($srv === false) {
    fail("cannot bind the fake NOTIFY_SOCKET: $errstr");
}

putenv('WATCHDOG_USEC=1000000');
$proc = null;
try {
    /* Run A. */
    putenv('WATCHDOG_PID=1');
    $proc = start_master($conf, $notify);
    if (!is_resource($proc)) {
        fail('cannot start the master (run A)');
    }
    $pid = (int) proc_get_status($proc)['pid'];

    $m = next_datagram($srv, 30);
    if ($m !== "READY=1\n") {
        fail('run A: the first datagram is not READY=1: ' . var_export($m, true)
            . "\n" . (string) @file_get_contents($log));
    }
    $m = next_datagram($srv, 2);
    if ($m !== null) {
        fail('run A: a datagram arrived although WATCHDOG_PID names another process: ' . var_export($m, true));
    }
    stop_master($srv, $pid, $log);
    proc_close($proc);
    $proc = null;
    echo "WATCHDOG_PID names another process: no WATCHDOG=1\n";

    /* Run B. */
    putenv('WATCHDOG_PID=');
    $proc = start_master($conf, $notify);
    if (!is_resource($proc)) {
        fail('cannot start the master (run B)');
    }
    $pid = (int) proc_get_status($proc)['pid'];

    $m = next_datagram($srv, 30);
    if ($m !== "WATCHDOG=1\n") {
        fail('run B: the first datagram is not the immediate WATCHDOG=1: ' . var_export($m, true));
    }
    $m = next_datagram($srv, 30);
    if ($m !== "READY=1\n") {
        fail('run B: the second datagram is not READY=1: ' . var_export($m, true)
            . "\n" . (string) @file_get_contents($log));
    }
    $pings = 0;
    while ($pings < 3) {
        $m = next_datagram($srv, 5);
        if ($m !== "WATCHDOG=1\n") {
            fail("run B: fewer than 3 WATCHDOG=1 within 5 s after READY=1 (got " . var_export($m, true) . ")");
        }
        $pings++;
    }
    echo "WATCHDOG=1 at a period of half WATCHDOG_USEC\n";

    stop_master($srv, $pid, $log);
    proc_close($proc);
    $proc = null;

    echo "Done\n";
} finally {
    putenv('WATCHDOG_USEC=');
    putenv('WATCHDOG_PID=');
    if (is_resource($proc)) {
        $status = proc_get_status($proc);
        if ($status['running']) {
            exec("kill -QUIT {$status['pid']} 2>/dev/null");
        }
        proc_close($proc);
    }
    fclose($srv);
    foreach (glob("$root/*") ?: [] as $f) {
        @unlink($f);
    }
    @rmdir($root);
}

?>
--EXPECTF--
WATCHDOG_PID names another process: no WATCHDOG=1
WATCHDOG=1 at a period of half WATCHDOG_USEC
Done
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>

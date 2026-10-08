--TEST--
fpm-ng: a NOTIFY_SOCKET nobody listens on never stops the master, a reload or the pools (issue #643)
--SKIPIF--
<?php include "fpmng-skipif.inc"; ?>
--FILE--
<?php

require_once "tester.inc";

/* Issue #643: a notification is best effort. systemd can be absent, restarting
 * or slow to read, and the master must not care: the datagram that cannot be
 * sent is logged once per master process and the service carries on
 * (fpm_sd_notify.c, fpm_sd_notify_failed()).
 *
 * NOTIFY_SOCKET names a path with no socket behind it, so every sendto() fails
 * with ENOENT. The claims:
 *  1. The master starts and its pools accept connections.
 *  2. A SIGUSR2 reload still replaces the generation and its pools listen.
 *  3. The WARNING appears once per master process: once for the first
 *     generation and once for the one a reload execs, never once per message.
 *     READY=1 is the first failure of each generation, so the RELOADING=1 and
 *     STOPPING=1 failures of the same generation are not logged. */

$root = sys_get_temp_dir() . '/fpmng-sd-notify-unreachable-' . getmypid();
@mkdir($root, 0700, true);
$log = "$root/error.log";
$conf = "$root/fpm.conf";
$notify = "$root/nobody-listens.sock";

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

function listening(string $address): bool
{
    $fp = @stream_socket_client("tcp://$address", $errno, $error, 2);
    if ($fp === false) {
        return false;
    }
    fclose($fp);
    return true;
}

/* Waits until the log holds $count lines of $needle, then checks that the
 * pool accepts a connection. "ready to handle connections" is logged by the
 * event loop of each generation, so it counts generations. */
function waitGeneration(string $log, string $address, int $count): void
{
    $deadline = time() + 60;
    while (time() < $deadline) {
        $text = (string) @file_get_contents($log);
        if (substr_count($text, 'ready to handle connections') >= $count && listening($address)) {
            return;
        }
        usleep(100000);
    }
    fail("generation $count did not come up\n" . (string) @file_get_contents($log));
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

$proc = null;
$masterPid = 0;
try {
    putenv("NOTIFY_SOCKET=$notify");
    $proc = proc_open([FPM\Tester::findExecutable(), '-n', '-y', $conf, '-F'],
        [0 => ['file', '/dev/null', 'r'], 1 => ['file', '/dev/null', 'w'], 2 => ['file', '/dev/null', 'w']], $pipes);
    putenv('NOTIFY_SOCKET=');
    if (!is_resource($proc)) {
        fail('cannot start the master');
    }
    $masterPid = (int) proc_get_status($proc)['pid'];

    waitGeneration($log, $addr, 1);
    echo "pool serves with an unreachable NOTIFY_SOCKET: ok\n";

    exec("kill -USR2 $masterPid");
    waitGeneration($log, $addr, 2);
    if (!pid_alive($masterPid)) {
        fail("master $masterPid is gone after a reload with an unreachable NOTIFY_SOCKET");
    }
    echo "reload with an unreachable NOTIFY_SOCKET: ok\n";

    exec("kill -QUIT $masterPid");
    $deadline = time() + 60;
    while (time() < $deadline && pid_alive($masterPid)) {
        usleep(100000);
    }
    if (pid_alive($masterPid)) {
        fail("master $masterPid is still running after SIGQUIT");
    }

    $warnings = substr_count((string) @file_get_contents($log), 'sd_notify: cannot send to NOTIFY_SOCKET');
    if ($warnings !== 2) {
        fail("expected one WARNING per master process (2), got $warnings\n" . (string) @file_get_contents($log));
    }
    echo "one WARNING per master process\n";

    echo "Done\n";
} finally {
    if ($masterPid > 1 && pid_alive($masterPid)) {
        exec("kill -QUIT $masterPid 2>/dev/null");
    }
    usleep(200000);
    if (is_resource($proc)) {
        proc_close($proc);
    }
    foreach (glob("$root/*") ?: [] as $f) {
        @unlink($f);
    }
    @rmdir($root);
}

?>
--EXPECTF--
pool serves with an unreachable NOTIFY_SOCKET: ok
reload with an unreachable NOTIFY_SOCKET: ok
one WARNING per master process
Done
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>

--TEST--
fpm-ng: NOTIFY_SOCKET gets READY after the pools listen, RELOADING around a reload, STOPPING at stop (issue #643)
--SKIPIF--
<?php include "fpmng-skipif.inc"; ?>
--FILE--
<?php

require_once "tester.inc";

/* Issue #643: a systemd Type=notify unit. The master reports its state
 * through NOTIFY_SOCKET (fpm_sd_notify.c), and this test is the service
 * manager: it binds the unix datagram socket the master is started with and
 * reads what arrives. The socket is a `udg://` stream, which PHP reads with
 * stream_socket_recvfrom(); each read is one datagram.
 *
 * The claims, each with its own check:
 *  1. READY=1 arrives once every pool has bound its listening socket. When the
 *     test reads it, a TCP connect to each pool succeeds. This proves the
 *     sockets are listening. It does NOT prove that a request is answered,
 *     which is why docs/systemd.md says READY means "bound", not "serving".
 *  2. A reload the configuration check refuses sends nothing: RELOADING=1 is
 *     sent only after the check passed (fpm_pctl(), fpm_process_ctl.c).
 *  3. A reload that passes sends RELOADING=1 with MONOTONIC_USEC, and the new
 *     generation sends READY=1 once its pools listen again. The new master is
 *     an execvp() of the old one, so NOTIFY_SOCKET must survive the exec.
 *  4. SIGQUIT (the packaged unit's KillSignal) sends STOPPING=1, and the
 *     master exits.
 * The whole sequence is exactly READY, RELOADING, READY, STOPPING. */

$root = sys_get_temp_dir() . '/fpmng-sd-notify-lifecycle-' . getmypid();
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

function listening(string $address): bool
{
    $fp = @stream_socket_client("tcp://$address", $errno, $error, 2);
    if ($fp === false) {
        return false;
    }
    fclose($fp);
    return true;
}

function writeConfig(string $conf, string $root, string $log, string $a, string $b, string $note, int $maxB): void
{
    file_put_contents($conf, <<<CFG
; $note
[global]
error_log = $log
pid = $root/fpm.pid
log_level = notice
[a]
listen = $a
pool.type = http-direct
pm = static
pm.max_children = 1
chdir = $root
http.front_controller = /app.php
[b]
listen = $b
pool.type = http-direct
pm = static
pm.max_children = $maxB
chdir = $root
http.front_controller = /app.php
CFG);
}

$tester = new FPM\Tester('[global]', '<?php');
$addrA = $tester->getAddr('ipv4', '[a]');
$addrB = $tester->getAddr('ipv4', '[b]');

if (FPM\Tester::findExecutable() === false) {
    fail('cannot find the php-fpm-ng binary to test');
}

file_put_contents("$root/app.php", <<<'PHP'
<?php
echo 'ok';
PHP);

$srv = @stream_socket_server("udg://$notify", $errno, $errstr, STREAM_SERVER_BIND);
if ($srv === false) {
    fail("cannot bind the fake NOTIFY_SOCKET: $errstr");
}

$proc = null;
$masterPid = 0;
try {
    writeConfig($conf, $root, $log, $addrA, $addrB, 'before the reload', 1);

    putenv("NOTIFY_SOCKET=$notify");
    $proc = proc_open([FPM\Tester::findExecutable(), '-n', '-y', $conf, '-F'],
        [0 => ['file', '/dev/null', 'r'], 1 => ['file', '/dev/null', 'w'], 2 => ['file', '/dev/null', 'w']], $pipes);
    putenv('NOTIFY_SOCKET=');
    if (!is_resource($proc)) {
        fail('cannot start the master');
    }
    $masterPid = (int) proc_get_status($proc)['pid'];

    /* 1. READY, and both pools accept a connection at the moment it is read. */
    $first = next_datagram($srv, 30);
    if ($first !== "READY=1\n") {
        fail('the first datagram is not READY=1: ' . var_export($first, true)
            . "\n" . (string) @file_get_contents($log));
    }
    if (!listening($addrA) || !listening($addrB)) {
        fail("READY=1 was sent before both pools listened\n" . (string) @file_get_contents($log));
    }
    echo "READY=1 after both pools listen\n";

    /* 2. A refused reload sends nothing. pm.max_children = 0 is refused by
     * the configuration check, and the running generation keeps serving. */
    writeConfig($conf, $root, $log, $addrA, $addrB, 'broken on purpose', 0);
    exec("kill -USR2 $masterPid");
    $deadline = time() + 30;
    while (time() < $deadline && !str_contains((string) @file_get_contents($log), 'reload refused')) {
        usleep(100000);
    }
    if (!str_contains((string) @file_get_contents($log), 'reload refused')) {
        fail("the broken configuration was not refused\n" . (string) @file_get_contents($log));
    }
    $unexpected = next_datagram($srv, 2);
    if ($unexpected !== null) {
        fail('a refused reload sent a datagram: ' . var_export($unexpected, true));
    }
    echo "refused reload sent nothing\n";

    /* 3. A valid reload: RELOADING=1 with a monotonic time, then READY=1 from
     * the new generation, once its pools listen again. */
    writeConfig($conf, $root, $log, $addrA, $addrB, 'after the edit', 1);
    exec("kill -USR2 $masterPid");
    $reloading = next_datagram($srv, 30);
    if ($reloading === null || preg_match('/\ARELOADING=1\nMONOTONIC_USEC=\d+\n\z/', $reloading) !== 1) {
        fail('no RELOADING=1 with MONOTONIC_USEC after a valid reload: ' . var_export($reloading, true)
            . "\n" . (string) @file_get_contents($log));
    }
    $ready = next_datagram($srv, 60);
    if ($ready !== "READY=1\n") {
        fail('no READY=1 after the reload: ' . var_export($ready, true)
            . "\n" . (string) @file_get_contents($log));
    }
    if (!listening($addrA) || !listening($addrB)) {
        fail("READY=1 after the reload was sent before both pools listened\n" . (string) @file_get_contents($log));
    }
    echo "reload: RELOADING=1, then READY=1 after both pools listen\n";

    /* 4. SIGQUIT: STOPPING=1, then the master exits and sends nothing more. */
    exec("kill -QUIT $masterPid");
    $stopping = next_datagram($srv, 30);
    if ($stopping !== "STOPPING=1\n") {
        fail('no STOPPING=1 at SIGQUIT: ' . var_export($stopping, true)
            . "\n" . (string) @file_get_contents($log));
    }
    $deadline = time() + 60;
    while (time() < $deadline && pid_alive($masterPid)) {
        usleep(100000);
    }
    if (pid_alive($masterPid)) {
        fail("master $masterPid is still running after STOPPING=1");
    }
    $after = next_datagram($srv, 1);
    if ($after !== null) {
        fail('a datagram arrived after STOPPING=1: ' . var_export($after, true));
    }
    echo "stop: STOPPING=1, then the master exits\n";

    echo "Done\n";
} finally {
    if ($masterPid > 1 && pid_alive($masterPid)) {
        exec("kill -QUIT $masterPid 2>/dev/null");
    }
    usleep(200000);
    if (is_resource($proc)) {
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
READY=1 after both pools listen
refused reload sent nothing
reload: RELOADING=1, then READY=1 after both pools listen
stop: STOPPING=1, then the master exits
Done
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>

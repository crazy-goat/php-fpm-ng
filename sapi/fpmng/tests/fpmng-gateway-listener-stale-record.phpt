--TEST--
fpm-ng: a FPMNG_HTTP_LISTENERS record that names a descriptor which is not a listening socket does not close it (issue #661)
--SKIPIF--
<?php
include "fpmng-skipif.inc";
if (!is_executable('/bin/bash')) {
    die("skip /bin/bash is needed to pass a descriptor to the master");
}
?>
--FILE--
<?php

require_once "tester.inc";

/* Issue #661: FPMNG_HTTP_LISTENERS is read from the environment, so a stale or
 * foreign value can name any open descriptor of the new master. The master
 * takes a descriptor over, or closes it, only when it is a listening TCP socket.
 * Here descriptor 7 is a regular file, which is not a socket, and the record says
 * it is the listener of an address that no gateway uses. The master must keep the
 * descriptor open and start normally. */

$root = sys_get_temp_dir() . '/fpmng-gw-listener-stale-' . getmypid();
@mkdir($root, 0700, true);
$log = "$root/error.log";
$pidFile = "$root/worker.pid";
$marker = "$root/marker";
file_put_contents($marker, '');
$marker = realpath($marker);
file_put_contents("$root/front.php", <<<'PHP'
<?php
file_put_contents(getenv('FPMNG_WORKER_PIDFILE'), (string) getmypid());
echo 'ok';
PHP);

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

function wait_gone(int $pid, int $seconds): bool
{
    $deadline = microtime(true) + $seconds;
    do {
        if (!pid_alive($pid)) {
            return true;
        }
        usleep(50000);
    } while (microtime(true) < $deadline);
    return false;
}

function request(string $address): bool
{
    $fp = @stream_socket_client("tcp://$address", $errno, $error, 2);
    if (!$fp) {
        return false;
    }
    stream_set_timeout($fp, 10);
    fwrite($fp, "GET /app HTTP/1.1\r\nHost: test\r\nConnection: close\r\n\r\n");
    $body = (string) stream_get_contents($fp);
    fclose($fp);
    return str_contains($body, 'ok');
}

$tester = new FPM\Tester('[global]', '<?php');
$keep = $tester->getAddr('ipv4', '[keep]');
$binary = FPM\Tester::findExecutable();
if ($binary === false) {
    fail('cannot find the php-fpm-ng binary');
}
file_put_contents("$root/fpm.conf", <<<CFG
[global]
error_log = $log
pid = $root/fpm.pid
log_level = notice
[keep]
listen = $keep
pool.type = http-direct
pm = static
pm.max_children = 1
chdir = $root
http.front_controller = /front.php
env[FPMNG_WORKER_PIDFILE] = $pidFile
CFG);

$proc = null;
$masterPid = 0;
$workerPid = 0;
try {
    /* The shell opens the marker file as descriptor 7 and passes it on; the master
     * inherits it, because the shell does not set close-on-exec. */
    $proc = proc_open(['/bin/bash', '-c',
            'exec 7<' . escapeshellarg($marker) . '; export FPMNG_HTTP_LISTENERS="7:0123456789abcdef:127.0.0.1:9"; exec "$0" "$@"',
            $binary, '-n', '-y', "$root/fpm.conf", '-F'],
        [0 => ['file', '/dev/null', 'r'], 1 => ['file', '/dev/null', 'w'], 2 => ['file', '/dev/null', 'w']], $pipes);
    if (!is_resource($proc)) {
        fail('cannot start the master');
    }
    $masterPid = (int) proc_get_status($proc)['pid'];

    $deadline = time() + 20;
    while (!request($keep)) {
        if (time() > $deadline) {
            fail("the pool never answered\n" . (string) @file_get_contents($log));
        }
        usleep(100000);
    }
    echo "service answers: ok\n";
    $workerPid = (int) @file_get_contents($pidFile);

    /* A closed descriptor 7 can be reused by a later open(), so the check is
     * that the master still holds the marker file there, not only an open one. */
    if (@readlink("/proc/$masterPid/fd/7") !== $marker) {
        fail("the master does not hold the marker file on descriptor 7\n" . (string) @file_get_contents($log));
    }
    echo "descriptor 7 of the master is still the marker file: ok\n";

    echo "Done\n";
} finally {
    if (pid_alive($masterPid)) {
        exec("kill -TERM $masterPid 2>/dev/null");
        if (!wait_gone($masterPid, 20)) {
            exec("kill -9 $masterPid 2>/dev/null");
        }
    }
    if (pid_alive($workerPid)) {
        exec("kill -9 $workerPid 2>/dev/null");
    }
    if (is_resource($proc)) {
        proc_close($proc);
    }
    foreach (glob("$root/*") ?: [] as $f) {
        @unlink($f);
    }
    @rmdir($root);
}

?>
--EXPECT--
service answers: ok
descriptor 7 of the master is still the marker file: ok
Done
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>

--TEST--
fpm-ng: a FPMNG_HTTP_LISTENERS record whose descriptor is a listening socket bound to another address is not adopted (issue #927)
--SKIPIF--
<?php
include "fpmng-skipif.inc";
fpmng_skip_if_pool_type_unsupported('gateway');
if (!is_executable('/bin/bash')) {
    die("skip /bin/bash is needed to pass a descriptor to the master");
}
?>
--FILE--
<?php

require_once "tester.inc";

/* Issue #927: the record names a descriptor number, and the master checked only
 * that the descriptor is a listening TCP socket. Here descriptor 7 is a listening
 * socket on 127.0.0.1 at a port of its own, which is some other socket. The record
 * names the address of the gateway, and its TLS fingerprint matches the gateway's
 * (no TLS in either). Before the fix the gateway adopted descriptor 7, so it
 * listened on the wrong address and the gateway address refused connections. The
 * master must reject the record, leave descriptor 7 open, and bind the gateway
 * address itself. */

$root = sys_get_temp_dir() . '/fpmng-gw-listener-foreign-' . getmypid();
@mkdir($root, 0700, true);
$log = "$root/error.log";
file_put_contents("$root/app.php", "<?php\nheader('Content-Length: 2');\necho 'ok';\n");

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

function gateway_ok(string $addr): bool
{
    $fp = @stream_socket_client("tcp://$addr", $errno, $error, 2);
    if (!$fp) {
        return false;
    }
    stream_set_timeout($fp, 5);
    fwrite($fp, "GET /app.php HTTP/1.1\r\nHost: test\r\nConnection: close\r\n\r\n");
    $body = (string) stream_get_contents($fp);
    fclose($fp);
    return str_starts_with($body, 'HTTP/1.1 200') && str_ends_with($body, 'ok');
}

/* The fingerprint that fpm_http_handoff_tls_fp() in fpm_http_handoff.c writes for a
 * gateway without TLS: FNV-1a 64-bit over six empty http.tls_* values, each followed
 * by 0x1f, then the http.tls_wait_for_cert flag ('0'). PHP integers are signed and
 * have no 64-bit unsigned multiply, so the hash is kept in two 32-bit halves. */
function tls_fingerprint(): string
{
    $hi = 0xcbf29ce4;
    $lo = 0x84222325;
    $mix = static function (int $byte) use (&$hi, &$lo): void {
        $lo ^= $byte;
        /* h * (2^40 + 0x1b3) mod 2^64. The 2^40 term moves the low 24 bits of lo to bit 8 of hi. */
        $prod = $lo * 0x1b3;
        $newHi = ($hi * 0x1b3 + ($prod >> 32) + (($lo & 0xffffff) << 8)) & 0xffffffff;
        $lo = $prod & 0xffffffff;
        $hi = $newHi;
    };
    for ($field = 0; $field < 6; $field++) {
        $mix(0x1f);
    }
    $mix(ord('0'));
    return sprintf('%08x%08x', $hi, $lo);
}

$tester = new FPM\Tester('[global]', '<?php');
$gateway = $tester->getAddr('ipv4', '[gw]');
$app = $tester->getAddr('ipv4', '[app]');
$binary = FPM\Tester::findExecutable();
if ($binary === false) {
    fail('cannot find the php-fpm-ng binary');
}
file_put_contents("$root/fpm.conf", <<<CFG
[global]
error_log = $log
pid = $root/fpm.pid
log_level = notice
[gw]
pool.type = gateway
listen = $gateway
chdir = $root
http.gateways = 1
http.route[app] = /
[app]
pool.type = fastcgi
listen = $app
chdir = $root
pm = static
pm.max_children = 1
CFG);

/* The foreign socket is passed to the master as descriptor 7. It listens on its own
 * port, which is not the gateway's address. */
$foreign = stream_socket_server('tcp://127.0.0.1:0', $errno, $error);
if ($foreign === false) {
    fail("cannot listen for the foreign socket: $error");
}

$proc = null;
$masterPid = 0;
$workerPid = 0;
try {
    $record = '7:' . tls_fingerprint() . ':' . $gateway;
    $proc = proc_open(['/bin/bash', '-c',
            'export FPMNG_HTTP_LISTENERS=' . escapeshellarg($record) . '; exec "$0" "$@"',
            $binary, '-n', '-y', "$root/fpm.conf", '-F'],
        [0 => ['file', '/dev/null', 'r'], 1 => ['file', '/dev/null', 'w'], 2 => ['file', '/dev/null', 'w'], 7 => $foreign], $pipes);
    if (!is_resource($proc)) {
        fail('cannot start the master');
    }
    $masterPid = (int) proc_get_status($proc)['pid'];

    $deadline = time() + 20;
    while (!gateway_ok($gateway)) {
        if (time() > $deadline) {
            fail("the gateway does not answer on its own address\n" . (string) @file_get_contents($log));
        }
        usleep(100000);
    }
    echo "the gateway answers on its own address: ok\n";

    if (!str_starts_with((string) @readlink("/proc/$masterPid/fd/7"), 'socket:')) {
        fail("the master does not hold the foreign socket on descriptor 7\n" . (string) @file_get_contents($log));
    }
    echo "descriptor 7 of the master is still a socket: ok\n";

    echo "Done\n";
} finally {
    if (pid_alive($masterPid)) {
        exec("kill -TERM $masterPid 2>/dev/null");
        if (!wait_gone($masterPid, 20)) {
            exec("kill -9 $masterPid 2>/dev/null");
        }
    }
    if (is_resource($proc)) {
        proc_close($proc);
    }
    fclose($foreign);
    foreach (glob("$root/*") ?: [] as $f) {
        @unlink($f);
    }
    @rmdir($root);
}

?>
--EXPECT--
the gateway answers on its own address: ok
descriptor 7 of the master is still a socket: ok
Done
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>

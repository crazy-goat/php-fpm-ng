--TEST--
fpm-ng: a FPMNG_HTTP_LISTENERS record with a port-only bind text takes over its listening socket (issue #927)
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

/* Issue #927: fpm_http_split_bind() must split a port-only bind text the way
 * fpm_http_listen() does, host NULL and the whole text as the port. A gateway with
 * `listen = <port>` binds the wildcard address on that port. Here descriptor 7 is
 * the listening socket of that wildcard address, as a previous generation would
 * leave it, and the record names the port alone. The master must take it over and
 * answer on the port. Before the matcher used the shared split, the record was
 * rejected as "not bound to that address", the socket stayed open, and the
 * master's own bind failed with EADDRINUSE. */

$root = sys_get_temp_dir() . '/fpmng-gw-listener-port-only-' . getmypid();
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
$port = substr($gateway, strrpos($gateway, ':') + 1);
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
listen = $port
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

/* The previous generation's listener: the wildcard address on the gateway port. */
$previous = stream_socket_server("tcp://0.0.0.0:$port", $errno, $error);
if ($previous === false) {
    fail("cannot listen for the previous generation: $error");
}

$proc = null;
$masterPid = 0;
try {
    $record = '7:' . tls_fingerprint() . ':' . $port;
    $proc = proc_open(['/bin/bash', '-c',
            'export FPMNG_HTTP_LISTENERS=' . escapeshellarg($record) . '; exec "$0" "$@"',
            $binary, '-n', '-y', "$root/fpm.conf", '-F'],
        [0 => ['file', '/dev/null', 'r'], 1 => ['file', '/dev/null', 'w'], 2 => ['file', '/dev/null', 'w'], 7 => $previous], $pipes);
    if (!is_resource($proc)) {
        fail('cannot start the master');
    }
    $masterPid = (int) proc_get_status($proc)['pid'];

    $deadline = time() + 20;
    while (!gateway_ok($gateway)) {
        if (time() > $deadline) {
            fail("the gateway does not answer on its port\n" . (string) @file_get_contents($log));
        }
        usleep(100000);
    }
    echo "the gateway answers on its port: ok\n";

    if (!str_contains((string) @file_get_contents($log), "took over the listener on $port")) {
        fail("the master did not take over the listener on $port\n" . (string) @file_get_contents($log));
    }
    echo "the master took over the listener: ok\n";

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
    fclose($previous);
    foreach (glob("$root/*") ?: [] as $f) {
        @unlink($f);
    }
    @rmdir($root);
}

?>
--EXPECT--
the gateway answers on its port: ok
the master took over the listener: ok
Done
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>

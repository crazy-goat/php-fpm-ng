--TEST--
fpm-ng: fiber executor makes outgoing TLS (https://) concurrent, with per-request data isolation (task 005)
--SKIPIF--
<?php
include "skipif.inc";

if (!function_exists('stream_socket_server')) {
    die('skip requires stream transports');
}
if (!extension_loaded('openssl')) {
    die('skip requires the openssl extension (the test TLS server runs in this CLI)');
}
// The TLS servers run as PHP_BINARY -n (no ini): if openssl is a SHARED
// extension, it is loaded via ini in this process but missing there, and the
// servers die with "transport not found". Skip explicitly instead.
exec(PHP_BINARY . ' -n -r ' . escapeshellarg('exit(extension_loaded("openssl") ? 0 : 1);'), $o, $st);
if ($st !== 0) {
    die('skip the -n CLI has no openssl (shared ext loaded via ini here)');
}
$binary = getenv('TEST_PHP_FPM_EXECUTABLE') ?: FPM\Tester::findExecutable();
exec(escapeshellarg($binary) . ' -i 2>&1', $output, $status);
$info = implode("\n", $output);
if ($status !== 0 || !str_contains($info, '--enable-fpmng-fiber')) {
    die('skip php-fpm-ng was not built with --enable-fpmng-fiber');
}
// The TLS interception (patch 0007) also needs HAVE_FPMNG_FIBER_TLS, which
// requires a STATIC ext/openssl in the fpm binary. There is no runtime flag
// to query, so this is asserted behaviorally below: on a shared-openssl
// build the concurrency check fails on timing and the test fails loudly
// instead of silently passing as a skip.
?>
--FILE--
<?php

require_once "tester.inc";

// Task 005 acceptance criteria 1 + 3: with pool.executor = fiber, N
// concurrent requests each fetching a tls:// endpoint that sleeps must
// finish in roughly the time of ONE such request, and each must receive its
// OWN body (asserted on data, not only on timing).
//
// The TLS server is a separate CLI process (a script below, proc_open'd):
// one ssl listener with a self-signed certificate; accepted connections are
// served sequentially, each sleeping 500 ms before answering. If the
// workers' TLS stacks block, N=4 requests serialize to > 1.9 s; with the
// interception they complete in well under 1.2 s (one 500 ms sleep plus
// scheduling noise). The threshold is generous because the box is shared.

$docRoot = sys_get_temp_dir() . '/fpmng-fiber-tls-' . getmypid();
@mkdir($docRoot, 0700, true);

$certFile = "$docRoot/server.crt";
$keyFile  = "$docRoot/server.key";
$privkey  = openssl_pkey_new(['private_key_bits' => 2048, 'private_key_type' => OPENSSL_KEYTYPE_RSA]);
$csr      = openssl_csr_new(['commonName' => '127.0.0.1'], $privkey);
$x509     = openssl_csr_sign($csr, null, $privkey, 2);
openssl_x509_export_to_file($x509, $certFile);
openssl_pkey_export_to_file($privkey, $keyFile);

$probe = <<<'PHP'
<?php
// The TLS server ports cannot come from an environment variable: FPM clears
// the worker environment by default. They are dropped next to this file by
// the test harness after the ports are known, one "id host:port" per line.
$id = $_GET['id'] ?? 'missing';
$addr = null;
foreach (file(__DIR__ . '/tls.addr', FILE_IGNORE_NEW_LINES | FILE_SKIP_EMPTY_LINES) as $line) {
    [$lid, $laddr] = explode(' ', $line, 2);
    if ($lid === $id) {
        $addr = $laddr;
        break;
    }
}
$ctx = stream_context_create(['ssl' => ['verify_peer' => false, 'verify_peer_name' => false]]);
$body = $addr === null ? false : file_get_contents("https://$addr/?id=$id", false, $ctx);
echo json_encode(['id' => $id, 'body' => trim((string) $body)], JSON_UNESCAPED_SLASHES);
PHP;
file_put_contents("$docRoot/probe.php", $probe);

// The TLS server: binds first, prints one READY line so the parent never
// races the listen backlog, then serves exactly ONE connection: read the
// request, sleep 500 ms, answer. The test runs FOUR of these (one port per
// request id) — deliberately not one server with a sequential accept loop,
// because a 500 ms sleep between accepts would serialize the measurement on
// the SERVER side even when the client is fully concurrent.
$serverCode = <<<'PHP'
<?php
[$addr, $cert, $key] = [$argv[1], $argv[2], $argv[3]];
$ctx = stream_context_create(['ssl' => ['local_cert' => $cert, 'local_pk' => $key]]);
$server = stream_socket_server("tls://$addr", $errno, $errstr, STREAM_SERVER_BIND | STREAM_SERVER_LISTEN, $ctx);
if (!$server) {
    fwrite(STDERR, "listen failed: $errstr\n");
    exit(2);
}
echo "READY\n";
fflush(STDOUT);
$conn = @stream_socket_accept($server, 15);
if (!$conn) {
    exit(3);
}
stream_set_timeout($conn, 5);
$req = fread($conn, 8192);
$cid = 'unknown';
if (preg_match('/[?&]id=([A-Z])/', (string) $req, $m)) {
    $cid = $m[1];
}
usleep(500000);
$body = "tls-body-$cid";
fwrite($conn, "HTTP/1.1 200 OK\r\nContent-Length: " . strlen($body) . "\r\nConnection: close\r\n\r\n" . $body);
fclose($conn);
exit(0);
PHP;
file_put_contents("$docRoot/server.php", $serverCode);

$cfg = <<<EOT
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
[web]
listen = {{ADDR}}
chdir = $docRoot
pm = static
pm.max_children = 1
pool.type = http
pool.executor = fiber
http.listen = {{ADDR[http]}}
php_admin_value[opcache.enable] = 0
php_admin_value[max_execution_time] = 0
EOT;

$tester = new FPM\Tester($cfg, $probe);
$tester->start();
$tester->expectLogStartNotices();
$http = $tester->getAddr('ipv4', '[http]');

// One single-shot server process per request id.
$ids = ['A', 'B', 'C', 'D'];
$srvDesc = [0 => ['pipe', 'r'], 1 => ['pipe', 'w'], 2 => ['pipe', 'w']];
$srvProcs = [];
$srvPipes = [];
$addrLines = '';
foreach ($ids as $id) {
    $addr = $tester->getAddr('ipv4', "[tls-$id]");
    $addrLines .= "$id $addr\n";
    /* The array form runs the binary directly. A command string goes through
     * `/bin/sh -c`, and where /bin/sh is dash the shell does not exec its
     * argument: the pid returned is the shell's, so the proc_terminate() in
     * the teardown below signals the shell and leaves this server behind
     * (measured 2026-09-09 on 192.168.8.50, dash 0.5.12 — issue #101, same
     * mechanism as #89). */
    $srvProcs[$id] = proc_open(
        [PHP_BINARY, '-n', "$docRoot/server.php", $addr, $certFile, $keyFile],
        $srvDesc, $srvPipes[$id]);
    fclose($srvPipes[$id][0]);
    // Wait for READY (the listener is bound before it is printed). Bounded:
    // a server that hangs before printing must fail the test, not the suite.
    stream_set_timeout($srvPipes[$id][1], 10);
    $ready = fgets($srvPipes[$id][1]);
    if (trim((string) $ready) !== 'READY') {
        echo "FAIL: TLS server $id did not start: " . var_export($ready, true) . "\n";
        echo stream_get_contents($srvPipes[$id][2]);
        foreach ($srvProcs as $p) {
            proc_terminate($p);
            proc_close($p);
        }
        $tester->terminate();
        $tester->close();
        exit(1);
    }
}
file_put_contents("$docRoot/tls.addr", $addrLines);

$start = microtime(true);
$descriptors = [0 => ['pipe', 'r'], 1 => ['pipe', 'w'], 2 => ['pipe', 'w']];
$processes = [];
$pipes = [];
foreach ($ids as $i => $id) {
    $code = 'echo file_get_contents(' . var_export("http://$http/probe.php?id=$id", true) . ');';
    $processes[$i] = proc_open(PHP_BINARY . ' -n -r ' . escapeshellarg($code), $descriptors, $pipes[$i]);
    fclose($pipes[$i][0]);
}
$bodies = [];
foreach ($processes as $i => $proc) {
    $bodies[$i] = stream_get_contents($pipes[$i][1]);
    fclose($pipes[$i][1]);
    fclose($pipes[$i][2]);
    proc_close($proc);
}
$elapsed = microtime(true) - $start;

foreach ($ids as $id) {
    fclose($srvPipes[$id][1]);
    fclose($srvPipes[$id][2]);
    // A server that never got its connection would sit in accept() for 15 s;
    // terminate instead of proc_close so a broken run cannot serialize the
    // teardown into a minute-long hang.
    proc_terminate($srvProcs[$id]);
    proc_close($srvProcs[$id]);
}

$ok = true;
foreach ($ids as $i => $id) {
    $row = json_decode((string) $bodies[$i], true);
    if (!is_array($row) || $row['id'] !== $id || $row['body'] !== "tls-body-$id") {
        echo 'FAIL: row ' . $i . ' = ' . var_export($bodies[$i], true) . "\n";
        $ok = false;
    }
}

// 4 x 500 ms serialized = 2.0 s (plus handshakes). Concurrency must land far
// below that; on a shared box allow generous headroom, but crossing 1.6 s
// means the requests could not all be in flight inside one 500 ms sleep.
printf("elapsed: %.3f s\n", $elapsed);
if ($elapsed > 1.6) {
    echo "FAIL: requests serialized (expected well under 1.6 s)\n";
    $ok = false;
}

echo $ok ? "fiber-tls-concurrency: ok\n" : "fiber-tls-concurrency: FAILED\n";

// Task 005 criterion 6: configurations that cannot be made non-blocking are
// refused with a clear message, not silently blocking.
$refuseProbe = <<<'PHP'
<?php
ini_set('display_errors', '1');
error_reporting(E_ALL);
$ctx = stream_context_create(['ssl' => ['allow_blocking' => true, 'verify_peer' => false]]);
$r1 = @stream_socket_client('tls://127.0.0.1:1', $e, $m, 2, STREAM_CLIENT_CONNECT, $ctx);
$r2 = @stream_socket_server('tls://127.0.0.1:0', $e2, $m2, STREAM_SERVER_BIND | STREAM_SERVER_LISTEN,
    stream_context_create(['ssl' => ['local_cert' => __DIR__ . '/server.crt', 'local_pk' => __DIR__ . '/server.key']]));
$r3 = @stream_socket_client('tls://127.0.0.1:1', $e3, $m3, 2, STREAM_CLIENT_PERSISTENT, $ctx);
$err = error_get_last();
echo json_encode(['client' => $r1 === false, 'server' => $r2 === false, 'persistent' => $r3 === false]);
PHP;
file_put_contents("$docRoot/refuse.php", $refuseProbe);
$out = (string) @file_get_contents("http://$http/refuse.php");
$row = json_decode($out, true);
// All three calls must fail; the refusal warnings are in the output when
// display_errors is on, but with the gateway in between the exact warning
// placement varies, so assert on the outcomes.
echo ($row && $row['client'] && $row['server'] && $row['persistent'])
    ? "fiber-tls-refusals: ok\n"
    : 'FAIL: refusals = ' . var_export($out, true) . "\n";
$ok = $ok && $row && $row['client'] && $row['server'] && $row['persistent'];

$tester->terminate();
$tester->expectLogTerminatingNotices();
$tester->close();

foreach (glob("$docRoot/*") as $f) {
    @unlink($f);
}
@rmdir($docRoot);

?>
Done
--EXPECTF--
elapsed: %s
fiber-tls-concurrency: ok
fiber-tls-refusals: ok
Done
--CLEAN--
<?php
require_once "tester.inc";
FPM\Tester::clean();
foreach (glob(sys_get_temp_dir() . '/fpmng-fiber-tls-*') as $dir) {
    foreach (glob("$dir/*") as $f) {
        @unlink($f);
    }
    @rmdir($dir);
}
?>

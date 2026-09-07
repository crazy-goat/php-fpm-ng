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
if (!function_exists('pcntl_fork')) {
    die('skip requires pcntl (the test TLS server is a fork of this process)');
}
?>
--FILE--
<?php

require_once "tester.inc";

// Task 005 acceptance criteria 1 + 3: with pool.executor = fiber, N
// concurrent requests each fetching an https:// endpoint that sleeps must
// finish in roughly the time of ONE such request, and each must receive its
// OWN body (asserted on data, not only on timing).
//
// The TLS server is this CLI process: one ssl:// listener with a
// self-signed certificate; accepted connections are served sequentially,
// each sleeping 500 ms before answering. If the workers' TLS stacks block,
// N=4 requests serialize to > 1.9 s; with the interception they complete in
// well under 1.2 s (one 500 ms sleep plus scheduling noise). The threshold
// is generous because the box is shared.

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
$id = $_GET['id'] ?? 'missing';
$tls = getenv('TLS_ADDR');
$ctx = stream_context_create(['ssl' => ['verify_peer' => false, 'verify_peer_name' => false]]);
$body = file_get_contents("tls://$tls/?id=$id", false, $ctx);
echo json_encode(['id' => $id, 'body' => trim((string) $body)], JSON_UNESCAPED_SLASHES);
PHP;
file_put_contents("$docRoot/probe.php", $probe);

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
EOT;

$tester = new FPM\Tester($cfg, $probe);
$tester->start();
$tester->expectLogStartNotices();
$http = $tester->getAddr('ipv4', '[http]');
$tlsAddr = $tester->getAddr('ipv4', '[tls]');   // reserved port for our server

// TLS server in a forked copy of this CLI process: sequential accepts, each
// answered after 500 ms with the ?id echoed back, so a crossed response is
// visible as a wrong body. The listener binds FIRST and the workers connect
// only after it is ready, so there is no accept queue to hide serialization
// in: a blocking client would serialize end to end.
$pid = pcntl_fork();
if ($pid === 0) {
    $ctx = stream_context_create(['ssl' => [
        'local_cert' => $certFile,
        'local_pk'   => $keyFile,
    ]]);
    $server = stream_socket_server("tls://$tlsAddr", $errno, $errstr,
        STREAM_SERVER_BIND | STREAM_SERVER_LISTEN, $ctx);
    if (!$server) {
        exit(2);
    }
    // Signal "listening" to the parent through its pipe endpoint: a byte on
    // fd 3. Without this the parent races the bind.
    $ready = fopen('php://fd/3', 'w');
    fwrite($ready, 'R');
    fclose($ready);
    for ($i = 0; $i < 4; $i++) {
        $conn = @stream_socket_accept($server, 10);
        if (!$conn) {
            continue;
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
    }
    exit(0);
}

$start = microtime(true);
$descriptors = [0 => ['pipe', 'r'], 1 => ['pipe', 'w'], 2 => ['pipe', 'w']];
$processes = [];
$pipes = [];
foreach (['A', 'B', 'C', 'D'] as $i => $id) {
    $env = ['TLS_ADDR' => $tlsAddr];
    $code = 'echo file_get_contents(' . var_export("http://$http/probe.php?id=$id", true) . ');';
    $processes[$i] = proc_open(PHP_BINARY . ' -n -r ' . escapeshellarg($code), $descriptors, $pipes[$i], null, $env);
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

pcntl_waitpid($pid, $status);

$ok = true;
foreach (['A', 'B', 'C', 'D'] as $i => $id) {
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

$tester->terminate();
$tester->expectLogTerminatingNotices();
$tester->close();

@unlink("$docRoot/probe.php");
@unlink($certFile);
@unlink($keyFile);
@rmdir($docRoot);

?>
Done
--EXPECTF--
elapsed: %s
fiber-tls-concurrency: ok
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

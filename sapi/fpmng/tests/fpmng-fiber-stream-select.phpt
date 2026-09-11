--TEST--
fpm-ng: fiber executor makes stream_select() suspend the request, not block the worker (task 006)
--SKIPIF--
<?php
include "skipif.inc";

if (!function_exists('stream_socket_server') || !function_exists('stream_select')) {
    die('skip requires stream transports');
}
$binary = getenv('TEST_PHP_FPM_EXECUTABLE') ?: FPM\Tester::findExecutable();
exec(escapeshellarg($binary) . ' -i 2>&1', $output, $status);
$info = implode("\n", $output);
if ($status !== 0 || !str_contains($info, '--enable-fpmng-fiber')) {
    die('skip php-fpm-ng was not built with --enable-fpmng-fiber');
}
?>
--FILE--
<?php

require_once "tester.inc";
require_once "fpmng-tester.inc";

// Task 006 acceptance criteria:
// 1) N concurrent requests, each calling stream_select() on its own socket
//    with a timeout, complete in about the time of one, not N (measured).
// 2) The return value and by-reference arrays keep the documented contract:
//    ready count, narrowed to the ready subset (asserted on the returned
//    data, not just on timing).
// 3) timeout 0 polls without suspending; timeout null blocks indefinitely.
//
// One single-shot plain-TCP server per request id: reads one line, sleeps
// 500 ms, replies. If stream_select() blocked the whole worker process
// instead of suspending only the calling request's fiber, pm.max_children=1
// would force N x 500 ms serialized.

$docRoot = sys_get_temp_dir() . '/fpmng-fiber-select-' . getmypid();
@mkdir($docRoot, 0700, true);

$probe = <<<'PHP'
<?php
$id = $_GET['id'] ?? 'missing';
$addr = null;
foreach (file(__DIR__ . '/tcp.addr', FILE_IGNORE_NEW_LINES | FILE_SKIP_EMPTY_LINES) as $line) {
    [$lid, $laddr] = explode(' ', $line, 2);
    if ($lid === $id) {
        $addr = $laddr;
        break;
    }
}
if ($addr === null) {
    echo json_encode(['id' => $id, 'error' => 'no server address']);
    exit;
}
$conn = @stream_socket_client("tcp://$addr", $errno, $errstr, 5);
if (!$conn) {
    echo json_encode(['id' => $id, 'error' => "connect: $errstr"]);
    exit;
}

// Criterion 3, timeout 0: the server has not replied yet (it sleeps 500 ms
// first), so this must report "not ready" immediately, not suspend.
$rPoll = [$conn];
$wNone = null;
$eNone = null;
$pollStart = microtime(true);
$nPoll = stream_select($rPoll, $wNone, $eNone, 0);
$pollElapsed = microtime(true) - $pollStart;

fwrite($conn, "id=$id\n");

// Criterion 3, timeout null: block (suspend, on the fiber executor) until
// the reply is ready — no upper bound needed, the harness itself has a
// timeout on the whole test.
$r = [$conn];
$w = null;
$e = null;
$n = stream_select($r, $w, $e, null);

$body = false;
$narrowedOk = ($n === 1 && count($r) === 1 && $r[0] === $conn);
if ($narrowedOk) {
    $body = fread($conn, 8192);
}
fclose($conn);

echo json_encode([
    'id' => $id,
    'n' => $n,
    'narrowedOk' => $narrowedOk,
    'body' => trim((string) $body),
    'pollOk' => ($nPoll === 0 && $pollElapsed < 0.25),
], JSON_UNESCAPED_SLASHES);
PHP;
file_put_contents("$docRoot/probe.php", $probe);

// The TCP server: binds first, prints one READY line so the parent never
// races the listen backlog, then serves exactly ONE connection: read the
// request line, sleep 500 ms, answer. One server per request id, not one
// server with a sequential accept loop, for the same reason as the fiber-tls
// concurrency test: a 500 ms sleep between accepts would serialize the
// measurement on the SERVER side even with a fully concurrent client.
$serverCode = <<<'PHP'
<?php
$addr = $argv[1];
$server = stream_socket_server("tcp://$addr", $errno, $errstr, STREAM_SERVER_BIND | STREAM_SERVER_LISTEN);
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
if (preg_match('/id=([A-Z])/', (string) $req, $m)) {
    $cid = $m[1];
}
usleep(500000);
$body = "select-body-$cid";
fwrite($conn, $body);
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
fpmng_expect_log_start_notices($tester);
$http = $tester->getAddr('ipv4', '[http]');

// One single-shot server process per request id.
$ids = ['A', 'B', 'C', 'D'];
$srvDesc = [0 => ['pipe', 'r'], 1 => ['pipe', 'w'], 2 => ['pipe', 'w']];
$srvProcs = [];
$srvPipes = [];
$addrLines = '';
foreach ($ids as $id) {
    $addr = $tester->getAddr('ipv4', "[tcp-$id]");
    $addrLines .= "$id $addr\n";
    /* The array form runs the binary directly. A command string goes through
     * `/bin/sh -c`, and where /bin/sh is dash the shell does not exec its
     * argument: the pid returned is the shell's, so the proc_terminate() in
     * the teardown below signals the shell and leaves this server behind
     * (measured 2026-09-09 on 192.168.8.50, dash 0.5.12 — issue #101, same
     * mechanism as #89). */
    $srvProcs[$id] = proc_open(
        [PHP_BINARY, '-n', "$docRoot/server.php", $addr],
        $srvDesc, $srvPipes[$id]);
    fclose($srvPipes[$id][0]);
    stream_set_timeout($srvPipes[$id][1], 10);
    $ready = fgets($srvPipes[$id][1]);
    if (trim((string) $ready) !== 'READY') {
        echo "FAIL: TCP server $id did not start: " . var_export($ready, true) . "\n";
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
file_put_contents("$docRoot/tcp.addr", $addrLines);

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
    proc_terminate($srvProcs[$id]);
    proc_close($srvProcs[$id]);
}

$ok = true;
foreach ($ids as $i => $id) {
    $row = json_decode((string) $bodies[$i], true);
    if (!is_array($row) || $row['id'] !== $id || $row['n'] !== 1 || !$row['narrowedOk']
        || $row['body'] !== "select-body-$id" || !$row['pollOk']) {
        echo 'FAIL: row ' . $i . ' = ' . var_export($bodies[$i], true) . "\n";
        $ok = false;
    }
}

// 4 x 500 ms serialized = 2.0 s. Concurrency must land far below that; on a
// shared box allow generous headroom, but crossing 1.6 s means the requests
// could not all be in flight inside one 500 ms sleep.
printf("elapsed: %.3f s\n", $elapsed);
if ($elapsed > 1.6) {
    echo "FAIL: requests serialized (expected well under 1.6 s)\n";
    $ok = false;
}

echo $ok ? "fiber-stream-select: ok\n" : "fiber-stream-select: FAILED\n";

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
fiber-stream-select: ok
Done
--CLEAN--
<?php
require_once "tester.inc";
FPM\Tester::clean();
foreach (glob(sys_get_temp_dir() . '/fpmng-fiber-select-*') as $dir) {
    foreach (glob("$dir/*") as $f) {
        @unlink($f);
    }
    @rmdir($dir);
}
?>

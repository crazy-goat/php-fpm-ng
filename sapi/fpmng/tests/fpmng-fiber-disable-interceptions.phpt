--TEST--
fpm-ng: fiber.disable_interceptions turns one IO interception off; that call blocks like stock PHP while the others stay concurrent (#531, docs/fiber_async_io.md)
--SKIPIF--
<?php
include "fpmng-skipif.inc";

$binary = getenv('TEST_PHP_FPM_EXECUTABLE') ?: FPM\Tester::findExecutable();
exec(escapeshellarg($binary) . ' -i 2>&1', $output, $status);
if ($status !== 0 || !str_contains(implode("\n", $output), '--enable-fpmng-fiber')) {
    die('skip php-fpm-ng was not built with --enable-fpmng-fiber');
}
?>
--FILE--
<?php

require_once "tester.inc";

/* Each URL is fetched by its own php process, so the N requests really are in
 * flight at the same time against the single worker below. */
function concurrentHttpGet(array $urls): array
{
    $descriptors = [0 => ['pipe', 'r'], 1 => ['pipe', 'w'], 2 => ['pipe', 'w']];
    $processes = [];
    $pipes = [];
    foreach ($urls as $i => $url) {
        $code = '$b=@file_get_contents(' . var_export($url, true) . '); echo $b === false ? "@@FALSE@@" : $b;';
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

    return $bodies;
}

/* FPM\Tester::getPort() is deterministic (9008, and 9009 for [http]), so a
 * php-fpm master leaked by an early exit() would still be bound to that port
 * for the NEXT fiber test in the suite, which would then fail to bind or
 * talk to this test's stale worker. FPM\Tester::clean() (--CLEAN--) only
 * unlinks the conf/log/pid files; it never signals the master. Route every
 * failure through this so the pool is always stopped first. */
function bail(string $message): never
{
    global $tester, $docRoot;

    echo $message;
    if ($tester instanceof FPM\Tester) {
        $tester->terminate();
        $tester->close();
    }
    if (is_string($docRoot) && is_dir($docRoot)) {
        rrmdir($docRoot);
    }
    exit(1);
}

/* 1. The switch is validated in the master: an unknown name fails the
 * configuration test and the message lists the registry's names, so a typo
 * cannot silently leave an interception on. */
$bad = <<<EOT
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
[web]
listen = {{ADDR}}
pm = static
pm.max_children = 1
pool.type = fastcgi
pool.executor = fiber
php_admin_value[opcache.enable] = 0
php_admin_value[max_execution_time] = 0
fiber.disable_interceptions = sleep, bogus
EOT;
$check = new FPM\Tester($bad, '<?php echo "ok";');
$messages = $check->testConfig(true);
if ($messages === null) {
    echo "FAIL: fiber.disable_interceptions = bogus unexpectedly passed validation\n";
    exit(1);
}
$text = implode("\n", $messages);
foreach (["unknown interception 'bogus'", 'known interceptions: xport, flock, sleep, select'] as $needle) {
    if (!str_contains($text, $needle)) {
        echo "FAIL: missing needle: $needle\ngot:\n$text\n";
        exit(1);
    }
}
echo "unknown-name: rejected\n";

$docRoot = sys_get_temp_dir() . '/fpmng-fiber-disable-' . getmypid();
@mkdir($docRoot, 0700, true);

/* Nothing is declared at top level on purpose: the class and function tables
 * are per PROCESS in this executor, so a second request would hit "Cannot
 * redeclare" (docs/fiber_errors.md). */
$probe = <<<'PHP'
<?php
$id = $_GET['id'] ?? 'missing';
$fn = $_GET['fn'] ?? 'usleep';
$t0 = microtime(true);
if ($fn === 'select') {
    /* Nothing is ever written to the pair, so stream_select() waits out its
     * full 700 ms timeout: a pure wait, the "select" interception's job. */
    $pair = stream_socket_pair(STREAM_PF_UNIX, STREAM_SOCK_STREAM, STREAM_IPPROTO_IP);
    $r = [$pair[0]];
    $w = $e = null;
    $n = stream_select($r, $w, $e, 0, 700000);
    fclose($pair[0]);
    fclose($pair[1]);
} else {
    usleep(500000);
    $n = null;
}
echo json_encode(['id' => $id, 'fn' => $fn, 'n' => $n, 't0' => $t0, 't1' => microtime(true)]);
PHP;
file_put_contents("$docRoot/probe.php", $probe);

/* max_execution_time must be 0: validate() refuses any other value under the
 * fiber executor (one setitimer per process cannot be per request). */
$cfg = <<<EOT
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
[gw]
pool.type = gateway
listen = {{ADDR[http]}}
chdir = $docRoot
http.route[web] = /
[web]
listen = {{ADDR}}
chdir = $docRoot
pm = static
pm.max_children = 1
pool.type = fastcgi
pool.executor = fiber
fiber.disable_interceptions = sleep
; The child's "interception 'sleep' disabled" notice and the "fiber: bailout
; escaped/suspended outside" warnings asserted below are emitted by the CHILD;
; stock FPM discards child stderr, so without this those checks are vacuous.
catch_workers_output = yes
php_admin_value[opcache.enable] = 0
php_admin_value[max_execution_time] = 0
EOT;

$tester = new FPM\Tester($cfg, $probe);
$tester->start();
$tester->expectLogStartNotices();
$http = $tester->getAddr('ipv4', '[http]');

function decodeAll(array $bodies): array
{
    $rows = [];
    foreach ($bodies as $i => $body) {
        $row = json_decode($body, true);
        if (!is_array($row)) {
            bail("FAIL: response $i not json: " . var_export($body, true) . "\n");
        }
        $rows[$row['id']] = $row;
    }

    return $rows;
}

function windows(array $rows): string
{
    $msg = '';
    foreach ($rows as $id => $row) {
        $msg .= "  $id: {$row['t0']} .. {$row['t1']}\n";
    }

    return $msg;
}

/* 2. sleep disabled: usleep() is the stock handler again and parks the one
 * worker process, so concurrent requests run one after the other. Strict, not
 * timing-tuned: with one process and no suspension the second request cannot
 * even start before the first one's usleep() returns, so the windows are
 * disjoint on any box. */
$rows = decodeAll(concurrentHttpGet([
    "http://$http/probe.php?id=A&fn=usleep",
    "http://$http/probe.php?id=B&fn=usleep",
]));
if (count($rows) !== 2) {
    bail('FAIL: expected 2 distinct ids, got ' . json_encode(array_keys($rows)) . "\n");
}
$lastStart = max(array_column($rows, 't0'));
$firstEnd = min(array_column($rows, 't1'));
if ($lastStart < $firstEnd) {
    bail("FAIL: usleep() overlapped with sleep disabled, last start $lastStart < first end $firstEnd\n" . windows($rows));
}
echo "sleep-disabled-blocks: ok\n";

/* 3. Only that one is off: stream_select() in the same pool still suspends,
 * so four concurrent 700 ms waits overlap (same argument and the same 700 ms
 * slack for four `php -n` startups as fpmng-fiber-sleep-concurrency.phpt). */
$rows = decodeAll(concurrentHttpGet([
    "http://$http/probe.php?id=S1&fn=select",
    "http://$http/probe.php?id=S2&fn=select",
    "http://$http/probe.php?id=S3&fn=select",
    "http://$http/probe.php?id=S4&fn=select",
]));
if (count($rows) !== 4) {
    bail('FAIL: expected 4 distinct ids, got ' . json_encode(array_keys($rows)) . "\n");
}
foreach ($rows as $id => $row) {
    if ($row['n'] !== 0) {
        bail("FAIL: stream_select() in $id returned " . var_export($row['n'], true) . ", expected a 0 timeout\n");
    }
}
$lastStart = max(array_column($rows, 't0'));
$firstEnd = min(array_column($rows, 't1'));
if ($lastStart >= $firstEnd) {
    bail("FAIL: stream_select() serialized with only sleep disabled, last start $lastStart >= first end $firstEnd\n" . windows($rows));
}
echo "select-still-concurrent: ok\n";

/* A miss prints LogTool's error, which fails --EXPECT--. */
$tester->expectLogPattern("/fiber: interception 'sleep' disabled by fiber.disable_interceptions/", true);
$tester->expectNoLogPattern('/fiber: (bailout escaped|request #\d+ suspended outside)/');
$tester->terminate();
$tester->expectLogTerminatingNotices();
$tester->close();

function rrmdir(string $dir): void
{
    foreach (scandir($dir) as $entry) {
        if ($entry === '.' || $entry === '..') {
            continue;
        }
        $path = "$dir/$entry";
        is_dir($path) ? rrmdir($path) : unlink($path);
    }
    rmdir($dir);
}
rrmdir($docRoot);

?>
Done
--EXPECT--
unknown-name: rejected
sleep-disabled-blocks: ok
select-still-concurrent: ok
Done
--CLEAN--
<?php
require_once "tester.inc";
FPM\Tester::clean();
?>

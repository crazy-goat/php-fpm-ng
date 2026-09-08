--TEST--
fpm-ng: fiber executor makes sleep()/usleep()/time_nanosleep() concurrent instead of parking the process (docs/fiber_async_io.md)
--SKIPIF--
<?php
include "skipif.inc";

$binary = getenv('TEST_PHP_FPM_EXECUTABLE') ?: FPM\Tester::findExecutable();
exec(escapeshellarg($binary) . ' -i 2>&1', $output, $status);
if ($status !== 0 || !str_contains(implode("\n", $output), '--enable-fpmng-fiber')) {
    die('skip php-fpm-ng was not built with --enable-fpmng-fiber');
}

// time_nanosleep() only exists when built with HAVE_NANOSLEEP
// (sapi/fpmng/fpm/fpm_pool_fiber_sleep.c:135); without it the probe dies
// with "undefined function", and the body-isn't-json failure is misleading.
if (!function_exists('time_nanosleep')) {
    die('skip requires time_nanosleep (HAVE_NANOSLEEP)');
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

$docRoot = sys_get_temp_dir() . '/fpmng-fiber-sleep-' . getmypid();
@mkdir($docRoot, 0700, true);

/* Nothing is declared at top level on purpose: the class and function tables
 * are per PROCESS in this executor, so a second request would hit "Cannot
 * redeclare" (docs/fiber_errors.md). */
$probe = <<<'PHP'
<?php
$id = $_GET['id'] ?? 'missing';
$fn = $_GET['fn'] ?? 'usleep';
$t0 = microtime(true);
switch ($fn) {
    case 'sleep':
        sleep(1);
        break;
    case 'nanosleep':
        time_nanosleep(0, 700000000);
        break;
    default:
        usleep(700000);
}
echo json_encode(['id' => $id, 'fn' => $fn, 't0' => $t0, 't1' => microtime(true)]);
PHP;
file_put_contents("$docRoot/probe.php", $probe);

/* max_execution_time must be 0: validate() refuses any other value under the
 * fiber executor (one setitimer per process cannot be per request). */
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
; The "fiber: bailout escaped/suspended outside" warnings asserted absent
; below are emitted by the CHILD; stock FPM discards child stderr, so
; without this the expectNoLogPattern() call would be vacuous.
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

/* The load-bearing assertion: the LAST sleep to start did so before the FIRST
 * one woke up, i.e. all four were suspended simultaneously. Were usleep() not
 * intercepted (fpm_pool_fiber_sleep.c swaps its zif_handler), the one worker
 * would serialize them and the windows would be disjoint. This does NOT
 * depend on how slow the box is in absolute terms, but it does require all
 * four clients to reach their sleep call within one sleep duration of each
 * other - that is why the probe sleeps 700 ms rather than something shorter:
 * it gives interpreter startup and TCP connect jitter across four separate
 * `php -n` processes room to land inside the same window. */
function expectOverlap(array $rows, string $what): void
{
    $lastStart = max(array_column($rows, 't0'));
    $firstEnd = min(array_column($rows, 't1'));
    if ($lastStart >= $firstEnd) {
        $msg = "FAIL: $what serialized, last start $lastStart >= first end $firstEnd\n";
        foreach ($rows as $id => $row) {
            $msg .= "  $id: {$row['t0']} .. {$row['t1']}\n";
        }
        bail($msg);
    }
}

$rows = decodeAll(concurrentHttpGet([
    "http://$http/probe.php?id=A&fn=usleep",
    "http://$http/probe.php?id=B&fn=usleep",
    "http://$http/probe.php?id=C&fn=usleep",
    "http://$http/probe.php?id=D&fn=usleep",
]));
if (count($rows) !== 4) {
    bail('FAIL: expected 4 distinct ids, got ' . json_encode(array_keys($rows)) . "\n");
}
/* No wall-clock bound here on purpose. The earlier version bounded the batch
 * at 1.4 s against 1.6 s serialized, but that elapsed time also contains four
 * `php -n` interpreter startups: on a loaded box a CORRECT run can approach
 * 1.4 s while a serialized run clears it by only 0.2 s, so the two cases are
 * not reliably separable that way. expectOverlap() below proves concurrency
 * from the worker's own clock instead, and needs no such bound. */
expectOverlap($rows, '4 x usleep(700ms)');
echo "usleep-concurrent: ok\n";

/* sleep() takes whole seconds and time_nanosleep() is compiled in only
 * #ifdef HAVE_NANOSLEEP, so check them in one mixed batch: they must overlap
 * with each other and with usleep. */
$rows = decodeAll(concurrentHttpGet([
    "http://$http/probe.php?id=S1&fn=sleep",
    "http://$http/probe.php?id=S2&fn=sleep",
    "http://$http/probe.php?id=N1&fn=nanosleep",
    "http://$http/probe.php?id=U1&fn=usleep",
]));
if (count($rows) !== 4) {
    bail('FAIL: expected 4 distinct ids, got ' . json_encode(array_keys($rows)) . "\n");
}
expectOverlap($rows, 'mixed sleep/nanosleep/usleep');
echo "sleep-family-concurrent: ok\n";

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
usleep-concurrent: ok
sleep-family-concurrent: ok
Done
--CLEAN--
<?php
require_once "tester.inc";
FPM\Tester::clean();
?>

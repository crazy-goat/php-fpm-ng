--TEST--
fpm-ng: fiber executor truncates time_nanosleep() below one microsecond and keeps the rest (docs/fiber_async_io.md)
--SKIPIF--
<?php
include "fpmng-skipif.inc";

$binary = getenv('TEST_PHP_FPM_EXECUTABLE') ?: FPM\Tester::findExecutable();
exec(escapeshellarg($binary) . ' -i 2>&1', $output, $status);
if ($status !== 0 || !str_contains(implode("\n", $output), '--enable-fpmng-fiber')) {
    die('skip php-fpm-ng was not built with --enable-fpmng-fiber');
}

// time_nanosleep() only exists when built with HAVE_NANOSLEEP
// (sapi/fpmng/fpm/fpm_pool_fiber_sleep.c); without it the probe dies
// with "undefined function", and the body-isn't-json failure is misleading.
if (!function_exists('time_nanosleep')) {
    die('skip requires time_nanosleep (HAVE_NANOSLEEP)');
}
?>
--FILE--
<?php

require_once "tester.inc";

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

$docRoot = sys_get_temp_dir() . '/fpmng-fiber-sleep-res-' . getmypid();
@mkdir($docRoot, 0700, true);

/* Nothing is declared at top level on purpose: the class and function tables
 * are per PROCESS in this executor, so a second request would hit "Cannot
 * redeclare" (docs/fiber_errors.md). */
$probe = <<<'PHP'
<?php
$case = $_GET['case'] ?? 'missing';
switch ($case) {
    // 500 ns is below the TIMER's timeval resolution, so the fiber executor
    // drops it and returns at once; upstream would sleep 500 ns. This case
    // cannot tell the two apart by timing — both return true in far under a
    // second, below what microtime(true) resolves — so it pins only that the
    // call neither errors nor hangs. The deviation itself is prose-pinned, not
    // assertion-pinned: docs/fiber_async_io.md, the comment above
    // tv.tv_usec in fpm_pool_fiber_sleep.c, and the CHANGELOG entry (#84).
    case 'submicro':
        $t0 = microtime(true);
        $r = time_nanosleep(0, 500);
        echo json_encode(['ok' => $r === true, 'elapsed' => microtime(true) - $t0]);
        break;
    // 1.5 ms keeps a whole-microsecond part after the truncation above, so
    // the sleep must really happen: the lower bound proves microsecond
    // resolution survived. No upper bound on purpose: on a loaded box the
    // request can wake late, and a late wake is still a correct sleep.
    case 'micro':
        $t0 = microtime(true);
        $r = time_nanosleep(0, 1500000);
        echo json_encode(['ok' => $r === true, 'elapsed' => microtime(true) - $t0]);
        break;
    case 'sleep-zero':
        $t0 = microtime(true);
        $r = sleep(0);
        echo json_encode(['ok' => $r === 0, 'elapsed' => microtime(true) - $t0]);
        break;
}
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

function get(string $url): array
{
    $body = @file_get_contents($url);
    if ($body === false) {
        bail("FAIL: no response from $url\n");
    }
    $row = json_decode($body, true);
    if (!is_array($row)) {
        bail('FAIL: response not json: ' . var_export($body, true) . "\n");
    }

    return $row;
}

$row = get("http://$http/probe.php?case=submicro");
if ($row['ok'] !== true || $row['elapsed'] > 1.0) {
    bail('FAIL: time_nanosleep(0, 500): ' . json_encode($row) . "\n");
}
echo "submicro-truncated: ok\n";

$row = get("http://$http/probe.php?case=micro");
if ($row['ok'] !== true || $row['elapsed'] < 0.001) {
    bail('FAIL: time_nanosleep(0, 1500000) slept less than 1 ms: ' . json_encode($row) . "\n");
}
echo "micro-kept: ok\n";

$row = get("http://$http/probe.php?case=sleep-zero");
if ($row['ok'] !== true) {
    bail('FAIL: sleep(0): ' . json_encode($row) . "\n");
}
echo "sleep-zero: ok\n";

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
submicro-truncated: ok
micro-kept: ok
sleep-zero: ok
Done
--CLEAN--
<?php
require_once "tester.inc";
FPM\Tester::clean();
?>

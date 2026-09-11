--TEST--
fpm-ng: fiber executor keeps exceptions per request, including a suspension while one is in flight (docs/fiber_errors.md)
--SKIPIF--
<?php
include "skipif.inc";

$binary = getenv('TEST_PHP_FPM_EXECUTABLE') ?: FPM\Tester::findExecutable();
exec(escapeshellarg($binary) . ' -i 2>&1', $output, $status);
if ($status !== 0 || !str_contains(implode("\n", $output), '--enable-fpmng-fiber')) {
    die('skip php-fpm-ng was not built with --enable-fpmng-fiber');
}
?>
--FILE--
<?php

require_once "tester.inc";
require_once "fpmng-tester.inc";

function concurrentHttpGet(array $urls): array
{
    $descriptors = [0 => ['pipe', 'r'], 1 => ['pipe', 'w'], 2 => ['pipe', 'w']];
    $processes = [];
    $pipes = [];
    foreach ($urls as $i => $url) {
        $code = 'echo file_get_contents(' . var_export($url, true) . ');';
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

$docRoot = sys_get_temp_dir() . '/fpmng-fiber-exc-' . getmypid();
@mkdir($docRoot, 0700, true);

/* One probe, one case per request. Nothing here declares a class or a function
 * at the top level: the class and function tables are per PROCESS while
 * included_files is per request, so a top-level declaration would fail with
 * "Cannot redeclare" on the second request (docs/fiber_errors.md). */
$probe = <<<'PHP'
<?php
$case = $_GET['case'] ?? '';
$id = $_GET['id'] ?? '?';
$out = [];
$t0 = microtime(true);

switch ($case) {
    case 'across':
        // try/catch spanning a scheduler suspension, then a throw from a deeper frame.
        try {
            usleep(250000);
            throw new RuntimeException("boom-$id");
        } catch (RuntimeException $e) {
            $out[] = 'caught:' . $e->getMessage();
        }
        $deeper = function () use ($id) {
            usleep(150000);
            throw new LogicException("deep-$id");
        };
        try {
            $deeper();
        } catch (LogicException $e) {
            $out[] = 'caught:' . $e->getMessage();
        }
        break;

    case 'finally':
        // The sharp one: suspend from finally while the exception is IN FLIGHT,
        // so EG(exception) is set across the switch back to the scheduler.
        // zend_fiber_suspend stashes and restores it (Zend/zend_fibers.c).
        try {
            try {
                throw new RuntimeException("exc-$id");
            } finally {
                usleep(250000);
                $out[] = "finally-$id";
            }
        } catch (RuntimeException $e) {
            $out[] = 'caught:' . $e->getMessage();
        }
        break;

    case 'nested':
        // Exception crossing a nested USER fiber boundary. The usleep(50000)
        // inside the nested Fiber closure below runs while fpm_pool_fiber_can_wait()
        // is 0 for a fiber nested under the request's own fiber, so it cannot
        // suspend through the scheduler and falls back to a real blocking
        // usleep(2) instead - that is the path this case exercises.
        $fiber = new Fiber(function () use ($id) {
            Fiber::suspend("sus-$id");
            usleep(50000);
            throw new RuntimeException("nested-$id");
        });
        $out[] = 'started:' . $fiber->start();
        try {
            $fiber->resume();
        } catch (RuntimeException $e) {
            $out[] = 'caught:' . $e->getMessage();
        }
        usleep(150000);
        $out[] = 'after-suspend';
        break;

    case 'handler-idle':
        // Installs an exception handler, then stays suspended while another
        // request throws uncaught. That other request must not reach this handler.
        set_exception_handler(function (Throwable $e) use ($id) {
            echo "HANDLER-$id:", $e->getMessage();
        });
        usleep(400000);
        $out[] = "handler-installed-$id";
        break;

    case 'handler-own':
        // Own handler must receive this request's own uncaught exception.
        set_exception_handler(function (Throwable $e) use ($id) {
            echo "HANDLER-$id:", $e->getMessage();
        });
        usleep(200000);
        throw new RuntimeException("boom-$id");

    case 'uncaught':
        // Uncaught, with no handler installed in THIS request. This case never
        // reaches the json_encode() below (the process-wide handler takes
        // over), so record the throw time on disk for the caller to check it
        // against handler-idle's [t0, t1] window.
        usleep(150000);
        file_put_contents(__DIR__ . '/uncaught.mark', (string) microtime(true));
        throw new DomainException("uncaught-$id");
}

echo json_encode(['id' => $id, 'case' => $case, 'out' => $out, 't0' => $t0, 't1' => microtime(true)], JSON_UNESCAPED_SLASHES);
PHP;
file_put_contents("$docRoot/probe.php", $probe);

/* max_execution_time = 0 explicitly: validate() refuses to start the pool with
 * any other value (one setitimer per process, many requests in flight), so
 * without this the test would depend on the ambient php.ini. */
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
php_admin_value[display_errors] = On
php_admin_value[html_errors] = Off
EOT;

$tester = new FPM\Tester($cfg, $probe);
$tester->start();
fpmng_expect_log_start_notices($tester);
$http = $tester->getAddr('ipv4', '[http]');

/* One batch, one worker: every case below is in flight in the SAME process at
 * the same time. 'uncaught' (150 ms) throws while 'handler-idle' (400 ms) is
 * suspended with its handler installed. */
$cases = [
    ['across', 'A'],
    ['across', 'B'],
    ['finally', 'C'],
    ['finally', 'D'],
    ['nested', 'E'],
    ['handler-idle', 'F'],
    ['uncaught', 'G'],
    ['handler-own', 'H'],
];

$urls = [];
foreach ($cases as [$case, $id]) {
    $urls[] = "http://$http/probe.php?case=$case&id=$id";
}
$bodies = concurrentHttpGet($urls);

$expectedOut = [
    'A' => ['caught:boom-A', 'caught:deep-A'],
    'B' => ['caught:boom-B', 'caught:deep-B'],
    'C' => ['finally-C', 'caught:exc-C'],
    'D' => ['finally-D', 'caught:exc-D'],
    'E' => ['started:sus-E', 'caught:nested-E', 'after-suspend'],
    'F' => ['handler-installed-F'],
];

$failed = false;
$jsonRows = [];
foreach ($cases as $i => [$case, $id]) {
    $body = $bodies[$i];

    if (isset($expectedOut[$id])) {
        $row = json_decode($body, true);
        if (!is_array($row)) {
            echo "FAIL: $case/$id response is not json: ", var_export($body, true), "\n";
            $failed = true;
            continue;
        }
        $jsonRows[$id] = $row;
        if ($row['id'] !== $id || $row['out'] !== $expectedOut[$id]) {
            echo "FAIL: $case/$id got ", json_encode($row), "\n";
            $failed = true;
        }
        continue;
    }

    if ($id === 'G') {
        // No handler in this request: the engine reports it, and F's handler
        // must not have been used.
        if (!str_contains($body, 'uncaught-G')) {
            echo "FAIL: uncaught/G lost its own message: ", var_export($body, true), "\n";
            $failed = true;
        }
        if (str_contains($body, 'HANDLER-')) {
            echo "FAIL: uncaught/G reached another request's handler: ", var_export($body, true), "\n";
            $failed = true;
        }
        continue;
    }

    if ($id === 'H') {
        if (!str_contains($body, 'HANDLER-H:boom-H')) {
            echo "FAIL: handler-own/H did not use its own handler: ", var_export($body, true), "\n";
            $failed = true;
        }
        continue;
    }
}

if ($failed) {
    bail('');
}

/* Prove the concurrency the --TEST-- line claims, the same shape as
 * expectOverlap() in fpmng-fiber-sleep-concurrency.phpt: the last of the six
 * JSON-returning cases to start did so before the first one finished, i.e.
 * all six really were in flight in the same worker at once. */
$lastStart = max(array_column($jsonRows, 't0'));
$firstEnd = min(array_column($jsonRows, 't1'));
if ($lastStart >= $firstEnd) {
    $msg = "FAIL: cases serialized, last start $lastStart >= first end $firstEnd\n";
    foreach ($jsonRows as $id => $row) {
        $msg .= "  $id: {$row['t0']} .. {$row['t1']}\n";
    }
    bail($msg);
}

/* The claim that gives this test its teeth: 'uncaught' (G) must have thrown
 * WHILE 'handler-idle' (F) was suspended with its own handler installed, not
 * before or after F's window. */
if (!is_file("$docRoot/uncaught.mark")) {
    bail("FAIL: the uncaught case never ran: $docRoot/uncaught.mark is missing\n");
}
$mark = (float) trim(file_get_contents("$docRoot/uncaught.mark"));
$fRow = $jsonRows['F'];
if (!($fRow['t0'] < $mark && $mark < $fRow['t1'])) {
    bail(
        "FAIL: uncaught/G did not throw while handler-idle/F was suspended: "
        . "F.t0={$fRow['t0']} mark=$mark F.t1={$fRow['t1']}\n"
    );
}
echo "exception-overlap: ok\n";

/* The worker must have survived every uncaught exception above. */
$after = concurrentHttpGet(["http://$http/probe.php?case=none&id=Z"]);
$row = json_decode($after[0], true);
if (!is_array($row) || $row['id'] !== 'Z') {
    bail('FAIL: worker did not survive: ' . var_export($after[0], true) . "\n");
}

echo "fiber-exceptions: ok\n";

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
exception-overlap: ok
fiber-exceptions: ok
Done
--CLEAN--
<?php
require_once "tester.inc";
FPM\Tester::clean();
?>

--TEST--
fpm-ng: fiber executor drops a request that suspends outside the scheduler, warns, and keeps serving (sapi/fpmng/fpm/fpm_pool_fiber.c fpm_fiber_after_switch)
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

/* Returns [$bodies, $durations]: $durations[$i] is how long, in seconds, the
 * fetch of $requests[$i] took according to the CHILD's own clock (written to
 * its stderr, which we read back here instead of discarding). Used to prove
 * a dropped request is answered/closed promptly rather than stalling the
 * client until default_socket_timeout (see the 5 s check below). */
function concurrentHttpGet(array $requests): array
{
    $descriptors = [0 => ['pipe', 'r'], 1 => ['pipe', 'w'], 2 => ['pipe', 'w']];
    $processes = [];
    $pipes = [];
    foreach ($requests as $i => $request) {
        [$url, $delay] = [$request[0], $request[1] ?? 0];
        $code = 'usleep(' . (int) $delay . ');'
            . '$__t0=microtime(true);'
            . '$b=@file_get_contents(' . var_export($url, true) . ');'
            . 'fwrite(STDERR, "@@T:" . (microtime(true) - $__t0) . "@@");'
            . 'echo $b === false ? "@@FALSE@@" : $b;';
        $processes[$i] = proc_open(PHP_BINARY . ' -n -r ' . escapeshellarg($code), $descriptors, $pipes[$i]);
        fclose($pipes[$i][0]);
    }

    $bodies = [];
    $durations = [];
    foreach ($processes as $i => $proc) {
        $bodies[$i] = stream_get_contents($pipes[$i][1]);
        fclose($pipes[$i][1]);
        /* Parse a delimited marker rather than casting the whole stderr:
         * a bare (float) cast of unexpected stderr output (or of no output at
         * all) silently yields 0.0, which would make the "answered promptly"
         * assertion below pass vacuously - exactly the failure mode it exists
         * to catch. An unparsable duration is recorded as -1 and rejected. */
        $err = stream_get_contents($pipes[$i][2]);
        $durations[$i] = preg_match('/@@T:([0-9.eE+-]+)@@/', $err, $m) ? (float) $m[1] : -1.0;
        fclose($pipes[$i][2]);
        proc_close($proc);
    }

    return [$bodies, $durations];
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

$docRoot = sys_get_temp_dir() . '/fpmng-fiber-drop-' . getmypid();
@mkdir($docRoot, 0700, true);

/* A request runs in an engine fiber, so userland CAN call Fiber::suspend() in
 * the request's main fiber. Nobody would ever resume it: the scheduler only
 * wakes fibers it parked itself through wait_fd(). */
$probe = <<<'PHP'
<?php
$id = $_GET['id'] ?? 'missing';
switch ($_GET['case'] ?? 'ok') {
    case 'drop':
        // This case never gets to report anything through the response (it
        // never answers), so record the moment it drops on disk for the
        // caller to check against S's [start, end] window.
        file_put_contents(__DIR__ . '/drop.mark', (string) microtime(true));
        // No scheduler frame underneath this: the suspend returns to the
        // libevent loop with fr->waiting == false.
        Fiber::suspend();
        echo json_encode(['id' => $id, 'out' => 'RESUMED']);
        break;

    case 'slow':
        $start = microtime(true);
        usleep(500000);
        echo json_encode(['id' => $id, 'out' => 'slow-done', 'start' => $start, 'end' => microtime(true)]);
        break;

    default:
        echo json_encode(['id' => $id, 'out' => 'ok']);
}
PHP;
file_put_contents("$docRoot/probe.php", $probe);

/* catch_workers_output is required to see this: the warning is emitted by the
 * CHILD, and without it the child's stderr is discarded and the log stays
 * empty even though the request was dropped. */
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
catch_workers_output = yes
php_admin_value[opcache.enable] = 0
php_admin_value[max_execution_time] = 0
EOT;

$tester = new FPM\Tester($cfg, $probe);
$tester->start();
$tester->expectLogStartNotices();
$http = $tester->getAddr('ipv4', '[http]');

/* D suspends and is dropped. S is already in flight, suspended in usleep(),
 * and must be unaffected: dropping one request must not take the loop or the
 * other in-flight requests down with it. */
[$bodies, $durations] = concurrentHttpGet([
    ["http://$http/probe.php?id=S&case=slow", 0],
    ["http://$http/probe.php?id=D&case=drop", 100000],
]);

$slow = json_decode($bodies[0], true);
if (($slow['out'] ?? null) !== 'slow-done') {
    bail('FAIL: in-flight request S was affected by the drop: ' . var_export($bodies[0], true) . "\n");
}
echo "concurrent-request-survives-drop: ok\n";

/* Prove the concurrency the --TEST-- line claims: D must have dropped WHILE S
 * was actually in flight, not before or after it. D itself never answers, so
 * read the mark it wrote to disk immediately before suspending. */
if (!is_file("$docRoot/drop.mark")) {
    bail("FAIL: the drop case never ran: $docRoot/drop.mark is missing\n");
}
$mark = (float) trim(file_get_contents("$docRoot/drop.mark"));
if (!($slow['start'] < $mark && $mark < $slow['end'])) {
    bail(
        "FAIL: inconclusive run, D did not drop while S was in flight: "
        . "s.start={$slow['start']} mark=$mark s.end={$slow['end']}\n"
    );
}

/* The dropped request must not produce a successful body. fcgi_finish_request()
 * is called with the error flag, so the client sees an empty body or a failed
 * fetch depending on how far the response got. */
if (str_contains($bodies[1], 'RESUMED')) {
    bail('FAIL: the suspended request was resumed: ' . var_export($bodies[1], true) . "\n");
}
if (trim($bodies[1]) !== '' && trim($bodies[1]) !== '@@FALSE@@') {
    bail('FAIL: dropped request returned a body: ' . var_export($bodies[1], true) . "\n");
}
/* "no body" alone is also what a client hitting default_socket_timeout (60s)
 * would see, so also require the fetch to have closed promptly. 5s is a
 * loose bound - it only needs to separate "closed immediately" from a 60s
 * timeout. A regression that drops fcgi_finish_request(fr->ctx->req, 1)
 * (sapi/fpmng/fpm/fpm_pool_fiber.c:164) while keeping the zlog warning would
 * stall this fetch the full 60s and would otherwise stay green. */
if ($durations[1] < 0) {
    bail("FAIL: could not measure how long the dropped request took (no @@T:...@@ from the client)\n");
}
if ($durations[1] > 5.0) {
    bail("FAIL: dropped request took {$durations[1]}s to answer/close, expected under 5s\n");
}
echo "dropped-request-has-no-body: ok\n";

$tester->expectLogPattern(
    '/fiber: request #\d+ suspended outside the scheduler/',
    true,
    2
);
echo "dropped-request-warning: ok\n";

/* The worker is not dead and not wedged: it serves further requests, including
 * concurrent ones, after having leaked a suspended fiber. */
[$bodies, ] = concurrentHttpGet([
    ["http://$http/probe.php?id=P&case=ok", 0],
    ["http://$http/probe.php?id=Q&case=slow", 0],
]);
$p = json_decode($bodies[0], true);
$q = json_decode($bodies[1], true);
if (($p['out'] ?? null) !== 'ok' || ($q['out'] ?? null) !== 'slow-done') {
    bail('FAIL: pool stopped serving after a drop: ' . json_encode($bodies) . "\n");
}
echo "pool-alive-after-drop: ok\n";

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
concurrent-request-survives-drop: ok
dropped-request-has-no-body: ok
dropped-request-warning: ok
pool-alive-after-drop: ok
Done
--CLEAN--
<?php
require_once "tester.inc";
FPM\Tester::clean();
?>

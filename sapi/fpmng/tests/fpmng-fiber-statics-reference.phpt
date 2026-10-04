--TEST--
fpm-ng: fiber executor isolates a typed static that a request holds a reference to across a real suspension (sapi/fpmng/fpm/fpm_pool_coop_statics.c)
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

/* Each URL is fetched by its own php process, so the two requests really are in
 * flight at the same time against the single worker below. `marker` is a file
 * the first request's probe creates right before it suspends: the client polls
 * for it (bounded) before sending, so the second request can only arrive once
 * the first is suspended, whatever the process start-up jitter. */
function concurrentHttpGet(array $requests): array
{
    $descriptors = [0 => ['pipe', 'r'], 1 => ['pipe', 'w'], 2 => ['pipe', 'w']];
    $processes = [];
    $pipes = [];
    foreach ($requests as $i => $request) {
        [$url, $delay, $marker] = [$request[0], $request[1] ?? 0, $request[2] ?? null];
        $code = ($marker === null ? '' : '$t=microtime(true);'
                . 'while(!file_exists(' . var_export($marker, true) . ')&&microtime(true)-$t<20)usleep(2000);')
            . 'usleep(' . (int) $delay . ');'
            . '$b=@file_get_contents(' . var_export($url, true) . '); echo $b === false ? "@@FALSE@@" : $b;';
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

function decodeAll(array $bodies): array
{
    $rows = [];
    foreach ($bodies as $i => $body) {
        $row = json_decode($body, true);
        if (!is_array($row)) {
            /* Not JSON is what the isolated-statics hazard looks like from
             * outside: a request that finds the live slot left IS_UNDEF
             * throws "Cannot access uninitialized non-nullable property ... by
             * reference" and answers 500, instead of this object. */
            bail("FAIL: response $i not json: " . var_export($body, true) . "\n");
        }
        $rows[$row['tag']] = $row;
    }

    return $rows;
}

$docRoot = sys_get_temp_dir() . '/fpmng-fiber-statics-ref-' . getmypid();
@mkdir($docRoot, 0700, true);

/* Timestamps are taken in the worker, so the assertions below compare events
 * inside ONE process clock and do not depend on client scheduling.
 *
 * The reference is taken BEFORE any suspension -- the case the statics spike
 * (task 008, done; see docs/task-archive.md) explicitly did not test. The
 * suspension is usleep(), which IS intercepted in this build
 * (fpm_pool_fiber_sleep.c swaps three zif_handlers in CG(function_table);
 * fpmng-fiber-sleep-concurrency.phpt measures that two overlapping
 * usleep()s really do overlap), so this is a real fiber switch, not a
 * blocking call that would serialize the two requests and make the test
 * vacuous. No MySQL needed. */
$probe = <<<'PHP'
<?php
$tag = $_GET['tag'] ?? 'X';
$ms = (int) ($_GET['ms'] ?? 700);
$out = ['tag' => $tag, 'pid' => getmypid(), 'start' => microtime(true)];

// The class table is per PROCESS while the entry script re-executes on every
// request, so an unguarded declaration is "Cannot redeclare" on the second
// request (docs/fiber_errors.md).
if (!class_exists('FpmNgStaticsTest', false)) {
    class FpmNgStaticsTest {
        public static int $counter = 0;
    }
}

$ref = &FpmNgStaticsTest::$counter;
$ref = 1;
$out['before_ref'] = $ref;
$out['before_static'] = FpmNgStaticsTest::$counter;

// Observable "I am about to suspend" signal for the client (see
// concurrentHttpGet()).
file_put_contents(__DIR__ . '/suspended-' . $tag, '1');
usleep($ms * 1000);
$out['woke'] = microtime(true);
$out['after_woke_static'] = FpmNgStaticsTest::$counter;

$ref++;
$out['after_ref'] = $ref;
$out['after_static'] = FpmNgStaticsTest::$counter;
$out['end'] = microtime(true);

echo json_encode($out);
PHP;
file_put_contents("$docRoot/probe.php", $probe);

/* max_execution_time must be 0: fpm_coop_validate() refuses any other value
 * under the fiber executor (one setitimer per process cannot represent N
 * concurrent deadlines), and tester.inc starts the binary with -n, so an
 * unset value is PHP's compiled-in default of 30 rather than anything from a
 * php.ini. Without the pin the pool never starts (issue #87).
 *
 * fiber.isolate_statics names the one property under test. The pool is
 * fiber.isolate_statics's only consumer: it is refused on every other
 * executor, so naming it here cannot pass vacuously on a classic pool. */
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
fiber.isolate_statics = FpmNgStaticsTest::counter
EOT;

$tester = new FPM\Tester($cfg, $probe);
$tester->start();
$tester->expectLogStartNotices();
/* What this line is for: it is the only check that the directive reached
 * fpm_coop_statics_container_start() with the expected item count. The value
 * assertions do NOT depend on it -- with the mechanism entirely off (an
 * unknown class, which fpm_coop_statics_resolve() skips silently) A and B
 * would bind $ref to the SAME zend_reference, because a slot that already
 * holds IS_REFERENCE is shared rather than re-referenced, so A sets 1, B
 * shares and sets 1, A wakes (sees 1) and $ref++ makes the shared reference
 * 2 -- and then B wakes on 2 and the after_woke_static assertion below fails
 * with "B saw another request's static on waking". So that case is caught,
 * but by an indirect route whose failure message does not name the cause.
 * This line names it directly. */
$tester->expectLogPattern('/coop-statics: per-request isolation of 1 class static property ENABLED/', true, 10);
$http = $tester->getAddr('ipv4', '[http]');

/* A holds for 1 s, B is sent 150 ms after A signalled that it suspended. Both
 * run the same sequence, so the reference direction and the isolation
 * direction are each checked from both requests. */
$rows = decodeAll(concurrentHttpGet([
    ["http://$http/probe.php?tag=A&ms=1000", 0],
    ["http://$http/probe.php?tag=B&ms=1000", 150000, "$docRoot/suspended-A"],
]));
if (count($rows) !== 2) {
    bail('FAIL: expected A, B, got ' . json_encode(array_keys($rows)) . "\n");
}
[$a, $b] = [$rows['A'], $rows['B']];

/* Precondition, in the worker's own clock: this batch only exercises the
 * hazard if both requests really ran in ONE process and B really did touch
 * the slot while A was suspended. Otherwise the run is inconclusive rather
 * than a pass or a detected bug, and a .phpt needs deterministic output, so
 * bail() rather than silently continuing. */
if ($a['pid'] !== $b['pid']) {
    bail("FAIL: not one process, a.pid={$a['pid']} b.pid={$b['pid']}\n");
}
if (!($a['start'] < $b['start'] && $b['start'] < $a['woke'])) {
    bail(
        "FAIL: inconclusive run, B did not arrive while A was suspended: "
        . "a.start={$a['start']} b.start={$b['start']} a.woke={$a['woke']}\n"
    );
}
echo "two-requests-one-process-overlapped: ok\n";

foreach (['A' => $a, 'B' => $b] as $tag => $row) {
    /* Before the suspension the reference and the static property already see
     * each other's writes -- the reference points at the same zend_reference
     * the isolated slot holds, so $ref = 1 lands in the slot. */
    if ($row['before_ref'] !== 1 || $row['before_static'] !== 1) {
        bail("FAIL: $tag reference and static disagree before the suspension: " . json_encode($row) . "\n");
    }

    /* THE regression. Waking up with the other request's value here would mean
     * isolation did not hold; a 500 with "Cannot access uninitialized
     * non-nullable property" would mean the live slot was left IS_UNDEF while
     * this request was away (fpm_coop_statics_req_leave() must refill it from
     * default_static_members_table). Both are caught above/by this line. */
    if ($row['after_woke_static'] !== 1) {
        bail("FAIL: $tag saw another request's static on waking: " . json_encode($row) . "\n");
    }

    /* After the suspension, in both directions: writing through the reference
     * is visible in the static, and the static still points at the reference.
     * This is the case the spike left untested. */
    if ($row['after_ref'] !== 2 || $row['after_static'] !== 2) {
        bail("FAIL: $tag reference and static disagree after the suspension: " . json_encode($row) . "\n");
    }
}
echo "reference-survives-suspension: ok\n";
echo "static-isolated-from-other-request: ok\n";

/* The worker must have survived the whole exchange. */
$rows = decodeAll(concurrentHttpGet([["http://$http/probe.php?tag=Z&ms=1", 0]]));
if (($rows['Z']['after_static'] ?? null) !== 2) {
    bail('FAIL: worker did not survive: ' . json_encode($rows['Z'] ?? null) . "\n");
}
echo "worker-survived: ok\n";

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
two-requests-one-process-overlapped: ok
reference-survives-suspension: ok
static-isolated-from-other-request: ok
worker-survived: ok
Done
--CLEAN--
<?php
require_once "tester.inc";
FPM\Tester::clean();
?>

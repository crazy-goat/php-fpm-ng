--TEST--
fpm-ng: fiber executor arbitrates flock() between fibers of one process instead of parking it (sapi/fpmng/fpm/fpm_pool_fiber_flock.c, docs/flock-streams-spike-report.md)
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

/* Each request gets its own php process. `delay` staggers the ARRIVAL of the
 * requests, which this test needs: the waiter must reach flock() while the
 * holder still holds the lock, and the unrelated request must arrive after
 * the waiter is already queued. */
function concurrentHttpGet(array $requests): array
{
    $descriptors = [0 => ['pipe', 'r'], 1 => ['pipe', 'w'], 2 => ['pipe', 'w']];
    $processes = [];
    $pipes = [];
    foreach ($requests as $i => $request) {
        [$url, $delay] = [$request[0], $request[1] ?? 0];
        $code = 'usleep(' . (int) $delay . ');'
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
            bail("FAIL: response $i not json: " . var_export($body, true) . "\n");
        }
        $rows[$row['id']] = $row;
    }

    return $rows;
}

$docRoot = sys_get_temp_dir() . '/fpmng-fiber-flock-' . getmypid();
@mkdir($docRoot, 0700, true);

/* Timestamps are taken in the worker, so the assertions below compare events
 * inside ONE process clock and do not depend on client scheduling. */
$probe = <<<'PHP'
<?php
$id = $_GET['id'] ?? 'missing';
$case = $_GET['case'] ?? 'ex';
$file = __DIR__ . '/lock.dat';
$out = ['id' => $id, 'case' => $case, 'start' => microtime(true)];

switch ($case) {
    case 'ex':
        $fp = fopen($file, 'c');
        flock($fp, LOCK_EX);
        $out['acquired'] = microtime(true);
        // 1 s hold, not 400 ms: gives hundreds of ms of margin against
        // relative jitter between independently proc_open()'d `php -n`
        // interpreters (startup + TCP connect + usleep overshoot) for the
        // server-clock-only comparisons below.
        usleep(1000000);
        $out['released'] = microtime(true);
        flock($fp, LOCK_UN);
        fclose($fp);
        break;

    case 'sh':
        $fp = fopen($file, 'c');
        flock($fp, LOCK_SH);
        $out['acquired'] = microtime(true);
        usleep(300000);
        $out['released'] = microtime(true);
        flock($fp, LOCK_UN);
        fclose($fp);
        break;

    case 'nb':
        // LOCK_NB must never suspend: it reports failure instead of waiting.
        $fp = fopen($file, 'c');
        $out['got'] = flock($fp, LOCK_EX | LOCK_NB) ? 'yes' : 'no';
        $out['acquired'] = microtime(true);
        fclose($fp);
        break;

    default:
        // Touches no lock at all: used to prove the process still serves other
        // requests while another fiber waits for the lock.
        usleep(50000);
        $out['acquired'] = microtime(true);
}

$out['end'] = microtime(true);
echo json_encode($out);
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

/* H holds LOCK_EX for 1 s (arrives at 0). W asks for the same lock 150 ms in,
 * so it must wait. F arrives 350 ms in and takes no lock at all - well
 * inside H's 1 s hold, leaving a wide margin for client-side stagger. */
$rows = decodeAll(concurrentHttpGet([
    ["http://$http/probe.php?id=H&case=ex", 0],
    ["http://$http/probe.php?id=W&case=ex", 150000],
    ["http://$http/probe.php?id=F&case=free", 350000],
]));
if (count($rows) !== 3) {
    bail('FAIL: expected H, W, F, got ' . json_encode(array_keys($rows)) . "\n");
}
[$h, $w, $f] = [$rows['H'], $rows['W'], $rows['F']];

/* Precondition: this batch only exercises mutual exclusion if W actually
 * reached flock() while H still held the lock. If it did not, the run is
 * inconclusive rather than a pass or a detected bug, but a .phpt needs
 * deterministic output, so bail() rather than silently continuing. */
if (!($w['start'] < $h['released'])) {
    bail(
        "FAIL: inconclusive run, W did not arrive before H released: "
        . "w.start={$w['start']} h.released={$h['released']}\n"
    );
}

/* Mutual exclusion: the kernel flock() is still the source of truth, and the
 * in-process registry must not hand the lock to W early. 'released' is
 * recorded BEFORE flock($fp, LOCK_UN) in the probe, so no fudge is needed. */
if ($w['acquired'] < $h['released']) {
    bail('FAIL: W acquired LOCK_EX at ' . $w['acquired'] . ' while H held it until ' . $h['released'] . "\n");
}
echo "flock-ex-mutual-exclusion: ok\n";

/* The whole point of the interception: while W waits for the lock, the ONE
 * OS thread is not parked in flock(2), so F is accepted and finishes before
 * H ever releases. Without the hook, F could not even start until
 * $h['released']. This comparison is deliberately expressed purely in the
 * worker's own clock (F ran entirely inside W's waiting window) rather than
 * against $h['released'] with a fudge factor: client-side stagger between
 * separate `php -n` interpreters is not bounded tightly enough to compare
 * against directly. */
if (!($f['start'] > $w['start'] && $f['end'] < $w['acquired'])) {
    bail(
        'FAIL: inconclusive run, F did not land inside W\'s waiting window: '
        . "w.start={$w['start']} w.acquired={$w['acquired']} f.start={$f['start']} f.end={$f['end']}\n"
    );
}
echo "flock-does-not-park-process: ok\n";

/* Two LOCK_SH holders are compatible and must overlap (fpm_flock_sh_add). */
$rows = decodeAll(concurrentHttpGet([
    ["http://$http/probe.php?id=S1&case=sh", 0],
    ["http://$http/probe.php?id=S2&case=sh", 50000],
]));
if (count($rows) !== 2) {
    bail('FAIL: expected S1, S2, got ' . json_encode(array_keys($rows)) . "\n");
}
$lastAcquired = max($rows['S1']['acquired'], $rows['S2']['acquired']);
$firstReleased = min($rows['S1']['released'], $rows['S2']['released']);
if ($lastAcquired >= $firstReleased) {
    bail("FAIL: LOCK_SH serialized, last acquire $lastAcquired >= first release $firstReleased\n");
}
echo "flock-sh-shared: ok\n";

/* LOCK_NB never waits: while H holds LOCK_EX, the non-blocking attempt must
 * come back false rather than suspend. */
$rows = decodeAll(concurrentHttpGet([
    ["http://$http/probe.php?id=H2&case=ex", 0],
    ["http://$http/probe.php?id=NB&case=nb", 120000],
]));
/* Same precondition idea as the mutual-exclusion batch above: NB only tells
 * us anything about non-blocking behaviour if it actually reached flock()
 * while H2 still held the lock. */
if (!($rows['NB']['start'] < $rows['H2']['released'])) {
    bail(
        'FAIL: inconclusive run, NB did not arrive before H2 released: '
        . "nb.start={$rows['NB']['start']} h2.released={$rows['H2']['released']}\n"
    );
}
if (($rows['NB']['got'] ?? null) !== 'no') {
    bail('FAIL: LOCK_NB against a held lock returned ' . json_encode($rows['NB'] ?? null) . "\n");
}
if ($rows['NB']['end'] > $rows['H2']['released'] - 0.05) {
    bail('FAIL: LOCK_NB request waited until the holder released' . "\n");
}
echo "flock-nb-does-not-wait: ok\n";

/* Registry hygiene: after all of the above the lock must be free again, so a
 * fresh request takes it immediately. A leaked holder would hang here. */
$rows = decodeAll(concurrentHttpGet([["http://$http/probe.php?id=Z&case=ex", 0]]));
if (($rows['Z']['acquired'] ?? null) === null || $rows['Z']['acquired'] - $rows['Z']['start'] > 0.2) {
    bail('FAIL: lock not free after the earlier requests: ' . json_encode($rows['Z'] ?? null) . "\n");
}
echo "flock-released-after-requests: ok\n";

/* Honest limitation: flock-ex-mutual-exclusion, flock-sh-shared and
 * flock-nb-does-not-wait also hold on a build WITHOUT the interception hook,
 * because each request fopen()s its own fd and the kernel already provides
 * exclusion, LOCK_SH compatibility, and immediate failure for LOCK_NB. They
 * still guard against a registry that wrongly serializes LOCK_SH or grants a
 * lock early. The only assertion that distinguishes hooked from unhooked
 * behaviour is flock-does-not-park-process above. */

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
flock-ex-mutual-exclusion: ok
flock-does-not-park-process: ok
flock-sh-shared: ok
flock-nb-does-not-wait: ok
flock-released-after-requests: ok
Done
--CLEAN--
<?php
require_once "tester.inc";
FPM\Tester::clean();
?>

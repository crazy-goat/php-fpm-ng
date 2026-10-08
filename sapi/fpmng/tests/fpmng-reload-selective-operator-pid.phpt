--TEST--
fpm-ng: a worker carried over by a selective reload is reported live with its own pid (issue #631)
--SKIPIF--
<?php include "skipif.inc"; ?>
--FILE--
<?php

require_once "tester.inc";
require_once "fpmng-operator.inc";

/* Issue #631. The scoreboard slot a carried-over worker is adopted into used to
 * be a fresh anonymous mapping, so its pid was 0, and the pid is what the
 * operator page's `?full` rows decide "live" by (issue #567: a slot with pid 0 is
 * never printed as live, because kill(2) reads 0 as the caller's whole process
 * group). The row of a running worker therefore read `live:0, pid:0` -- an
 * operator page hiding a process it is supposed to account for. Since #537 the
 * scoreboard survives the reload in a memfd and
 * fpm_reload_shm_prepare_adopt() hands adoption the slot that already carries
 * the worker's pid, so nothing has to stamp one (docs/reload.md, "Shared memory
 * of a spared pool": the status page keeps counting the worker, and adoption
 * takes the slot whose pid matches).
 *
 * The page is read from the operator listener of an http-direct pool because
 * that is where the per-child rows with a pid come from
 * (fpm_http_direct_ops_status_workers()). pm = static with one child makes the
 * assertions exact: there is exactly one row, so "the row" is unambiguous.
 *
 * The pid the script writes with getmypid() IS the worker's pid -- http-direct
 * runs the request inside the worker process -- which is what lets the test
 * name the expected pid instead of trusting the page to be self-consistent. */
$root = sys_get_temp_dir() . '/fpmng-reload-sel-opspid-' . getmypid();
@mkdir($root, 0700, true);
$pidFile = "$root/worker.pid";
@unlink($pidFile);
file_put_contents("$root/front.php", <<<'PHP'
<?php
file_put_contents(getenv('FPMNG_WORKER_PIDFILE'), (string) getmypid());
echo 'ok';
PHP);

$cfg = <<<CFG
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
log_level = notice
reload.selective = yes
[direct]
listen = {{ADDR[direct]}}
pool.type = http-direct
pm = static
pm.max_children = 1
chdir = $root
http.front_controller = /front.php
operator.status_path = /status
operator.status_listen = {{ADDR[ops]}}
env[FPMNG_WORKER_PIDFILE] = $pidFile
CFG;

function fail(string $what): void
{
    throw new RuntimeException($what);
}

/* The master's pid, or 0 when the pid file says nothing usable. FPM\Tester::getPid()
 * calls $this->error() (which fails the test) when the file is missing or is not
 * a number -- and fpm_pctl_exit() unlinks the pid file on its way out, so a
 * teardown that runs after terminate() gets nothing from it. Read it directly.
 *
 * Under -F, which is what tester.inc passes (`:528`) and what
 * packaging/deb/php-fpm-ng.service ships, the reload's execvp() happens in this
 * same process and the pid does not change. Daemonized it does, and for a
 * different reason than it looks: fpm_unix_init_main() forks when `daemonize` is
 * set (fpm_unix.c:596-618) and that gate is re-evaluated by the start after the
 * reload, so the surviving master is the fork's child. Either way both pids go
 * into the teardown, which is why it reads the file twice. */
function master_pid(FPM\Tester $tester): int
{
    $file = $tester->getPrefixedFile(FPM\Tester::FILE_EXT_PID);
    $data = is_file($file) ? trim((string) file_get_contents($file)) : '';
    return preg_match('/^[0-9]+$/', $data) ? (int) $data : 0;
}

/* Whether a pid is a live process. A zombie counts as gone: the master is this
 * test's proc_open() child (tester.inc:549) and the only thing that reaps it is
 * proc_close(), which close() calls after this function -- so without this the
 * wait below would always spend its whole budget on the master and the test
 * would show 10 s in the suite's slow.tsv for nothing. */
function pid_alive(int $pid): bool
{
    if ($pid <= 1 || !is_dir("/proc/$pid")) {
        return false;
    }
    $stat = @file_get_contents("/proc/$pid/stat");
    /* "pid (comm) state ..." -- comm is parenthesised and may contain spaces, so
     * the state is the first field after the last ')'. */
    $close = $stat === false ? false : strrpos($stat, ')');
    return $close === false || ($stat[$close + 2] ?? '') !== 'Z';
}

/* Kill exactly these pids, and nothing else, then wait for them to be gone.
 * `kill` through exec() rather than posix_kill(): ext/posix is not in the CI
 * configure flags and is a shared module that a packaged CLI run with -n does
 * not load, so a posix-based test skipped everywhere (issue #467); same wording
 * as fpmng-http-route-http-direct-fail.phpt.
 *
 * The master goes first: a pool that still has a master respawns the worker
 * this test is about to kill, and the respawned one is the leak. */
function kill_pids(array $pids): void
{
    $pids = array_values(array_unique(array_filter(array_map('intval', $pids))));
    foreach ($pids as $pid) {
        if (pid_alive($pid)) {
            exec("kill -9 $pid 2>/dev/null");
        }
    }
    $deadline = microtime(true) + 10;
    do {
        $left = array_filter($pids, 'pid_alive');
        if ($left === []) {
            return;
        }
        usleep(50000);
    } while (microtime(true) < $deadline);
}

/* One request on a fresh connection, so the worker this runs in is the one the
 * pool would have accepted on anyway. */
function request(string $address): void
{
    $fp = @stream_socket_client("tcp://$address", $errno, $error, 5);
    if (!$fp) {
        fail("connect $address: $error");
    }
    stream_set_timeout($fp, 10);
    fwrite($fp, "GET /app HTTP/1.1\r\nHost: test\r\nConnection: close\r\n\r\n");
    $body = '';
    while (!feof($fp)) {
        $chunk = fread($fp, 8192);
        if ($chunk === false || $chunk === '') {
            break;
        }
        $body .= $chunk;
    }
    fclose($fp);
    if (!str_contains($body, 'ok')) {
        fail("no answer from $address: " . var_export($body, true));
    }
}

/* The single per-child row of /status?json&full, as [slot, live, pid]. */
function workerRow(string $ops): array
{
    [$status, , $body] = fpmng_operator_fetch($ops, '/status?json&full');
    if ($status !== 200) {
        fail("status?json&full answered $status\n$body");
    }
    $decoded = json_decode($body, true);
    if (!is_array($decoded) || !isset($decoded['workers']) || !is_array($decoded['workers'])) {
        fail("status?json&full has no workers array\n$body");
    }
    if (count($decoded['workers']) !== 1) {
        fail('expected exactly one worker row, got ' . count($decoded['workers']) . "\n$body");
    }
    $row = $decoded['workers'][0];
    return [(int) $row['slot'], (int) $row['live'], (int) $row['pid']];
}

$tester = new FPM\Tester($cfg, '<?php');
$direct = $tester->getAddr('ipv4', '[direct]');
$ops = $tester->getAddr('ipv4', '[ops]');

$waitForPid = function () use ($pidFile): int {
    $deadline = time() + 20;
    while (time() < $deadline) {
        $data = @file_get_contents($pidFile);
        if ($data !== false && preg_match('/^[0-9]+$/', $data)) {
            return (int) $data;
        }
        usleep(50000);
    }
    fail("no worker pid ever written to $pidFile");
};

try {
    /* Issue #405: start() defaults to forceStderr=true (FPM's -O), which leaves
     * error_log = {{FILE:LOG}} unwritten -- and the adoption NOTICE below is
     * the positive proof that the pool really was carried over. Same pattern as
     * fpmng-reload-selective-on.phpt. */
    @unlink($tester->getPrefixedFile(FPM\Tester::FILE_EXT_LOG_ERR));
    $tester->start([], false);
    $tester->switchLogSource('{{FILE:LOG}}');
    $tester->expectLogStartNotices();

    /* Kept for the teardown, which kills the master of both generations: under
     * -F the pid does not change across the reload, but daemonized the reload
     * forks (see master_pid()), and the teardown must not depend on which mode
     * it is running in. */
    $masterPid = master_pid($tester);

    request($direct);
    $pidBefore = $waitForPid();

    /* Before the reload the row must already name that pid, or the assertions
     * after the reload would pass on a page that never worked. */
    [$slot, $live, $pid] = workerRow($ops);
    if ($live !== 1 || $pid !== $pidBefore) {
        fail("before the reload: expected live:1 pid:$pidBefore on slot $slot, got live:$live pid:$pid");
    }
    echo "before reload: ok\n";

    /* Byte-for-byte the same configuration, so reload.selective spares the pool
     * and the new master adopts the running worker instead of restarting it. */
    $tester->reload($cfg);
    /* 0 inherited sockets: the "using inherited socket" NOTICE per socket comes
     * from a daemonized start, where the fork re-runs fpm_sockets_init_main().
     * Under -F there is no fork and no such line. Not measured: whether the log
     * stays clean here -- the count is not what makes this pass, LogTool::match()
     * drops a recorded error once readUntil() matches further down
     * (logtool.inc:146-163), which is a harness quirk and not a fact about the
     * master. */
    $tester->expectLogReloadingNotices(0);
    $masterPidAfter = master_pid($tester);

    $errorLogPath = $tester->getPrefixedFile(FPM\Tester::FILE_EXT_LOG_ERR);
    $adopted = '/\[pool direct\][^\n]*issue #330: adopted 1 child\(ren\) carried over/';
    $deadline = time() + 30;
    $sawAdoption = false;
    while (time() < $deadline) {
        $logText = is_file($errorLogPath) ? (string) file_get_contents($errorLogPath) : '';
        if (preg_match($adopted, $logText)) {
            $sawAdoption = true;
            break;
        }
        usleep(100000);
    }
    if (!$sawAdoption) {
        $logText = is_file($errorLogPath) ? (string) file_get_contents($errorLogPath) : '(no log)';
        fail("the master never logged that it adopted a carried-over worker\n--- $errorLogPath\n$logText\n---");
    }
    echo "adopted: ok\n";

    [$slot, $live, $pid] = workerRow($ops);
    if ($live !== 1) {
        fail("after the reload the carried-over worker reads live:$live, expected live:1 (pid:$pid)");
    }
    if ($pid !== $pidBefore) {
        fail("after the reload the row carries pid:$pid, expected the carried-over worker's pid:$pidBefore");
    }
    echo "after reload: ok\n";

    /* And the adopted worker is really still the one serving: this request runs
     * in a process with that pid. Without it, a page reading the right pid out
     * of a stale slot would satisfy every assertion above. */
    request($direct);
    if ($waitForPid() !== $pidBefore) {
        fail('the pool restarted its worker across the reload, so nothing was carried over');
    }
    echo "still serving: ok\n";

    echo "Done\n";
} finally {
    /* Kills what this test started, by pid, after terminate(). Needed because of
     * #792 (measured on 192.168.8.50, reported there): a DAEMONIZED selective
     * reload leaves the master unable to reap the worker it adopted -- the
     * reload's fork (fpm_unix.c:596-618) is what loses the parent-child
     * relation -- so its death raises no SIGCHLD, running_children never reaches
     * 0 and a SIGTERM never completes, with the pool's and the operator's
     * listening sockets still held. A leftover then makes the next run of this
     * test, or any other test in the same lane, fail with "Address already in
     * use", which run-tests.php reports as a NOTICE mismatch and reads like a
     * regression. Not measured: that the harness's -F path (tester.inc:528)
     * actually hangs here -- measured, it exits cleanly -- so this is insurance
     * against the daemonized mode rather than a fix for an observed hang.
     *
     * FPM\Tester::close() would also block on a master that ignores SIGTERM:
     * its registered shutdown handler waits for the process to stop running with
     * no timeout (tester.inc:551-560). */
    $errorLogPath = $tester->getPrefixedFile(FPM\Tester::FILE_EXT_LOG_ERR);
    $logText = is_file($errorLogPath) ? (string) file_get_contents($errorLogPath) : '';
    $pids = [$masterPid ?? 0, $masterPidAfter ?? 0, master_pid($tester)];
    if (isset($pidBefore)) {
        $pids[] = $pidBefore;
    }
    /* The operator endpoint is a pool of its own (fpm_pool_type.c:513), and it
     * forks its own child in EVERY generation -- fpm_children.c:615 logs one
     * "child N started" line per generation -- so the live one is the last match
     * and killing only the first would leave the generation still holding the
     * operator socket. All of them go; best effort, since a missing log line
     * costs one leftover rather than a wrong result. */
    if (preg_match_all('/\[pool __operator[^\]]*\] child (\d+) started/', $logText, $m)) {
        foreach ($m[1] as $childPid) {
            $pids[] = (int) $childPid;
        }
    }
    $tester->terminate();
    kill_pids($pids);
    $tester->close();
    @unlink($pidFile);
    @unlink("$root/front.php");
    @rmdir($root);
}

?>
--EXPECT--
before reload: ok
adopted: ok
after reload: ok
still serving: ok
Done
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>
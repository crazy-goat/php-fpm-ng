--TEST--
fpm-ng: supervisor.max_runtime kills an iteration that overruns it and the
pool respawns (issue #326)
--SKIPIF--
<?php include "skipif.inc"; ?>
--ENV--
FPMNG_DEBUG_CLOCK_RATE=10
--FILE--
<?php
/* FPMNG_DEBUG_CLOCK_RATE (issue #396). What this test waits for is
 * supervisor.max_runtime followed by a further supervisor.stop_timeout, and
 * the assertion below reads the duration the MASTER logged, which is measured
 * on the same clock -- so scaling it keeps "~4s, not just max_runtime alone"
 * true while costing a tenth of the wall clock. A master built with
 * --enable-fpmng-debug-clock runs both its clocks -- and the blocking waits
 * derived from them -- this many times faster, which is why the ENV section
 * above sets the rate. Every deadline in this test stays in REAL seconds and
 * stays generous on purpose: with a binary built WITHOUT that flag the
 * variable is ignored, and the test must still pass at real speed rather than
 * skip. That is what keeps it running in the release package gate, where the
 * suite executes on Alpine. */

require_once "tester.inc";

$work = sys_get_temp_dir() . '/fpmng-supmaxrt-' . getmypid();
@mkdir($work, 0700, true);
$runsFile = "$work/runs.log";
@unlink($runsFile);

/* Same reasoning as fpmng-supervisor-restart.phpt (issue #72): append-only, a
 * torn read can only under-count the last partial line, never report zero. */
$cleanup = function () use ($work, $runsFile) {
    @unlink($runsFile);
    @unlink("$work/loop.php");
    @rmdir($work);
};

/* A run this test wants killed, not one that ever finishes -- and, crucially,
 * one that does NOT quietly cooperate with the SIGTERM the max_runtime
 * watchdog sends. A plain sleep(30) is not good enough for that: sleep()
 * returns early (EINTR) the instant our own SIGTERM handler runs, and a naive
 * script would then fall straight through to its next statement and exit(0)
 * quickly on its own, which would only prove the watchdog's signal was
 * delivered -- not that supervisor.stop_timeout's hard SIGKILL fallback
 * actually fires for a script that never notices. Looping past every early
 * wakeup until the deadline is what actually models "ignores the stop
 * signal" without needing pcntl (unavailable in this --disable-all build).
 * The pid is logged BEFORE the loop so the test can tell "the same process is
 * still going" apart from "a fresh process started" without reading anything
 * from the master's log. */
$script = <<<PHP
<?php
error_reporting(0);
@file_put_contents('{$runsFile}', getmypid() . "\\n", FILE_APPEND);
\$deadline = time() + 30;
while (time() < \$deadline) {
    sleep(1);
}
exit(0);
PHP;
file_put_contents("$work/loop.php", $script);

/* max_runtime = 1, stop_timeout = 3: chosen to be far apart from each other
 * (not just both "small") so the two-stage kill this feature composes from
 * (stop_signal at max_runtime, hard SIGKILL only after stop_timeout MORE)
 * is distinguishable from a hypothetical bug that skipped straight to SIGKILL
 * at max_runtime -- that bug would show a ~1s kill, the correct
 * implementation a ~4s one (see the duration check below). */
$cfg = <<<EOT
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
[sup]
pool.type = supervisor
supervisor.script = $work/loop.php
supervisor.processes = 1
supervisor.restart = always
supervisor.restart_delay = 1
supervisor.max_runtime = 1
supervisor.stop_timeout = 3
EOT;

$tester = new FPM\Tester($cfg, $script);
@unlink($tester->getPrefixedFile(FPM\Tester::FILE_EXT_LOG_ERR));

$tester->start([], false);
$tester->switchLogSource('{{FILE:LOG}}');
$tester->expectLogStartNotices();

$errorLog = $tester->getPrefixedFile(FPM\Tester::FILE_EXT_LOG_ERR);

/* fpm_children.c (untouched reference code) logs
 * "[pool sup] child <pid> exited on signal 9 (SIGKILL) after <secs> seconds
 * from start" for any child WIFSIGNALED-ed with SIGKILL -- reading the raw
 * file directly (rather than through the tester's log-consuming LogTool
 * helpers) is what lets this test recover the actual duration and assert on
 * it, not just on the line's presence. */
$killPattern = '/\[pool sup\] child (\d+) exited on signal 9 \(SIGKILL\) after (\d+\.\d+) seconds from start/';
$killedPid = null;
$killDuration = null;
$deadline = time() + 30;
while (time() < $deadline) {
    $log = @file_get_contents($errorLog);
    if (is_string($log) && preg_match($killPattern, $log, $m)) {
        $killedPid = (int) $m[1];
        $killDuration = (float) $m[2];
        break;
    }
    usleep(200000);
}

if ($killedPid === null) {
    echo "FAIL: no '... exited on signal 9 (SIGKILL) ...' line for pool sup within 30s\n";
    echo "runs file: " . (is_file($runsFile) ? file_get_contents($runsFile) : "ABSENT") . "\n";
    echo "--- error log ($errorLog):\n";
    echo is_file($errorLog) ? file_get_contents($errorLog) : "(the error log does not exist)\n";
    echo "--- end of error log\n";
    $tester->close(true);
    $cleanup();
    exit(1);
}

/* supervisor.max_runtime = 1 alone would kill in ~1s; the correct two-stage
 * composition (stop_signal at max_runtime, SIGKILL only after a FURTHER
 * stop_timeout = 3s) kills at ~4s. 2.0s is comfortably above "just
 * max_runtime fired" and comfortably below "~4s plus CI scheduling slack",
 * so this distinguishes the two without being a tight race. */
if ($killDuration < 2.0) {
    echo "FAIL: killed after {$killDuration}s, expected roughly max_runtime (1s) + stop_timeout (3s) =~ 4s, "
        . "not just max_runtime alone\n";
    $tester->close(true);
    $cleanup();
    exit(1);
}

/* fpm_children.c (untouched) respawns unconditionally for pm = static -- the
 * new process is a FRESH one, with its own pid, that starts the loop over
 * from fpm_pool_supervisor_child_main() and logs its own run BEFORE it too
 * gets a chance to overrun max_runtime. */
$respawned = false;
$deadline = time() + 15;
while (time() < $deadline) {
    $data = @file_get_contents($runsFile);
    if (is_string($data)) {
        $pids = array_filter(array_map('intval', explode("\n", trim($data))));
        foreach ($pids as $pid) {
            if ($pid !== $killedPid) {
                $respawned = true;
                break 2;
            }
        }
    }
    usleep(200000);
}

if (!$respawned) {
    echo "FAIL: pool did not respawn a new process after the max_runtime kill\n";
    echo "runs file: " . (is_file($runsFile) ? file_get_contents($runsFile) : "ABSENT") . "\n";
    $tester->close(true);
    $cleanup();
    exit(1);
}

echo "supervisor-max-runtime: ok\n";

/* The current iteration of the respawned process is still sleeping past its
 * own max_runtime; terminate(), like the master's own SIGTERM, is delivered
 * to the master, which asks every pool to stop -- no need to wait out a
 * second kill cycle just to shut this test down cleanly. */
$tester->terminate();
$tester->close();

$cleanup();

?>
Done
--EXPECT--
supervisor-max-runtime: ok
Done
--CLEAN--
<?php
require_once "tester.inc";
FPM\Tester::clean();
$stale = time() - 300;
foreach (glob(sys_get_temp_dir() . '/fpmng-supmaxrt-*') as $dir) {
    if (@filemtime($dir) > $stale) {
        continue;
    }
    foreach (glob("$dir/*") as $file) {
        @unlink($file);
    }
    @rmdir($dir);
}
?>

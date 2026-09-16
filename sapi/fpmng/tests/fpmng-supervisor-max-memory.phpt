--TEST--
fpm-ng: supervisor.max_memory recycles the process without counting as a
failure and without overriding supervisor.restart, using supervisor.stop_signal
to do it (issue #324)
--SKIPIF--
<?php include "skipif.inc"; ?>
--FILE--
<?php

require_once "tester.inc";

$work = sys_get_temp_dir() . '/fpmng-supmem-' . getmypid();
@mkdir($work, 0700, true);
$alwaysRuns = "$work/always-runs.log";
$neverRuns = "$work/never-runs.log";
@unlink($alwaysRuns);
@unlink($neverRuns);

/* Append-only, never rewritten in place, same reasoning as
 * fpmng-supervisor-restart.phpt: a read racing a write can only under-count
 * the last partial line, never report zero (issue #72). */
$cleanup = function () use ($work, $alwaysRuns, $neverRuns) {
    @unlink($alwaysRuns);
    @unlink($neverRuns);
    @unlink("$work/always.php");
    @unlink("$work/never.php");
    @rmdir($work);
};

/* Pool "always": restart = always, exits 0 every time. max_memory = 1 (one
 * byte) always compares true against any real ru_maxrss, so every single
 * iteration recycles the process -- not a realistic limit, just a
 * deterministic and frequent trigger for a short test window.
 *
 * This is the "not counted as a failure" scenario: fpm_pool_supervisor_apply_policy()
 * runs BEFORE the max_memory check (issue #324's fix -- an earlier draft ran
 * it after, which routed the recycle around apply_policy() entirely and could
 * never observe a REAL failure either) and already treats an exit-0 run as no
 * failure regardless of memory, so restart_max is never consumed by the
 * recycle itself: many more runs than restart_max happen, and the pool never
 * "gives up".
 *
 * supervisor.stop_signal = USR1 here (not the default TERM) exercises the
 * OTHER half of the fix: php_request_startup()/php_request_shutdown() reset
 * more than SIGTERM's disposition on every call (Zend/zend_signal.c's
 * zend_sigs[] also lists SIGQUIT, SIGUSR1, SIGUSR2), so fpm_pool_script_run()
 * must save/restore whichever signal supervisor.stop_signal names, in
 * addition to SIGTERM, or a handler installed once before the loop silently
 * stops being the handler starting on the SECOND iteration. If that save/
 * restore were missing, USR1's disposition would already be back to default
 * by the time the SECOND recycle's kill(getpid(), SIGUSR1) fires -- and the
 * default action for SIGUSR1 also terminates the process, so the pool would
 * still visibly restart, but WITHOUT logging the per-run NOTICE below first
 * (that NOTICE is logged BEFORE the kill(), only reached because the
 * previous iteration's kill() was actually caught and returned control to
 * this loop) and without the graceful "Terminating .../exiting, bye-bye!"
 * master-side notices this test asserts on close() below -- both would be
 * replaced by an abrupt, unlogged death. Running well past the first recycle
 * (multiple runs, not just one) is what gives the broken save/restore a
 * chance to show up. */
$alwaysScript = <<<PHP
<?php
error_reporting(0);
@file_put_contents('{$alwaysRuns}', "run\\n", FILE_APPEND);
exit(0);
PHP;
file_put_contents("$work/always.php", $alwaysScript);

/* Pool "never": restart = never, exits 0 once, ALSO over max_memory. This is
 * the supervisor.restart contract regression the fix addresses: an earlier
 * draft checked max_memory before apply_policy() and unconditionally
 * kill()ed + exited on every recycle, so fpm_children.c respawned a fresh
 * process that read shared->terminal == 0 (apply_policy() never ran, so
 * restart = never was never actually applied) and ran the script AGAIN --
 * forever, in direct contradiction of "restart = never". With the fix,
 * apply_policy() runs first, sees restart = never, sets shared->terminal = 1
 * and the process parks (fpm_pool_supervisor_park()) instead of running the
 * script again, regardless of memory. */
$neverScript = <<<PHP
<?php
error_reporting(0);
@file_put_contents('{$neverRuns}', "run\\n", FILE_APPEND);
exit(0);
PHP;
file_put_contents("$work/never.php", $neverScript);

$cfg = <<<EOT
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
log_level = notice
[always]
pool.type = supervisor
supervisor.script = $work/always.php
supervisor.processes = 1
supervisor.restart = always
supervisor.restart_delay = 1
supervisor.restart_max = 2
supervisor.max_memory = 1
supervisor.stop_signal = USR1
[never]
pool.type = supervisor
supervisor.script = $work/never.php
supervisor.processes = 1
supervisor.restart = never
supervisor.max_memory = 1
EOT;

$tester = new FPM\Tester($cfg, $alwaysScript);
@unlink($tester->getPrefixedFile(FPM\Tester::FILE_EXT_LOG_ERR));

$tester->start([], false);
$tester->switchLogSource('{{FILE:LOG}}');
$tester->expectLogStartNotices();

/* "always": every iteration recycles (max_memory = 1), so "well past
 * restart_max" is a handful of runs, not a stress test -- 15 s is a wide
 * margin, as in the sibling restart test. */
$alwaysCount = 0;
$deadline = time() + 15;
while (time() < $deadline) {
    $data = @file_get_contents($alwaysRuns);
    if (is_string($data)) {
        $alwaysCount = max($alwaysCount, substr_count($data, "\n"));
    }
    if ($alwaysCount > 5) {
        break;
    }
    usleep(200000);
}

/* "never": give it the same wall-clock budget to prove a NEGATIVE -- that it
 * does NOT keep restarting. A budget shared with the "always" polling above
 * (rather than an extra fixed sleep) keeps the test's total time bounded by
 * the same 15 s either way. */
$neverCount = 0;
$deadline2 = time() + 15;
while (time() < $deadline2) {
    $data = @file_get_contents($neverRuns);
    if (is_string($data)) {
        $neverCount = max($neverCount, substr_count($data, "\n"));
    }
    if ($neverCount > 1) {
        /* Already violated -- no need to wait out the rest of the budget. */
        break;
    }
    usleep(200000);
}

$errorLog = $tester->getPrefixedFile(FPM\Tester::FILE_EXT_LOG_ERR);
$logText = is_file($errorLog) ? (string) file_get_contents($errorLog) : '';
$recycleLines = substr_count($logText, 'supervisor.max_memory');
$gaveUp = str_contains($logText, 'giving up');

/* Direct regression check for the OTHER bug the fix addresses (review issue
 * #1): fpm_children.c logs "[pool NAME] child PID exited on signal N (...)"
 * (fpm_children.c's WIFSIGNALED branch) only when the child died from a
 * signal its own code never caught -- the exact symptom of the missing
 * stop_signal save/restore in fpm_pool_script_run(): USR1's handler reverts
 * to default disposition after the first php_request_startup()/
 * php_request_shutdown() cycle, so the SECOND self-kill(getpid(), SIGUSR1) in
 * fpm_pool_supervisor_child_main() (fired right after logging the
 * "supervisor.max_memory" NOTICE above) kills the process outright instead of
 * being caught by fpm_pool_supervisor_sigterm() and going through the clean
 * exit(FPM_EXIT_OK) path -- which fpm_children.c logs as "exited with code 0"
 * instead. A recycle that always goes through the caught-signal path must
 * therefore never produce an "on signal" line for the "always" pool, whether
 * or not it also happens to keep producing enough runs (a fresh respawned
 * process runs the script again regardless of HOW the previous one died, so
 * $alwaysCount alone cannot tell the two apart -- this is what makes this
 * assertion, not just the run count, the one that actually catches review
 * issue #1). */
$alwaysSignaled = (bool) preg_match('/\[pool always\][^\n]*on signal/', $logText);

$ok = $alwaysCount > 5 && $recycleLines >= 1 && !$gaveUp && $neverCount === 1 && !$alwaysSignaled;

if (!$ok) {
    echo "FAIL: supervisor-max-memory always-runs=$alwaysCount recycle-notices=$recycleLines "
        . "gave-up=" . ($gaveUp ? 'yes' : 'no') . " never-runs=$neverCount (want exactly 1) "
        . "always-signaled=" . ($alwaysSignaled ? 'yes (BAD: uncaught signal death)' : 'no') . "\n";

    foreach (['always' => $alwaysRuns, 'never' => $neverRuns] as $label => $file) {
        if (is_file($file)) {
            printf("%s runs file: present, %d bytes, first 200 bytes %s\n",
                $label, filesize($file),
                var_export(substr((string) file_get_contents($file), 0, 200), true));
        } else {
            echo "$label runs file: ABSENT\n";
        }
    }

    $keepLast = 80;
    echo "--- error log ($errorLog):\n";
    if ($logText !== '') {
        $lines = explode("\n", $logText);
        if (count($lines) > $keepLast) {
            $lines = array_slice($lines, -$keepLast);
        }
        echo implode("\n", $lines);
        echo "\n--- end of error log\n";
    } else {
        echo "(the error log does not exist or is empty)\n--- end of error log\n";
        $tester->printLogs();
    }

    $tester->close(true);
    $cleanup();
    exit(1);
}
echo "supervisor-max-memory: ok\n";

/* This still uses the default SIGTERM path to stop the master's children
 * (Tester::terminate() signals the MASTER, not either pool's stop_signal) --
 * supervisor.stop_signal only governs how a pool's own child is asked to
 * stop, not master-driven shutdown, which the master always escalates through
 * its own SIGTERM/SIGQUIT regardless of any pool's stop_signal (see
 * docs/supervisor.md). The "never" pool is parked, not running a script, but
 * still responds to the master's own SIGTERM the same as any other child. */
$tester->terminate();
$tester->expectLogTerminatingNotices();
$tester->close();

$cleanup();

?>
Done
--EXPECT--
supervisor-max-memory: ok
Done
--CLEAN--
<?php
require_once "tester.inc";
FPM\Tester::clean();
$stale = time() - 300;
foreach (glob(sys_get_temp_dir() . '/fpmng-supmem-*') as $dir) {
    if (@filemtime($dir) > $stale) {
        continue;
    }
    foreach (glob("$dir/*") as $file) {
        @unlink($file);
    }
    @rmdir($dir);
}
?>

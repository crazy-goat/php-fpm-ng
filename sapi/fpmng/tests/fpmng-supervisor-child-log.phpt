--TEST--
fpm-ng: a supervisor pool's own messages reach error_log without catch_workers_output (issue #121)
--SKIPIF--
<?php include "skipif.inc"; ?>
--FILE--
<?php

require_once "tester.inc";

$work = sys_get_temp_dir() . '/fpmng-suplog-' . getmypid();
@mkdir($work, 0700, true);

$cleanup = function () use ($work) {
    foreach (glob("$work/*") as $file) {
        @unlink($file);
    }
    @rmdir($work);
};

/* Exits non-zero at once, which is the whole input this test needs: the
 * messages under test are the SUPERVISOR's account of that exit, not anything
 * the script itself writes anywhere. */
file_put_contents("$work/fail.php", "<?php\nexit(3);\n");

/* Deliberately never created — a supervisor.script that does not exist is not
 * a configuration error (fpm_pool_supervisor_validate() only requires the
 * directive to be set), so the failure surfaces at run time, in the child, as
 * "cannot open script". */
$missing = "$work/missing.php";

/* No catch_workers_output anywhere in this configuration: that it was needed
 * at all was issue #121. supervisor.restart_delay = 30 on the backoff pool so
 * that exactly ONE failure is logged and the pool then sits in backoff for the
 * rest of the test — a fast loop would keep appending to the log while the
 * assertions below read it. */
$cfg = <<<EOT
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
daemonize = no
[backoff]
pool.type = supervisor
supervisor.script = $work/fail.php
supervisor.processes = 1
supervisor.restart = always
supervisor.restart_delay = 30
[gone]
pool.type = supervisor
supervisor.script = $missing
supervisor.processes = 1
supervisor.restart = never
[giveup]
pool.type = supervisor
supervisor.script = $work/fail.php
supervisor.processes = 1
supervisor.restart = always
supervisor.restart_delay = 30
supervisor.restart_max = 1
EOT;

$tester = new FPM\Tester($cfg);

/* forceStderr = false + the log in a file: the assertions below read the same
 * error_log an operator would, which is the behaviour under test. */
@unlink($tester->getPrefixedFile(FPM\Tester::FILE_EXT_LOG_ERR));
$tester->start([], false);
$tester->switchLogSource('{{FILE:LOG}}');
$tester->expectLogStartNotices();

/* Criterion 1 and 2: the pool's own NOTICE and the failed script open, at
 * their own levels. The patterns pin the LEVEL as well as the text, because
 * arriving as a master-side WARNING "child N said into stderr: ..." — which is
 * what catch_workers_output = yes produced before this fix — is exactly the
 * failure mode this test exists to catch. */
/* The "(child N)" suffix is asserted here and nowhere else: with
 * supervisor.processes = 1 it is redundant, but the channel is per POOL, so it
 * is the only thing that tells four children of one pool apart
 * (fpm_child_log.c, FPM_CHILD_LOG_HDR). */
$tester->expectLogPattern(
    '/NOTICE: .*\[pool backoff\] supervisor: script exited \(code 3\) after \d+s, '
        . 'restarting in 30s \(failure 1\/unlimited\) \(child \d+\)/',
    true,
    10
);
echo "backoff notice: logged\n";

$tester->expectLogPattern(
    '/ERROR: .*\[pool gone\] cannot open script \'' . preg_quote($missing, '/') . '\'/',
    true,
    10
);
echo "cannot open script: logged\n";

$tester->expectLogPattern(
    '/ALERT: .*\[pool giveup\] supervisor: 1 consecutive failures, giving up '
        . '\(supervisor\.restart_max = 1\)/',
    true,
    10
);
echo "restart_max alert: logged\n";

/* Criterion 2, the other half: nothing here went through the
 * catch_workers_output path, so no message may be wrapped. */
$tester->expectNoLogPattern('/said into std(out|err)/', true);
echo "not wrapped: ok\n";

/* No expectLogTerminatingNotices(): with the fix, three supervisor pools log
 * their own lines whenever they feel like it, including between "Terminating
 * ..." and "exiting, bye-bye!", and LogTool's terminator expectation forgives
 * only DEBUG lines in between. What shutdown logs is fpmng-shutdown-timeout-warnings.phpt's
 * subject, not this test's. */
$tester->close(true);

$cleanup();

?>
Done
--EXPECT--
backoff notice: logged
cannot open script: logged
restart_max alert: logged
not wrapped: ok
Done
--CLEAN--
<?php
require_once "tester.inc";
FPM\Tester::clean();
/* Same age-based sweep as fpmng-supervisor-restart.phpt: these directories are
 * named after the pid of the --FILE-- process, which this one does not know,
 * and a run that never reached its own cleanup (Ctrl-C, --no-clean) leaves one
 * behind. Age, not pid: run-tests.php may be running another copy of this test
 * in parallel and its directory must not be touched. */
$stale = time() - 300;
foreach (glob(sys_get_temp_dir() . '/fpmng-suplog-*') as $dir) {
    if (@filemtime($dir) > $stale) {
        continue;
    }
    foreach (glob("$dir/*") as $file) {
        @unlink($file);
    }
    @rmdir($dir);
}
?>

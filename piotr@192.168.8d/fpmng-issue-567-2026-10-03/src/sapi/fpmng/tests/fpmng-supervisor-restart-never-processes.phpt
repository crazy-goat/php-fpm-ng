--TEST--
fpm-ng: supervisor restart = never with processes > 1 runs the script once per copy (issue #347)
--SKIPIF--
<?php include "skipif.inc"; ?>
--FILE--
<?php

require_once "tester.inc";

$work = sys_get_temp_dir() . '/fpmng-sup347-' . getmypid();
@mkdir($work, 0700, true);
$runsFile = "$work/runs.log";
@unlink($runsFile);

/* --CLEAN-- cannot do this: Tester::clean() only globs the harness's own
 * test-prefixed files, and this directory is named after the pid of THIS
 * process, which the clean process does not have. */
$cleanup = function () use ($work, $runsFile) {
    @unlink($runsFile);
    @unlink("$work/once.php");
    @rmdir($work);
};

/* One APPENDED line per run, and the line is the runner's PID. The bug this
 * test pins (issue #347) is that the first copy to finish set a pool-wide
 * terminal flag, so with processes = 4 and restart = never only ONE copy ever
 * ran. Counting distinct PIDs is therefore the whole assertion: before the fix
 * the file holds one line, after it four, and a line is only written by a
 * process that actually executed the script. */
$script = <<<PHP
<?php
error_reporting(0);
@file_put_contents('{$runsFile}', getmypid() . "\\n", FILE_APPEND);
exit(0);
PHP;
file_put_contents("$work/once.php", $script);

$cfg = <<<EOT
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
daemonize = no
[once]
pool.type = supervisor
supervisor.script = $work/once.php
supervisor.processes = 4
supervisor.restart = never
; The bug is a start-order race: the first copy to finish set a pool-wide
; terminal flag, and every copy still inside its cold-start delay then saw it
; and parked without ever running. Without a stagger all four copies run
; concurrently and the race never bites (which is why this test would pass on
; the broken code with no jitter), so the jitter is what makes it a regression
; test rather than a coin flip.
supervisor.start_jitter = 3
EOT;

$tester = new FPM\Tester($cfg, $script);
@unlink($tester->getPrefixedFile(FPM\Tester::FILE_EXT_LOG_ERR));

$tester->start([], false);
$tester->switchLogSource('{{FILE:LOG}}');
$tester->expectLogStartNotices();

/* Four forks and four PHP request startups after the startup notices; poll
 * rather than failing the first attempt. 15 s is a very wide margin for a
 * script whose only statement is one append. */
$pids = [];
$deadline = time() + 15;
while (time() < $deadline) {
    $data = @file_get_contents($runsFile);
    if (is_string($data) && $data !== '') {
        $pids = array_unique(array_filter(explode("\n", $data), 'strlen'));
    }
    if (count($pids) >= 4) {
        break;
    }
    usleep(200000);
}

if (count($pids) !== 4) {
    printf("FAIL: supervisor restart = never with processes = 4 ran %d distinct copy(ies), expected 4\n", count($pids));
    echo "--- error log:\n";
    $tester->printLogs();
    $tester->close(true);
    $cleanup();
    exit(1);
}
echo "supervisor-restart-never: 4 copies, 4 distinct pids\n";

/* Issue #492 review: the pool-wide count must reach 4, not stop at 3. The old
 * shared counter was a plain read-modify-write, so two copies finishing in the
 * same microsecond could both read the same value and both store +1; the pool
 * then stayed one short of processes and terminal was never set (status stuck
 * in "backoff" forever). The count is now derived by scanning the per-copy
 * bytes, and the log line prints that scan, so "4 of 4" is the observable
 * invariant that every completion was counted and the pool reached FINISHED.
 * The lost update itself is not deterministically forceable from a test (the
 * window is a few instructions wide); this asserts the invariant instead. */
$tester->expectLogPattern('/this copy is done: 4 of 4/');
echo "supervisor-restart-never: pool counted all 4 copies done\n";

/* restart = never means the pool is FINISHED once every copy has run, and the
 * copies park instead of exiting, so the master must still shut down cleanly. */
$tester->terminate();
$tester->expectLogTerminatingNotices();
$tester->close();

$cleanup();

?>
Done
--EXPECT--
supervisor-restart-never: 4 copies, 4 distinct pids
supervisor-restart-never: pool counted all 4 copies done
Done
--CLEAN--
<?php
require_once "tester.inc";
FPM\Tester::clean();
/* Whatever the run above leaked: a start() failure or a thrown expectation
 * skips the cleanup in --FILE--, and these directories are named after a pid
 * this process does not know. Age check, not a pid check -- run-tests.php may
 * run another copy of this test in parallel and that directory must not be
 * touched. */
$stale = time() - 300;
foreach (glob(sys_get_temp_dir() . '/fpmng-sup347-*') as $dir) {
    if (@filemtime($dir) > $stale) {
        continue;
    }
    foreach (glob("$dir/*") as $file) {
        @unlink($file);
    }
    @rmdir($dir);
}
?>

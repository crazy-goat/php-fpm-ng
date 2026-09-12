--TEST--
fpm-ng: a supervisor script that returns at once is warned about, once (issue #122)
--SKIPIF--
<?php include "skipif.inc"; ?>
--FILE--
<?php

require_once "tester.inc";

/* supervisor.restart = always restarts a script that exits 0 with no delay at
 * all -- the script sets the pace, deliberately. Issue #122 measured what that
 * costs when the script returns instead of looping (12086 runs/s, one whole
 * core, nothing in the log) and decided to warn about it rather than throttle
 * it. So what this asserts is the warning, and that nothing else changed: the
 * pool is still restarting the script afterwards, at the same pace. */

$work = sys_get_temp_dir() . '/fpmng-fast-' . getmypid();
@mkdir($work, 0700, true);

$cleanup = function () use ($work) {
    @unlink("$work/nothing.php");
    @rmdir($work);
};

/* The whole point of the test: a script that does nothing and returns. Nothing
 * is written per run, because writing is the one thing that would make a run
 * take longer than the 5 ms threshold on a slow disk. */
file_put_contents("$work/nothing.php", "<?php\n/* returns at once, on purpose */\n");

$cfg = <<<EOT
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
[spin]
pool.type = supervisor
supervisor.script = $work/nothing.php
supervisor.processes = 1
supervisor.restart = always
EOT;

$tester = new FPM\Tester($cfg, '<?php');

/* Same reasoning as fpmng-supervisor-restart.phpt: -O would send the master's
 * log to stderr and leave error_log unwritten, and the file has to be read
 * twice here -- once by the matcher, once at the end to count the warnings --
 * so it must be a file, and it must not be a previous run's file. */
@unlink($tester->getPrefixedFile(FPM\Tester::FILE_EXT_LOG_ERR));

$tester->start([], false);
$tester->switchLogSource('{{FILE:LOG}}');
$tester->expectLogStartNotices();

/* 1000 runs at even a hundredth of the measured rate is well inside this. */
$tester->expectLogPattern('/supervisor: \d+ consecutive runs of .* finished in under \d+ms each/', false, 30);
echo "warned: ok\n";

/* Once, not once per streak-length: the warning latches until a run does some
 * work, and this script never will. Give it a moment to prove it -- at the
 * measured pace that is another ~12000 runs, i.e. twelve more warnings if the
 * latch did not hold. */
usleep(1000000);

$tester->terminate();
$tester->expectLogTerminatingNotices();

$log = (string) @file_get_contents($tester->getPrefixedFile(FPM\Tester::FILE_EXT_LOG_ERR));
$warnings = preg_match_all('/consecutive runs of .* finished in under/', $log);
if ($warnings !== 1) {
    echo "FAIL: expected exactly one warning, got $warnings\n";
} else {
    echo "warned once: ok\n";
}

$tester->close();
$cleanup();

?>
Done
--EXPECT--
warned: ok
warned once: ok
Done
--CLEAN--
<?php
require_once "tester.inc";
FPM\Tester::clean();
?>

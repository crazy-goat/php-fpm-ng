--TEST--
fpm-ng: cron.log is opened by the master, so a run still appends after the pool user loses write access (issue #775)
--SKIPIF--
<?php
include "skipif.inc";
?>
--ENV--
FPMNG_DEBUG_CLOCK_RATE=60
TEST_TIMEOUT=180
--FILE--
<?php
/* The test runs as an unprivileged user, so it cannot create a root-owned log
 * directory. chmod 0444 on the log file, done after the master has started,
 * reproduces the same failure: a per-run open() by the pool child fails with
 * EACCES, while a file descriptor the master opened at start does not need
 * a new open() at all. */
require_once "tester.inc";

$work = sys_get_temp_dir() . '/fpmng-cron-log-master-' . getmypid();
@mkdir($work, 0700, true);
file_put_contents("$work/run.php", "<?php\n");
$logPath = "$work/cron.log";
file_put_contents($logPath, "");

$cleanup = function () use ($work, $logPath) {
    @chmod($logPath, 0644);
    @unlink($logPath);
    @unlink("$work/run.php");
    @rmdir($work);
};

$cfg = <<<EOT
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
log_level = notice
[runs]
pool.type = cron
cron.schedule = * * * * *
cron.script = $work/run.php
cron.log = $logPath
EOT;

$tester = new FPM\Tester($cfg, '<?php');
@unlink($tester->getPrefixedFile(FPM\Tester::FILE_EXT_LOG_ERR));
$tester->start([], false);
$tester->switchLogSource('{{FILE:LOG}}');
$tester->expectLogStartNotices();

if (!chmod($logPath, 0444)) {
    echo "FAIL: cannot make the log file read-only for the pool user\n";
    $tester->close(true);
    $cleanup();
    exit(1);
}

$errorLog = $tester->getPrefixedFile(FPM\Tester::FILE_EXT_LOG_ERR);
$deadline = microtime(true) + 150;
do {
    $run = (string) @file_get_contents($logPath);
    if (preg_match('/exit=0 duration=/', $run)) {
        break;
    }
    usleep(100000);
} while (microtime(true) < $deadline);

$log = (string) @file_get_contents($errorLog);
if (!preg_match('/exit=0 duration=/', $run)) {
    echo "FAIL: no run line was appended to the read-only cron.log\n";
    echo "cron.log:\n$run\n";
    echo "error_log:\n$log";
    $tester->close(true);
    $cleanup();
    exit(1);
}
if (str_contains($log, 'cron.log: cannot open')) {
    echo "FAIL: the pool child tried to open cron.log again\n";
    echo $log;
    $tester->close(true);
    $cleanup();
    exit(1);
}
echo "run-line-appended: ok\n";

$tester->terminate();
$tester->expectLogTerminatingNotices();
$tester->close();
$cleanup();
?>
--EXPECT--
run-line-appended: ok
--CLEAN--
<?php
require_once "tester.inc";
FPM\Tester::clean();
$stale = time() - 300;
foreach (glob(sys_get_temp_dir() . '/fpmng-cron-log-master-*') as $dir) {
    if (@filemtime($dir) > $stale) {
        continue;
    }
    foreach (glob("$dir/*") as $file) {
        @unlink($file);
    }
    @rmdir($dir);
}
?>

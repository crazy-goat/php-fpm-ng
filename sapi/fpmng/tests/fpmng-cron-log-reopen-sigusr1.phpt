--TEST--
fpm-ng: cron.log is reopened on SIGUSR1, so the next run lands in the new file after logrotate (issue #775)
--SKIPIF--
<?php
include "skipif.inc";
?>
--ENV--
FPMNG_DEBUG_CLOCK_RATE=60
TEST_TIMEOUT=180
--FILE--
<?php
/* The master holds cron.log open from startup, so a rename alone would keep
 * every later run in the renamed file. logrotate's postrotate sends SIGUSR1,
 * which must make the master open the path again. The test does the two steps
 * logrotate does, then checks where the runs go. */
require_once "tester.inc";

$work = sys_get_temp_dir() . '/fpmng-cron-log-reopen-' . getmypid();
@mkdir($work, 0700, true);
file_put_contents("$work/run.php", "<?php\n");
$logPath = "$work/cron.log";
$rotated = "$work/cron.log.rotated";
file_put_contents($logPath, "");

$cleanup = function () use ($work, $logPath, $rotated) {
    @unlink($logPath);
    @unlink($rotated);
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

function runLines(string $file): int
{
    return preg_match_all('/exit=0 duration=/', (string) @file_get_contents($file));
}

function waitFor(callable $done, float $seconds = 150.0): bool
{
    $deadline = microtime(true) + $seconds;
    do {
        if ($done()) {
            return true;
        }
        usleep(100000);
    } while (microtime(true) < $deadline);

    return false;
}

$tester = new FPM\Tester($cfg, '<?php');
@unlink($tester->getPrefixedFile(FPM\Tester::FILE_EXT_LOG_ERR));
$tester->start([], false);
$tester->switchLogSource('{{FILE:LOG}}');
$tester->expectLogStartNotices();

$errorLog = $tester->getPrefixedFile(FPM\Tester::FILE_EXT_LOG_ERR);
$fail = function (string $message) use ($tester, $cleanup, $errorLog, $logPath, $rotated) {
    echo "FAIL: $message\n";
    echo "cron.log:\n" . (string) @file_get_contents($logPath) . "\n";
    echo "cron.log.rotated:\n" . (string) @file_get_contents($rotated) . "\n";
    echo "error_log:\n" . (string) @file_get_contents($errorLog);
    $tester->close(true);
    $cleanup();
    exit(1);
};

if (!waitFor(fn () => runLines($logPath) >= 1)) {
    $fail('no run line before the rotation');
}

if (!rename($logPath, $rotated)) {
    $fail('cannot rename cron.log');
}
$tester->reloadLogs();

if (!waitFor(fn () => preg_match('/\[pool runs\] cron\.log re-opened/', (string) @file_get_contents($errorLog)))) {
    $fail('the master did not report reopening cron.log');
}
echo "cron-log-reopened: ok\n";

if (!waitFor(fn () => runLines($logPath) >= 1)) {
    $fail('no run line in the reopened cron.log');
}
echo "next-run-in-reopened-file: ok\n";

/* A run started before SIGUSR1 can still write to the renamed file, once. No
 * run can write there after the first line in the new file: pm = static
 * with max_children = 1 forks the next run only after the previous one has
 * exited. So the size of the renamed file, read now, must not change. */
$rotatedSize = filesize($rotated);
if (!waitFor(fn () => runLines($logPath) >= 2)) {
    $fail('no second run line in the reopened cron.log');
}
clearstatcache();
if (filesize($rotated) !== $rotatedSize) {
    $fail('a run after the reopen still wrote to the renamed cron.log');
}
echo "renamed-file-not-written-after: ok\n";

$tester->terminate();
$tester->expectLogTerminatingNotices();
$tester->close();
$cleanup();
?>
--EXPECT--
cron-log-reopened: ok
next-run-in-reopened-file: ok
renamed-file-not-written-after: ok
--CLEAN--
<?php
require_once "tester.inc";
FPM\Tester::clean();
$stale = time() - 300;
foreach (glob(sys_get_temp_dir() . '/fpmng-cron-log-reopen-*') as $dir) {
    if (@filemtime($dir) > $stale) {
        continue;
    }
    foreach (glob("$dir/*") as $file) {
        @unlink($file);
    }
    @rmdir($dir);
}
?>

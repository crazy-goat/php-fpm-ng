--TEST--
fpm-ng: cron.log open and write failures identify their pool (issue #355)
--SKIPIF--
<?php
include "skipif.inc";
if (!file_exists('/dev/full')) {
    die('skip /dev/full is required to trigger a deterministic write failure');
}
?>
--ENV--
FPMNG_DEBUG_CLOCK_RATE=60
--FILE--
<?php
/* /dev/full opens successfully but fails writes with ENOSPC, so together with
 * a path below a nonexistent directory it drives both failure branches in
 * fpm_pool_cron_log_run() without permissions, disk pressure, or shared state.
 * Both cron jobs use every-minute schedules accelerated by the debug clock. */
require_once "tester.inc";

$work = sys_get_temp_dir() . '/fpmng-cron-log-failure-' . getmypid();
@mkdir($work, 0700, true);
file_put_contents("$work/open.php", "<?php\n");
file_put_contents("$work/write.php", "<?php\n");
$openPath = "$work/missing/cron.log";

$cleanup = function () use ($work) {
    @unlink("$work/open.php");
    @unlink("$work/write.php");
    @rmdir($work);
};

$cfg = <<<EOT
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
log_level = notice
[openfail]
pool.type = cron
cron.schedule = * * * * *
cron.script = $work/open.php
cron.log = $openPath
[writefail]
pool.type = cron
cron.schedule = * * * * *
cron.script = $work/write.php
cron.log = /dev/full
EOT;

$tester = new FPM\Tester($cfg, '<?php');
@unlink($tester->getPrefixedFile(FPM\Tester::FILE_EXT_LOG_ERR));
$tester->start([], false);
$tester->switchLogSource('{{FILE:LOG}}');
$tester->expectLogStartNotices();

$errorLog = $tester->getPrefixedFile(FPM\Tester::FILE_EXT_LOG_ERR);
$deadline = microtime(true) + 20;
do {
    $log = (string) @file_get_contents($errorLog);
    $openLogged = preg_match('/\[pool openfail\] cron\.log: cannot open/', $log);
    $writeLogged = preg_match('/\[pool writefail\] cron\.log: write to/', $log);
    if ($openLogged && $writeLogged) {
        break;
    }
    usleep(100000);
} while (microtime(true) < $deadline);

if (!str_contains($log, "[pool openfail] cron.log: cannot open '$openPath' (")) {
    echo "FAIL: open failure did not identify pool and path\n";
    echo $log;
    $tester->close(true);
    $cleanup();
    exit(1);
}
if (!str_contains($log, "[pool writefail] cron.log: write to '/dev/full' failed (")) {
    echo "FAIL: write failure did not identify pool and path\n";
    echo $log;
    $tester->close(true);
    $cleanup();
    exit(1);
}
echo "open-failure-pool-prefix: ok\n";
echo "write-failure-pool-prefix: ok\n";

$tester->terminate();
$tester->expectLogTerminatingNotices();
$tester->close();
$cleanup();
?>
--EXPECT--
open-failure-pool-prefix: ok
write-failure-pool-prefix: ok
--CLEAN--
<?php
require_once "tester.inc";
FPM\Tester::clean();
$stale = time() - 300;
foreach (glob(sys_get_temp_dir() . '/fpmng-cron-log-failure-*') as $dir) {
    if (@filemtime($dir) > $stale) {
        continue;
    }
    foreach (glob("$dir/*") as $file) {
        @unlink($file);
    }
    @rmdir($dir);
}
?>

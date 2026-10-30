--TEST--
fpm-ng: a child of another pool does not inherit the cron.log descriptor of a cron pool (issue #775)
--SKIPIF--
<?php
include "skipif.inc";
if (!is_dir('/proc/self/fd')) {
    die('skip /proc/self/fd is required to list the descriptors of a child');
}
?>
--FILE--
<?php
/* The master opens cron.log at startup, and fork() copies that descriptor
 * into every child. A fastcgi child of another pool must close it
 * (the cron type's close hook, run by
 * fpm_pool_type_close_master_logs_in_child()). The child counts the descriptors
 * that point at the cron.log path. The web pool is forked at startup, after
 * the master opened the file, so the check does not depend on timing. */
require_once "tester.inc";

$work = sys_get_temp_dir() . '/fpmng-cron-log-inherit-' . getmypid();
@mkdir($work, 0700, true);
file_put_contents("$work/run.php", "<?php\n");
$logPath = "$work/cron.log";
file_put_contents($logPath, "");
$logPath = realpath($logPath);

$cleanup = function () use ($work, $logPath) {
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
[www]
listen = {{ADDR}}
pm = static
pm.max_children = 1
EOT;

$code = '<?php'
    . ' $target = ' . var_export($logPath, true) . ';'
    . ' $n = 0;'
    . ' foreach (glob("/proc/self/fd/*") as $fd) {'
    . '     if (@readlink($fd) === $target) { $n++; }'
    . ' }'
    . ' echo "cron-log-fds:$n";';

$tester = new FPM\Tester($cfg, $code);
$tester->start();
$tester->expectLogStartNotices();

$tester->request()->expectBody('cron-log-fds:0', skipHeadersCheck: true);
echo "cron-log-not-inherited: ok\n";

$tester->terminate();
$tester->expectLogTerminatingNotices();
$tester->close();
$cleanup();
?>
--EXPECT--
cron-log-not-inherited: ok
--CLEAN--
<?php
require_once "tester.inc";
FPM\Tester::clean();
$stale = time() - 300;
foreach (glob(sys_get_temp_dir() . '/fpmng-cron-log-inherit-*') as $dir) {
    if (@filemtime($dir) > $stale) {
        continue;
    }
    foreach (glob("$dir/*") as $file) {
        @unlink($file);
    }
    @rmdir($dir);
}
?>

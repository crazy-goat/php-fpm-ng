--TEST--
fpm-ng: supervisor.max_memory at or below the master's baseline RSS is warned about at startup (issue #350)
--SKIPIF--
<?php include "skipif.inc"; ?>
--FILE--
<?php

require_once "tester.inc";

/* issue #350: a supervisor.max_memory at or below what the master already
 * holds is not a budget, it is a guaranteed recycle loop -- a child forks from
 * the master and starts at roughly the master's resident set, so its first
 * iteration is over budget every time. The guard is a startup WARNING, not a
 * rejection (ru_maxrss is a high-water mark and could overstate steady-state
 * RSS), so what this asserts is the warning for the too-low pool and its
 * absence for the comfortably-high one.
 *
 * 1 byte is below any real baseline; 1 GiB is above it on any runner. Only the
 * relation to the measured baseline matters, and the warning prints both
 * numbers itself. restart = never keeps both pools from spinning: each runs
 * the script once and parks. */

$work = sys_get_temp_dir() . '/fpmng-memfloor-' . getmypid();
@mkdir($work, 0700, true);

$cleanup = function () use ($work) {
    @unlink("$work/noop.php");
    @rmdir($work);
};

file_put_contents("$work/noop.php", "<?php\n");

$cfg = <<<EOT
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
[low]
pool.type = supervisor
supervisor.script = $work/noop.php
supervisor.processes = 1
supervisor.restart = never
supervisor.max_memory = 1
[high]
pool.type = supervisor
supervisor.script = $work/noop.php
supervisor.processes = 1
supervisor.restart = never
supervisor.max_memory = 1G
EOT;

$tester = new FPM\Tester($cfg, '<?php');
@unlink($tester->getPrefixedFile(FPM\Tester::FILE_EXT_LOG_ERR));

$tester->start([], false);
$tester->switchLogSource('{{FILE:LOG}}');
$tester->expectLogStartNotices();

$tester->terminate();
$tester->expectLogTerminatingNotices();

/* Counted from the file rather than with expectLogPattern: the warning is
 * logged during pool init, before the start notices expectLogStartNotices()
 * has already read past. The recycle NOTICE also contains the string
 * "supervisor.max_memory", so a count of exactly 1 for pool low is also
 * positive evidence that the pool did NOT recycle -- restart = never parks it
 * first (issue #347). */
$log = (string) @file_get_contents($tester->getPrefixedFile(FPM\Tester::FILE_EXT_LOG_ERR));
$low = preg_match_all('/\[pool low\] supervisor\.max_memory/', $log);
$high = preg_match_all('/\[pool high\] supervisor\.max_memory/', $log);

if ($low !== 1) {
    echo "FAIL: expected exactly one warning for pool low, got $low\n";
} elseif ($high !== 0) {
    echo "FAIL: pool high (1G, above the baseline) was warned about $high time(s)\n";
} else {
    echo "warned for the low pool, silent for the high one: ok\n";
}

$tester->close();
$cleanup();

?>
Done
--EXPECT--
warned for the low pool, silent for the high one: ok
Done
--CLEAN--
<?php
require_once "tester.inc";
FPM\Tester::clean();
$stale = time() - 300;
foreach (glob(sys_get_temp_dir() . '/fpmng-memfloor-*') as $dir) {
    if (@filemtime($dir) > $stale) {
        continue;
    }
    foreach (glob("$dir/*") as $file) {
        @unlink($file);
    }
    @rmdir($dir);
}
?>

--TEST--
fpm-ng: cron.expect_within logs stale without an operator page scrape (issue #357)
--SKIPIF--
<?php include "skipif.inc"; ?>
--ENV--
FPMNG_DEBUG_CLOCK_RATE=60
TEST_TIMEOUT=60
--FILE--
<?php
require_once "tester.inc";

$work = sys_get_temp_dir() . '/fpmng-cron-stale-no-scrape-' . getmypid();
@mkdir($work, 0700, true);
$marker = "$work/runs.log";
@unlink($marker);

/* With the test clock at 60x, a schedule minute and its one-second
 * cron.expect_within grace elapse in about one real second. The same run is
 * kept alive well beyond that threshold so the master timer, not a scrape,
 * must notice the stale episode. */
$script = <<<PHP
<?php
file_put_contents('{$marker}', "run\\n", FILE_APPEND);
sleep(10);
PHP;
file_put_contents("$work/job.php", $script);

$cfg = <<<EOT
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
daemonize = no
log_level = notice
[tick]
pool.type = cron
cron.schedule = * * * * *
cron.script = $work/job.php
cron.expect_within = 1
EOT;

$tester = new FPM\Tester($cfg, $script);
try {
    $tester->start(extraArgs: ['-R'], forceStderr: true, daemonize: false);
    $tester->expectLogStartNotices();

    $pattern = '/WARNING: .*\[pool tick\] cron: stale -- the schedule\'s next run after the last one was '
        . 'due at \d+, and it is now more than cron\.expect_within = 1s past that/';
    $tester->expectLogPattern($pattern, false, 8);
    echo "stale warning without scrape: ok\n";

    /* The child is still in its first ten-second run; timer ticks must not
     * spam the same stale episode. The reader has passed the first match, so
     * this checks only for an additional warning in the next interval. */
    $tester->expectNoLogPattern($pattern, false, null, 1500000);
    if (substr_count((string) @file_get_contents($marker), "run\n") !== 1) {
        throw new RuntimeException('the timer must not start or catch up cron runs');
    }
    echo "one warning, no timer-triggered run: ok\n";
    echo "Done\n";
} finally {
    $tester->terminate();
    $tester->close();
    @unlink($marker);
    @unlink("$work/job.php");
    @rmdir($work);
}
?>
--EXPECT--
stale warning without scrape: ok
one warning, no timer-triggered run: ok
Done
--CLEAN--
<?php
require_once "tester.inc";
FPM\Tester::clean();
$stale = time() - 300;
foreach (glob(sys_get_temp_dir() . '/fpmng-cron-stale-no-scrape-*') as $dir) {
    if (@filemtime($dir) > $stale) {
        continue;
    }
    foreach (glob("$dir/*") as $file) {
        @unlink($file);
    }
    @rmdir($dir);
}
?>

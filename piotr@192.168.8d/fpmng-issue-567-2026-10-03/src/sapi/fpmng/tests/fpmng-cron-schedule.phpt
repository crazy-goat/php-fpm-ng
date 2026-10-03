--TEST--
fpm-ng: cron pool runs its script on schedule (docs/cron.md, docs/NOTES.md §3r)
--SKIPIF--
<?php include "skipif.inc"; ?>
--ENV--
FPMNG_DEBUG_CLOCK_RATE=10
--FILE--
<?php

/* FPMNG_DEBUG_CLOCK_RATE above (issue #396): the schedule below is "* * * * *"
 * and cron's granularity is one minute, so this test used to spend a whole
 * real minute -- 40.0 s of the owned suite's 266, the second-largest single
 * entry in slow.tsv -- waiting for a tick. At rate 10 that tick arrives in
 * about six real seconds.
 *
 * Safe here because the only assertion is that the marker exists and is not
 * empty. The job script does write gmdate('c') into it, and PHP's clock in
 * that script is the REAL one -- only the master's C code is scaled -- but
 * nothing ever compares that timestamp to anything. A test that did compare it
 * against a configured interval could not be accelerated this way; see the
 * virtual clock section of docs/fpmng-phpt.md, and fpmng-cron-jitter.phpt,
 * which is exactly that case and is deliberately left at real speed.
 *
 * The 75-second deadline below stays in REAL seconds and stays untouched, so a
 * binary built without --enable-fpmng-debug-clock ignores the variable and this
 * test still passes, one real minute at a time. */
require_once "tester.inc";

$work = sys_get_temp_dir() . '/fpmng-cron-' . getmypid();
@mkdir($work, 0700, true);
$marker = "$work/runs.log";
@unlink($marker);

$script = <<<PHP
<?php
file_put_contents('{$marker}', getmypid() . ' ' . gmdate('c') . "\n", FILE_APPEND);
PHP;
file_put_contents("$work/job.php", $script);

$cfg = <<<EOT
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
daemonize = no
[job]
pool.type = cron
cron.schedule = * * * * *
cron.script = $work/job.php
EOT;

$tester = new FPM\Tester($cfg, $script);
$tester->start(extraArgs: ['-R'], forceStderr: true, daemonize: false);
$tester->expectLogStartNotices();

$deadline = time() + 75;
while (time() < $deadline) {
    if (is_file($marker) && filesize($marker) > 0) {
        break;
    }
    usleep(200000);
}

if (!is_file($marker) || filesize($marker) === 0) {
    echo "FAIL: cron did not run within 75 seconds\n";
    exit(1);
}

$first = trim((string) file($marker)[0]);
if ($first === '') {
    echo "FAIL: cron marker empty\n";
    exit(1);
}
echo "cron-fired: ok\n";

$tester->terminate();
$tester->expectLogTerminatingNotices();
$tester->close();

@unlink($marker);
@unlink("$work/job.php");
@rmdir($work);

?>
Done
--EXPECT--
cron-fired: ok
Done
--CLEAN--
<?php
require_once "tester.inc";
FPM\Tester::clean();
?>

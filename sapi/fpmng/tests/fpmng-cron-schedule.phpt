--TEST--
fpm-ng: cron pool runs its script on schedule (docs/cron.md, docs/NOTES.md §3r)
--SKIPIF--
<?php include "skipif.inc"; ?>
--FILE--
<?php

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

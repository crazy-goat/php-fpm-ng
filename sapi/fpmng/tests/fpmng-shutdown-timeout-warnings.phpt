--TEST--
fpm-ng: startup warnings when process_control_timeout is too small for supervisor/cron (task 017)
--SKIPIF--
<?php include "skipif.inc"; ?>
--FILE--
<?php

require_once "tester.inc";
require_once "fpmng-tester.inc";

$script = <<<'EOT'
<?php
sleep(60);
EOT;

$base = <<<EOT
[global]
error_log = {{FILE:LOG}}
daemonize = no
EOT;

// Stock globals: process_control_timeout = 0, supervisor.stop_timeout = 10s.
$supCfg = $base . "\n"
    . "[worker]\n"
    . "pool.type = supervisor\n"
    . "supervisor.script = {{FILE:*worker.php}}\n"
    . "supervisor.processes = 1\n";
$sup = new FPM\Tester($supCfg, $script);
$sup->start();
fpmng_expect_log_start_notices($sup);
$sup->expectLogPattern(
    '/WARNING: .*\\[pool worker\\] supervisor\\.stop_timeout = 10s but global process_control_timeout = 0s;/',
    true
);
$sup->terminate();
$sup->expectLogTerminatingNotices();
$sup->close();

// cron.timeout unset (0): no warning — documented decision in docs/shutdown-timeouts.md.
$cronDefault = $base . "\n"
    . "[tick]\n"
    . "pool.type = cron\n"
    . "cron.schedule = * * * * *\n"
    . "cron.script = {{FILE:*tick.php}}\n";
$cron0 = new FPM\Tester($cronDefault, $script);
$cron0->start();
fpmng_expect_log_start_notices($cron0);
$cron0->expectNoLogPattern(
    '/WARNING: .*\\[pool tick\\] cron\\.timeout =/',
    true
);
$cron0->terminate();
$cron0->expectLogTerminatingNotices();
$cron0->close();

// cron.timeout = 30s with process_control_timeout = 0: warn.
$cronWarn = $base . "\n"
    . "[job]\n"
    . "pool.type = cron\n"
    . "cron.schedule = * * * * *\n"
    . "cron.script = {{FILE:*job.php}}\n"
    . "cron.timeout = 30\n";
$cron = new FPM\Tester($cronWarn, $script);
$cron->start();
fpmng_expect_log_start_notices($cron);
$cron->expectLogPattern(
    '/WARNING: .*\\[pool job\\] cron\\.timeout = 30s but global process_control_timeout = 0s;/',
    true
);
$cron->terminate();
$cron->expectLogTerminatingNotices();
$cron->close();

// process_control_timeout large enough: supervisor warning suppressed.
$okCfg = $base . "\n"
    . "process_control_timeout = 15s\n"
    . "[worker]\n"
    . "pool.type = supervisor\n"
    . "supervisor.script = {{FILE:*worker2.php}}\n"
    . "supervisor.processes = 1\n";
$ok = new FPM\Tester($okCfg, $script);
$ok->start();
fpmng_expect_log_start_notices($ok);
$ok->expectNoLogPattern(
    '/WARNING: .*\\[pool worker\\] supervisor\\.stop_timeout =/',
    true
);
$ok->terminate();
$ok->expectLogTerminatingNotices();
$ok->close();

echo "supervisor default: warned\n";
echo "cron default timeout: silent\n";
echo "cron.timeout=30: warned\n";
echo "process_control_timeout=15s: silent\n";

?>
Done
--EXPECT--
supervisor default: warned
cron default timeout: silent
cron.timeout=30: warned
process_control_timeout=15s: silent
Done

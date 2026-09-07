--TEST--
fpm-ng: supervisor pool respawns a script that exits (docs/NOTES.md §3o)
--SKIPIF--
<?php include "skipif.inc"; ?>
--FILE--
<?php

require_once "tester.inc";

$work = sys_get_temp_dir() . '/fpmng-sup-' . getmypid();
@mkdir($work, 0700, true);
$counter = "$work/count.txt";
@unlink($counter);

$script = <<<PHP
<?php
\$n = is_file('{$counter}') ? (int) file_get_contents('{$counter}') : 0;
file_put_contents('{$counter}', (string) (\$n + 1));
exit(0);
PHP;
file_put_contents("$work/loop.php", $script);

$cfg = <<<EOT
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
[sup]
pool.type = supervisor
supervisor.script = $work/loop.php
supervisor.processes = 1
supervisor.restart = always
supervisor.restart_delay = 1
EOT;

$tester = new FPM\Tester($cfg, $script);
$tester->start();
$tester->expectLogStartNotices();

// Two restart cycles need at least 2 x restart_delay; poll instead of failing
// the first attempt when the child is still spawning.
$deadline = time() + 15;
while (time() < $deadline) {
    if (is_file($counter) && (int) file_get_contents($counter) >= 2) {
        break;
    }
    usleep(200000);
}

$runs = is_file($counter) ? (int) file_get_contents($counter) : 0;
if ($runs < 2) {
    echo "FAIL: supervisor restarts=$runs\n";
    exit(1);
}
echo "supervisor-restart: ok\n";

$tester->terminate();
$tester->expectLogTerminatingNotices();
$tester->close();

@unlink($counter);
@unlink("$work/loop.php");
@rmdir($work);

?>
Done
--EXPECT--
supervisor-restart: ok
Done
--CLEAN--
<?php
require_once "tester.inc";
FPM\Tester::clean();
?>

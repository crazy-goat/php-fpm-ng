--TEST--
fpm-ng: supervisor restart = on-failure keeps a completed-success copy's state across SIGKILL (issue #492)
--SKIPIF--
<?php include "skipif.inc"; ?>
--FILE--
<?php

require_once "tester.inc";

/* Issue #492, the restart = on-failure half. A copy that exits 0 under
 * on-failure is exactly as "done" as a copy under restart = never: it parks,
 * and issue #347 recorded its completion only pool-wide. SIGKILLing that parked
 * copy while a sibling is still running used to make the respawned slot run the
 * script again -- duplicating a SUCCESSFUL non-idempotent task. The scenario
 * and assertions mirror fpmng-supervisor-restart-never-killed.phpt; only the
 * directive under test differs. */
$work = sys_get_temp_dir() . '/fpmng-sup492of-' . getmypid();
@mkdir($work, 0700, true);
$runsFile = "$work/invocations.log";
$releaseFile = "$work/release";
@unlink($runsFile);
@unlink($releaseFile);

/* --CLEAN-- cannot do this: Tester::clean() only globs the harness's own
 * test-prefixed files, and this directory is named after the pid of THIS
 * process, which the clean process does not have. */
$cleanup = function () use ($work) {
    @unlink("$work/invocations.log");
    @unlink("$work/release");
    @unlink("$work/claim.lock");
    @unlink("$work/once.php");
    @rmdir($work);
};

$script = <<<PHP
<?php
error_reporting(0);
\$work = '{$work}';
\$lock = fopen(\$work . '/claim.lock', 'c');
flock(\$lock, LOCK_EX);
\$log = \$work . '/invocations.log';
\$n = 1;
if (is_file(\$log)) {
    \$lines = file(\$log, FILE_IGNORE_NEW_LINES | FILE_SKIP_EMPTY_LINES);
    \$n = count(\$lines) + 1;
}
file_put_contents(\$log, \$n . ' ' . getmypid() . "\\n", FILE_APPEND);
flock(\$lock, LOCK_UN);
fclose(\$lock);
if (\$n === 1) {
    exit(0);
}
while (!is_file(\$work . '/release')) {
    usleep(100000);
}
exit(0);
PHP;
file_put_contents("$work/once.php", $script);

$cfg = <<<EOT
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
daemonize = no
[once]
pool.type = supervisor
supervisor.script = $work/once.php
supervisor.processes = 2
supervisor.restart = on-failure
EOT;

$readEntries = function () use ($runsFile) {
    $data = @file_get_contents($runsFile);
    $entries = [];
    if (is_string($data) && $data !== '') {
        foreach (array_filter(explode("\n", $data), 'strlen') as $line) {
            $parts = explode(' ', trim($line));
            if (count($parts) === 2) {
                $entries[] = [(int) $parts[0], (int) $parts[1]];
            }
        }
    }
    return $entries;
};

$fail = function (string $why) use (&$tester, $cleanup) {
    printf("FAIL: %s\n", $why);
    echo "--- error log:\n";
    $tester->printLogs();
    $tester->close(true);
    $cleanup();
    exit(1);
};

$tester = new FPM\Tester($cfg, $script);
@unlink($tester->getPrefixedFile(FPM\Tester::FILE_EXT_LOG_ERR));

$tester->start([], false);
$tester->switchLogSource('{{FILE:LOG}}');
$tester->expectLogStartNotices();

$entries = [];
$deadline = time() + 15;
while (time() < $deadline) {
    $entries = $readEntries();
    if (count($entries) >= 2) {
        break;
    }
    usleep(100000);
}
if (count($entries) !== 2) {
    $fail(sprintf('expected 2 one-shot invocations, saw %d', count($entries)));
}

$tester->expectLogPattern('/this copy is done: 1 of 2/');

$tester->signal('KILL', $entries[0][1]);

$third = false;
$deadline = time() + 3;
while (time() < $deadline) {
    if (count($readEntries()) > 2) {
        $third = true;
        break;
    }
    usleep(100000);
}
if ($third) {
    $fail('the respawned copy ran the script again after a successful one-shot');
}

$tester->expectNoLogPattern('/this copy is done: 2 of 2/');

file_put_contents($releaseFile, "go\n");
$tester->expectLogPattern('/this copy is done: 2 of 2/');

if (count($readEntries()) !== 2) {
    $fail(sprintf('expected exactly 2 one-shot invocations after the sibling finished, saw %d', count($readEntries())));
}

echo "supervisor-restart-onfailure-death: 2 copies, 2 invocations across a SIGKILL\n";

$tester->terminate();
$tester->expectLogTerminatingNotices();
$tester->close();

$cleanup();

?>
Done
--EXPECT--
supervisor-restart-onfailure-death: 2 copies, 2 invocations across a SIGKILL
Done
--CLEAN--
<?php
require_once "tester.inc";
FPM\Tester::clean();
$stale = time() - 300;
foreach (glob(sys_get_temp_dir() . '/fpmng-sup492of-*') as $dir) {
    if (@filemtime($dir) > $stale) {
        continue;
    }
    foreach (glob("$dir/*") as $file) {
        @unlink($file);
    }
    @rmdir($dir);
}
?>

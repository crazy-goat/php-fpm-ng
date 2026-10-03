--TEST--
fpm-ng: supervisor restart = never keeps a completed copy's state across SIGKILL (issue #492)
--SKIPIF--
<?php include "skipif.inc"; ?>
--FILE--
<?php

require_once "tester.inc";

/* Issue #492. Issue #347 made each of a pool's supervisor.processes copies run
 * the one-shot once and then PARK; it recorded completion only in a pool-wide
 * counter. A parked copy that is SIGKILLed (OOM, an operator's kill, ...) is
 * respawned by fpm_children.c into the same scoreboard slot, and the
 * replacement used to read only the pool-wide terminal flag -- still 0 while a
 * sibling was running -- and execute the script a THIRD time despite
 * restart = never. This test reproduces exactly that: two copies, one returns
 * and parks, the other is held; kill the parked one and assert no third
 * invocation happens before the held one is released. */
$work = sys_get_temp_dir() . '/fpmng-sup492-' . getmypid();
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

/* Each run takes the file lock, appends "number pid", releases the lock, and
 * returns. Run number 1 returns immediately (its copy is done and parks); run
 * number 2 and any later run waits for the release file. The lock is what makes
 * the numbering sequential rather than racy, so "run number 1" is unambiguously
 * the completed copy whose pid the test then kills. */
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
supervisor.restart = never
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

/* Two forks and two PHP startups after the startup notices; poll rather than
 * failing on the first attempt. */
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

/* The first copy has returned and parked; its completion is recorded and the
 * pool is deliberately NOT finished while its sibling is still held. */
$tester->expectLogPattern('/this copy is done: 1 of 2/');

/* Kill the parked, already-completed copy. fpm_children.c respawns its slot. */
$tester->signal('KILL', $entries[0][1]);

/* Give the master ample time to reap and respawn. Before the fix the
 * replacement ran the script again (a third invocation); it must not. */
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
    $fail('the respawned copy ran the script again despite restart = never');
}

/* Pool completion must not be reported while the sibling is unfinished. */
$tester->expectNoLogPattern('/this copy is done: 2 of 2/');

/* Let the unfinished sibling complete normally. */
file_put_contents($releaseFile, "go\n");
$tester->expectLogPattern('/this copy is done: 2 of 2/');

if (count($readEntries()) !== 2) {
    $fail(sprintf('expected exactly 2 one-shot invocations after the sibling finished, saw %d', count($readEntries())));
}

echo "supervisor-restart-never-death: 2 copies, 2 invocations across a SIGKILL\n";

$tester->terminate();
$tester->expectLogTerminatingNotices();
$tester->close();

$cleanup();

?>
Done
--EXPECT--
supervisor-restart-never-death: 2 copies, 2 invocations across a SIGKILL
Done
--CLEAN--
<?php
require_once "tester.inc";
FPM\Tester::clean();
/* Whatever the run above leaked: a start() failure or a thrown expectation
 * skips the cleanup in --FILE--, and these directories are named after a pid
 * this process does not know. Age check, not a pid check -- run-tests.php may
 * run another copy of this test in parallel and that directory must not be
 * touched. */
$stale = time() - 300;
foreach (glob(sys_get_temp_dir() . '/fpmng-sup492-*') as $dir) {
    if (@filemtime($dir) > $stale) {
        continue;
    }
    foreach (glob("$dir/*") as $file) {
        @unlink($file);
    }
    @rmdir($dir);
}
?>

--TEST--
fpm-ng: STDIN, STDOUT and STDERR exist in a script-running pool (issue #126)
--SKIPIF--
<?php include "skipif.inc"; ?>
--FILE--
<?php

require_once "tester.inc";

$work = sys_get_temp_dir() . '/fpmng-std-' . getmypid();
@mkdir($work, 0700, true);
$fdFile = "$work/fds.log";

$cleanup = function () use ($work) {
    foreach (glob("$work/*") as $file) {
        @unlink($file);
    }
    @rmdir($work);
};

/* The three things the issue says an author reaches for. STDIN is asserted
 * through error_log() rather than through STDOUT, so that "STDIN is a handle
 * on /dev/null, not a fatal" is checked even if the stdout path were broken. */
file_put_contents("$work/say.php", <<<'PHP'
<?php
fwrite(STDOUT, "stdout constant works\n");
fwrite(STDERR, "stderr constant works\n");
error_log('stdin: defined=' . (int) defined('STDIN')
    . ' read=' . var_export(fread(STDIN, 8), true)
    . ' eof=' . (int) feof(STDIN));
PHP);

/* One appended line per run with this process's open descriptor count. The
 * constants are re-registered for every iteration and outside CLI php://std*
 * dup()s the descriptor, so a stream that survives its request would show up
 * here as three more fds per run — see fpm_std_streams_register().
 * The usleep keeps this pool at ~10 runs/s: with supervisor.restart = always
 * and a script that exits at once the supervisor starts the next iteration
 * with no delay at all (12086 runs/s measured, issue #122). */
file_put_contents("$work/fds.php", <<<PHP
<?php
error_reporting(0);
\$n = is_dir('/proc/self/fd') ? count(@scandir('/proc/self/fd')) : -1;
@file_put_contents('{$fdFile}', \$n . "\\n", FILE_APPEND);
usleep(100000);
PHP);

$cfg = <<<EOT
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
daemonize = no
[say]
pool.type = supervisor
supervisor.script = $work/say.php
supervisor.processes = 1
supervisor.restart = never
catch_workers_output = yes
; No catch_workers_output: this pool restarts a script ten times a second and
; every run ends with a flush marker the master logs (see the measurement in
; fpmng-supervisor-restart.phpt). It writes its evidence to a file instead.
[fds]
pool.type = supervisor
supervisor.script = $work/fds.php
supervisor.processes = 1
supervisor.restart = always
EOT;

$tester = new FPM\Tester($cfg);

@unlink($tester->getPrefixedFile(FPM\Tester::FILE_EXT_LOG_ERR));
$tester->start([], false);
$tester->switchLogSource('{{FILE:LOG}}');
$tester->expectLogStartNotices();

/* What the script wrote through the constants reaches the operator by the same
 * route as echo does, which is the whole point of having them. */
$tester->expectLogPattern(
    '/WARNING: .*\[pool say\] child \d+ said into stdout: "stdout constant works"/',
    true,
    10
);
echo "STDOUT: written\n";

$tester->expectLogPattern(
    '/WARNING: .*\[pool say\] child \d+ said into stderr: "stderr constant works"/',
    true,
    10
);
echo "STDERR: written\n";

/* Before the fix this line was the fatal from the issue instead. */
$tester->expectNoLogPattern('/Undefined constant "STD(IN|OUT|ERR)"/', true);
echo "no undefined-constant fatal\n";

/* A valid handle on the child's /dev/null stdin: an immediate EOF, not a
 * fatal and not a blocking read. */
$tester->expectLogPattern(
    "/\[pool say\] PHP message: stdin: defined=1 read='' eof=1/",
    true,
    10
);
echo "STDIN: empty and at EOF\n";

/* Descriptor accounting over at least six iterations of the [fds] pool. */
$deadline = microtime(true) + 20;
$counts = [];
while (microtime(true) < $deadline) {
    $counts = array_values(array_filter(array_map('trim', @file($fdFile) ?: []), 'strlen'));
    if (count($counts) >= 6) {
        break;
    }
    usleep(100000);
}

/* "ok" only on the two outcomes that are not a leak, and never on a run that
 * measured nothing: fewer than six iterations means the pool did not restart
 * the script, which is a failure of this test's premise, not a pass.
 *
 * The tolerance is 3 rather than 0 because the count is not perfectly steady:
 * measured over 100 consecutive runs on the test box it alternates between 16
 * and 17 open descriptors, with no trend. A stream that outlived its request
 * would add three per run (STDIN, STDOUT, STDERR, each a dup()), so six runs
 * put it 15 or more above the floor — far outside that band.
 *
 * Without /proc/self/fd there is nothing to count and this half of the test
 * becomes "ok (not measured)" — a pass that cannot go red. Deliberate, and
 * only true off Linux: every cell of .github/workflows/build-matrix.yml runs
 * on ubuntu-latest, and so does the test box, so the band above IS asserted
 * everywhere this suite gates a merge. The constants themselves are still
 * checked on any platform by the four assertions before this one. */
if (count($counts) < 6) {
    printf("fd counts: only %d runs recorded\n", count($counts));
} elseif ($counts[0] === '-1') {
    echo "fd counts: ok (not measured, no /proc/self/fd)\n";
} else {
    $counts = array_map('intval', $counts);
    printf("fd counts: %s\n", max($counts) - min($counts) <= 3
        ? sprintf('ok (%d..%d over %d runs)', min($counts), max($counts), count($counts))
        : sprintf('GREW %d..%d over %d runs', min($counts), max($counts), count($counts)));
}

/* No expectLogTerminatingNotices(): as in fpmng-supervisor-php-error-log.phpt,
 * these pools log their own lines whenever they feel like it. */
$tester->close(true);

$cleanup();

?>
Done
--CLEAN--
<?php
require_once "tester.inc";
FPM\Tester::clean();
/* Age-based sweep, for the same reason as fpmng-supervisor-php-error-log.phpt:
 * the directory is named after the pid of the --FILE-- process, which this one
 * does not know, and a run that never reached its own cleanup leaves one
 * behind. */
$stale = time() - 300;
foreach (glob(sys_get_temp_dir() . '/fpmng-std-*') as $dir) {
    if (@filemtime($dir) > $stale) {
        continue;
    }
    foreach (glob("$dir/*") as $file) {
        @unlink($file);
    }
    @rmdir($dir);
}
?>
--EXPECTF--
STDOUT: written
STDERR: written
no undefined-constant fatal
STDIN: empty and at EOF
fd counts: ok %s
Done

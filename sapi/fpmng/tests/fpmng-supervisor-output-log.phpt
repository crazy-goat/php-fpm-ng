--TEST--
fpm-ng: supervisor.output_log redirects the script's STDOUT/STDERR straight to
a file, bypassing catch_workers_output's pipe/reader thread entirely even when
that directive is also set (issue #328)
--SKIPIF--
<?php include "skipif.inc"; ?>
--FILE--
<?php

require_once "tester.inc";

$work = sys_get_temp_dir() . '/fpmng-suplog-' . getmypid();
@mkdir($work, 0700, true);
$outputLog = "$work/output.log";
@unlink($outputLog);

$cleanup = function () use ($work, $outputLog) {
    @unlink($outputLog);
    @unlink("$work/job.php");
    @rmdir($work);
};

/* restart = never, single run: this test is about WHERE stdout/stderr land,
 * not about supervisor's restart/backoff policy -- one run is enough evidence
 * and keeps the file's content deterministic (no interleaving of several
 * iterations to reason about). */
$script = <<<PHP
<?php
error_reporting(0);
fwrite(STDOUT, "stdout-line\\n");
fwrite(STDERR, "stderr-line\\n");
exit(0);
PHP;
file_put_contents("$work/job.php", $script);

/* catch_workers_output = yes is deliberately ALSO set: the point of issue
 * #328 is that supervisor.output_log bypasses that pipe entirely, whether or
 * not it is configured -- without this, a bug that fell back to the pipe
 * instead of the dedicated file would still pass a test that never turned the
 * pipe on in the first place. */
$cfg = <<<EOT
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
log_level = notice
[sup]
pool.type = supervisor
supervisor.script = $work/job.php
supervisor.processes = 1
supervisor.restart = never
catch_workers_output = yes
supervisor.output_log = $outputLog
EOT;

$tester = new FPM\Tester($cfg, $script);
$tester->start([], false);
$tester->switchLogSource('{{FILE:LOG}}');
$tester->expectLogStartNotices();

$content = '';
$deadline = time() + 15;
while (time() < $deadline) {
    if (is_file($outputLog)) {
        $content = (string) file_get_contents($outputLog);
        if (str_contains($content, "stdout-line") && str_contains($content, "stderr-line")) {
            break;
        }
    }
    usleep(200000);
}

$hasStdout = str_contains($content, "stdout-line");
$hasStderr = str_contains($content, "stderr-line");

/* The negative half of the assertion: with the redirect in place, nothing the
 * script wrote to STDOUT/STDERR should have reached the master's
 * catch_workers_output reader -- if it did, output_log did not actually
 * bypass the pipe, it just ALSO captured a copy. */
$errorLog = $tester->getPrefixedFile(FPM\Tester::FILE_EXT_LOG_ERR);
$logText = is_file($errorLog) ? (string) file_get_contents($errorLog) : '';
$leakedToPipe = str_contains($logText, 'said into stdout') || str_contains($logText, 'said into stderr');

$ok = $hasStdout && $hasStderr && !$leakedToPipe;

if (!$ok) {
    echo "FAIL: supervisor-output-log stdout=" . ($hasStdout ? 'yes' : 'no')
        . " stderr=" . ($hasStderr ? 'yes' : 'no')
        . " leaked-to-catch_workers_output-pipe=" . ($leakedToPipe ? 'yes (BAD)' : 'no') . "\n";
    echo "output_log content: " . var_export($content, true) . "\n";

    $tester->close(true);
    $cleanup();
    exit(1);
}
echo "supervisor-output-log: ok\n";

$tester->terminate();
$tester->expectLogTerminatingNotices();
$tester->close();

$cleanup();

?>
Done
--EXPECT--
supervisor-output-log: ok
Done
--CLEAN--
<?php
require_once "tester.inc";
FPM\Tester::clean();
$stale = time() - 300;
foreach (glob(sys_get_temp_dir() . '/fpmng-suplog-*') as $dir) {
    if (@filemtime($dir) > $stale) {
        continue;
    }
    foreach (glob("$dir/*") as $file) {
        @unlink($file);
    }
    @rmdir($dir);
}
?>

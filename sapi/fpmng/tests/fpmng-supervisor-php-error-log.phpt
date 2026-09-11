--TEST--
fpm-ng: a PHP error in a supervisor script reaches error_log as plain text (issue #124)
--SKIPIF--
<?php include "skipif.inc"; ?>
--FILE--
<?php

require_once "tester.inc";
require_once "fpmng-tester.inc";

$work = sys_get_temp_dir() . '/fpmng-phperr-' . getmypid();
@mkdir($work, 0700, true);

$cleanup = function () use ($work) {
    foreach (glob("$work/*") as $file) {
        @unlink($file);
    }
    @rmdir($work);
};

/* An undefined function: a fatal that needs no extension and whose message,
 * file and line are all things this test can pin exactly. */
file_put_contents("$work/fatal.php", "<?php\nno_such_function_here();\n");

/* A warning, then a deliberate write to stdout: the second pool checks that
 * catch_workers_output still carries what the script writes ON PURPOSE, which
 * is a different path from the one under test. */
file_put_contents("$work/warn.php", "<?php\ntrigger_error('a deliberate warning', E_USER_WARNING);\necho \"on purpose\\n\";\n");

/* No catch_workers_output on [fatal] — that having to be set at all was the
 * bug. restart = never so exactly one run is logged. */
$cfg = <<<EOT
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
daemonize = no
[fatal]
pool.type = supervisor
supervisor.script = $work/fatal.php
supervisor.processes = 1
supervisor.restart = never
[said]
pool.type = supervisor
supervisor.script = $work/warn.php
supervisor.processes = 1
supervisor.restart = never
catch_workers_output = yes
EOT;

$tester = new FPM\Tester($cfg);

/* forceStderr = false + a log file: the assertions read the same error_log an
 * operator would. */
@unlink($tester->getPrefixedFile(FPM\Tester::FILE_EXT_LOG_ERR));
$tester->start([], false);
$tester->switchLogSource('{{FILE:LOG}}');
fpmng_expect_log_start_notices($tester);

/* Criterion 1 and 2: one entry, at an FPM level derived from the severity
 * (ERROR for a fatal, not the NOTICE upstream's sapi_cgi_log_message() uses
 * for everything), carrying the message, the file and the line. */
$tester->expectLogPattern(
    '/ERROR: .*\[pool fatal\] PHP message: PHP Fatal error: +Uncaught Error: '
        . 'Call to undefined function no_such_function_here\(\) in '
        . preg_quote("$work/fatal.php", '/') . ':2/',
    true,
    10
);
echo "fatal: logged\n";

/* Criterion 2, the other half — and asserted on [said], not on [fatal]: the
 * [fatal] pool sets no catch_workers_output, so the master has no pipe from
 * that child and STRUCTURALLY cannot produce either line, with or without the
 * fix. [said] does set it, so it is the only pool where "the error did not
 * arrive as HTML wrapped in a master-side WARNING" can fail. */
$tester->expectNoLogPattern('/\[pool said\] child \d+ said into std(out|err): .*(<br \/>|<b>)/', true);
echo "no markup: ok\n";

$tester->expectNoLogPattern('/\[pool said\] child \d+ said into std(out|err): .*a deliberate warning/', true);
echo "not wrapped: ok\n";

/* A warning keeps its own level rather than being flattened into the fatal's. */
$tester->expectLogPattern(
    '/WARNING: .*\[pool said\] PHP message: PHP Warning: +a deliberate warning in '
        . preg_quote("$work/warn.php", '/') . ' on line 2/',
    true,
    10
);
echo "warning: logged at WARNING\n";

/* Criterion 3: what the script writes to stdout on purpose still reaches the
 * operator through catch_workers_output, unchanged. */
$tester->expectLogPattern(
    '/WARNING: .*\[pool said\] child \d+ said into stdout: "on purpose"/',
    true,
    10
);
echo "stdout still captured: ok\n";

/* No expectLogTerminatingNotices(): as in fpmng-supervisor-child-log.phpt,
 * these pools log their own lines whenever they feel like it. */
$tester->close(true);

$cleanup();

?>
Done
--CLEAN--
<?php
require_once "tester.inc";
FPM\Tester::clean();
/* Same age-based sweep as fpmng-supervisor-child-log.phpt: the directory is
 * named after the pid of the --FILE-- process, which this one does not know,
 * and a run that never reached its own cleanup (Ctrl-C, --no-clean) leaves one
 * behind. Age, not pid: run-tests.php may be running another copy of this test
 * in parallel and its directory must not be touched. */
$stale = time() - 300;
foreach (glob(sys_get_temp_dir() . '/fpmng-phperr-*') as $dir) {
    if (@filemtime($dir) > $stale) {
        continue;
    }
    foreach (glob("$dir/*") as $file) {
        @unlink($file);
    }
    @rmdir($dir);
}
?>
--EXPECT--
fatal: logged
no markup: ok
not wrapped: ok
warning: logged at WARNING
stdout still captured: ok
Done

--TEST--
fpm-ng: supervisor pool respawns a script that exits (docs/NOTES.md §3o)
--SKIPIF--
<?php include "skipif.inc"; ?>
--FILE--
<?php

require_once "tester.inc";

$work = sys_get_temp_dir() . '/fpmng-sup-' . getmypid();
@mkdir($work, 0700, true);
$runsFile = "$work/runs.log";
@unlink($runsFile);

/* Called from both exits below. --CLEAN-- cannot do this instead: Tester::clean()
 * only globs the harness's own test-prefixed files, and this directory is named
 * after the pid of THIS process, which the clean process does not have. */
$cleanup = function () use ($work, $runsFile) {
    @unlink($runsFile);
    @unlink("$work/loop.php");
    @rmdir($work);
};

/* One APPENDED line per run — never a rewrite. Issue #72: this test used to
 * have the script read a counter, add one and write the total back with
 * file_put_contents(), which opens the file with O_TRUNC. With
 * supervisor.restart = always and a script that exits 0 the supervisor starts
 * the next iteration with no delay at all (fpm_pool_supervisor.c, "start the
 * next iteration immediately, without artificial throttling on our side"), so
 * on the test box that rewrite ran 12086 times per second and the file was
 * empty for 18.9% of the wall clock. (int) '' is 0, so an unsynchronised read
 * of it reports "no runs at all" — measured: 2 failures in 300 consecutive
 * runs of the pre-fix test, each printing exactly the "FAIL: supervisor
 * restarts=0" CI reported twice. Nothing was ever wrong with the pool.
 *
 * An append-only file is never truncated, so once the script has run once,
 * every read of it yields at least one whole line: a torn read can only
 * under-count by the last, partial line, never report zero. */
/* error_reporting(0) and @: a failed write must degrade into "restarts=0" plus
 * the diagnostics below, not into log-matcher noise. With
 * catch_workers_output = yes anything this script writes to stderr comes back
 * as a master-side WARNING "child N said into stderr: ..." (fpm_stdio.c:202),
 * and LogTool::error() only forgives lines containing DEBUG, so one PHP warning
 * landing between "Terminating ..." and "exiting, bye-bye!" would fail
 * expectLogTerminatingNotices() with an opaque matcher diff instead. */
$script = <<<PHP
<?php
error_reporting(0);
@file_put_contents('{$runsFile}', "run\\n", FILE_APPEND);
exit(0);
PHP;
file_put_contents("$work/loop.php", $script);

$cfg = <<<EOT
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
; The failure path below has to tell "no child was ever forked" apart from "a
; child was forked and the script never completed a run", and the ONLY evidence
; that a fork happened is DEBUG: fpm_children_create_initial() spawns the first
; child through fpm_children_make(..., is_debug = 1) (fpm_children.c), and
; fpm_pool_supervisor_child_main() then re-runs the script inside that one child
; (the for (;;) at fpm_pool_supervisor.c:394), so there is no per-iteration
; spawn line at any level. The cost of the level is paid in noise, not in
; signal: measured on the test box, 15 s of a pool restarting a script that
; writes nothing wrote 50,288,958 bytes of log, of which 463,822 lines were
; fpm_event_loop() "event module triggered 1 events" -- one per flush marker
; that catch_workers_output makes the child write after every run -- and 6 lines
; were the startup notices plus "[pool sup] child N started". The dump below
; therefore filters and caps instead of printing the file.
log_level = debug
[sup]
pool.type = supervisor
; Without this the pool's own account of what it did with the child --
; "script finished (exit code N)", the backoff notices, the restart_max ALERT
; -- is written to the child's stderr, which fpm_stdio_child_use_pipes()
; (sapi/fpmng/fpm/fpm_stdio.c) points at the master's stdout, and
; fpm_stdio_init_main() has already pointed that at /dev/null. Measured on the
; test box: with catch_workers_output = no a supervisor whose script exits 3
; logs nothing at all about it; with it on, the same run logs
; `WARNING: [pool sup] child N said into stderr: "NOTICE:
; fpm_pool_supervisor_apply_policy(), line 352: [pool sup] supervisor: script
; exited (code 3) after 0s, restarting in 1s (failure 1/unlimited)"`. The
; failure path below dumps this log and that line is the whole point of dumping
; it, so the test asks for it explicitly. That a test has to ask at all is
; issue #121; when that is fixed this directive can go.
catch_workers_output = yes
supervisor.script = $work/loop.php
supervisor.processes = 1
supervisor.restart = always
supervisor.restart_delay = 1
EOT;

$tester = new FPM\Tester($cfg, $script);
/* forceStderr = false plus an explicit switch to {{FILE:LOG}}: Tester::start()
 * passes FPM's -O by default, which sends the master's log to stderr and
 * leaves error_log = {{FILE:LOG}} unwritten — which is why the CI artifact of
 * the two failures in issue #72 contains no FPM error log at all, only
 * whatever the harness had already read off that pipe. With the log in a file
 * the failure path below can print all of it, and the log reader tails the
 * file instead of the pipe (LogFileSource waits for the file to appear, so
 * switching before FPM has created it is safe). */
/* fpm_stdio_open_error_log() opens error_log with O_WRONLY|O_APPEND|O_CREAT
 * (sapi/fpmng/fpm/fpm_stdio.c) and LogFileSource reads it from offset 0, so a
 * file left behind by a run that never reached --CLEAN-- (run-tests
 * --no-clean, Ctrl-C) would let expectLogStartNotices() match the PREVIOUS
 * run's startup notices. The pipe-based source this test used before could not
 * do that. */
@unlink($tester->getPrefixedFile(FPM\Tester::FILE_EXT_LOG_ERR));

$tester->start([], false);
$tester->switchLogSource('{{FILE:LOG}}');
$tester->expectLogStartNotices();

/* The first run still needs a fork and a PHP request startup after the startup
 * notices, so an empty file is legitimate for a few milliseconds; poll instead
 * of failing the first attempt. supervisor.restart_delay does NOT apply here —
 * it is the backoff for a FAILED run — so a healthy pool reaches two runs in
 * milliseconds, not in two seconds, and 15 s is a very wide margin.
 *
 * Keep the largest count seen rather than re-reading the file after the loop:
 * the read that satisfied the condition IS the measurement, and re-reading a
 * file that a hot loop keeps rewriting is what issue #72 was. */
$runs = 0;
$deadline = time() + 15;
while (time() < $deadline) {
    $data = @file_get_contents($runsFile);
    if (is_string($data)) {
        $runs = max($runs, substr_count($data, "\n"));
    }
    if ($runs >= 2) {
        break;
    }
    usleep(200000);
}

if ($runs < 2) {
    echo "FAIL: supervisor restarts=$runs\n";

    /* Issue #72, criterion 3: the fpmng-phpt CI artifact carries no FPM error
     * log, so a failure here has to explain itself in its own output or it is
     * not diagnosable at all. Three states, read in this order:
     *
     *   runs file present -> N runs completed; the count is the diagnosis.
     *   ABSENT + "[pool sup] child N started" in the log below -> a child WAS
     *       forked and the script never completed its single statement
     *       (unreadable script, fatal during startup, killed mid-run). The
     *       log's own "cannot open script" / "script exited (code N)" lines
     *       say which.
     *   ABSENT + no child line at all -> no child was ever forked; the bug is
     *       upstream of the script, in the pool or the master.
     *
     * The runs file alone cannot separate the last two, because the script
     * CREATES that file: "never forked" and "forked and died first" both leave
     * it absent. That is what log_level = debug in the config is for. */
    if (is_file($runsFile)) {
        /* Capped: a pool that recovers between the last poll and this line
         * appends thousands of lines a second, and this output is read as a
         * diff. */
        printf("runs file: present, %d bytes, first 200 bytes %s\n",
            filesize($runsFile),
            var_export(substr((string) file_get_contents($runsFile), 0, 200), true));
    } else {
        echo "runs file: ABSENT — the supervised script never reached its first statement\n";
    }

    /* What is worth printing out of this log: everything that is not DEBUG
     * (the startup notices, the pool's own backoff/give-up messages, which
     * arrive as "child N said into stderr: ..." WARNINGs, and any child death),
     * plus the DEBUG lines about the child, which are the fork evidence. What
     * is not: the fpm_event_loop() flood measured above, which is why this
     * filters instead of tailing — a raw tail of the 50 MB case is 8 KB of
     * "event module triggered 1 events" and answers nothing. Bounded to the
     * last 40 kept lines because this text is read as a phpt diff. */
    $keepLast = 40;
    $errorLog = $tester->getPrefixedFile(FPM\Tester::FILE_EXT_LOG_ERR);
    echo "--- error log ($errorLog):\n";
    if (is_file($errorLog)) {
        printf("(%d bytes; DEBUG lines other than the child ones are omitted)\n",
            filesize($errorLog));
        $kept = [];
        $skipped = 0;
        $handle = fopen($errorLog, 'r');
        while (($line = fgets($handle)) !== false) {
            if (str_contains($line, ' DEBUG: ') && ! str_contains($line, 'child')) {
                $skipped++;
                continue;
            }
            $kept[] = $line;
            if (count($kept) > $keepLast) {
                array_shift($kept);
            }
        }
        fclose($handle);
        printf("(%d DEBUG lines omitted)\n", $skipped);
        echo implode('', $kept);
        echo "--- end of error log\n";
    } else {
        /* fpm_stdio_open_error_log() creates this file with O_CREAT before
         * almost anything else can fail, so this branch is narrow: it means FPM
         * died before it got that far (an unparsable config), and the only
         * account of it is on the pipe the harness holds. */
        echo "(the error log does not exist)\n--- end of error log\n";
        $tester->printLogs();
    }

    /* Do not leave the master behind: with restart = always it would keep
     * respawning the script for as long as this machine lives. */
    $tester->close(true);
    $cleanup();
    exit(1);
}
echo "supervisor-restart: ok\n";

$tester->terminate();
$tester->expectLogTerminatingNotices();
$tester->close();

$cleanup();

?>
Done
--EXPECT--
supervisor-restart: ok
Done
--CLEAN--
<?php
require_once "tester.inc";
FPM\Tester::clean();
/* Whatever the run above leaked: start() failing or an expectation throwing
 * skips the cleanup in --FILE--, and these directories are named after a pid
 * this process does not know. */
$stale = time() - 300;
foreach (glob(sys_get_temp_dir() . '/fpmng-sup-*') as $dir) {
    /* Age check, not a pid check: the pid is another process's and may have
     * been recycled, and nothing here may touch the work directory of a run
     * that is still going -- run-tests.php can execute this test in parallel
     * with another copy of itself. */
    if (@filemtime($dir) > $stale) {
        continue;
    }
    foreach (glob("$dir/*") as $file) {
        @unlink($file);
    }
    @rmdir($dir);
}
?>

--TEST--
fpm-ng: cron.stop_signal asks a running cron child to stop with the configured
signal instead of the hardcoded SIGTERM every other non-request-serving pool
still gets (issue #325)
--SKIPIF--
<?php include "skipif.inc"; ?>
--FILE--
<?php

require_once "tester.inc";

$work = sys_get_temp_dir() . '/fpmng-cron-stop-' . getmypid();
@mkdir($work, 0700, true);
$marker = "$work/running.log";
@unlink($marker);

$cleanup = function () use ($work, $marker) {
    @unlink($marker);
    @unlink("$work/job.php");
    @rmdir($work);
};

/* Long enough to still be asleep in this sleep() call when the test below
 * calls $tester->terminate() right after seeing the marker -- the point of
 * this test is to catch the master's signal WHILE the script is running, not
 * after it has already exited. The marker write happens before the sleep, so
 * detecting it and terminating takes a small fraction of a second, nowhere
 * near this 20s budget. */
$script = <<<PHP
<?php
file_put_contents('{$marker}', getmypid() . "\\n", FILE_APPEND);
sleep(20);
PHP;
file_put_contents("$work/job.php", $script);

/* cron.stop_signal = USR1 (issue #325): the master's own shutdown escalation
 * (fpm_pctl_kill_all()) must send THIS signal to the job's child instead of
 * the hardcoded SIGTERM every other non-request-serving pool still gets --
 * see fpm_pool_type_s.stop_signal and fpm_pool_cron_stop_signal() in
 * fpm_pool_cron.c. log_level = debug is what makes that decision observable
 * from here: fpm_pctl_kill_all() logs "[pool job] sending signal N NAME to
 * child PID" for every child it signals (fpm_process_ctl.c), and NAME comes
 * from fpm_signal_names[] -- a name, not a raw number, is what keeps this
 * assertion portable between platforms where SIGUSR1 is a different integer
 * (30 on Darwin, 10 on Linux). */
$cfg = <<<EOT
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
daemonize = no
log_level = debug
[job]
pool.type = cron
cron.schedule = * * * * *
cron.script = $work/job.php
cron.stop_signal = USR1
EOT;

$tester = new FPM\Tester($cfg, $script);
$tester->start(extraArgs: ['-R'], forceStderr: true, daemonize: false);
$tester->expectLogStartNotices();

/* cron.schedule = * * * * * fires at most once a minute — the same bound
 * fpmng-cron-schedule.phpt already relies on within the CI phpt timeout
 * (TEST_FPM_TIMEOUT = 120s, .github/workflows/build-matrix.yml). Everything
 * after the marker appears is fast (a signal send and a few log lines), so
 * this is the only slow part of the test. */
$deadline = time() + 75;
while (time() < $deadline) {
    if (is_file($marker) && filesize($marker) > 0) {
        break;
    }
    usleep(200000);
}

if (!is_file($marker) || filesize($marker) === 0) {
    echo "FAIL: cron job did not start running within 75 seconds\n";
    $tester->close(true);
    $cleanup();
    exit(1);
}
echo "cron-running: ok\n";

/* The script is asleep in sleep(20) right now (it just wrote the marker) —
 * this is the "actively-running cron child" the issue asks about. Terminate
 * the MASTER (not the child directly): the master's own shutdown/reload
 * escalation is what issue #325 changes, and only $tester->terminate() (a
 * SIGTERM to the master, matching `docker stop`) exercises that path. */
$tester->terminate();

/* The log reader only moves forward — reading the "Terminating ..." notice
 * before the debug line below (in log order) instead of using
 * expectLogTerminatingNotices() up front, which would also wait for "exiting,
 * bye-bye!" and consume past our own line before we get to look for it. */
$tester->expectLogNotice('Terminating \.\.\.');

/* SIGUSR1, by name, sent to the job pool's child — never the hardcoded
 * SIGTERM every other non-request-serving pool still gets from
 * fpm_pctl_kill_all(). */
$tester->expectLogPattern(
    '/DEBUG: .*\[pool job\] sending signal \d+ SIGUSR1 to child \d+/'
);
echo "stop-signal: ok\n";

/* Same reasoning as fpmng-supervisor-restart.phpt's default-globals test: the
 * child does not exit on SIGUSR1 alone (it is still inside sleep(20), and the
 * C-level handler only sets a flag -- fpm_pool_cron_sigterm()'s comment in
 * fpm_pool_cron.c), so the master's own process_control_timeout escalation
 * (default 0 = "escalate to SIGKILL almost immediately") is what actually
 * ends it. The final "exiting, bye-bye!" notice still arrives after that,
 * same as the identical sleep(60)/process_control_timeout=0 case in
 * fpmng-shutdown-timeout-warnings.phpt. */
$tester->expectLogNotice('exiting, bye-bye!');
$tester->close();

$cleanup();

?>
Done
--EXPECT--
cron-running: ok
stop-signal: ok
Done
--CLEAN--
<?php
require_once "tester.inc";
FPM\Tester::clean();
$stale = time() - 300;
foreach (glob(sys_get_temp_dir() . '/fpmng-cron-stop-*') as $dir) {
    if (@filemtime($dir) > $stale) {
        continue;
    }
    foreach (glob("$dir/*") as $file) {
        @unlink($file);
    }
    @rmdir($dir);
}
?>

--TEST--
fpm-ng: cron.expect_within surfaces an overrunning cron job as stale, without touching no-catch-up (issue #327)
--SKIPIF--
<?php include "skipif.inc"; ?>
--FILE--
<?php

require_once "tester.inc";
require_once "fpmng-operator.inc";

/* cron fires at most once a minute (fpm_cron_schedule_next() always jumps to
 * the START of the next matching minute -- see fpm_pool_cron.c) and
 * cron.expect_within is only meaningful once the schedule's NEXT occurrence
 * after a completed/started run is already in the past. There is no way to
 * observe that in less than "the time until the next minute boundary" (0-60s,
 * random depending on when this test happens to start) PLUS "one minute plus
 * cron.expect_within" past the run that overruns it. This test's job script
 * therefore starts on the first tick and then sleeps well past the following
 * one, so the SAME run is still "running" (never a second, catch-up run --
 * that is exactly what this test also confirms) when the schedule says it
 * should already have handed off to the next occurrence.
 *
 * cron.expect_within = 1 (the smallest usable value) keeps the "past the next
 * boundary" wait as short as this feature allows: about a minute, no matter
 * what. Combined with the random 0-60s wait for the first tick, the worst
 * case is up to ~121s -- there is no faster way to demonstrate a real,
 * schedule-driven overrun, and no sub-minute cron.schedule to shorten either
 * leg (fpm_cron_schedule.c parses plain 5-field crontab syntax only).
 *
 * TEST_FPM_TIMEOUT=120 (.github/workflows/build-matrix.yml) sounds like a hard
 * cap that number could exceed, but it is not one: run-tests.php's
 * system_with_timeout() drives the whole --FILE-- script through
 * stream_select() with that number of seconds and only kills it on an IDLE
 * stream -- no output at all for that long -- not on total wall time. This
 * test's own poll loop below produces no stdout while it waits, but the whole
 * run (up to ~150s of wall time in the worst case) has been run repeatedly
 * under the real harness/run-tests.php with TEST_FPM_TIMEOUT=120 and never
 * once hit "process timed out" -- only genuine pass/fail outcomes -- so
 * whatever exact accounting run-tests.php's system_with_timeout() does here,
 * it does not treat this loop's silence as fatal in practice. */
$work = sys_get_temp_dir() . '/fpmng-cron-expect-within-' . getmypid();
@mkdir($work, 0700, true);
/* set_time_limit(0): this pool has no max_execution_time override, and the
 * default (30s) would otherwise fatal the script well before the schedule's
 * next occurrence even arrives -- a timeout of the harness, not of the
 * feature under test. */
file_put_contents("$work/job.php", '<?php set_time_limit(0); sleep(75);');

$cfg = <<<EOT
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
daemonize = no

[tick]
pool.type = cron
cron.schedule = * * * * *
cron.script = $work/job.php
cron.expect_within = 1
pm.status_listen = {{ADDR[operator]}}
pm.status_path = /tick-status
EOT;

$tester = new FPM\Tester($cfg, '<?php');
try {
    $tester->start(extraArgs: ['-R'], forceStderr: true, daemonize: false);
    $tester->expectLogStartNotices();

    $operator = $tester->getListen('{{ADDR[operator]}}');

    $pool = null;
    /* Up to ~121s worst case (see the comment above): 0-60s for the first
     * tick, plus a fixed 61s (one schedule period + cron.expect_within) from
     * there. 150s leaves slack for a slow runner without pretending the
     * schedule-driven wait can be bounded any tighter. run-tests.php's own
     * per-test budget (TEST_FPM_TIMEOUT, system_with_timeout() in
     * run-tests.php) is an IDLE timeout, not a wall-clock one -- it only kills
     * a test that writes NOTHING at all for that long -- so a silent poll
     * loop up to 150s here does not by itself risk a kill. */
    $deadline = time() + 150;
    do {
        usleep(300000);
        $json = json_decode(fpmng_operator_body($operator, '/tick-status'), true, flags: JSON_THROW_ON_ERROR);
        $pool = $json['pools'][0];
    } while (empty($pool['stale']) && time() < $deadline);

    if (empty($pool['stale'])) {
        throw new RuntimeException('cron.expect_within never went stale within the schedule + timeout budget: '
            . var_export($pool, true));
    }
    echo "stale on the status page: ok\n";

    /* No catch-up: still the FIRST (and only) run, still running -- a second,
     * "make up for lost time" run would show as runs > 1 or as running having
     * gone back to false and true again. */
    if (($pool['runs'] ?? null) !== 1) {
        throw new RuntimeException('no-catch-up violated, runs = ' . var_export($pool['runs'] ?? null, true));
    }
    if (($pool['state'] ?? null) !== 'running') {
        throw new RuntimeException('expected the overrunning run to still be running, state = '
            . var_export($pool['state'] ?? null, true));
    }
    echo "no catch-up, same run still going: ok\n";

    /* Not expectLogWarning(): this WARNING is logged from fpm_pool_cron_status(),
     * which runs in the operator-endpoint child while it renders the status
     * page (fpm_operator_pages.c), not in "tick"'s own child -- so like every
     * other pool-policy message that opts into the child-log-via-master
     * channel (fpm_child_log.h), it comes back through the master with a
     * " (child N)" suffix appended (fpm_child_log.c). expectLogWarning()'s
     * matcher anchors the pattern at end-of-line right after the expected
     * text, which that suffix would break; expectLogPattern() is what the
     * existing supervisor tests (e.g. fpmng-supervisor-child-log.phpt) use for
     * exactly this reason.
     *
     * Not "if (!$tester->expectLogPattern(...))" either: expectLogPattern()
     * (tester.inc) does not return the underlying match result -- it is void --
     * and every other call site in this suite relies on that: a mismatch makes
     * LogTool echo "ERROR: ..." to stdout instead (via LogReader::printError()),
     * which then fails the --EXPECT-- diff on its own. Wrapping the call in a
     * truthiness check here would always be true regardless of whether the
     * pattern actually matched, throwing even on a genuine match. */
    $tester->expectLogPattern(
        '/WARNING: .*\[pool tick\] cron: stale -- the schedule\'s next run after the last one was '
        . 'due at \d+, and it is now more than cron\.expect_within = \d+s past that/',
        true,
        30
    );
    echo "warning logged: ok\n";

    echo "Done\n";
} finally {
    $tester->terminate();
    $tester->close();
    @unlink("$work/job.php");
    @rmdir($work);
}
?>
--EXPECT--
stale on the status page: ok
no catch-up, same run still going: ok
warning logged: ok
Done
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>

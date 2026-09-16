--TEST--
fpm-ng: fpmng_supervisor_heartbeat() reports liveness on the status page, and a stalled script shows growing staleness (issue #327)
--SKIPIF--
<?php include "skipif.inc"; ?>
--FILE--
<?php

require_once "tester.inc";
require_once "fpmng-operator.inc";

/* Two supervisor pools, same status endpoint, different paths (the same
 * one-listener-many-paths pattern fpmng-baseline-counters-cron.phpt uses for
 * metrics + status):
 *
 * - "beating" calls fpmng_supervisor_heartbeat() and returns almost at once,
 *   over and over (supervisor.restart = always) -- its heartbeat_age should
 *   stay small the whole time this test watches it.
 * - "silent" calls fpmng_supervisor_heartbeat() exactly ONCE and then sleeps
 *   for a long time in the SAME iteration -- simulating a script that got
 *   stuck doing real work and stopped checking in. Its heartbeat_age should
 *   grow, roughly tracking wall-clock time, while it is still "running" (not
 *   crashed, not restarted -- just quiet).
 *
 * No minute-granularity schedule is involved here (unlike cron.expect_within),
 * so this test only needs a handful of seconds, comfortably inside
 * TEST_FPM_TIMEOUT=120. */
$work = sys_get_temp_dir() . '/fpmng-supervisor-heartbeat-' . getmypid();
@mkdir($work, 0700, true);
file_put_contents("$work/beating.php", '<?php fpmng_supervisor_heartbeat(); usleep(200000);');
file_put_contents("$work/silent.php", '<?php fpmng_supervisor_heartbeat(); sleep(30);');

$cfg = <<<EOT
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
daemonize = no

[beating]
pool.type = supervisor
supervisor.script = $work/beating.php
supervisor.restart = always
pm.status_listen = {{ADDR[operator]}}
pm.status_path = /beating-status

[silent]
pool.type = supervisor
supervisor.script = $work/silent.php
supervisor.restart = never
pm.status_listen = {{ADDR[operator]}}
pm.status_path = /silent-status
EOT;

function pool_row(string $address, string $path): array
{
    $json = json_decode(fpmng_operator_body($address, $path), true, flags: JSON_THROW_ON_ERROR);
    return $json['pools'][0];
}

$tester = new FPM\Tester($cfg, '<?php');
try {
    $tester->start(extraArgs: ['-R'], forceStderr: true, daemonize: false);
    $tester->expectLogStartNotices();

    $operator = $tester->getListen('{{ADDR[operator]}}');

    /* Give both scripts time to make their first call: "beating" within its
     * first ~0.2s iteration, "silent" right at the start of its one long
     * iteration. */
    $deadline = time() + 20;
    do {
        usleep(200000);
        $beating = pool_row($operator, '/beating-status');
        $silent = pool_row($operator, '/silent-status');
    } while ((!array_key_exists('heartbeat_age', $beating) || !array_key_exists('heartbeat_age', $silent))
        && time() < $deadline);

    if (!array_key_exists('heartbeat_age', $beating) || !array_key_exists('heartbeat_age', $silent)) {
        throw new RuntimeException('heartbeat_age missing: beating=' . var_export($beating, true)
            . ' silent=' . var_export($silent, true));
    }
    echo "both pools report a heartbeat: ok\n";

    /* "silent" keeps running (still inside its one sleep(30) iteration, not
     * restarted, not gave_up) the whole time this test watches it -- if it had
     * restarted, that would itself call fpmng_supervisor_heartbeat() again and
     * defeat the point of the scenario. */
    $silent = pool_row($operator, '/silent-status');
    if (($silent['state'] ?? null) !== 'running') {
        throw new RuntimeException('expected silent to still be running its one long iteration, state = '
            . var_export($silent['state'] ?? null, true));
    }

    /* Sample "beating" repeatedly over a few seconds: every sample should stay
     * small, because it calls the function again every ~0.2-0.5s (fork +
     * script startup included). Sample "silent" once now and once after the
     * wait: its age should have grown by roughly the wait, because nothing in
     * that pool calls the function a second time. Deliberately compares
     * "grew by a lot" rather than an exact value -- avoiding a flaky
     * exact-timing assertion while still being a real check. */
    $silent_age_before = $silent['heartbeat_age'];
    $max_beating_age = 0;
    $sample_until = time() + 6;
    while (time() < $sample_until) {
        $beating = pool_row($operator, '/beating-status');
        $max_beating_age = max($max_beating_age, $beating['heartbeat_age']);
        usleep(300000);
    }

    if ($max_beating_age > 5) {
        throw new RuntimeException('beating heartbeat_age grew too large: ' . $max_beating_age);
    }
    echo "beating heartbeat_age stays small: ok\n";

    $silent = pool_row($operator, '/silent-status');
    $silent_age_after = $silent['heartbeat_age'];
    if (($silent['state'] ?? null) !== 'running') {
        throw new RuntimeException('silent stopped running mid-test, state = '
            . var_export($silent['state'] ?? null, true));
    }
    if ($silent_age_after < $silent_age_before + 4) {
        throw new RuntimeException("silent heartbeat_age did not grow with time: before=$silent_age_before "
            . "after=$silent_age_after");
    }
    echo "silent heartbeat_age grows while stuck: ok\n";

    echo "Done\n";
} finally {
    $tester->terminate();
    $tester->close();
    @unlink("$work/beating.php");
    @unlink("$work/silent.php");
    @rmdir($work);
}
?>
--EXPECT--
both pools report a heartbeat: ok
beating heartbeat_age stays small: ok
silent heartbeat_age grows while stuck: ok
Done
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>

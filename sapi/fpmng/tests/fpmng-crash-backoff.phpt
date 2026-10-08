--TEST--
fpm-ng: a pm child that fails fast is respawned with backoff, the pool gives up once and says so, and a served request or a whole window ends the streak (issue #727)
--SKIPIF--
<?php
include "fpmng-skipif.inc";
if (PHP_OS_FAMILY !== 'Linux') {
    die('skip the test reads the process table with ps -eo and expects the Linux process title');
}
?>
--FILE--
<?php

require_once "tester.inc";
require_once "fpmng-operator.inc";
require_once "fpmng-saturation.inc";

/* Failing children are simulated: the test SIGKILLs the pm child of a pool
 * while it is idle, so it dies inside the 10 second window without serving a
 * request. Each kill is one fast failure.
 *
 * Three pools, one operator listener:
 *   ramp   never gives up (pm.max_consecutive_failures = 0). The respawn delay
 *          grows: the first respawn is at once, then about 0.5-1 s, then 1-2 s.
 *   gave   gives up after 3 failures. One ALERT, and the operator pages say so.
 *   served a child that serves a request before it dies ends the streak. */
$root = sys_get_temp_dir() . '/fpmng-crash-backoff-' . getmypid();
@mkdir($root, 0700, true);
file_put_contents("$root/ok.php", '<?php echo "ok";');

$cfg = <<<EOT
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
log_level = notice

[ramp]
listen = {{ADDR[ramp]}}
chdir = $root
pm = static
pm.max_children = 1
pm.max_consecutive_failures = 0
operator.metrics_listen = {{ADDR[operator]}}
operator.metrics_path = /metrics/ramp
operator.status_listen = {{ADDR[operator]}}
operator.status_path = /ramp-status

[gave]
listen = {{ADDR[gave]}}
chdir = $root
pm = static
pm.max_children = 1
pm.max_consecutive_failures = 3
operator.metrics_listen = {{ADDR[operator]}}
operator.metrics_path = /metrics/gave
operator.status_listen = {{ADDR[operator]}}
operator.status_path = /gave-status

[served]
listen = {{ADDR[served]}}
chdir = $root
pm = static
pm.max_children = 1
pm.max_consecutive_failures = 3
operator.metrics_listen = {{ADDR[operator]}}
operator.metrics_path = /metrics/served
EOT;

/* The pm child of $pool: a direct child of the master whose process title ends
 * in "pool <name>". The master's own pid is passed in, so another php-fpm-ng
 * instance on the box is not counted. */
function crashChildPid(int $masterPid, string $pool): ?int
{
    foreach (explode("\n", (string) shell_exec('ps -eo pid,ppid,args 2>/dev/null')) as $line) {
        if (preg_match('/^\s*(\d+)\s+(\d+)\s+(.*)$/', $line, $m)
            && (int) $m[2] === $masterPid
            && preg_match('/pool ' . preg_quote($pool, '/') . '\s*$/', $m[3])) {
            return (int) $m[1];
        }
    }
    return null;
}

/* Waits until $pool has a child other than $not, and returns its pid. */
function awaitChild(int $masterPid, string $pool, ?int $not = null): int
{
    $deadline = microtime(true) + 15.0;
    do {
        $pid = crashChildPid($masterPid, $pool);
        if ($pid !== null && $pid !== $not) {
            return $pid;
        }
        usleep(20000);
    } while (microtime(true) < $deadline);
    throw new RuntimeException("no child for pool $pool after $not");
}

/* Kills the pm child $old of $pool and returns the seconds until its successor
 * runs. */
function killAndAwaitRespawn(int $masterPid, string $pool, int $old): float
{
    $t0 = microtime(true);
    shell_exec('kill -9 ' . $old);
    awaitChild($masterPid, $pool, $old);
    return microtime(true) - $t0;
}

/* The row of $pool in a status JSON page, or null. */
function statusRow(array $status, string $pool): ?array
{
    foreach ($status['pools'] ?? [] as $row) {
        if (($row['name'] ?? null) === $pool) {
            return $row;
        }
    }
    return null;
}

$tester = new FPM\Tester($cfg, '<?php echo "unused";');
try {
    @unlink($tester->getPrefixedFile(FPM\Tester::FILE_EXT_LOG_ERR));
    $tester->start([], false);
    $tester->switchLogSource('{{FILE:LOG}}');
    $tester->expectLogStartNotices();

    $masterPid = $tester->getPid();
    $operator = $tester->getListen('{{ADDR[operator]}}');
    $served = $tester->getListen('{{ADDR[served]}}');
    $metricsOf = static fn(string $pool): string => fpmng_operator_body($operator, "/metrics/$pool");

    /* The first kills of all three pools run within the first seconds after
     * the start. A child that lives longer than the window ends the streak when
     * it dies, so an older child would not count as a fast failure.
     *
     * ramp: three fast failures. The first respawn is at once, the second
     * waits 0.5-1 s, the third 1-2 s. Polling adds up to 20 ms per step. */
    awaitChild($masterPid, 'ramp');
    $gaps = [];
    for ($i = 0; $i < 3; $i++) {
        $gaps[] = killAndAwaitRespawn($masterPid, 'ramp', awaitChild($masterPid, 'ramp'));
    }
    fpmng_saturation_check($gaps[0] < 0.5, 'the first respawn is not at once: ' . var_export($gaps, true));
    fpmng_saturation_check($gaps[1] >= 0.45 && $gaps[1] <= 1.3, 'the second respawn gap is outside 0.5-1 s: ' . var_export($gaps, true));
    fpmng_saturation_check($gaps[2] >= 0.95 && $gaps[2] <= 2.3, 'the third respawn gap is outside 1-2 s: ' . var_export($gaps, true));
    $body = $metricsOf('ramp');
    fpmng_saturation_check(fpmng_saturation_value($body, 'fpmng_pool_consecutive_crashes{pool="ramp"}') === 3.0,
        "ramp does not count three crashes:\n$body");
    fpmng_saturation_check(fpmng_saturation_value($body, 'fpmng_pool_crash_gave_up{pool="ramp"}') === 0.0,
        "ramp gave up with pm.max_consecutive_failures = 0:\n$body");
    /* The respawn delay is the wait for the next child. The child is forked
     * now, so no wait is pending any more. */
    fpmng_saturation_wait(static function () use ($metricsOf): bool {
        return fpmng_saturation_value($metricsOf('ramp'), 'fpmng_pool_respawn_delay_ms{pool="ramp"}') === 0.0;
    }, 'the ramp respawn delay to clear once its child is forked', static fn(): string => $metricsOf('ramp'));
    echo "ramp: the first respawn is at once, then the delay grows: ok\n";

    /* gave: the third fast failure gives up. The respawn then waits 30-60 s,
     * so the test does not wait for it. */
    awaitChild($masterPid, 'gave');
    killAndAwaitRespawn($masterPid, 'gave', awaitChild($masterPid, 'gave'));
    killAndAwaitRespawn($masterPid, 'gave', awaitChild($masterPid, 'gave'));
    $old = awaitChild($masterPid, 'gave');
    shell_exec('kill -9 ' . $old);
    fpmng_saturation_wait(static function () use ($metricsOf): bool {
        return fpmng_saturation_value($metricsOf('gave'), 'fpmng_pool_crash_gave_up{pool="gave"}') === 1.0;
    }, 'the gave pool to give up', static fn(): string => $metricsOf('gave'));
    $body = $metricsOf('gave');
    fpmng_saturation_check(fpmng_saturation_value($body, 'fpmng_pool_consecutive_crashes{pool="gave"}') === 3.0,
        "gave pool does not count three crashes:\n$body");
    fpmng_saturation_check((fpmng_saturation_value($body, 'fpmng_pool_respawn_delay_ms{pool="gave"}') ?? 0) >= 30000,
        "gave pool does not wait the capped delay:\n$body");
    $status = json_decode(fpmng_operator_body($operator, '/gave-status'), true, 512, JSON_THROW_ON_ERROR);
    $row = statusRow($status, 'gave');
    fpmng_saturation_check($row !== null && ($row['crash_gave_up'] ?? null) === true && ($row['consecutive_crashes'] ?? null) === 3,
        'gave pool status JSON lacks the crash keys: ' . var_export($row, true));
    echo "gave: the pool reports that it gave up: ok\n";

    /* served: two fast failures, then a child serves a request and dies. The
     * request ends the streak, so the counter is back at zero. */
    awaitChild($masterPid, 'served');
    killAndAwaitRespawn($masterPid, 'served', awaitChild($masterPid, 'served'));
    $child = awaitChild($masterPid, 'served');
    killAndAwaitRespawn($masterPid, 'served', $child);
    $child = awaitChild($masterPid, 'served');
    $fp = fpmng_saturation_fcgi_start($served, "$root/ok.php");
    stream_set_timeout($fp, 15);
    $raw = (string) stream_get_contents($fp);
    fclose($fp);
    fpmng_saturation_check(str_contains($raw, 'ok'), "served pool did not answer:\n$raw");
    fpmng_saturation_check(fpmng_saturation_value($metricsOf('served'), 'fpmng_pool_consecutive_crashes{pool="served"}') === 2.0,
        "served pool does not count two crashes before the request:\n" . $metricsOf('served'));
    killAndAwaitRespawn($masterPid, 'served', $child);
    fpmng_saturation_wait(static function () use ($metricsOf): bool {
        return fpmng_saturation_value($metricsOf('served'), 'fpmng_pool_consecutive_crashes{pool="served"}') === 0.0;
    }, 'the served streak to end', static fn(): string => $metricsOf('served'));
    $body = $metricsOf('served');
    fpmng_saturation_check(fpmng_saturation_value($body, 'fpmng_pool_crash_gave_up{pool="served"}') === 0.0,
        "served pool still gave up:\n$body");
    echo "served: a served request ends the streak: ok\n";

    /* ramp: the child that is now alive lives a whole window, so the streak
     * ends while it still runs. */
    fpmng_saturation_wait(static function () use ($metricsOf): bool {
        return fpmng_saturation_value($metricsOf('ramp'), 'fpmng_pool_consecutive_crashes{pool="ramp"}') === 0.0;
    }, 'the ramp streak to end after a whole window', static fn(): string => $metricsOf('ramp'));
    echo "ramp: a child that lives a whole window ends the streak: ok\n";

    /* ALERT once: the gave pool logged it on its third failure and never again,
     * and the ramp pool, which never gives up, logged none. */
    $log = (string) file_get_contents($tester->getPrefixedFile(FPM\Tester::FILE_EXT_LOG_ERR));
    fpmng_saturation_check(substr_count($log, '[pool gave] gave up after') === 1,
        "gave pool does not log exactly one ALERT:\n$log");
    fpmng_saturation_check(preg_match('/ALERT.*\[pool gave\] gave up after 3 children in a row/', $log) === 1,
        "the give-up line is not an ALERT:\n$log");
    fpmng_saturation_check(!str_contains($log, '[pool ramp] gave up'),
        "ramp pool logged a give-up:\n$log");
    echo "gave up is logged once as an ALERT: ok\n";
    echo "Done\n";
} finally {
    $tester->terminate();
    $tester->close();
    @unlink("$root/ok.php");
    @rmdir($root);
}
?>
--EXPECT--
ramp: the first respawn is at once, then the delay grows: ok
gave: the pool reports that it gave up: ok
served: a served request ends the streak: ok
ramp: a child that lives a whole window ends the streak: ok
gave up is logged once as an ALERT: ok
Done

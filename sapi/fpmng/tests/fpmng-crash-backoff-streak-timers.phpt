--TEST--
fpm-ng: the crash backoff timers keep the master alive when a second streak starts while the give-up gate is queued, and a dynamic pool in backoff does not log "seems busy" (issue #727)
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

/* Failing children are simulated with SIGKILL, as in fpmng-crash-backoff.phpt:
 * a child killed inside the 10 second window without serving a request is a
 * fast failure.
 *
 * Two pools, one master, one event queue:
 *   twin  pm = static, two children, gives up after 3 fast failures. The give-up
 *         leaves the gate timer queued for 30-60 s. A later streak then arms the
 *         survive timer, and that timer fires while the gate timer is still
 *         queued. The survive timer must not remove the gate timer. Before the fix
 *         the master crashed on this sequence.
 *   dyn   pm = dynamic, gives up after 2 fast failures. The gate stays closed for
 *         30-60 s. The idle maintenance runs once a second in that time, and it
 *         must not log "seems busy" for a pool that waits out its backoff. */
$root = sys_get_temp_dir() . '/fpmng-crash-backoff-timers-' . getmypid();
@mkdir($root, 0700, true);
file_put_contents("$root/ok.php", '<?php echo "ok";');

$cfg = <<<EOT
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
log_level = notice

[twin]
listen = {{ADDR[twin]}}
chdir = $root
pm = static
pm.max_children = 2
pm.max_consecutive_failures = 3
operator.metrics_listen = {{ADDR[operator]}}
operator.metrics_path = /metrics/twin
operator.status_listen = {{ADDR[operator]}}
operator.status_path = /twin-status

[dyn]
listen = {{ADDR[dyn]}}
chdir = $root
pm = dynamic
pm.max_children = 2
pm.start_servers = 1
pm.min_spare_servers = 1
pm.max_spare_servers = 1
pm.max_consecutive_failures = 2
operator.metrics_listen = {{ADDR[operator]}}
operator.metrics_path = /metrics/dyn
EOT;

/* The pm children of $pool: direct children of the master whose process title
 * ends in "pool <name>". */
function poolChildPids(int $masterPid, string $pool): array
{
    $pids = [];
    foreach (explode("\n", (string) shell_exec('ps -eo pid,ppid,args 2>/dev/null')) as $line) {
        if (preg_match('/^\s*(\d+)\s+(\d+)\s+(.*)$/', $line, $m)
            && (int) $m[2] === $masterPid
            && preg_match('/pool ' . preg_quote($pool, '/') . '\s*$/', $m[3])) {
            $pids[] = (int) $m[1];
        }
    }
    return $pids;
}

/* Waits until $pool has a pm child whose pid is not in $known, and returns it. */
function awaitNewChild(int $masterPid, string $pool, array $known): int
{
    $deadline = microtime(true) + 15.0;
    do {
        foreach (poolChildPids($masterPid, $pool) as $pid) {
            if (!in_array($pid, $known, true)) {
                return $pid;
            }
        }
        usleep(20000);
    } while (microtime(true) < $deadline);
    throw new RuntimeException("no new child for pool $pool");
}

$tester = new FPM\Tester($cfg, '<?php echo "unused";');
try {
    @unlink($tester->getPrefixedFile(FPM\Tester::FILE_EXT_LOG_ERR));
    $tester->start([], false);
    $tester->switchLogSource('{{FILE:LOG}}');
    $tester->expectLogStartNotices();

    $masterPid = $tester->getPid();
    $operator = $tester->getListen('{{ADDR[operator]}}');
    $served = $tester->getListen('{{ADDR[twin]}}');
    $metricsOf = static fn(string $pool): string => fpmng_operator_body($operator, "/metrics/$pool");

    /* dyn: the start child is the first fast failure, the child that the
     * maintenance starts after it is the second, and that one gives up. */
    fpmng_saturation_wait(static fn(): bool => count(poolChildPids($masterPid, 'dyn')) === 1, 'the dyn start child');
    $dynFirst = poolChildPids($masterPid, 'dyn')[0];
    shell_exec('kill -9 ' . $dynFirst);
    $dynSecond = awaitNewChild($masterPid, 'dyn', [$dynFirst]);
    shell_exec('kill -9 ' . $dynSecond);
    fpmng_saturation_wait(static function () use ($metricsOf): bool {
        return fpmng_saturation_value($metricsOf('dyn'), 'fpmng_pool_crash_gave_up{pool="dyn"}') === 1.0;
    }, 'the dyn pool to give up', static fn(): string => $metricsOf('dyn'));

    /* twin: two children. Three fast failures give up the pool. The first two
     * respawns are at once and after 0.5-1 s; the third gives up. */
    fpmng_saturation_wait(static fn(): bool => count(poolChildPids($masterPid, 'twin')) === 2, 'the two twin children');
    [$a, $b] = poolChildPids($masterPid, 'twin');
    shell_exec('kill -9 ' . $a);
    $a1 = awaitNewChild($masterPid, 'twin', [$a, $b]);
    shell_exec('kill -9 ' . $b);
    $b1 = awaitNewChild($masterPid, 'twin', [$a1, $b]);
    shell_exec('kill -9 ' . $a1);
    fpmng_saturation_wait(static function () use ($metricsOf): bool {
        return fpmng_saturation_value($metricsOf('twin'), 'fpmng_pool_crash_gave_up{pool="twin"}') === 1.0;
    }, 'the twin pool to give up', static fn(): string => $metricsOf('twin'));
    echo "twin: three fast failures give up the pool: ok\n";

    /* The only live child, b1, serves a request, then dies. The request ends the
     * streak at once, so the pool forks a replacement without a wait. The gate
     * timer of the give-up stays queued. */
    $fp = fpmng_saturation_fcgi_start($served, "$root/ok.php");
    stream_set_timeout($fp, 15);
    $raw = (string) stream_get_contents($fp);
    fclose($fp);
    fpmng_saturation_check(str_contains($raw, 'ok'), "twin pool did not answer:\n$raw");
    shell_exec('kill -9 ' . $b1);
    $b2 = awaitNewChild($masterPid, 'twin', [$b1]);
    fpmng_saturation_wait(static function () use ($metricsOf): bool {
        return fpmng_saturation_value($metricsOf('twin'), 'fpmng_pool_crash_gave_up{pool="twin"}') === 0.0;
    }, 'the twin streak to end after a served request', static fn(): string => $metricsOf('twin'));

    /* A new streak. The fresh child dies inside the window, so its replacement
     * starts at once, and that replacement arms the survive timer. The survive
     * timer fires after a whole window, while the gate timer is still queued. */
    shell_exec('kill -9 ' . $b2);
    awaitNewChild($masterPid, 'twin', [$b2]);
    fpmng_saturation_wait(static function () use ($metricsOf): bool {
        return fpmng_saturation_value($metricsOf('twin'), 'fpmng_pool_consecutive_crashes{pool="twin"}') === 1.0;
    }, 'the twin pool to count the new streak', static fn(): string => $metricsOf('twin'));
    fpmng_saturation_wait(static function () use ($metricsOf): bool {
        return fpmng_saturation_value($metricsOf('twin'), 'fpmng_pool_consecutive_crashes{pool="twin"}') === 0.0;
    }, 'the new twin streak to end after a whole window', static fn(): string => $metricsOf('twin'));
    $body = $metricsOf('twin');
    fpmng_saturation_check(fpmng_saturation_value($body, 'fpmng_pool_crash_gave_up{pool="twin"}') === 0.0,
        "twin pool still gave up after its streak ended:\n$body");
    $log = (string) file_get_contents($tester->getPrefixedFile(FPM\Tester::FILE_EXT_LOG_ERR));
    fpmng_saturation_check(preg_match('/\[pool twin\] crash streak of 1 ended: child \d+ has lived a whole window/', $log) === 1,
        "the twin streak did not end on the window:\n$log");
    echo "twin: the master survives a streak that ends on the window: ok\n";

    /* dyn: the maintenance ran several times while the gate was closed (the
     * twin steps and the window above take more than 10 seconds), and it did
     * not call the spawn rate of the pool "busy". */
    fpmng_saturation_check(!str_contains($log, '[pool dyn] seems busy'),
        "dyn pool logs seems busy while it waits out its backoff:\n$log");
    fpmng_saturation_check(substr_count($log, '[pool dyn] gave up after') === 1,
        "dyn pool does not log exactly one ALERT:\n$log");
    echo "dyn: a pool in backoff does not report itself busy: ok\n";
    echo "Done\n";
} finally {
    $tester->terminate();
    $tester->close();
    @unlink("$root/ok.php");
    @rmdir($root);
}
?>
--EXPECT--
twin: three fast failures give up the pool: ok
twin: the master survives a streak that ends on the window: ok
dyn: a pool in backoff does not report itself busy: ok
Done

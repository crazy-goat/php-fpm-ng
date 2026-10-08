--TEST--
fpm-ng: a metrics slot still held by a #329 survivor is not given to a pool added by a second reload (issue #692)
--SKIPIF--
<?php
include "fpmng-skipif.inc";
fpmng_skip_if_pool_type_unsupported('supervisor');
fpmng_skip_if_pool_type_unsupported('http-direct');
?>
--FILE--
<?php

require_once "tester.inc";
require_once "fpmng-operator.inc";

/* Two selective reloads back to back. Pool [s] is a supervisor with two
 * copies at first and occupies metrics slots 0-1. Reload A grows it to three
 * copies and, with supervisor.start_jitter, its new generation waits before
 * the first start of each copy. Reload A spares one old copy: that is the
 * #329 survivor. It keeps writing its counter into slots 0-1.
 *
 * Reload B runs while the survivor is still unconfirmed, so the generation
 * after reload A still tracks it. Reload B grows [s] again and adds pool [n].
 * Before issue #692 the survivor was not carried across reload B, its slot
 * reservation ran out, and [n] was placed on slots 0-1: its endpoint then
 * showed the survivor's counter. Pool [h] is
 * spared by both reloads, so the metrics region is carried across both execs.
 *
 * supervisor.start_jitter is random in [0, 30] seconds. The survivor is still
 * unconfirmed at reload B unless the jitter drawn for the new copies is
 * shorter than the gap between the reloads, so the test fails on an unfixed
 * build in almost every run and passes on every run of a fixed build. */
$root = sys_get_temp_dir() . '/fpmng-reload-surv-b2b-' . getmypid();
@mkdir($root, 0700, true);
$alive = "$root/alive.log";
$stop = "$root/stop";

$cleanup = function () use ($root, $alive, $stop) {
    @unlink($stop);
    @unlink($alive);
    @unlink("$root/s.php");
    @unlink("$root/h.php");
    @unlink("$root/n.php");
    @rmdir($root);
};

/* Writes its pid and the counter [s] keeps in its slot. The stop file ends
 * the orphaned survivor when the test is over: nothing else would. exit()
 * is not enough, a supervisor copy does not end with its script, and the
 * posix extension may be absent, so the copy kills itself with kill(1). */
file_put_contents("$root/s.php", <<<PHP
<?php
error_reporting(0);
fpm_metric_register('s_ticks', 'counter', 'survivor ticks');
for (;;) {
    if (file_exists('{$stop}')) {
        exec('kill -9 ' . getmypid());
    }
    fpm_metric_inc('s_ticks', 1.0);
    @file_put_contents('{$alive}', getmypid() . "\\n", FILE_APPEND);
    usleep(20000);
}
PHP);
file_put_contents("$root/h.php", <<<'PHP'
<?php
echo getmypid();
PHP);
file_put_contents("$root/n.php", <<<'PHP'
<?php
fpm_metric_register('n_hits', 'counter', 'n hits');
fpm_metric_inc('n_hits', 1.0);
echo getmypid();
PHP);

/* $processes copies of [s]; the start jitter is set only from reload A on, so
 * the first generation starts without a delay. [n] is added by reload B. */
$config = function (int $processes, int $jitter, bool $withN) use ($root) {
    $jitterLine = $jitter > 0 ? "supervisor.start_jitter = $jitter\n" : '';
    $cfg = <<<EOT
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
log_level = notice
reload.selective = yes

[s]
pool.type = supervisor
supervisor.script = $root/s.php
supervisor.processes = $processes
supervisor.restart = never
supervisor.restart_delay = 1
supervisor.stop_timeout = 1
{$jitterLine}
[h]
listen = {{ADDR[h]}}
chdir = $root
pm = static
pm.max_children = 1
pool.type = http-direct
http.front_controller = /h.php
EOT;
    if ($withN) {
        $cfg .= <<<EOT

[n]
listen = {{ADDR[n]}}
chdir = $root
pm = static
pm.max_children = 2
pool.type = http-direct
http.front_controller = /n.php
operator.metrics_listen = {{ADDR[operatorn]}}
operator.metrics_path = /n-metrics
EOT;
    }
    return $cfg;
};

function check(bool $condition, string $message): void
{
    if (!$condition) {
        throw new RuntimeException($message);
    }
}

/* The operator endpoint and the front controller of [n] are only reachable
 * once the new generation has started them: retry until they answer. */
function retryUntilUp(callable $fn, int $seconds)
{
    $deadline = time() + $seconds;
    for (;;) {
        try {
            return $fn();
        } catch (Throwable $e) {
            if (time() >= $deadline) {
                throw $e;
            }
            usleep(100000);
        }
    }
}

function waitForLog(string $file, string $needle, int $seconds): bool
{
    $deadline = time() + $seconds;
    while (time() < $deadline) {
        if (str_contains((string) @file_get_contents($file), $needle)) {
            return true;
        }
        usleep(100000);
    }
    return false;
}

$tester = new FPM\Tester($config(2, 0, false), '<?php');
try {
    @unlink($tester->getPrefixedFile(FPM\Tester::FILE_EXT_LOG_ERR));
    $tester->start([], false);
    $tester->switchLogSource('{{FILE:LOG}}');
    $tester->expectLogStartNotices();
    $logFile = $tester->getPrefixedFile(FPM\Tester::FILE_EXT_LOG_ERR);

    /* Both copies of [s] are running before the first reload. */
    $deadline = time() + 15;
    $pidsSeen = [];
    while (time() < $deadline && count($pidsSeen) < 2) {
        foreach (explode("\n", (string) @file_get_contents($alive)) as $line) {
            if (trim($line) !== '') {
                $pidsSeen[trim($line)] = true;
            }
        }
        usleep(100000);
    }
    check(count($pidsSeen) >= 2, 'before reload A: pool [s] did not start two copies');

    /* Reload A: one old copy of [s] is spared as the survivor. The new
     * generation logs it, which is the point where its window is open. */
    $tester->reload($config(3, 30, false));
    check(waitForLog($logFile, 'is still running the previous', 20), 'reload A: no survivor was tracked');

    /* Reload B, while the survivor is still unconfirmed. */
    $tester->reload($config(4, 30, true));

    $opN = $tester->getListen('{{ADDR[operatorn]}}');
    $n = $tester->getListen('{{ADDR[n]}}');
    retryUntilUp(fn () => fpmng_operator_body($opN, '/n-metrics'), 20);
    retryUntilUp(function () use ($n) {
        $fp = stream_socket_client("tcp://$n", $errno, $error, 5);
        check((bool) $fp, "connect $n: $error");
        stream_set_timeout($fp, 5);
        fwrite($fp, "GET / HTTP/1.1\r\nHost: test\r\nConnection: close\r\n\r\n");
        $raw = (string) stream_get_contents($fp);
        fclose($fp);
        return $raw;
    }, 20);

    $body = fpmng_operator_body($opN, '/n-metrics');
    $hits = preg_match('/^n_hits\{pool="n"\} (\S+)$/m', $body, $m) ? (float) $m[1] : null;
    check($hits === 1.0, "pool [n] did not count its hit: n_hits = " . var_export($hits, true));

    /* The survivor of reload A is carried across reload B (issue #692): the
     * generation that reload B started tracks it again, and logs it again. */
    check(substr_count((string) @file_get_contents($logFile), 'is still running the previous') >= 2,
        'reload B: the survivor of reload A was not carried into the next generation');

    /* The region was carried across both execs only if [h] was spared by
     * each reload; without that the test proves nothing. */
    $log = (string) @file_get_contents($logFile);
    check(preg_match_all('/\[pool h\][^\n]*config unchanged -- sparing/', $log) >= 2, 'pool [h] was not spared by both reloads');

    /* The survivor writes s_ticks on every iteration into the slots of [s]'s
     * replaced copy. [n]'s endpoint shows slots 0-1 only if [n] was given them. */
    $shared = preg_match('/^s_ticks\{[^}]*\} (\S+)$/m', $body, $m) ? (float) $m[1] : 0.0;
    check($shared === 0.0, "pool [n] shares a slot with the survivor of [s]: s_ticks = $shared in its endpoint");

    echo "no pool shares a slot with the survivor: ok\n";
    echo "Done\n";
} finally {
    $tester->terminate();
    $tester->close();
    /* The survivor is not a child of any running master, so terminate()
     * does not reach it. It checks the stop file every 20 ms and kills itself.
     * It must see the file before cleanup() removes it, or it keeps the
     * test's listener open for the next test in this port lane. */
    @touch($stop);
    usleep(500000);
    $cleanup();
}
?>
--EXPECT--
no pool shares a slot with the survivor: ok
Done
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>

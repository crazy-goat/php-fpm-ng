--TEST--
fpm-ng: SIGUSR2 with a configuration that does not load keeps the old generation serving (issue #640)
--SKIPIF--
<?php include "skipif.inc"; ?>
--FILE--
<?php

require_once "tester.inc";

/* Issue #640: a reload is an execvp() of the master, so a configuration the
 * next generation cannot parse took the whole service down with it. nginx keeps
 * the old configuration; this master now asks its own `-t` before it signals
 * anything (fpm_reload_config_check.c, called from fpm_pctl() before the state
 * changes).
 *
 * The pool answers with its own pid between two markers, so "the old generation
 * is still serving" is checked as an identity and not as a log line: the pid
 * that answers after the refused reload has to be the very pid that answered
 * before it. A master that tore the pool down and re-forked it -- the old
 * behaviour -- cannot produce the same pid twice.
 *
 * The Tester only hands out free addresses. The master is started from this
 * test because the file it reloads has to be rewritten in place, and the
 * argument vector this master saves for the reload is exactly what the gate
 * hands the check (fpm_reload_config_check.h). */

$root = sys_get_temp_dir() . '/fpmng-reload-badconf-' . getmypid();
@mkdir($root, 0700, true);
$log = "$root/error.log";
$conf = "$root/fpm.conf";

function fail(string $what): void
{
    throw new RuntimeException($what);
}

function pid_alive(int $pid): bool
{
    if ($pid <= 1 || !is_dir("/proc/$pid")) {
        return false;
    }
    $stat = @file_get_contents("/proc/$pid/stat");
    $close = $stat === false ? false : strrpos($stat, ')');
    return $close === false || ($stat[$close + 2] ?? '') !== 'Z';
}

/* The pid of the http-direct child that answered, or '' if none did within
 * $seconds. */
function request(string $address, int $seconds = 20): string
{
    $deadline = microtime(true) + $seconds;
    do {
        $fp = @stream_socket_client("tcp://$address", $errno, $error, 2);
        if ($fp) {
            stream_set_timeout($fp, 10);
            fwrite($fp, "GET /app.php HTTP/1.1\r\nHost: test\r\nConnection: close\r\n\r\n");
            $body = '';
            while (!feof($fp)) {
                $chunk = fread($fp, 8192);
                if ($chunk === false || $chunk === '') {
                    break;
                }
                $body .= $chunk;
            }
            fclose($fp);
            if (preg_match('/PID:(\d+):END/', $body, $m)) {
                return $m[1];
            }
        }
        usleep(100000);
    } while (microtime(true) < $deadline);
    return '';
}

function writeConfig(string $conf, string $root, string $log, string $keep, string $maxChildren): void
{
    file_put_contents($conf, <<<CFG
[global]
error_log = $log
pid = $root/fpm.pid
log_level = notice
[keep]
listen = $keep
pool.type = http-direct
pm = static
pm.max_children = $maxChildren
chdir = $root
http.front_controller = /app.php
CFG);
}

$tester = new FPM\Tester('[global]', '<?php');
$keep = $tester->getAddr('ipv4', '[keep]');

if (FPM\Tester::findExecutable() === false) {
    fail('cannot find the php-fpm-ng binary to test');
}

file_put_contents("$root/app.php", <<<'PHP'
<?php
echo 'PID:', getmypid(), ':END';
PHP);

$proc = null;
$masterPid = 0;
try {
    writeConfig($conf, $root, $log, $keep, '1');
    $proc = proc_open([FPM\Tester::findExecutable(), '-n', '-y', $conf, '-F'],
        [0 => ['file', '/dev/null', 'r'], 1 => ['file', '/dev/null', 'w'], 2 => ['file', '/dev/null', 'w']], $pipes);
    if (!is_resource($proc)) {
        fail('cannot start the master');
    }
    $masterPid = (int) proc_get_status($proc)['pid'];

    $before = request($keep);
    if ($before === '') {
        fail("the pool never answered before the reload\n" . (string) @file_get_contents($log));
    }
    echo "pool serving before the reload: pid $before\n";

    /* The break is a value, not a syntax error: an operator's real mistake is
     * pm.max_children = 0, which fpm_conf_check_pool_pm() refuses
     * (fpm_conf.c, "pm.max_children must be a positive value"). */
    writeConfig($conf, $root, $log, $keep, '0');

    exec("kill -USR2 $masterPid");

    /* A reload that starts would have signalled the worker within milliseconds;
     * three seconds is far more than the gate needs and still quick enough that
     * a hang here shows up as this test failing rather than as a stalled suite. */
    usleep(3000000);

    if (!pid_alive($masterPid)) {
        fail("master $masterPid is gone: the refused reload took the service down\n"
            . (string) @file_get_contents($log));
    }
    echo "master still running: ok\n";

    $after = request($keep, 5);
    if ($after === '') {
        fail("the pool stopped answering after the refused reload\n" . (string) @file_get_contents($log));
    }
    if ($after !== $before) {
        fail("the pool was restarted by the refused reload: pid $before became $after");
    }
    echo "same worker still serving: pid $after\n";

    $logText = (string) @file_get_contents($log);

    /* The ERROR the gate writes. */
    if (!str_contains($logText, 'reload refused: the configuration test says')) {
        fail("no ERROR line about the refused reload\n$logText");
    }
    /* The child's own diagnostic, from the -t the gate forked. */
    if (!str_contains($logText, 'pm.max_children must be a positive value')) {
        fail("the child's diagnostic for the broken value is not in the log\n$logText");
    }
    echo "ERROR with the child's diagnostic: ok\n";

    /* The check's "configuration file ... test is successful" NOTICE must not be
     * there: that would mean the gate tested something else. */
    if (str_contains($logText, 'test is successful')) {
        fail("a configuration test was reported successful\n$logText");
    }

    /* A fixed configuration loads on the next SIGUSR2, without a restart: the
     * refusal is not a latch. */
    writeConfig($conf, $root, $log, $keep, '2');
    exec("kill -USR2 $masterPid");

    $deadline = time() + 30;
    $newPid = '';
    while (time() < $deadline) {
        $newPid = request($keep, 2);
        if ($newPid !== '' && $newPid !== $before) {
            break;
        }
        usleep(200000);
    }
    if ($newPid === '' || $newPid === $before) {
        fail("the reload after the fix did not replace the worker (was $before, now '$newPid')\n"
            . (string) @file_get_contents($log));
    }
    echo "reload works again after the fix: pid $newPid\n";

    echo "Done\n";
} finally {
    if ($masterPid > 1 && pid_alive($masterPid)) {
        exec("kill -QUIT $masterPid 2>/dev/null");
    }
    usleep(200000);
    if (is_resource($proc)) {
        proc_close($proc);
    }
    foreach (glob("$root/*") ?: [] as $f) {
        @unlink($f);
    }
    @rmdir($root);
}

?>
--EXPECTF--
pool serving before the reload: pid %d
master still running: ok
same worker still serving: pid %d
ERROR with the child's diagnostic: ok
reload works again after the fix: pid %d
Done
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>

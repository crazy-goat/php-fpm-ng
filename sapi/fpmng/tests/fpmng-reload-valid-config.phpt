--TEST--
fpm-ng: SIGUSR2 with a configuration that does load still replaces the generation (issue #640)
--SKIPIF--
<?php include "skipif.inc"; ?>
--FILE--
<?php

require_once "tester.inc";

/* The other half of issue #640, and the reason the gate can be trusted: a
 * configuration that passes the check must reload exactly as it did before the
 * gate existed. The master forks a second php-fpm-ng -t before every reload now
 * (fpm_reload_config_check.c), so "still works" is a claim about a path with one
 * more process in it.
 *
 * Each pool answers with its own pid between two markers, so the reload is
 * observed as both workers being replaced: a pid that never changed would mean
 * the reload was silently skipped, and one pool replaced but not the other
 * would mean it was half done. The configuration is EDITED (a comment) before
 * the signal, which is also what makes this a reload whose configuration the
 * gate has never seen: the file on disk is not the one the running generation
 * loaded. */

$root = sys_get_temp_dir() . '/fpmng-reload-goodconf-' . getmypid();
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

function writeConfig(string $conf, string $root, string $log, string $a, string $b, string $note): void
{
    file_put_contents($conf, <<<CFG
; $note
[global]
error_log = $log
pid = $root/fpm.pid
log_level = notice
[a]
listen = $a
pool.type = http-direct
pm = static
pm.max_children = 1
chdir = $root
http.front_controller = /app.php
[b]
listen = $b
pool.type = http-direct
pm = static
pm.max_children = 1
chdir = $root
http.front_controller = /app.php
CFG);
}

$tester = new FPM\Tester('[global]', '<?php');
$addrA = $tester->getAddr('ipv4', '[a]');
$addrB = $tester->getAddr('ipv4', '[b]');

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
    writeConfig($conf, $root, $log, $addrA, $addrB, 'before the reload');
    $proc = proc_open([FPM\Tester::findExecutable(), '-n', '-y', $conf, '-F'],
        [0 => ['file', '/dev/null', 'r'], 1 => ['file', '/dev/null', 'w'], 2 => ['file', '/dev/null', 'w']], $pipes);
    if (!is_resource($proc)) {
        fail('cannot start the master');
    }
    $masterPid = (int) proc_get_status($proc)['pid'];

    $aBefore = request($addrA);
    $bBefore = request($addrB);
    if ($aBefore === '' || $bBefore === '') {
        fail("a pool never answered before the reload\n" . (string) @file_get_contents($log));
    }
    echo "pools serving before the reload: $aBefore $bBefore\n";

    /* A valid configuration, different from the one this generation loaded --
     * a comment line the parser ignores and the reload's config test reads. */
    writeConfig($conf, $root, $log, $addrA, $addrB, 'after the edit');

    exec("kill -USR2 $masterPid");

    $deadline = time() + 30;
    $aAfter = $aBefore;
    $bAfter = $bBefore;
    while (time() < $deadline) {
        $aAfter = request($addrA, 2);
        $bAfter = request($addrB, 2);
        if ($aAfter !== '' && $bAfter !== '' && $aAfter !== $aBefore && $bAfter !== $bBefore) {
            break;
        }
        usleep(200000);
    }

    if ($aAfter === '' || $aAfter === $aBefore) {
        fail("pool a was not reloaded (was $aBefore, now '$aAfter')\n" . (string) @file_get_contents($log));
    }
    if ($bAfter === '' || $bAfter === $bBefore) {
        fail("pool b was not reloaded (was $bBefore, now '$bAfter')\n" . (string) @file_get_contents($log));
    }
    echo "both pools reloaded: $aAfter $bAfter\n";

    if (!pid_alive($masterPid)) {
        fail("master $masterPid is gone after a valid reload");
    }

    $logText = (string) @file_get_contents($log);
    /* The gate ran and passed, which is what the child's own NOTICE says. */
    if (!str_contains($logText, 'test is successful')) {
        fail("the configuration check did not pass on a valid configuration\n$logText");
    }
    if (str_contains($logText, 'reload refused')) {
        fail("a valid configuration was refused\n$logText");
    }
    echo "config check passed, reload not refused: ok\n";

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
pools serving before the reload: %d %d
both pools reloaded: %d %d
config check passed, reload not refused: ok
Done
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>

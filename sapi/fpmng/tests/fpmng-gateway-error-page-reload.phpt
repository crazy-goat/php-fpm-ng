--TEST--
fpm-ng: a reload reads http.error_page[] files again, and a reload whose page file is gone is refused while the old generation keeps serving (issue #650)
--SKIPIF--
<?php
include "fpmng-skipif.inc";
fpmng_skip_if_pool_type_unsupported('gateway');
?>
--FILE--
<?php

require_once "tester.inc";

/* Issue #650. The master reads each page when it builds the gateway's policy
 * (fpm_http_reply_policy_build()), and the gateway children inherit the bytes
 * by fork(). Two things follow, and this test pins both:
 *
 *   1. SIGUSR2 after the file changed: the new generation serves the new body.
 *   2. SIGUSR2 after the file was deleted: the gate that runs -t first
 *      (fpm_reload_config_check.c, issue #640) refuses the reload, and the old
 *      generation keeps answering with the page it already holds.
 *
 * The master is started here, not by the Tester, because the page and the
 * configuration are rewritten in place (model: fpmng-reload-broken-config.phpt). */

$root = sys_get_temp_dir() . '/fpmng-gw-page-reload-' . getmypid();
@mkdir($root, 0700, true);
$log = "$root/error.log";
$conf = "$root/fpm.conf";
$page = "$root/404.html";

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

/* The body of a request no route claims, or '' while the gateway is not up. */
function body(string $address, int $seconds = 20): string
{
    $deadline = microtime(true) + $seconds;
    do {
        $fp = @stream_socket_client("tcp://$address", $errno, $error, 2);
        if ($fp) {
            stream_set_timeout($fp, 5);
            fwrite($fp, "GET /nothing HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n");
            $raw = (string) stream_get_contents($fp);
            fclose($fp);
            if (str_starts_with($raw, 'HTTP/1.1 404')) {
                return (string) substr($raw, (int) strpos($raw, "\r\n\r\n") + 4);
            }
        }
        usleep(100000);
    } while (microtime(true) < $deadline);
    return '';
}

function writeConfig(string $conf, string $root, string $log, string $http, string $web, string $page): void
{
    file_put_contents($conf, <<<CFG
[global]
error_log = $log
pid = $root/fpm.pid
log_level = notice
reload.selective = yes
[gw]
pool.type = gateway
listen = $http
chdir = $root
http.gateways = 1
http.route[web] = /api
http.error_page[404] = $page
[web]
pool.type = fastcgi
listen = $web
chdir = $root
pm = static
pm.max_children = 1
CFG);
}

$tester = new FPM\Tester('[global]', '<?php');
$http = $tester->getAddr('ipv4', '[http]');
$web = $tester->getAddr('ipv4', '[web]');

if (FPM\Tester::findExecutable() === false) {
    fail('cannot find the php-fpm-ng binary to test');
}

$proc = null;
$masterPid = 0;
try {
    file_put_contents($page, '<h1>old page</h1>');
    writeConfig($conf, $root, $log, $http, $web, $page);
    $proc = proc_open([FPM\Tester::findExecutable(), '-n', '-y', $conf, '-F'],
        [0 => ['file', '/dev/null', 'r'], 1 => ['file', '/dev/null', 'w'], 2 => ['file', '/dev/null', 'w']], $pipes);
    if (!is_resource($proc)) {
        fail('cannot start the master');
    }
    $masterPid = (int) proc_get_status($proc)['pid'];

    $got = body($http);
    if ($got !== '<h1>old page</h1>') {
        fail("before the reload the body is " . var_export($got, true) . "\n" . (string) @file_get_contents($log));
    }
    echo "before the reload: old page\n";

    /* 1. The file changed. */
    file_put_contents($page, '<h1>new page</h1>');
    exec("kill -USR2 $masterPid");
    $deadline = time() + 30;
    do {
        usleep(200000);
        $got = body($http, 2);
    } while ($got !== '<h1>new page</h1>' && time() < $deadline);
    if ($got !== '<h1>new page</h1>') {
        fail("after the reload the body is " . var_export($got, true) . "\n" . (string) @file_get_contents($log));
    }
    echo "after the reload: new page\n";

    /* 2. The file is gone: the reload is refused, the old generation serves. */
    unlink($page);
    exec("kill -USR2 $masterPid");
    usleep(3000000);
    if (!pid_alive($masterPid)) {
        fail("master $masterPid is gone: the refused reload took the service down\n" . (string) @file_get_contents($log));
    }
    $got = body($http, 5);
    if ($got !== '<h1>new page</h1>') {
        fail("after the refused reload the body is " . var_export($got, true) . "\n" . (string) @file_get_contents($log));
    }
    $logText = (string) @file_get_contents($log);
    if (!str_contains($logText, 'reload refused: the configuration test says') || !str_contains($logText, 'http.error_page')) {
        fail("no ERROR line that names http.error_page\n$logText");
    }
    echo "page file gone: reload refused, old page still served\n";
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
--EXPECT--
before the reload: old page
after the reload: new page
page file gone: reload refused, old page still served
Done
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>

--TEST--
fpm-ng: http.ready_path answers 503 "starting" until a target child accepts, answers 200 while at least one target serves, and http.ready_require_target answers 503 "no live target" once every target is dead (issue #646)
--SKIPIF--
<?php
include "fpmng-skipif.inc";
if (PHP_OS_FAMILY !== 'Linux') die('skip requires Linux /proc');
exec('id -u', $uid);
if (trim($uid[0] ?? '') === '0') die('skip root ignores directory modes');
fpmng_skip_if_pool_type_unsupported('gateway');
?>
--FILE--
<?php

require_once "tester.inc";

/* Issue #646. The probe reads the target pool's scoreboard, so it can tell
 * apart a target that has never had a child accept requests ("starting", 503)
 * from one that had and now has no live child at all. The starting case uses
 * a target directory without the search bit: the config check passes, but
 * each child fails its chdir() before it accepts. The dead case removes the
 * target directory, so every child the master starts again fails the same way. */

$root = sys_get_temp_dir() . '/fpmng-gw-ready-states-' . getmypid();
@mkdir($root, 0700, true);

function config(string $targetDir, string $extra): string
{
    global $root;
    return <<<EOT
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
log_level = notice
process_control_timeout = 10
[gw]
pool.type = gateway
listen = {{ADDR[http]}}
chdir = $root
http.gateways = 1
http.ready_path = /ready
$extra
http.route[app] = /
[app]
pool.type = fastcgi
listen = {{ADDR}}
chdir = $targetDir
pm = static
pm.max_children = 2
EOT;
}

/* The master named by the pid file. The start lines can reach the log before
 * the pid file and the listener exist, so wait for both. */
function wait_for_master(string $http): int
{
    $pidFile = preg_replace('/\.php$/', '.pid', __FILE__);
    $deadline = microtime(true) + 20;
    do {
        $master = is_file($pidFile) ? (int) @file_get_contents($pidFile) : 0;
        if ($master > 0 && is_dir("/proc/$master")) {
            break;
        }
        usleep(50000);
    } while (microtime(true) < $deadline);
    if ($master <= 0) {
        throw new RuntimeException('the pid file does not name a master');
    }
    $fp = null;
    do {
        $fp = @stream_socket_client("tcp://$http", $errno, $error, 1);
        if ($fp) {
            fclose($fp);
            return $master;
        }
        usleep(50000);
    } while (microtime(true) < $deadline);
    throw new RuntimeException("no listener on $http: $error");
}

/* The children of $master that are not gateway processes: the pool children. */
function pool_children(int $master): array
{
    $pids = [];
    foreach (glob('/proc/[0-9]*/stat') as $stat) {
        $raw = @file_get_contents($stat);
        if ($raw === false || !preg_match('/^(\d+) \((.*)\) \S (\d+) /s', $raw, $m)) {
            continue;
        }
        if ((int) $m[3] !== $master) {
            continue;
        }
        $cmd = @file_get_contents(dirname($stat) . '/cmdline');
        if ($cmd !== false && !str_contains($cmd, 'http gateway')) {
            $pids[] = (int) $m[1];
        }
    }
    return $pids;
}

function read_all($fp, int $seconds): string
{
    stream_set_timeout($fp, $seconds);
    $out = '';
    while (!feof($fp)) {
        $chunk = fread($fp, 65536);
        if ($chunk === false || $chunk === '') {
            $meta = stream_get_meta_data($fp);
            if ($chunk === false || $meta['timed_out'] || feof($fp)) {
                break;
            }
            continue;
        }
        $out .= $chunk;
    }
    return $out;
}

/* One request on a fresh connection, closed by the client after the reply. */
function get_once(string $http, string $path): string
{
    $fp = @stream_socket_client("tcp://$http", $errno, $error, 5);
    if (!$fp) {
        throw new RuntimeException("connect: $error");
    }
    fwrite($fp, "GET $path HTTP/1.1\r\nHost: test\r\nConnection: close\r\n\r\n");
    $r = read_all($fp, 5);
    fclose($fp);
    return $r;
}

/* Starting: a target whose children can never accept. The directory has no
 * search (x) bit, so the master's config check still finds it, but each child
 * fails its chdir() before it reaches accept. The probe is 503 "starting"
 * every time, with or without http.ready_require_target. */
function starting_case(string $locked, string $extra): void
{
    @mkdir($locked, 0600, true);
    chmod($locked, 0600);
    $tester = new FPM\Tester(config($locked, $extra), '<?php');
    try {
        $tester->start([], false);
        $tester->switchLogSource('{{FILE:LOG}}');
        $tester->expectLogStartNotices();
        $http = $tester->getAddr('ipv4', '[http]');
        wait_for_master($http);

        for ($i = 0; $i < 3; $i++) {
            $r = get_once($http, '/ready');
            if (!str_starts_with($r, 'HTTP/1.1 503') || !str_ends_with($r, 'starting')) {
                throw new RuntimeException("the probe is not 503 starting while no target can serve ($extra)\n$r");
            }
            usleep(200000);
        }
        echo "503 starting while no target child accepts" . ($extra ? ' (require target)' : '') . ": ok\n";
    } finally {
        $tester->terminate();
        $tester->close();
        @chmod($locked, 0700);
        @rmdir($locked);
    }
}

/* Partial: two targets, one of them never serves. The probe is the at-least-one
 * rule: a target that cannot serve does not hold back "ready" while the other
 * one serves, even with http.ready_require_target. */
function partial_case(string $live, string $locked): void
{
    @mkdir($live, 0700, true);
    file_put_contents("$live/app.php", <<<'PHP'
<?php
echo 'app';
PHP);
    @mkdir($locked, 0600, true);
    chmod($locked, 0600);
    $cfg = config($live, "http.ready_require_target = yes\nhttp.route[bad] = /admin")
        . "\n[bad]\npool.type = fastcgi\nlisten = {{ADDR[bad]}}\nchdir = $locked\npm = static\npm.max_children = 1";
    $tester = new FPM\Tester($cfg, '<?php');
    try {
        $tester->start([], false);
        $tester->switchLogSource('{{FILE:LOG}}');
        $tester->expectLogStartNotices();
        $http = $tester->getAddr('ipv4', '[http]');
        wait_for_master($http);

        $deadline = microtime(true) + 20;
        do {
            $r = get_once($http, '/ready');
        } while (!str_starts_with($r, 'HTTP/1.1 200') && microtime(true) < $deadline);
        if (!str_starts_with($r, 'HTTP/1.1 200')) {
            throw new RuntimeException("the probe is not 200 ready while one target serves and one never does\n$r");
        }
        echo "200 ready with one target serving and one locked target (require target): ok\n";
    } finally {
        $tester->terminate();
        $tester->close();
        @unlink("$live/app.php");
        @rmdir($live);
        @chmod($locked, 0700);
        @rmdir($locked);
    }
}

/* Dead: the target served, then its chdir is gone and its children are
 * killed. The master starts children again, they fail, and no child is live.
 * With http.ready_require_target the probe turns 503 "no live target". */
function dead_case(string $live, string $extra, bool $requireTarget): void
{
    @mkdir($live, 0700, true);
    file_put_contents("$live/app.php", <<<'PHP'
<?php
echo 'app';
PHP);
    $tester = new FPM\Tester(config($live, $extra), '<?php');
    try {
        $tester->start([], false);
        $tester->switchLogSource('{{FILE:LOG}}');
        $tester->expectLogStartNotices();
        $http = $tester->getAddr('ipv4', '[http]');
        $master = wait_for_master($http);

        $deadline = microtime(true) + 20;
        do {
            $r = get_once($http, '/ready');
        } while (!str_starts_with($r, 'HTTP/1.1 200') && microtime(true) < $deadline);
        if (!str_starts_with($r, 'HTTP/1.1 200')) {
            throw new RuntimeException("the probe is not 200 ready while the target serves\n$r");
        }

        /* The target directory goes away, then the live children are killed. */
        @unlink("$live/app.php");
        @rmdir($live);
        /* exec() rather than posix_kill(): CI builds ext/posix out. */
        foreach (pool_children($master) as $pid) {
            exec('kill -KILL ' . (int) $pid);
        }

        if ($requireTarget) {
            $deadline = microtime(true) + 20;
            $dead = false;
            do {
                $r = get_once($http, '/ready');
                if (str_starts_with($r, 'HTTP/1.1 503') && str_ends_with($r, 'no live target')) {
                    $dead = true;
                    break;
                }
                usleep(50000);
            } while (microtime(true) < $deadline);
            if (!$dead) {
                throw new RuntimeException("the probe never turned 503 'no live target' with http.ready_require_target\n$r");
            }
            echo "503 no live target once every target is dead (require target): ok\n";
        } else {
            /* Without the directive the latched 200 stands while no child lives. */
            for ($i = 0; $i < 10; $i++) {
                $r = get_once($http, '/ready');
                if (!str_starts_with($r, 'HTTP/1.1 200')) {
                    throw new RuntimeException("the probe is not 200 once every target is dead, without the directive\n$r");
                }
                usleep(100000);
            }
            echo "200 ready once every target was dead, without http.ready_require_target: ok\n";
        }
    } finally {
        $tester->terminate();
        $tester->close();
        @unlink("$live/app.php");
        @rmdir($live);
    }
}

try {
    starting_case("$root/locked-1", '');
    starting_case("$root/locked-2", "http.ready_require_target = yes");
    partial_case("$root/live-3", "$root/locked-3");
    dead_case("$root/live-1", "http.ready_require_target = yes", true);
    dead_case("$root/live-2", '', false);
    echo "Done\n";
} finally {
    @rmdir($root);
}
?>
--EXPECT--
503 starting while no target child accepts: ok
503 starting while no target child accepts (require target): ok
200 ready with one target serving and one locked target (require target): ok
503 no live target once every target is dead (require target): ok
200 ready once every target was dead, without http.ready_require_target: ok
Done

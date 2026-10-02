--TEST--
fpm-ng: an evhttp pool listening on a unix socket survives its first connection (issue #467)
--SKIPIF--
<?php
include "fpmng-skipif.inc";
fpmng_skip_if_pool_type_unsupported('gateway');
fpmng_skip_if_pool_type_unsupported('http-direct');
?>
--FILE--
<?php

require_once "tester.inc";

/* Issue #467: libevent's evhttp calls getnameinfo() on every accepted peer and
 * exits the process when it fails. musl fails for AF_UNIX, so on Alpine a pool
 * listening on a unix socket died on its first connection. Two listeners are
 * covered here, because they are two different evhttp servers: the gateway's
 * own unix listen (below), and an http-direct pool on a unix listener, which
 * fpmng-http-route-http-direct-fail.phpt reaches through a route. Each is asked
 * twice, so that "served the first connection and then went away" is caught. */

function check(bool $condition, string $message): void
{
    if (!$condition) throw new RuntimeException($message);
}

/* The body may arrive chunked, so look for it rather than at the end. */
function served(string $raw): bool
{
    return str_starts_with($raw, 'HTTP/1.1 200') && str_contains($raw, 'unix-ok');
}

function unixGet(string $sock, string $path): string
{
    $fp = @stream_socket_client("unix://$sock", $errno, $error, 5);
    check($fp !== false, "connect $sock: $error");
    stream_set_timeout($fp, 5);
    fwrite($fp, "GET $path HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n");
    $raw = stream_get_contents($fp);
    fclose($fp);
    return $raw;
}

$root = sys_get_temp_dir() . '/fpmng-unix-listen-' . getmypid();
@mkdir($root, 0700, true);
$gwSock = "$root/gw.sock";
$directSock = "$root/direct.sock";
file_put_contents($root . '/index.php', '<?php echo "unix-ok";');

$config = <<<EOT
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
[gw]
pool.type = gateway
listen = $gwSock
chdir = $root
http.gateways = 1
http.route[app] = /
[app]
pool.type = fastcgi
listen = {{ADDR}}
chdir = $root
pm = static
pm.max_children = 1

[direct]
listen = $directSock
pm = static
pm.max_children = 1
pool.type = http-direct
chdir = $root
EOT;

$tester = new FPM\Tester($config, '<?php echo "unused";');
try {
    $tester->start();
    $tester->expectLogStartNotices();

    for ($i = 1; $i <= 2; $i++) {
        $raw = unixGet($gwSock, '/index.php');
        check(served($raw),
            "gateway on a unix listen, request $i: " . var_export($raw, true));
    }
    echo "gateway-unix-listen: ok\n";

    for ($i = 1; $i <= 2; $i++) {
        $raw = unixGet($directSock, '/index.php');
        check(served($raw),
            "http-direct on a unix listen, request $i: " . var_export($raw, true));
    }
    echo "http-direct-unix-listen: ok\n";
} finally {
    $tester->terminate();
    $tester->expectLogTerminatingNotices();
    $tester->close();
    @unlink($root . '/index.php');
    @unlink($gwSock);
    @unlink($directSock);
    @rmdir($root);
}
echo "Done\n";
?>
--EXPECT--
gateway-unix-listen: ok
http-direct-unix-listen: ok
Done
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>

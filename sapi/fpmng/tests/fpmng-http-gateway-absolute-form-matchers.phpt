--TEST--
fpm-ng: an absolute-form request-target is normalized once -- ping.path, the operator ACL, access.suppress_path[] and Host see origin-form (issue #534, RFC 9112 3.2.2)
--SKIPIF--
<?php
include "fpmng-skipif.inc";
fpmng_skip_if_pool_type_unsupported('gateway');
?>
--FILE--
<?php

require_once "tester.inc";

/* Before #534 the gateway's own matchers compared the raw request-target, so
 * "GET http://h/metrics/app" was not under the operator namespace (the
 * operator ACL was skipped), "GET http://h/ping" was not the ping probe and
 * "GET http://h/quiet" was not suppressed from the access log; and the Host
 * header was kept instead of being replaced by the authority. */

$root = sys_get_temp_dir() . '/fpmng-gw-absform-' . getmypid();
@mkdir($root, 0700, true);
file_put_contents($root . '/index.php', '<?php echo "app:" . $_SERVER["REQUEST_URI"] . ":" . $_SERVER["HTTP_HOST"];');

$cfg = <<<EOT
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}

[gw]
pool.type = gateway
listen = {{ADDR[http]}}
chdir = $root
http.gateways = 1
http.route[app] = /
http.front_controller = /index.php
http.operator = yes
http.operator_allowed_clients = 10.9.9.9
http.access_log = {{FILE:LOG:ACC}}
ping.path = /ping
ping.response = pong
access.suppress_path[] = /quiet
operator.metrics_listen = {{ADDR[operator]}}

[app]
pool.type = fastcgi
listen = {{ADDR[app]}}
pm = static
pm.max_children = 1
chdir = $root
operator.metrics_listen = {{ADDR[operator]}}
operator.metrics = on
EOT;

function rawGet(string $addr, string $target, string $host = 't'): string
{
    $s = stream_socket_client("tcp://$addr", $errno, $errstr, 5);
    if (!$s) {
        throw new RuntimeException("connect: $errstr");
    }
    stream_set_timeout($s, 10);
    fwrite($s, "GET $target HTTP/1.0\r\nHost: $host\r\n\r\n");
    $r = stream_get_contents($s);
    fclose($s);
    return (string) $r;
}

function statusOf(string $r): string
{
    return preg_match('#^HTTP/\S+ (\d+)#', $r, $m) ? $m[1] : 'no status';
}

function bodyOf(string $r): string
{
    $p = strpos($r, "\r\n\r\n");
    return $p === false ? '' : substr($r, $p + 4);
}

$tester = new FPM\Tester($cfg, '<?php echo "unused";');
try {
    $tester->start();
    $tester->expectLogStartNotices();
    $http = $tester->getAddr('ipv4', '[http]');

    foreach (['origin-form' => '', 'absolute-form' => 'http://t'] as $form => $prefix) {
        $r = rawGet($http, "$prefix/ping");
        echo "$form ping: " . statusOf($r) . ' ' . bodyOf($r) . "\n";
        $r = rawGet($http, "$prefix/metrics/app?json");
        echo "$form operator: " . statusOf($r) . "\n";
    }

    $r = rawGet($http, 'http://other.example:8080/x?a=1', 't');
    echo "host: " . statusOf($r) . ' ' . bodyOf($r) . "\n";
    $r = rawGet($http, 'http://other.example', 't');
    echo "no path: " . statusOf($r) . ' ' . bodyOf($r) . "\n";

    rawGet($http, 'http://t/quiet');
    rawGet($http, 'http://t/loud');
    $accessLog = $tester->getPrefixedFile(FPM\Tester::FILE_EXT_LOG_ACC);
    $deadline = microtime(true) + 10;
    do {
        $content = (string) @file_get_contents($accessLog);
        if (str_contains($content, '/loud')) {
            break;
        }
        usleep(100000);
    } while (microtime(true) < $deadline);
    echo "suppressed: " . (str_contains($content, '/quiet') ? 'no' : 'yes') . "\n";
    echo "logged: " . (str_contains($content, '/loud') ? 'yes' : 'no') . "\n";
} finally {
    $tester->terminate();
    $tester->close();
    @unlink($root . '/index.php');
    @rmdir($root);
}
?>
--EXPECT--
origin-form ping: 200 pong
origin-form operator: 403
absolute-form ping: 200 pong
absolute-form operator: 403
host: 200 app:/x?a=1:other.example:8080
no path: 200 app:/:other.example
suppressed: yes
logged: yes
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>

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
http.route[direct] = /d
http.route[app] = /
http.front_controller = /index.php
http.operator = yes
http.operator_allowed_clients = 10.9.9.9
http.access_log = {{FILE:LOG:ACC}}
ping.path = /ping
ping.response = pong
access.suppress_path[] = /quiet
operator.metrics_listen = {{ADDR[operator]}}

[direct]
pool.type = http-direct
listen = {{ADDR[direct]}}
pm = static
pm.max_children = 1
chdir = $root
http.front_controller = /index.php

[app]
pool.type = fastcgi
listen = {{ADDR[app]}}
pm = static
pm.max_children = 1
chdir = $root
operator.metrics_listen = {{ADDR[operator]}}
operator.metrics = on
EOT;

function rawGet(string $addr, string $target, ?string $host = 't'): string
{
    $s = stream_socket_client("tcp://$addr", $errno, $errstr, 5);
    if (!$s) {
        throw new RuntimeException("connect: $errstr");
    }
    stream_set_timeout($s, 10);
    fwrite($s, "GET $target HTTP/1.0\r\n" . ($host === null ? '' : "Host: $host\r\n") . "\r\n");
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

    /* No Host header at all: the authority still defines HTTP_HOST, on the
     * FastCGI transport and on the http one (which sends it as Host:). */
    $r = rawGet($http, 'http://other.example:81/x', null);
    echo "no host fastcgi: " . statusOf($r) . ' ' . bodyOf($r) . "\n";
    $r = rawGet($http, 'http://other.example:81/d/x', null);
    echo "no host http: " . statusOf($r) . ' ' . bodyOf($r) . "\n";
    $r = rawGet($http, 'http://other.example:81/d/x', 't');
    echo "host http: " . statusOf($r) . ' ' . bodyOf($r) . "\n";

    /* Other absolute-URI / network-path shapes libevent parses: the matchers
     * must see the path routing sees. */
    foreach (['http:/metrics/app', 'x+y:/metrics/app', '//h/metrics/app'] as $t) {
        echo "$t operator: " . statusOf(rawGet($http, $t)) . "\n";
    }
    $r = rawGet($http, 'http:/ping');
    echo "http:/ping: " . statusOf($r) . ' ' . bodyOf($r) . "\n";
    /* The forwarded target comes from the same parse: REQUEST_URI on the
     * FastCGI transport and the request line to an http.route[] target. */
    $r = rawGet($http, 'http:/x?a=1');
    echo "http:/x fastcgi: " . statusOf($r) . ' ' . bodyOf($r) . "\n";
    $r = rawGet($http, 'http:/d/x?a=1');
    echo "http:/d/x http: " . statusOf($r) . ' ' . bodyOf($r) . "\n";
    /* A target starting with "/" is origin-form, whatever follows: "//h/x" is
     * the path "//h/x", not host "h" plus "/x" (RFC 9112 3.2.1). The app sees
     * it unchanged and it is not under /metrics. */
    foreach (['//h/x?a=1', '//api/users?x=1', '///etc/x', '//wp-admin'] as $t) {
        $r = rawGet($http, $t);
        echo "$t fastcgi: " . statusOf($r) . ' ' . bodyOf($r) . "\n";
    }
    $r = rawGet($http, 'http:');
    echo "no authority: " . (str_starts_with(bodyOf($r), 'app:') ? 'served' : 'refused') . "\n";
    $r = rawGet($http, 'http://' . str_repeat('a', 300) . '/x', 't');
    echo "long authority: " . statusOf($r) . "\n";
    $r = rawGet($http, 'http://' . str_repeat('a', 253) . ':65535/x', 't');
    echo "max authority: " . statusOf($r) . "\n";

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
no host fastcgi: 200 app:/x:other.example:81
no host http: 200 app:/d/x:other.example:81
host http: 200 app:/d/x:other.example:81
http:/metrics/app operator: 403
x+y:/metrics/app operator: 403
//h/metrics/app operator: 200
http:/ping: 200 pong
http:/x fastcgi: 200 app:/x?a=1:t
http:/d/x http: 200 app:/d/x?a=1:t
//h/x?a=1 fastcgi: 200 app://h/x?a=1:t
//api/users?x=1 fastcgi: 200 app://api/users?x=1:t
///etc/x fastcgi: 200 app:///etc/x:t
//wp-admin fastcgi: 200 app://wp-admin:t
no authority: refused
long authority: 400
max authority: 200
suppressed: yes
logged: yes
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>

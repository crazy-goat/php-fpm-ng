--TEST--
fpm-ng: http.route[] HTTP transport appends the direct peer, not the resolved client, to X-Forwarded-For (issue #452)
--SKIPIF--
<?php
include "fpmng-skipif.inc";
fpmng_skip_if_pool_type_unsupported('gateway');
fpmng_skip_if_pool_type_unsupported('http-direct');
?>
--FILE--
<?php

require_once "tester.inc";

function check(bool $condition, string $message): void
{
    if (!$condition) throw new RuntimeException($message);
}

/* Issue #452: with a trusted direct peer the gateway appended
 * fwd.remote_addr -- its RESOLUTION of the client, taken from the client's
 * own XFF chain -- so a client that arrived with "X-Forwarded-For: 10.9.9.9"
 * reached the target as "10.9.9.9, 10.9.9.9": the client duplicated, the
 * gateway's own address (nginx $proxy_add_x_forwarded_for's contribution)
 * missing. The append is now always the direct peer. On a loopback test the
 * peer address reads 127.0.0.1 whether the peer is trusted or not, so both
 * configurations must produce the same chain. */

$docroot = sys_get_temp_dir() . '/fpmng-http-route-xff-' . getmypid();
@mkdir($docroot, 0700, true);

file_put_contents($docroot . '/index.php', <<<'PHP'
<?php
echo json_encode([
    'xff' => $_SERVER['HTTP_X_FORWARDED_FOR'] ?? null,
    'remote_addr' => $_SERVER['REMOTE_ADDR'] ?? null,
]);
PHP);

function gatewayConfig(string $trusted): string
{
    global $docroot;
    return <<<EOT
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
[gw]
pool.type = gateway
listen = {{ADDR[http]}}
chdir = $docroot
http.gateways = 1
http.front_controller = /index.php
http.route[direct] = /direct
http.route[web] = /
{$trusted}

[web]
pool.type = fastcgi
listen = {{ADDR}}
chdir = $docroot
pm = static
pm.max_children = 2

[direct]
listen = {{ADDR[direct]}}
pm = static
pm.max_children = 1
pool.type = http-direct
chdir = $docroot
http.front_controller = /index.php
EOT;
}

function fetchJson(string $url): array
{
    $ctx = stream_context_create(['http' => [
        'timeout' => 5,
        'ignore_errors' => true,
        'header' => "X-Forwarded-For: 10.9.9.9\r\n",
    ]]);
    $raw = @file_get_contents($url, false, $ctx);
    if ($raw === false) throw new RuntimeException('no body from ' . $url);
    return json_decode($raw, true, 2, JSON_THROW_ON_ERROR);
}

/* Trusted direct peer: the client's chain plus the address the gateway
 * accepted the connection from. */
$tester = new FPM\Tester(gatewayConfig("http.trusted_proxies = 127.0.0.1"), '<?php echo "unused";');
try {
    $tester->start();
    $tester->expectLogStartNotices();
    $http = $tester->getAddr('ipv4', '[http]');
    $seen = fetchJson("http://$http/direct/index.php");
    check(($seen['xff'] ?? '') === '10.9.9.9, 127.0.0.1',
        'trusted peer: XFF is "client chain, direct peer": ' . var_export($seen['xff'] ?? null, true));
    echo "trusted-peer-appends-peer: ok\n";
} finally {
    $tester->terminate();
    $tester->close();
}

/* Untrusted peer: behaviour must be unchanged -- the untrusted path always
 * appended the peer address. */
$tester = new FPM\Tester(gatewayConfig(""), '<?php echo "unused";');
try {
    $tester->start();
    $tester->expectLogStartNotices();
    $http = $tester->getAddr('ipv4', '[http]');
    $seen = fetchJson("http://$http/direct/index.php");
    check(($seen['xff'] ?? '') === '10.9.9.9, 127.0.0.1',
        'untrusted peer: XFF unchanged: ' . var_export($seen['xff'] ?? null, true));
    echo "untrusted-peer-unchanged: ok\n";
} finally {
    $tester->terminate();
    $tester->close();
    @unlink("$docroot/index.php");
    @rmdir($docroot);
}
echo "Done\n";
?>
--EXPECT--
trusted-peer-appends-peer: ok
untrusted-peer-unchanged: ok
Done
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>

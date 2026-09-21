--TEST--
fpm-ng: pool.type = gateway refuses listen.allowed_clients instead of silently ignoring it; http.allowed_clients is the public listener's ACL (issue #493)
--SKIPIF--
<?php
include "fpmng-skipif.inc";
fpmng_skip_if_pool_type_unsupported('gateway');
?>
--FILE--
<?php

require_once "tester.inc";

/* Issue #493. listen.allowed_clients is a FastCGI-worker ACL: on every other
 * listening type it restricts the worker socket, and on the retired combined
 * `http` pool it restricted the FastCGI half -- never the public port, which
 * used http.allowed_clients. A gateway has no worker socket (`listen` IS the
 * public port), so accepting the directive left an operator who wrote it
 * believing the public listener was restricted while it served everyone.
 *
 * This pins both halves of the fix: the ineffective directive is REFUSED by
 * name with the replacement, and the replacement (http.allowed_clients)
 * actually denies a peer outside its list, so a restrictive configuration
 * cannot start while serving strangers. */

function check(bool $condition, string $message): void
{
    if (!$condition) throw new RuntimeException($message);
}

$root = sys_get_temp_dir() . '/fpmng-gw-listen-acl-' . getmypid();
@mkdir($root, 0700, true);
file_put_contents($root . '/front.php', '<?php echo "APP";');

function gatewayGet(string $url): array
{
    $ctx = stream_context_create(['http' => ['timeout' => 10, 'ignore_errors' => true]]);
    $body = @file_get_contents($url, false, $ctx);
    $headers = $http_response_header ?? [];
    $status = 0;
    foreach ($headers as $h) {
        if (preg_match('#^HTTP/\S+\s+(\d+)#', $h, $m)) {
            $status = (int) $m[1];
        }
    }
    return [$status, $body === false ? '' : $body];
}

$gw = <<<EOT
[gw]
pool.type = gateway
listen = {{ADDR[http]}}
chdir = $root
http.gateways = 1
http.route[app] = /
ping.path = /ping
ping.response = pong
ACL_LINE
EOT;

$app = <<<EOT
[app]
pool.type = http-direct
listen = {{ADDR[app]}}
pm = static
pm.max_children = 1
chdir = $root
http.front_controller = /front.php
EOT;

$global = <<<EOT
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
EOT;

/* 1. The ineffective directive is refused at configuration time, and the
 * message names the supported replacement. It must sit in [gw]: appended after
 * [app] it would land in the wrong section (and be accepted there). */
$tester = new FPM\Tester($global . "\n" . str_replace('ACL_LINE', 'listen.allowed_clients = 192.0.2.1', $gw) . "\n" . $app, '<?php echo "unused";');
$messages = $tester->testConfig(true);
check($messages !== null, 'listen.allowed_clients was accepted on a gateway, expected a refusal');
$text = implode("\n", $messages);
check(str_contains($text, 'listen.allowed_clients is not enforced on pool.type = gateway'),
    "refusal did not name the directive:\n$text");
check(str_contains($text, 'http.allowed_clients'),
    "refusal did not name the replacement http.allowed_clients:\n$text");
echo "listen.allowed_clients: refused with the replacement named\n";

/* 2. The replacement is accepted and actually enforces the restriction. The
 * test's peer is 127.0.0.1, so 192.0.2.1 denies it. */
$tester = new FPM\Tester($global . "\n" . str_replace('ACL_LINE', 'http.allowed_clients = 192.0.2.1', $gw) . "\n" . $app, '<?php echo "unused";');
try {
    $tester->start();
    $tester->expectLogStartNotices();
    $http = $tester->getAddr('ipv4', '[http]');

    foreach (['/' => 'the routed application', '/ping' => 'the ping endpoint'] as $path => $what) {
        [$status, $body] = gatewayGet("http://$http$path");
        check($status === 403, "$path ($what) answered $status for a denied peer, expected 403:\n$body");
        echo "$path: 403 for a peer outside http.allowed_clients\n";
    }
} finally {
    $tester->terminate();
    $tester->expectLogTerminatingNotices();
    $tester->close();
    @unlink("$root/front.php");
    @rmdir($root);
}
echo "Done\n";
?>
--EXPECT--
listen.allowed_clients: refused with the replacement named
/: 403 for a peer outside http.allowed_clients
/ping: 403 for a peer outside http.allowed_clients
Done
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>

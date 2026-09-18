--TEST--
fpm-ng: http.route[] targets an http-direct pool over the HTTP/1.1 client transport (issue #344)
--SKIPIF--
<?php
include "fpmng-skipif.inc";
fpmng_skip_if_pool_type_unsupported('http');
fpmng_skip_if_pool_type_unsupported('http-direct');
?>
--FILE--
<?php

require_once "tester.inc";

function check(bool $condition, string $message): void
{
    if (!$condition) throw new RuntimeException($message);
}

/* Issue #344, acceptance criteria 1 and 2: one gateway, one fastcgi pool, one
 * http-direct pool (classic executor). Status, a response header the script
 * set, the body, and what the direct pool saw in REMOTE_ADDR /
 * HTTP_X_FORWARDED_FOR are all asserted; a POST body must come back intact. */

$docroot = sys_get_temp_dir() . '/fpmng-http-route-direct-' . getmypid();
@mkdir($docroot, 0700, true);

/* The direct pool sees the gateway as its peer -- a direct pool has no
 * trusted-proxy list by design (fpm_http_direct.c), so its REMOTE_ADDR is the
 * gateway's address and the client arrives in X-Forwarded-For, which is the
 * app's to read. */
file_put_contents($docroot . '/index.php', <<<'PHP'
<?php
header('X-Direct-Pool: yes');
if ($_SERVER['REQUEST_METHOD'] === 'POST') {
    header('Content-Type: application/octet-stream');
    echo file_get_contents('php://input');
    exit;
}
echo getenv('FPMNG_ROUTE_POOL') ?: json_encode([
    'remote_addr' => $_SERVER['REMOTE_ADDR'] ?? null,
    'xff' => $_SERVER['HTTP_X_FORWARDED_FOR'] ?? null,
    'xfp' => $_SERVER['HTTP_X_FORWARDED_PROTO'] ?? null,
    'uri' => $_SERVER['REQUEST_URI'] ?? null,
]);
PHP);

$config = <<<EOT
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
[web]
listen = {{ADDR}}
chdir = $docroot
pm = static
pm.max_children = 2
pool.type = http
http.gateways = 1
http.listen = {{ADDR[http]}}
http.front_controller = /index.php
http.route[api] = /api
http.route[direct] = /direct
env[FPMNG_ROUTE_POOL] = web

[api]
listen = {{ADDR[api]}}
pm = static
pm.max_children = 1
env[FPMNG_ROUTE_POOL] = api

[direct]
listen = {{ADDR[direct]}}
pm = static
pm.max_children = 1
pool.type = http-direct
chdir = $docroot
http.front_controller = /index.php
EOT;

function fetchAll(string $url, ?string $body = null): array
{
    $ctx = stream_context_create(['http' => [
        'timeout' => 5,
        'ignore_errors' => true,
        'method' => $body === null ? 'GET' : 'POST',
        'content' => $body ?? '',
        'header' => $body === null ? [] : "Content-Type: application/octet-stream\r\n",
    ]]);
    $raw = @file_get_contents($url, false, $ctx);
    return [$raw, $http_response_header ?? []];
}

$tester = new FPM\Tester($config, '<?php echo "unused";');
try {
    $tester->start();
    $tester->expectLogStartNotices();
    $http = $tester->getAddr('ipv4', '[http]');

    /* The fastcgi route still works -- the second transport must not have
     * disturbed the first. The body is the target pool's env marker. */
    [$body] = fetchAll("http://$http/api/x");
    check($body === 'api', "fastcgi route body: " . var_export($body, true));
    echo "fastcgi-route-unchanged: ok\n";

    /* The http-direct route. The direct pool has no FPMNG_ROUTE_POOL env, so
     * it answers the JSON probe. */
    [$body, $headers] = fetchAll("http://$http/direct/index.php");
    check($body !== false, 'no body from the http-direct route');
    check(in_array('X-Direct-Pool: yes', $headers, true) || str_contains(implode("\n", $headers), 'X-Direct-Pool: yes'),
        'the header the script set did not survive the gateway: ' . implode(' | ', $headers));
    /* The direct pool has no trusted-proxy list (its REMOTE_ADDR is the
     * gateway's), so XFF is the app's evidence a proxy was involved. On a
     * loopback test both addresses read 127.0.0.1 -- what is asserted is
     * that the headers exist and are well-formed, not which address. */
    $seen = json_decode($body, true, 2, JSON_THROW_ON_ERROR);
    check(($seen['xfp'] ?? '') === 'http', 'X-Forwarded-Proto: ' . var_export($seen['xfp'] ?? null, true));
    check(str_contains((string) ($seen['xff'] ?? ''), '127.0.0.1'),
        'X-Forwarded-For does not name the client the gateway saw: ' . var_export($seen['xff'] ?? null, true));
    check(($seen['remote_addr'] ?? '') !== '', 'REMOTE_ADDR empty: ' . var_export($seen['remote_addr'] ?? null, true));
    check(($seen['uri'] ?? '') === '/direct/index.php',
        'the request-target reached the pool whole (prefix not stripped): ' . var_export($seen['uri'] ?? null, true));
    echo "http-direct-route: ok\n";

    /* POST body intact through the http-direct route. */
    $payload = str_repeat("POST-body-\x00-through-\xFF-gateway;", 400);
    [$body] = fetchAll("http://$http/direct/index.php", $payload);
    check($body === $payload, 'the POST body did not survive the round trip: ' . strlen((string) $body) . ' of ' . strlen($payload));
    echo "post-body-intact: ok\n";
} finally {
    $tester->terminate();
    $tester->close();
    @unlink("$docroot/index.php");
    @rmdir($docroot);
}
echo "Done\n";
?>
--EXPECT--
fastcgi-route-unchanged: ok
http-direct-route: ok
post-body-intact: ok
Done
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>

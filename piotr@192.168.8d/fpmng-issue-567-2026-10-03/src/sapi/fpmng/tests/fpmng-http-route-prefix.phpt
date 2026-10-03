--TEST--
fpm-ng: http.route[] sends path prefixes to other pools, longest prefix first (issue #340)
--SKIPIF--
<?php
include "fpmng-skipif.inc";
fpmng_skip_if_pool_type_unsupported('gateway');
?>
--FILE--
<?php

require_once "tester.inc";

/* Issue #340, acceptance criteria 1 and 3. One gateway in front of three pools,
 * each of which says its own name through env[]: the body of every response is
 * the name of the pool that produced it, so a routing mistake is not a subtle
 * difference in behaviour but a different word on the wire.
 *
 * The front controller is what makes this readable: /api/v1/x is a path, not a
 * file, so every request ends up in the same index.php and the only thing that
 * varies between them is which pool ran it. */

function httpGet(string $url): string|false
{
    $ctx = stream_context_create(['http' => ['timeout' => 5, 'ignore_errors' => true]]);
    return @file_get_contents($url, false, $ctx);
}

function expectPool(string $http, string $path, string $expected): void
{
    $body = httpGet("http://$http$path");
    if ($body !== $expected) {
        echo "FAIL: $path was served by " . var_export($body, true) . ", expected $expected\n";
        exit(1);
    }
    echo "$path -> $expected\n";
}

$docroot = sys_get_temp_dir() . '/fpmng-http-route-' . getmypid();
@mkdir($docroot, 0700, true);
file_put_contents($docroot . '/index.php', '<?php echo getenv("FPMNG_ROUTE_POOL") ?: "no-marker";');

/* ping.path lives on the gateway pool and #382 made the gateway answer it
 * itself. It is deliberately put UNDER a routed prefix here: if the routing
 * lookup ever ran first, /api/v1/ping would come back as "api" and this test
 * would say so. */
$config = <<<EOT
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
[gw]
pool.type = gateway
listen = {{ADDR[http]}}
chdir = $docroot
http.gateways = 1
http.front_controller = /index.php
http.route[web] = /
http.route[api] = /api/v1
http.route[events] = /sse
ping.path = /api/v1/ping
ping.response = pong

[web]
pool.type = fastcgi
listen = {{ADDR}}
chdir = $docroot
pm = static
pm.max_children = 2
env[FPMNG_ROUTE_POOL] = web

[api]
listen = {{ADDR[api]}}
pm = static
pm.max_children = 2
env[FPMNG_ROUTE_POOL] = api

[events]
listen = {{ADDR[events]}}
pm = static
pm.max_children = 2
env[FPMNG_ROUTE_POOL] = events
EOT;

$tester = new FPM\Tester($config, '<?php echo "unused";');
$tester->start();
$tester->expectLogStartNotices();
$http = $tester->getAddr('ipv4', '[http]');

expectPool($http, '/api/v1/x', 'api');
expectPool($http, '/api/v1', 'api');
/* The segment rule: /apiary is not under /api/v1 and is not under /api either,
 * so it falls to the "/" row routing to the explicit web target (issue #388
 * removed the implicit own-pool row, so web is now a target like any other). */
expectPool($http, '/apiary', 'web');
expectPool($http, '/', 'web');
expectPool($http, '/sse/stream', 'events');

$ping = httpGet("http://$http/api/v1/ping");
if ($ping !== 'pong') {
    echo 'FAIL: ping.path under a routed prefix was not answered locally: ' . var_export($ping, true) . "\n";
    exit(1);
}
echo "ping-answered-locally: ok\n";

$tester->terminate();
$tester->expectLogTerminatingNotices();
$tester->close();

/* Issue #388 criterion: the gateway needs no pool of its own; every target is
 * named by http.route[], and "/" here routes to web like any other prefix. */
$config2 = <<<EOT
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
[gw]
pool.type = gateway
listen = {{ADDR[http]}}
chdir = $docroot
http.gateways = 1
http.front_controller = /index.php
http.route[web] = /
http.route[api] = /api/v1

[web]
listen = {{ADDR[web]}}
pm = static
pm.max_children = 2
env[FPMNG_ROUTE_POOL] = web

[api]
listen = {{ADDR[api]}}
pm = static
pm.max_children = 2
env[FPMNG_ROUTE_POOL] = api
EOT;

$tester2 = new FPM\Tester($config2, '<?php echo "unused";');
$tester2->start();
$tester2->expectLogStartNotices();
$http2 = $tester2->getAddr('ipv4', '[http]');

expectPool($http2, '/', 'web');
expectPool($http2, '/anything-else', 'web');
expectPool($http2, '/api/v1/thing', 'api');
echo "own-pool-unused: ok\n";

$tester2->terminate();
$tester2->expectLogTerminatingNotices();
$tester2->close();

@unlink($docroot . '/index.php');
@rmdir($docroot);

?>
Done
--EXPECT--
/api/v1/x -> api
/api/v1 -> api
/apiary -> web
/ -> web
/sse/stream -> events
ping-answered-locally: ok
/ -> web
/anything-else -> web
/api/v1/thing -> api
own-pool-unused: ok
Done
--CLEAN--
<?php
require_once "tester.inc";
FPM\Tester::clean();
?>

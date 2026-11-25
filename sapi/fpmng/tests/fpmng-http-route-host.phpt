--TEST--
fpm-ng: http.route[] entries bound to a host match the request Host, with host-first precedence (issue #658)
--SKIPIF--
<?php
include "fpmng-skipif.inc";
fpmng_skip_if_pool_type_unsupported('gateway');
?>
--FILE--
<?php

require_once "tester.inc";

/* Issue #658. An entry "<host>[/<prefix>]" is routed only for requests whose
 * Host names that host. Every pool answers with its own name between << and >>,
 * so a routing mistake is a different name on the wire.
 *
 * Precedence, decided in the issue's terms: a host-bound row ranks before every
 * row bound to no host, then the longest prefix ranks first. So
 * a.example/api/x goes to site (a.example/), not to api (/api), because the
 * host's own site wins for every path of that host. */

function rawGet(string $addr, string $headers, string $path = '/x'): string
{
    [$host, $port] = explode(':', $addr);
    $fp = fsockopen($host, (int) $port, $errno, $errstr, 5);
    if (!$fp) {
        echo "FAIL: connect: $errstr ($errno)\n";
        exit(1);
    }
    fwrite($fp, "GET $path HTTP/1.1\r\n$headers\r\nConnection: close\r\n\r\n");
    stream_set_blocking($fp, true);
    $out = '';
    while (!feof($fp)) {
        $chunk = fgets($fp);
        if ($chunk === false) {
            break;
        }
        $out .= $chunk;
    }
    fclose($fp);
    return $out;
}

function pool(string $resp): string
{
    if (!str_starts_with($resp, 'HTTP/1.1 200')) {
        echo "FAIL: not a 200: " . strtok($resp, "\r\n") . "\n";
        exit(1);
    }
    if (!preg_match('/<<(\w+)>>/', $resp, $m)) {
        echo "FAIL: no pool marker in " . var_export($resp, true) . "\n";
        exit(1);
    }
    return $m[1];
}

function expect(string $addr, string $label, string $headers, string $path, string $expected): void
{
    $got = pool(rawGet($addr, $headers, $path));
    if ($got !== $expected) {
        echo "FAIL: $label: $path served by $got, expected $expected\n";
        exit(1);
    }
    echo "$label: $path -> $expected\n";
}

$docroot = sys_get_temp_dir() . '/fpmng-http-route-host-' . getmypid();
@mkdir($docroot, 0700, true);
file_put_contents($docroot . '/index.php', '<?php echo "<<" . getenv("FPMNG_ROUTE_POOL") . ">>";');

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
http.route[api] = /api
http.route[site] = A.Example/
http.route[shop] = shop.example/
http.route[shopapi] = shop.example/api

[web]
pool.type = fastcgi
listen = {{ADDR[web]}}
chdir = $docroot
pm = static
pm.max_children = 1
env[FPMNG_ROUTE_POOL] = web

[api]
pool.type = fastcgi
listen = {{ADDR[api]}}
chdir = $docroot
pm = static
pm.max_children = 1
env[FPMNG_ROUTE_POOL] = api

[site]
pool.type = fastcgi
listen = {{ADDR[site]}}
chdir = $docroot
pm = static
pm.max_children = 1
env[FPMNG_ROUTE_POOL] = site

[shop]
pool.type = fastcgi
listen = {{ADDR[shop]}}
chdir = $docroot
pm = static
pm.max_children = 1
env[FPMNG_ROUTE_POOL] = shop

[shopapi]
pool.type = fastcgi
listen = {{ADDR[shopapi]}}
chdir = $docroot
pm = static
pm.max_children = 1
env[FPMNG_ROUTE_POOL] = shopapi
EOT;

$tester = new FPM\Tester($config, '<?php echo "unused";');
$tester->start();
$tester->expectLogStartNotices();
$http = $tester->getAddr('ipv4', '[http]');

/* Match: the Host names the host of the bound entry. */
expect($http, 'match', "Host: a.example", '/x', 'site');

/* Host-first precedence: site (a.example/) beats api (/api, host-less), which
 * is the longer prefix. */
expect($http, 'precedence', "Host: a.example", '/api/x', 'site');

/* Mismatch: another host gets the host-less rows only. */
expect($http, 'mismatch', "Host: other.example", '/x', 'web');
expect($http, 'mismatch', "Host: other.example", '/api/x', 'api');

/* Case folding: the Host is matched ignoring case, in both directions (the
 * configuration says A.Example). */
expect($http, 'case-folding', "Host: A.EXAMPLE", '/x', 'site');
expect($http, 'case-folding', "Host: a.Example", '/x', 'site');

/* Port in Host: the port is ignored when the host is compared. */
expect($http, 'port-in-host', "Host: a.example:8080", '/x', 'site');
expect($http, 'port-in-host', "Host: shop.example:80", '/api/x', 'shopapi');

/* Longest prefix among the host's own rows: shopapi (shop.example/api) beats
 * shop (shop.example/), and the segment rule keeps /apiary on shop. */
expect($http, 'longest-own', "Host: shop.example", '/api/x', 'shopapi');
expect($http, 'longest-own', "Host: shop.example", '/x', 'shop');
expect($http, 'segment', "Host: shop.example", '/apiary', 'shop');

/* Absolute-form: the authority of the target replaces Host (RFC 9112 3.2.2),
 * so the Host header is ignored for the match. */
expect($http, 'absolute-form', "Host: other.example", '/x', 'web');
$resp = rawGet($http, "Host: other.example", 'http://a.example:8080/x');
if (pool($resp) !== 'site') {
    echo "FAIL: absolute-form authority a.example:8080 was not routed to site\n";
    exit(1);
}
echo "absolute-form: http://a.example:8080/x -> site\n";
$resp = rawGet($http, "Host: a.example", 'http://other.example/x');
if (pool($resp) !== 'web') {
    echo "FAIL: absolute-form authority other.example was not routed to web\n";
    exit(1);
}
echo "absolute-form: http://other.example/x -> web\n";

$tester->terminate();
$tester->expectLogTerminatingNotices();
$tester->close();

echo "Done\n";
?>
--EXPECT--
match: /x -> site
precedence: /api/x -> site
mismatch: /x -> web
mismatch: /api/x -> api
case-folding: /x -> site
case-folding: /x -> site
port-in-host: /x -> site
port-in-host: /api/x -> shopapi
longest-own: /api/x -> shopapi
longest-own: /x -> shop
segment: /apiary -> shop
absolute-form: /x -> web
absolute-form: http://a.example:8080/x -> site
absolute-form: http://other.example/x -> web
Done
--CLEAN--
<?php
require_once "tester.inc";
FPM\Tester::clean();
foreach (glob(sys_get_temp_dir() . '/fpmng-http-route-host-*') as $dir) {
    @unlink($dir . '/index.php');
    @rmdir($dir);
}
?>

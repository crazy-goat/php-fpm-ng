--TEST--
fpm-ng: a malformed or repeated Host never selects a host-bound http.route[] entry (issue #658)
--SKIPIF--
<?php
include "fpmng-skipif.inc";
fpmng_skip_if_pool_type_unsupported('gateway');
?>
--FILE--
<?php

require_once "tester.inc";

/* Issue #658. The reply to a malformed Host is the fall-through: the request is
 * routed as if it named no host, so only the rows bound to no host can match.
 * The gateway answers the same 200 it answered before the feature existed, so a
 * gateway without host rows sees no change, and the request is never sent to
 * the host's own site.
 *
 * Every value below would name a.example if it were read loosely (a port, a
 * case change, a trailing dot, several headers). Each must route to web for /x
 * and to api for /api/x. A request that reached site would print site. A request
 * with no Host header at all is the last case: it has no name to match, so it
 * falls through the same way. */

function rawGet(string $addr, string $headers, string $path): string
{
    [$host, $port] = explode(':', $addr);
    $fp = fsockopen($host, (int) $port, $errno, $errstr, 5);
    if (!$fp) {
        echo "FAIL: connect: $errstr ($errno)\n";
        exit(1);
    }
    $lines = $headers === "" ? "" : "$headers\r\n";
    fwrite($fp, "GET $path HTTP/1.1\r\n$lines" . "Connection: close\r\n\r\n");
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

$docroot = sys_get_temp_dir() . '/fpmng-http-route-host-malformed-' . getmypid();
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
http.route[site] = a.example/

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
EOT;

$tester = new FPM\Tester($config, '<?php echo "unused";');
$tester->start();
$tester->expectLogStartNotices();
$http = $tester->getAddr('ipv4', '[http]');

$cases = [
    'empty value' => "Host: ",
    'port above 65535' => "Host: a.example:99999",
    'empty port' => "Host: a.example:",
    'non-digit port' => "Host: a.example:80x",
    'space in name' => "Host: a example",
    'IPv6 literal' => "Host: [::1]",
    'trailing dot' => "Host: a.example.",
    'empty label' => "Host: a..example",
    'userinfo' => "Host: user@a.example",
    'two Host headers, same name' => "Host: a.example\r\nHost: a.example",
    'two Host headers, other first' => "Host: other.example\r\nHost: a.example",
    'two Host headers, a.example first' => "Host: a.example\r\nHost: other.example",
    'no Host header' => "",
];

foreach ($cases as $label => $headers) {
    $web = pool(rawGet($http, $headers, '/x'));
    if ($web !== 'web') {
        echo "FAIL: $label: /x served by $web, expected web\n";
        exit(1);
    }
    $api = pool(rawGet($http, $headers, '/api/x'));
    if ($api !== 'api') {
        echo "FAIL: $label: /api/x served by $api, expected api\n";
        exit(1);
    }
    echo "$label: /x -> web, /api/x -> api\n";
}

/* Control: a well-formed Host with the same name does reach site. */
if (pool(rawGet($http, "Host: a.example", '/x')) !== 'site') {
    echo "FAIL: control: a well-formed Host did not reach site\n";
    exit(1);
}
echo "control: Host: a.example -> site\n";

$tester->terminate();
$tester->expectLogTerminatingNotices();
$tester->close();

echo "Done\n";
?>
--EXPECT--
empty value: /x -> web, /api/x -> api
port above 65535: /x -> web, /api/x -> api
empty port: /x -> web, /api/x -> api
non-digit port: /x -> web, /api/x -> api
space in name: /x -> web, /api/x -> api
IPv6 literal: /x -> web, /api/x -> api
trailing dot: /x -> web, /api/x -> api
empty label: /x -> web, /api/x -> api
userinfo: /x -> web, /api/x -> api
two Host headers, same name: /x -> web, /api/x -> api
two Host headers, other first: /x -> web, /api/x -> api
two Host headers, a.example first: /x -> web, /api/x -> api
no Host header: /x -> web, /api/x -> api
control: Host: a.example -> site
Done
--CLEAN--
<?php
require_once "tester.inc";
FPM\Tester::clean();
foreach (glob(sys_get_temp_dir() . '/fpmng-http-route-host-malformed-*') as $dir) {
    @unlink($dir . '/index.php');
    @rmdir($dir);
}
?>

--TEST--
fpm-ng: a bad http.route[] table is refused at startup, naming the pool and the entry (issue #340)
--SKIPIF--
<?php
include "fpmng-skipif.inc";
fpmng_skip_if_pool_type_unsupported('gateway');
?>
--FILE--
<?php

require_once "tester.inc";

$routeRoot = sys_get_temp_dir() . '/fpmng-route-invalid-' . getmypid();
@mkdir($routeRoot, 0700, true);
file_put_contents($routeRoot . '/index.php', '<?php echo "ok";');

/* Issue #340, acceptance criterion 5. Every one of these is a configuration
 * that cannot be made to work at request time, so it has to be a startup
 * refusal: a route naming a pool that is not there would otherwise be a 502
 * per request on a prefix the operator believes is configured.
 *
 * These are gateway pools, which a distribution libphp has always supported;
 * issue #420 removed the libphp capability guard entirely, so there is no
 * "type unsupported" refusal left to accommodate here. */

function expectConfigFailure(string $label, string $cfg, array $needles): void
{
    $tester = new FPM\Tester($cfg, '<?php echo "ok";');
    $messages = $tester->testConfig(true);
    if ($messages === null) {
        echo "FAIL: $label unexpectedly passed validation\n";
        exit(1);
    }
    $text = implode("\n", $messages);
    foreach ($needles as $needle) {
        if (!str_contains($text, $needle)) {
            echo "FAIL: $label missing needle: $needle\n";
            echo "got:\n$text\n";
            exit(1);
        }
    }
    echo "$label: rejected\n";
}

function expectConfigAccepted(string $label, string $cfg): void
{
    $tester = new FPM\Tester($cfg, '<?php echo "ok";');
    $messages = $tester->testConfig(true);
    if ($messages !== null) {
        echo "FAIL: $label unexpectedly failed validation\n";
        echo implode("\n", $messages) . "\n";
        exit(1);
    }
    echo "$label: accepted\n";
}

function gateway(string $routes, string $extra = '', string $apiListen = '{{ADDR[api]}}'): string
{
    return <<<EOT
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
[gw]
pool.type = gateway
listen = {{ADDR[http]}}
http.route[web] = /
$routes

[web]
pool.type = fastcgi
listen = {{ADDR}}
pm = static
pm.max_children = 1

[api]
listen = $apiListen
pm = static
pm.max_children = 1
$extra
EOT;
}

expectConfigFailure(
    'unknown pool',
    gateway('http.route[nope] = /x'),
    ['http.route[nope]', "no pool named 'nope'"]
);

expectConfigFailure(
    'pool named twice',
    gateway("http.route[api] = /a\nhttp.route[api] = /b"),
    ['http.route[api]', 'named twice']
);

expectConfigFailure(
    'duplicate prefix',
    gateway("http.route[api] = /a,/a"),
    ['http.route[api]', "'/a'", 'already routed']
);

expectConfigFailure(
    'prefix without a leading slash',
    gateway('http.route[api] = api/v1'),
    ['http.route[api]', "must begin with '/'"]
);

expectConfigFailure(
    'empty value',
    gateway('http.route[api] ='),
    ['http.route[api]', 'empty value']
);

/* A gateway target is a gateway in front of a gateway; nothing about issue
 * #388 makes that work (a gateway serves neither FastCGI nor HTTP/1.1 on a
 * listener a target could speak to), so it is refused as a target type. */
expectConfigFailure(
    'gateway target',
    <<<EOT
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
[gw]
pool.type = gateway
listen = {{ADDR[http]}}
http.route[web] = /
http.route[other] = /x

[web]
pool.type = fastcgi
listen = {{ADDR}}
pm = static
pm.max_children = 1

[other]
pool.type = gateway
listen = {{ADDR[http2]}}
http.route[web] = /

EOT,
    ['http.route[other]', 'cannot use as a target']
);

/* Issue #344: loopback and Unix targets can use the cleartext HTTP/1.1 client.
 * TLS targets and network-reachable addresses are refused at config time
 * (issue #450). HTTP targets accept numeric loopback literals only; DNS names
 * are refused to avoid a resolver/rebinding gap between validation and use. */
expectConfigFailure(
    'http-direct TLS target',
    gateway('http.route[api] = /x', "pool.type = http-direct\nchdir = $routeRoot\nhttp.tls_cert = /fpmng-route-invalid-no-such-cert.pem"),
    ['http.route[api]', 'terminates TLS']
);

expectConfigAccepted(
    'http-direct Unix target',
    gateway('http.route[api] = /x', "pool.type = http-direct\nchdir = $routeRoot", '/tmp/fpmng-route-safe.sock')
);
expectConfigAccepted(
    'http-direct IPv4 loopback target',
    gateway('http.route[api] = /x', "pool.type = http-direct\nchdir = $routeRoot", '127.0.0.2:29040')
);
expectConfigAccepted(
    'http-direct IPv6 loopback target',
    gateway('http.route[api] = /x', "pool.type = http-direct\nchdir = $routeRoot", '[::1]:29040')
);

foreach ([
    '192.0.2.7:29040',
    '0.0.0.0:29040',
    '[2001:db8::1]:29040',
    '[::ffff:127.0.0.1]:29040',
    'localhost:29040',
] as $address) {
    expectConfigFailure(
        "http-direct target at $address",
        gateway('http.route[api] = /x', "pool.type = http-direct\nchdir = $routeRoot", $address),
        ['http.route[api]', $address, 'gateway speaks cleartext']
    );
}

?>
Done
--EXPECT--
unknown pool: rejected
pool named twice: rejected
duplicate prefix: rejected
prefix without a leading slash: rejected
empty value: rejected
gateway target: rejected
http-direct TLS target: rejected
http-direct Unix target: accepted
http-direct IPv4 loopback target: accepted
http-direct IPv6 loopback target: accepted
http-direct target at 192.0.2.7:29040: rejected
http-direct target at 0.0.0.0:29040: rejected
http-direct target at [2001:db8::1]:29040: rejected
http-direct target at [::ffff:127.0.0.1]:29040: rejected
http-direct target at localhost:29040: rejected
Done
--CLEAN--
<?php
require_once "tester.inc";
FPM\Tester::clean();
foreach (glob(sys_get_temp_dir() . '/fpmng-route-invalid-*') as $dir) {
    @unlink($dir . '/index.php');
    @rmdir($dir);
}
?>

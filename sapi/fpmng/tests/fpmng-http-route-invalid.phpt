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

function gateway(string $routes, string $extra = ''): string
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
listen = {{ADDR[api]}}
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

/* Issue #344: an http-direct target is now accepted (the HTTP/1.1 client
 * transport), so nothing here asserts its refusal any more -- it is exercised
 * end to end by fpmng-http-route-http-direct.phpt. Still refused: a target
 * that terminates TLS on its own listener, because the client transport
 * speaks cleartext to loopback and unix sockets only. */
expectConfigFailure(
    'http-direct TLS target',
    gateway('http.route[api] = /x', "pool.type = http-direct\nchdir = /tmp\nhttp.tls_cert = /fpmng-route-invalid-no-such-cert.pem"),
    ['http.route[api]', 'terminates TLS']
);

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
Done
--CLEAN--
<?php
require_once "tester.inc";
FPM\Tester::clean();
?>

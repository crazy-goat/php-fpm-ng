--TEST--
fpm-ng: a bad http.route[] table is refused at startup, naming the pool and the entry (issue #340)
--SKIPIF--
<?php
include "fpmng-skipif.inc";
fpmng_skip_if_pool_type_unsupported('http');
?>
--FILE--
<?php

require_once "tester.inc";

/* Issue #340, acceptance criterion 5. Every one of these is a configuration
 * that cannot be made to work at request time, so it has to be a startup
 * refusal: a route naming a pool that is not there would otherwise be a 502
 * per request on a prefix the operator believes is configured.
 *
 * A binary linked against a distribution libphp refuses `pool.type = http`
 * before it reads any directive of the pool, so that refusal counts as a
 * rejection here -- the same accommodation fpmng-config-rejected-directives.phpt
 * makes, and for the same reason (issue #215). */
const FPMNG_TYPE_UNSUPPORTED = 'does not carry patches/0006';

function expectConfigFailure(string $label, string $cfg, array $needles): void
{
    $tester = new FPM\Tester($cfg, '<?php echo "ok";');
    $messages = $tester->testConfig(true);
    if ($messages === null) {
        echo "FAIL: $label unexpectedly passed validation\n";
        exit(1);
    }
    $text = implode("\n", $messages);
    if (str_contains($text, FPMNG_TYPE_UNSUPPORTED)) {
        echo "$label: rejected\n";
        return;
    }
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
[web]
listen = {{ADDR}}
pm = static
pm.max_children = 1
pool.type = http
http.listen = {{ADDR[http]}}
$routes

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

/* An http target is a gateway in front of a gateway; nothing about this issue
 * makes that work, so it is refused as a target type. */
expectConfigFailure(
    'http target',
    <<<EOT
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
[web]
listen = {{ADDR}}
pm = static
pm.max_children = 1
pool.type = http
http.listen = {{ADDR[http]}}
http.route[other] = /x

[other]
listen = {{ADDR[api]}}
pm = static
pm.max_children = 1
pool.type = http
http.listen = {{ADDR[http2]}}
EOT,
    ['http.route[other]', 'cannot use as a target']
);

/* The wording matters as much as the refusal: routing to an http-direct pool
 * is a capability that #344 adds, not something ruled out by design. */
expectConfigFailure(
    'http-direct target',
    gateway('http.route[api] = /x', "pool.type = http-direct\nhttp.listen = {{ADDR[http2]}}"),
    ['http.route[api]', 'not yet supported', '#344']
);

?>
Done
--EXPECT--
unknown pool: rejected
pool named twice: rejected
duplicate prefix: rejected
prefix without a leading slash: rejected
empty value: rejected
http target: rejected
http-direct target: rejected
Done
--CLEAN--
<?php
require_once "tester.inc";
FPM\Tester::clean();
?>

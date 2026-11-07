--TEST--
fpm-ng: an invalid host name in http.route[] is refused by -t, and a valid one is accepted (issue #658)
--SKIPIF--
<?php
include "fpmng-skipif.inc";
fpmng_skip_if_pool_type_unsupported('gateway');
?>
--FILE--
<?php

require_once "tester.inc";

/* Issue #658. The host part of an entry is checked by `php-fpm-ng -t`, so a bad
 * name is a startup refusal and never a route that silently matches nothing.
 * A port is refused on purpose: the gateway matches the name without it, so a
 * configured port would look right and never be compared. */

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

function gateway(string $routeValue): string
{
    return <<<EOT
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
[gw]
pool.type = gateway
listen = {{ADDR[http]}}
http.route[web] = /
http.route[site] = $routeValue

[web]
pool.type = fastcgi
listen = {{ADDR}}
pm = static
pm.max_children = 1

[site]
pool.type = fastcgi
listen = {{ADDR[site]}}
pm = static
pm.max_children = 1
EOT;
}

expectConfigFailure(
    'host with a port',
    gateway('a.example:8080/'),
    ['http.route[site]', "'a.example:8080/'", 'nor a valid host name', 'no port']
);

expectConfigFailure(
    'wildcard host',
    gateway('*.example/'),
    ['http.route[site]', "'*.example/'", 'nor a valid host name']
);

expectConfigFailure(
    'IPv6 literal host',
    gateway('[::1]/'),
    ['http.route[site]', 'nor a valid host name']
);

expectConfigFailure(
    'empty label',
    gateway('a..example/'),
    ['http.route[site]', "'a..example/'", 'nor a valid host name']
);

expectConfigFailure(
    'label that ends in a hyphen',
    gateway('a-.example/'),
    ['http.route[site]', "'a-.example/'", 'nor a valid host name']
);

expectConfigFailure(
    'trailing dot',
    gateway('a.example./'),
    ['http.route[site]', "'a.example./'", 'nor a valid host name']
);

expectConfigFailure(
    'label that begins with a hyphen',
    gateway('-a.example/'),
    ['http.route[site]', "'-a.example/'", 'nor a valid host name']
);

expectConfigFailure(
    'underscore in host',
    gateway('a_b.example/'),
    ['http.route[site]', "'a_b.example/'", 'nor a valid host name']
);

expectConfigFailure(
    'host name with a space',
    gateway('a example/'),
    ['http.route[site]', "'a example/'", 'nor a valid host name']
);

/* The same host with a different case is the same entry (the gateway matches
 * ignoring case), so claiming it twice is refused as a duplicate. */
expectConfigFailure(
    'host claimed twice, case only',
    gateway("A.example/x, a.example/x"),
    ['http.route[site]', "entry 'a.example/x' is already routed"]
);

expectConfigAccepted('bare host', gateway('a.example'));
expectConfigAccepted('host with a path', gateway('a.example/api'));
expectConfigAccepted('upper-case host', gateway('A.Example/'));
expectConfigAccepted('dotless host', gateway('localhost/x'));
expectConfigAccepted('same path, two hosts', gateway('a.example/x, b.example/x'));

?>
Done
--EXPECT--
host with a port: rejected
wildcard host: rejected
IPv6 literal host: rejected
empty label: rejected
label that ends in a hyphen: rejected
trailing dot: rejected
label that begins with a hyphen: rejected
underscore in host: rejected
host name with a space: rejected
host claimed twice, case only: rejected
bare host: accepted
host with a path: accepted
upper-case host: accepted
dotless host: accepted
same path, two hosts: accepted
Done
--CLEAN--
<?php
require_once "tester.inc";
FPM\Tester::clean();
?>

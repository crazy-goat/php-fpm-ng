--TEST--
fpm-ng: -t refuses a bad http.response_header[] and the directive on a pool type that has no gateway (issue #650)
--SKIPIF--
<?php
include "fpmng-skipif.inc";
fpmng_skip_if_pool_type_unsupported('gateway');
fpmng_skip_if_pool_type_unsupported('http-direct');
?>
--FILE--
<?php

require_once "tester.inc";

/* Issue #650: the name and the value are checked when the line is read. A CR
 * or LF in a value would split the reply. A LF never gets that far: the INI
 * parser itself refuses a quoted value that spans lines. */

function validate(string $cfg): ?array
{
    $tester = new FPM\Tester($cfg, '<?php echo "ok";');
    $messages = $tester->testConfig(true);
    FPM\Tester::clean();
    return $messages;
}

function expectRefused(string $label, string $cfg, string $needle): void
{
    $messages = validate($cfg);
    if ($messages === null) {
        echo "FAIL: $label unexpectedly passed validation\n";
        exit(1);
    }
    $text = implode("\n", $messages);
    if (!str_contains($text, $needle)) {
        echo "FAIL: $label missing needle: $needle\ngot:\n$text\n";
        exit(1);
    }
    echo "$label: refused\n";
}

function expectAccepted(string $label, string $cfg): void
{
    $messages = validate($cfg);
    if ($messages !== null) {
        echo "FAIL: $label unexpectedly failed validation\n" . implode("\n", $messages) . "\n";
        exit(1);
    }
    echo "$label: accepted\n";
}

function gateway(string $extra): string
{
    return <<<EOT
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
[gw]
pool.type = gateway
listen = {{ADDR[http]}}
chdir = /tmp
http.gateways = 1
http.route[web] = /
$extra
[web]
pool.type = fastcgi
listen = {{ADDR}}
pm = static
pm.max_children = 1
chdir = /tmp
EOT;
}

expectAccepted('plain value', gateway('http.response_header[X-Frame-Options] = DENY'));
/* An unquoted ';' starts an INI comment, so the value is quoted. */
expectAccepted('quoted value with a semicolon',
    gateway('http.response_header[Strict-Transport-Security] = "max-age=31536000; includeSubDomains"'));
expectAccepted('two different names',
    gateway("http.response_header[X-A] = a\nhttp.response_header[X-B] = b"));

expectRefused('name with a space', gateway('http.response_header[X Bad] = v'), 'http.response_header[X Bad]');
expectRefused('name with a colon', gateway('http.response_header[X:Bad] = v'), 'may contain only letters');
expectRefused('CR in the value', gateway("http.response_header[X-A] = \"a\rb\""), 'CR or LF');
expectRefused('LF in a quoted value', gateway("http.response_header[X-A] = \"a\nX-Evil: 1\""), 'syntax error');
expectRefused('control character in the value', gateway("http.response_header[X-A] = \"a\x01b\""), 'control character');
expectRefused('empty value', gateway('http.response_header[X-A] = ""'), 'empty');
expectRefused('Content-Length', gateway('http.response_header[Content-Length] = 5'), 'sets this header itself');
expectRefused('X-Request-Id in other case', gateway('http.response_header[x-request-id] = 5'), 'sets this header itself');
expectRefused('Proxy- prefix', gateway('http.response_header[Proxy-Foo] = 5'), 'belong to the gateway');
expectRefused('X-Fpmng- prefix', gateway('http.response_header[X-Fpmng-Foo] = 5'), 'belong to the gateway');
expectRefused('the same name in two cases',
    gateway("http.response_header[X-A] = a\nhttp.response_header[x-a] = b"), 'given twice');
expectRefused('value over 4096 bytes', gateway('http.response_header[X-A] = ' . str_repeat('a', 4097)), '4096');
expectAccepted('value of exactly 4096 bytes', gateway('http.response_header[X-A] = ' . str_repeat('a', 4096)));

/* The directive is the gateway's: the other pool types refuse it by name. */
$fastcgi = <<<EOT
[global]
error_log = {{FILE:LOG}}
[web]
pool.type = fastcgi
listen = {{ADDR}}
pm = static
pm.max_children = 1
chdir = /tmp
http.response_header[X-A] = a
EOT;
expectRefused('fastcgi pool', $fastcgi, 'http.response_header');
foreach (['classic' => '', 'worker' => "pool.executor = worker\nhttp.front_controller = /w.php\n"] as $label => $extra) {
    $direct = <<<EOT
[global]
error_log = {{FILE:LOG}}
[d]
pool.type = http-direct
listen = {{ADDR}}
pm = static
pm.max_children = 1
chdir = /tmp
$extra
http.response_header[X-A] = a
EOT;
    expectRefused("http-direct $label pool", $direct, 'http.response_header');
}
echo "Done\n";
?>
--EXPECT--
plain value: accepted
quoted value with a semicolon: accepted
two different names: accepted
name with a space: refused
name with a colon: refused
CR in the value: refused
LF in a quoted value: refused
control character in the value: refused
empty value: refused
Content-Length: refused
X-Request-Id in other case: refused
Proxy- prefix: refused
X-Fpmng- prefix: refused
the same name in two cases: refused
value over 4096 bytes: refused
value of exactly 4096 bytes: accepted
fastcgi pool: refused
http-direct classic pool: refused
http-direct worker pool: refused
Done
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>

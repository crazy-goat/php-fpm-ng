--TEST--
fpm-ng: -t refuses a bad http.error_page[], reads the page files, and refuses the directive on a pool type that has no gateway (issue #650)
--SKIPIF--
<?php
include "fpmng-skipif.inc";
fpmng_skip_if_pool_type_unsupported('gateway');
fpmng_skip_if_pool_type_unsupported('http-direct');
?>
--FILE--
<?php

require_once "tester.inc";

/* Issue #650: the master reads each page at configuration time, so -t is where
 * a missing, oversize or unknown-type page is refused. The limit is 64 KiB. */

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

$root = sys_get_temp_dir() . '/fpmng-gw-error-page-config-' . getmypid();
@mkdir($root, 0700, true);
file_put_contents("$root/ok.html", '<h1>maintenance</h1>');
file_put_contents("$root/ok.json", '{"error":"maintenance"}');
file_put_contents("$root/max.html", str_repeat('a', 65536));
file_put_contents("$root/over.html", str_repeat('a', 65537));
file_put_contents("$root/empty.html", '');
file_put_contents("$root/page.bin", 'x');
@mkdir("$root/dir.html");
exec('mkfifo ' . escapeshellarg("$root/fifo.html"));

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

try {
    expectAccepted('three pages', gateway("http.error_page[404] = $root/ok.html\nhttp.error_page[502] = $root/ok.json\nhttp.error_page[503] = $root/ok.html"));
    expectAccepted('a file of exactly 64 KiB', gateway("http.error_page[503] = $root/max.html"));
    expectRefused('a file of 64 KiB + 1', gateway("http.error_page[503] = $root/over.html"), 'the limit is 65536');
    expectRefused('key 413', gateway("http.error_page[413] = $root/ok.html"), 'docs/gateway.md');
    expectRefused('key 500', gateway("http.error_page[500] = $root/ok.html"), 'the key must be 404, 502 or 503');
    expectRefused('key 504', gateway("http.error_page[504] = $root/ok.html"), 'the key must be 404, 502 or 503');
    expectRefused('a relative path', gateway('http.error_page[404] = ok.html'), 'the path must be absolute');
    expectRefused('a missing file', gateway("http.error_page[404] = $root/missing.html"), 'cannot open the file');
    expectRefused('a directory', gateway("http.error_page[404] = $root/dir.html"), 'not a regular file');
    expectRefused('a FIFO', gateway("http.error_page[404] = $root/fifo.html"), 'not a regular file');
    expectRefused('an empty file', gateway("http.error_page[404] = $root/empty.html"), 'the file is empty');
    expectRefused('an unknown extension', gateway("http.error_page[404] = $root/page.bin"), 'no known content type');
    expectRefused('the same key twice', gateway("http.error_page[404] = $root/ok.html\nhttp.error_page[404] = $root/ok.json"), 'given twice');

    $fastcgi = <<<EOT
[global]
error_log = {{FILE:LOG}}
[web]
pool.type = fastcgi
listen = {{ADDR}}
pm = static
pm.max_children = 1
chdir = /tmp
http.error_page[404] = $root/ok.html
EOT;
    expectRefused('fastcgi pool', $fastcgi, 'http.error_page');
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
http.error_page[404] = $root/ok.html
EOT;
        expectRefused("http-direct $label pool", $direct, 'http.error_page');
    }
    echo "Done\n";
} finally {
    foreach (glob("$root/*") as $f) {
        is_dir($f) ? @rmdir($f) : @unlink($f);
    }
    @rmdir($root);
}
?>
--EXPECT--
three pages: accepted
a file of exactly 64 KiB: accepted
a file of 64 KiB + 1: refused
key 413: refused
key 500: refused
key 504: refused
a relative path: refused
a missing file: refused
a directory: refused
a FIFO: refused
an empty file: refused
an unknown extension: refused
the same key twice: refused
fastcgi pool: refused
http-direct classic pool: refused
http-direct worker pool: refused
Done
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>

--TEST--
fpm-ng: pool.type = gateway serves the public port named by 'listen'; http.listen is refused as redundant (issue #388)
--SKIPIF--
<?php
include "fpmng-skipif.inc";
fpmng_skip_if_pool_type_unsupported('gateway');
?>
--FILE--
<?php

require_once "tester.inc";

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

/* Issue #388: on the gateway `listen` is the public HTTP port itself. Before
 * the type existed this was http.listen, defaulting to the FastCGI port + 1;
 * there is no FastCGI port now, so the address is exactly what `listen` says
 * and http.listen has nothing left to override. */
$docroot = sys_get_temp_dir() . '/fpmng-gateway-listen-' . getmypid();
@mkdir($docroot, 0700, true);
file_put_contents($docroot . '/index.php', '<?php echo "gateway-listen-public";');

$label = 'gateway-listen-public';
$cfg = <<<EOT
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
[gw]
pool.type = gateway
listen = {{ADDR[http]}}
chdir = $docroot
http.route[web] = /
[web]
pool.type = fastcgi
listen = {{ADDR}}
pm = static
pm.max_children = 1
chdir = $docroot
EOT;

$tester = new FPM\Tester($cfg, '<?php echo "unused";');
$tester->start();
$tester->expectLogStartNotices();

$addr = $tester->getAddr('ipv4', '[http]');
$body = @file_get_contents("http://$addr/index.php");
if ($body !== $label) {
    echo "FAIL: $label body=" . var_export($body, true) . "\n";
    exit(1);
}
echo "$label: ok\n";

$tester->terminate();
$tester->expectLogTerminatingNotices();
$tester->close();

/* http.listen is redundant on the gateway and refused by name. */
expectConfigFailure(
    'gateway-http-listen-redundant',
    <<<EOT
[global]
error_log = {{FILE:LOG}}
[gw]
pool.type = gateway
listen = {{ADDR[http]}}
http.listen = {{ADDR[redundant]}}
http.route[web] = /
[web]
pool.type = fastcgi
listen = {{ADDR}}
pm = static
pm.max_children = 1
EOT,
    ['http.listen is redundant on pool.type = gateway']
);

@unlink($docroot . '/index.php');
@rmdir($docroot);

?>
Done
--EXPECT--
gateway-listen-public: ok
gateway-http-listen-redundant: rejected
Done
--CLEAN--
<?php
require_once "tester.inc";
FPM\Tester::clean();
?>

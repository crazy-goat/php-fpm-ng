--TEST--
fpm-ng: pool.type = http with a TCP listen and no http.listen defaults to the
FastCGI port + 1; a unix-socket listen is still refused (task 015)
--SKIPIF--
<?php include "skipif.inc"; ?>
--FILE--
<?php

require_once "tester.inc";
require_once "fpmng-tester.inc";

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

/* Acceptance criterion 1: a TCP pool with no http.listen starts and the
 * gateway listens on the FastCGI port + 1 — the documented default. Before
 * task 015, fpm_conf.c ran fpm_http_validate_pool() before the "listen" block
 * populated wp->listen_address_domain, so the domain was always unset (never
 * FPM_AF_INET) and http.listen was required for every http pool, TCP included. */
$dir = __DIR__;
$label = 'http-default-listen';
$cfg = <<<EOT
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
[web]
listen = {{ADDR}}
chdir = $dir
pm = static
pm.max_children = 1
pool.type = http
EOT;

$tester = new FPM\Tester($cfg, '<?php echo "' . $label . '";');
$tester->start();
fpmng_expect_log_start_notices($tester);

$addr = $tester->getAddr('ipv4');
[$host, $port] = explode(':', $addr);
$httpAddr = $host . ':' . ((int) $port + 1);

$script = basename($tester->makeSourceFile($label . '-'));
$body = @file_get_contents("http://$httpAddr/$script");
if ($body !== $label) {
    echo "FAIL: $label body=" . var_export($body, true) . "\n";
    exit(1);
}
echo "$label: ok\n";

$tester->terminate();
$tester->expectLogTerminatingNotices();
$tester->close();

/* Acceptance criterion 2: a unix-socket pool still requires http.listen —
 * there is no FastCGI port to bump by one — with the same message as before,
 * which is now actually true for the case it fires on. */
$sockPath = sys_get_temp_dir() . '/fpmng-http-listen-required-' . getmypid() . '.sock';
expectConfigFailure(
    'http-unix-socket-requires-http-listen',
    <<<EOT
[global]
error_log = {{FILE:LOG}}
[uxweb]
listen = $sockPath
pm = static
pm.max_children = 1
pool.type = http
EOT,
    ['pool.type = http requires http.listen when listen is a unix socket']
);

?>
Done
--EXPECT--
http-default-listen: ok
http-unix-socket-requires-http-listen: rejected
Done
--CLEAN--
<?php
require_once "tester.inc";
FPM\Tester::clean();
?>

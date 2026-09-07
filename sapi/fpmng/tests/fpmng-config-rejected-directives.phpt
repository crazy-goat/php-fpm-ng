--TEST--
fpm-ng: rejected directives and illegal executors fail at configuration validation (fpm_pool_type.c rejects[], docs/NOTES.md §3i/3o)
--SKIPIF--
<?php include "skipif.inc"; ?>
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

$base = <<<EOT
[global]
error_log = {{FILE:LOG}}
[pool]
listen = {{ADDR}}
pm = static
pm.max_children = 1
EOT;

expectConfigFailure(
    'fastcgi-http-directive',
    $base . "\nhttp.listen = 127.0.0.1:8080",
    ["'http.listen' is not supported by pool.type = fastcgi"]
);

expectConfigFailure(
    'http-fiber-directive-on-classic',
    $base . "\npool.type = http\nfiber.revalidate_freq = 0",
    ["'fiber.revalidate_freq' is not supported by pool.type = http"]
);

expectConfigFailure(
    'supervisor-listen',
    str_replace('[pool]', '[sup]', $base) . "\npool.type = supervisor\nsupervisor.script = {{FILE:*src.php}}\nsupervisor.processes = 1",
    ["'listen' is not supported by pool.type = supervisor"]
);

expectConfigFailure(
    'cron-pm',
    str_replace('[pool]', '[job]', $base) . "\npool.type = cron\ncron.schedule = * * * * *\ncron.script = {{FILE:*src.php}}\npm.max_children = 2",
    ["'pm.max_children' is not supported by pool.type = cron"]
);

expectConfigFailure(
    'supervisor-executor',
    str_replace('[pool]', '[sup2]', $base) . "\npool.type = supervisor\npool.executor = fiber\nsupervisor.script = {{FILE:*src.php}}\nsupervisor.processes = 1",
    ['pool.executor is not supported by pool.type = supervisor']
);

expectConfigFailure(
    'default-fastcgi-executor',
    $base . "\npool.executor = classic",
    ['pool.executor is not supported by pool.type = fastcgi']
);

expectConfigFailure(
    'async-disabled',
    $base . "\npool.type = http\npool.executor = async\nhttp.listen = {{ADDR[http]}}",
    ['--enable-fpmng-async']
);

?>
Done
--EXPECT--
fastcgi-http-directive: rejected
http-fiber-directive-on-classic: rejected
supervisor-listen: rejected
cron-pm: rejected
supervisor-executor: rejected
default-fastcgi-executor: rejected
async-disabled: rejected
Done
--CLEAN--
<?php
require_once "tester.inc";
FPM\Tester::clean();
?>

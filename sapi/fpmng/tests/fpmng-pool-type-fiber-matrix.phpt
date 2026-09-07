--TEST--
fpm-ng: legal pool.type values with the fiber executor start, serve one request, and shut down (docs/NOTES.md §3t)
--SKIPIF--
<?php
include "skipif.inc";

$binary = getenv('TEST_PHP_FPM_EXECUTABLE') ?: FPM\Tester::findExecutable();
exec(escapeshellarg($binary) . ' -i 2>&1', $output, $status);
if ($status !== 0 || !str_contains(implode("\n", $output), '--enable-fpmng-fiber')) {
    die('skip php-fpm-ng was not built with --enable-fpmng-fiber');
}
?>
--FILE--
<?php

require_once "tester.inc";

$dir = __DIR__;

function exercise(string $label, string $extraConfig, bool $http = false): void
{
    global $dir;
    $cfg = <<<EOT
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
[matrix]
listen = {{ADDR}}
chdir = $dir
pm = static
pm.max_children = 1
php_admin_value[opcache.enable] = 0
$extraConfig
EOT;

    $tester = new FPM\Tester($cfg, '<?php echo "' . $label . '";');
    $tester->start();
    $tester->expectLogStartNotices();

    if ($http) {
        $addr = $tester->getAddr('ipv4', '[http]');
        $script = basename($tester->makeSourceFile($label . '-'));
        $body = @file_get_contents("http://$addr/$script");
        if ($body !== $label) {
            echo "FAIL: $label body=" . var_export($body, true) . "\n";
            exit(1);
        }
    } else {
        $tester->request()->expectBody($label, skipHeadersCheck: true);
    }

    $tester->terminate();
    $tester->expectLogTerminatingNotices();
    $tester->close();
    echo "$label: ok\n";
}

exercise('fastcgi-ng-fiber', "pool.type = fastcgi-ng\npool.executor = fiber");
exercise('http-fiber', "pool.type = http\nhttp.listen = {{ADDR[http]}}", http: true);

?>
Done
--EXPECT--
fastcgi-ng-fiber: ok
http-fiber: ok
Done
--CLEAN--
<?php
require_once "tester.inc";
FPM\Tester::clean();
?>

--TEST--
fpm-ng: legal pool.type values with the classic executor start, serve one request, and shut down (docs/NOTES.md §3i)
--SKIPIF--
<?php include "skipif.inc"; ?>
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
$extraConfig
EOT;

    $tester = new FPM\Tester($cfg, '<?php echo "' . $label . '";');
    $tester->start();
    $tester->expectLogStartNotices();

    if ($http) {
        $addr = $tester->getAddr('ipv4', '[http]');
        $script = basename($tester->makeSourceFile($label . '-'));
        $body = @file_get_contents("http://$addr/$script");
    } else {
        $tester->request()->expectBody($label, skipHeadersCheck: true);
    }

    if ($http) {
        if ($body !== $label) {
            echo "FAIL: $label body=" . var_export($body, true) . "\n";
            exit(1);
        }
    }

    $tester->terminate();
    $tester->expectLogTerminatingNotices();
    $tester->close();
    echo "$label: ok\n";
}

exercise('fastcgi-ng-classic', "pool.type = fastcgi-ng\npool.executor = classic");
exercise('http-classic', "pool.type = http\nhttp.listen = {{ADDR[http]}}", http: true);

?>
Done
--EXPECT--
fastcgi-ng-classic: ok
http-classic: ok
Done
--CLEAN--
<?php
require_once "tester.inc";
FPM\Tester::clean();
?>

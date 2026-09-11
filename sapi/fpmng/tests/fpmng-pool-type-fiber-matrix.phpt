--TEST--
fpm-ng: legal pool.type values with the fiber executor start, serve one request, and shut down (docs/NOTES.md §3t)
--SKIPIF--
<?php
include "fpmng-skipif.inc";
fpmng_skip_if_pool_type_unsupported('fastcgi-ng');
fpmng_skip_if_pool_type_unsupported('http');

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
    /* max_execution_time is pinned in the pool, not inherited: fpm_pool_coop.c:263
     * refuses a fiber pool unless it is 0 (one setitimer()/SIGPROF timer per
     * process cannot represent N concurrent deadlines), and php-fpm-ng is
     * started with -n by tester.inc, so the value comes from PHP's compiled-in
     * default of 30 rather than from any php.ini. Without the pin the pool never
     * starts and the test fails on its first expected NOTICE, which reads as an
     * unrelated startup failure (issue #87). */
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
php_admin_value[max_execution_time] = 0
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
/* pool.executor = fiber spelled out: without it this case is pool.type = http
 * on the classic executor, i.e. character for character what
 * fpmng-pool-type-classic-matrix.phpt's http-classic case already covers, in a
 * file that only runs in an --enable-fpmng-fiber build (issue #87). */
exercise('http-fiber', "pool.type = http\npool.executor = fiber\nhttp.listen = {{ADDR[http]}}", http: true);

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

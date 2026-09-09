--TEST--
FPM http gateway: front-controller fallback for a directory, and its containment check runs at startup (task 018)
--SKIPIF--
<?php
include "skipif.inc";
?>
--FILE--
<?php

require_once "tester.inc";

// Task 018 gap 1: a URL that resolves to an existing directory with no
// index.php of its own (e.g. "/somedir") used to reach the worker as a
// directory path ("File not found"), because stat()/realpath() succeed for a
// directory just like they do for a file. The gateway now treats a directory
// the same as a missing script for http.front_controller purposes -- matching
// nginx's try_files (a bare $uri never matches a directory), not php -S's own
// index.php/index.html directory walk, so no extra syscall is needed. See
// fpm_http_serve_static()'s S_ISDIR handling and the mirrored stat() check in
// fpm_http_build_request(), sapi/fpmng/fpm/fpm_http.c.
function fetch(string $addr, string $path, string $method = 'GET'): string
{
    [$host, $port] = explode(':', $addr);
    $context = stream_context_create(['http' => [
        'method'        => $method,
        'ignore_errors' => true,
        'timeout'       => 5,
    ]]);
    $body = @file_get_contents("http://$host:$port$path", false, $context);

    return $body === false ? 'REQUEST FAILED' : $body;
}

$www = sys_get_temp_dir() . '/' . basename(__FILE__, '.php') . '-www-' . getmypid();
@mkdir($www, 0700, true);
mkdir("$www/somedir");            // exists, no index.php of its own -- gap 1
mkdir("$www/withindex");
file_put_contents("$www/withindex/index.php", "<?php\necho 'unused';");

// One script, reused everywhere, that reports exactly what the gateway
// decided: which script actually ran, and whether PATH_INFO carried the
// original request path.
$script = <<<'EOT'
<?php
echo 'M=', $_SERVER['REQUEST_METHOD'],
    ' SN=', $_SERVER['SCRIPT_NAME'],
    ' PI=', ($_SERVER['PATH_INFO'] ?? 'NONE'),
    ' URI=', $_SERVER['REQUEST_URI'];
EOT;
file_put_contents("$www/index.php", $script);
file_put_contents("$www/real.php", $script);
file_put_contents("$www/withindex/index.php", $script);

$cfg = <<<EOT
[global]
error_log = {{FILE:LOG}}
[main]
listen = {{ADDR}}
pm = static
pm.max_children = 1
chdir = $www
pool.type = http
http.listen = {{ADDR[gw]}}
EOT;

$tester = new FPM\Tester($cfg, '<?php');
$tester->start();
$tester->expectLogStartNotices();

$addr = $tester->getAddr('ipv4', '[gw]');

// Regression: the root request and a plain existing .php file are untouched.
echo fetch($addr, '/'), "\n";
echo fetch($addr, '/real.php'), "\n";
// Regression: a genuinely missing path already fell back before task 018.
echo fetch($addr, '/doesnotexist'), "\n";
// Gap 1, GET/HEAD with http.static on: fpm_http_serve_static()'s fstat() sees
// S_ISDIR and reports "missing" for free, no extra syscall.
echo fetch($addr, '/somedir'), "\n";
// Gap 1, the generic path: POST never goes through fpm_http_serve_static(),
// so this exercises fpm_http_build_request()'s own stat()+S_ISDIR check.
echo fetch($addr, '/somedir', 'POST'), "\n";
// Deliberate simplification, not a bug: a directory that DOES contain its own
// index.php still falls back to the top-level front controller when
// requested without a trailing slash -- this project's try_files does not
// probe a directory for its own index the way nginx's "index" directive or
// php -S would, because that costs an extra stat() per directory segment.
echo fetch($addr, '/withindex'), "\n";
// A trailing slash is unaffected by gap 1: SCRIPT_FILENAME already points at
// withindex/index.php before the front-controller check ever runs.
echo fetch($addr, '/withindex/'), "\n";

$tester->terminate();
$tester->expectLogTerminatingNotices();
$tester->close();

// Task 018 gap 2: http.front_controller's containment check (does it resolve
// inside the document root?) used to be a function-level static evaluated
// lazily on the first request of each gateway process. It now runs once in
// the master, in fpm_http_init_pool_ex(), before the first gateway fork --
// so a misconfiguration is loud at startup, exactly like http.tls_cert
// validation already is. Prove it by never sending this pool a single
// request: if the warning were still lazy, it would never appear at all.
$badRoot  = sys_get_temp_dir() . '/' . basename(__FILE__, '.php') . '-badroot-' . getmypid();
$outside  = sys_get_temp_dir() . '/' . basename(__FILE__, '.php') . '-outside-' . getmypid();
@mkdir($badRoot, 0700, true);
@mkdir($outside, 0700, true);
file_put_contents("$outside/real.php", "<?php\necho 'should never run';");
symlink("$outside/real.php", "$badRoot/escape.php");

$cfg2 = <<<EOT
[global]
error_log = {{FILE:LOG}}
[badpool]
listen = {{ADDR}}
pm = static
pm.max_children = 1
chdir = $badRoot
pool.type = http
http.listen = {{ADDR[gw2]}}
http.front_controller = /escape.php
EOT;

$tester2 = new FPM\Tester($cfg2, '<?php');
$tester2->start();
$tester2->expectLogStartNotices();
// checkAllLogs: the warning is logged from fpm_run(), before the "ready to
// handle connections" notice expectLogStartNotices() just consumed -- rescan
// from the beginning instead of only reading forward from here.
// %s (not a literal '/escape.php'): LogTool's matcher builds a '/'-delimited
// regex out of this string, so an actual slash in the expected message breaks
// the pattern -- %s is its own placeholder mechanism for exactly this case.
$tester2->expectLogWarning(
    "http: http.front_controller '%s' resolves outside the document root, fallback disabled",
    'badpool',
    1,
    true
);
echo "gap2: containment warning seen before any request was ever sent\n";

$tester2->terminate();
$tester2->expectLogTerminatingNotices();
$tester2->close();

// Cleanup -- own scratch directories only (workflow.md: clean up temporary dirs).
function rrmdir(string $dir): void
{
    if (!is_dir($dir)) {
        return;
    }
    foreach (scandir($dir) as $entry) {
        if ($entry === '.' || $entry === '..') {
            continue;
        }
        $path = "$dir/$entry";
        is_link($path) || is_file($path) ? unlink($path) : rrmdir($path);
    }
    rmdir($dir);
}
rrmdir($www);
rrmdir($badRoot);
rrmdir($outside);

?>
Done
--EXPECT--
M=GET SN=/index.php PI=NONE URI=/
M=GET SN=/real.php PI=NONE URI=/real.php
M=GET SN=/index.php PI=/doesnotexist URI=/doesnotexist
M=GET SN=/index.php PI=/somedir URI=/somedir
M=POST SN=/index.php PI=/somedir URI=/somedir
M=GET SN=/index.php PI=/withindex URI=/withindex
M=GET SN=/withindex/index.php PI=NONE URI=/withindex/
gap2: containment warning seen before any request was ever sent
Done
--CLEAN--
<?php
require_once "tester.inc";
FPM\Tester::clean();
?>

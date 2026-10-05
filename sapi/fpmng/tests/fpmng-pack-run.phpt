--TEST--
fpm-ng: a packed executable runs its embedded application, refuses damaged or unusable payloads, and follows a repack on reload (issue #430)
--SKIPIF--
<?php
include "fpmng-skipif.inc";
if (PHP_OS_FAMILY !== 'Linux') die('skip requires Linux');
?>
--FILE--
<?php

require_once "tester.inc";
require_once "fpmng-pack-app.inc";

/* Issue #430, the run half of `php-fpm-ng pack` (#429). The application is a
 * small PHAR (front controller, an included library, a resource, a supervisor
 * script) packed together with a php.ini and an fpm.conf naming it with
 * fpmng-app://. The sources are deleted before the executable starts. Checked:
 *   - the embedded ini wins and the host php.ini and scan dir are ignored (D1);
 *   - include, resource access, SCRIPT_NAME, the classic http-direct executor and a
 *     supervisor script run from the extracted archive;
 *   - traversal and non-public paths reach the front controller only;
 *   - the state directory is private, content-addressed and leaves no temporary
 *     file;
 *   - SIGUSR1 reopens the log, SIGUSR2 re-execs the same app, SIGTERM stops;
 *   - a repack renamed over the running executable plus SIGUSR2 serves the new
 *     code from a different state directory (no stale code), with the OPcache
 *     script paths as evidence;
 *   - the binary's own reader refuses a flipped byte and a truncation, and a
 *     missing entry, a bad conf, a missing extension and an unpacked binary
 *     with fpmng-app:// all fail before "ready", with a message. */

function check(bool $condition, string $message): void
{
    if (!$condition) {
        throw new RuntimeException($message);
    }
}
set_exception_handler(function (Throwable $e): void {
    echo 'FAIL: ' . $e->getMessage() . "\n";
});

function rmtree(string $dir): void
{
    foreach (glob("$dir/{,.}*", GLOB_BRACE) ?: [] as $f) {
        if (in_array(basename($f), ['.', '..'], true)) {
            continue;
        }
        is_dir($f) && !is_link($f) ? rmtree($f) : @unlink($f);
    }
    @rmdir($dir);
}

function get(int $port, string $path): array
{
    $fp = @stream_socket_client("tcp://127.0.0.1:$port", $errno, $error, 2);
    if (!$fp) {
        return [0, ''];
    }
    stream_set_timeout($fp, 5);
    fwrite($fp, "GET $path HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n");
    $raw = stream_get_contents($fp);
    fclose($fp);
    [$head, $body] = explode("\r\n\r\n", $raw, 2) + [1 => ''];
    preg_match('#^HTTP/1\.\d (\d+)#', $head, $m);
    if (stripos($head, 'Transfer-Encoding: chunked') !== false) {
        $plain = '';
        while ($body !== '' && ($eol = strpos($body, "\r\n")) !== false && ($n = hexdec(substr($body, 0, $eol))) > 0) {
            $plain .= substr($body, $eol + 2, $n);
            $body = substr($body, $eol + 2 + $n + 2);
        }
        $body = $plain;
    }
    return [(int) ($m[1] ?? 0), $body];
}

function waitFor(callable $f, string $what, float $seconds = 20)
{
    $end = microtime(true) + $seconds;
    do {
        $v = $f();
        if ($v) {
            return $v;
        }
        usleep(50000);
    } while (microtime(true) < $end);
    check(false, "timed out waiting for $what");
}

/** Runs to completion; returns [status, stdout + stderr]. */
function runToEnd(array $cmd, array $env): array
{
    $proc = proc_open($cmd, [0 => ['file', '/dev/null', 'r'], 1 => ['pipe', 'w'], 2 => ['redirect', 1]], $pipes, null, $env);
    check(is_resource($proc), 'proc_open failed');
    $out = '';
    $end = microtime(true) + 20;
    stream_set_blocking($pipes[1], false);
    while (microtime(true) < $end && proc_get_status($proc)['running']) {
        $out .= stream_get_contents($pipes[1]);
        usleep(20000);
    }
    $running = proc_get_status($proc)['running'];
    if ($running) {
        proc_terminate($proc, 9);
    }
    $out .= stream_get_contents($pipes[1]);
    $status = proc_close($proc);
    check(!$running, 'did not end within 20 s: ' . $out);
    return [$status, $out];
}

function appFiles(string $v, string $suffix = ''): array
{
    return [
        'public/index.php' => <<<PHP
<?php
\$p = parse_url(\$_SERVER['REQUEST_URI'], PHP_URL_PATH);
if (\$p === '/') {
    echo "$v mem=", ini_get('memory_limit'), " precision=", ini_get('precision'), " name=", \$_SERVER['SCRIPT_NAME'], " self=", \$_SERVER['PHP_SELF'];
} elseif (\$p === '/inc') {
    require_once __DIR__ . '/../lib/lib.php';
    echo lib_version();
} elseif (\$p === '/res') {
    echo trim(file_get_contents(__DIR__ . '/../res/data.txt'));
} elseif (\$p === '/opcache') {
    \$st = opcache_get_status(false);
    echo 'file=', __FILE__, "\\ncached=", \$st['opcache_statistics']['num_cached_scripts'], "\\nenabled=", (int) \$st['opcache_enabled'];
} else {
    echo 'route ', \$p;
}
PHP,
        'lib/lib.php' => "<?php\nfunction lib_version() { return 'lib-$v'; }\n",
        'res/data.txt' => "resource-$v\n",
        'bin/sup.php' => "<?php\nrequire __DIR__ . '/../lib/lib.php';\nfile_put_contents(getenv('OUT'), lib_version() . ' ' . ini_get('memory_limit') . \"\\n\", FILE_APPEND);\nsleep(1000);\n",
    ];
}

function appConf(string $work, int $port, string $front = 'fpmng-app://public/index.php', string $extra = ''): string
{
    return <<<CFG
[global]
daemonize = no
error_log = $work/err.log
$extra
[web]
listen = 127.0.0.1:$port
pool.type = http-direct
pm = static
pm.max_children = 1
chdir = /
http.front_controller = $front
[sup]
pool.type = supervisor
supervisor.script = fpmng-app://bin/sup.php
supervisor.processes = 1
supervisor.restart = always
env[OUT] = $work/sup.out
[cron]
pool.type = cron
cron.schedule = 0 0 1 1 *
cron.script = fpmng-app://bin/sup.php
CFG;
}

$binary = FPM\Tester::findExecutable();
$work = sys_get_temp_dir() . '/fpmng-430-run';
rmtree($work);
mkdir("$work/host/conf.d", 0777, true);
mkdir("$work/state", 0700);
$base = 28192 + 200 * (int) getenv('TEST_PHP_WORKER') + (int) getenv('FPMNG_PHPT_PORT_SHIFT');
$ini = "; embedded\nextension=phar\nmemory_limit=77M\nmax_execution_time=0\nopcache.enable=1\n";

/* A host that would change the answer if it were read. */
file_put_contents("$work/host/php.ini", "memory_limit=11M\n");
file_put_contents("$work/host/conf.d/x.ini", "precision=3\n");
$env = [
    'PATH' => getenv('PATH'),
    'PHPRC' => "$work/host",
    'PHP_INI_SCAN_DIR' => "$work/host/conf.d",
    'FPMNG_APP_DIR' => "$work/state",
];

$proc = null;
$master = 0;
$errLog = "$work/err.log";
$logText = fn () => (string) @file_get_contents($errLog);

function startPacked(string $exe, array $env, string $work, string $tag)
{
    $spec = [0 => ['file', '/dev/null', 'r'], 1 => ['file', "$work/$tag.out", 'w'], 2 => ['file', "$work/$tag.err", 'w']];
    $proc = proc_open([$exe], $spec, $pipes, null, $env);
    check(is_resource($proc), "$tag: proc_open failed");
    return $proc;
}

try {
    /* 1. pack, delete the inputs, test the configuration, start. */
    $a = "$work/app-a";
    fpmng_pack($binary, $work, fpmng_mini_phar(appFiles('A')), $ini, appConf($work, $base), $a);
    [$status, $text] = runToEnd([$a, '-t'], $env);
    check($status === 0 && str_contains($text, 'test is successful'), "-t on the packed executable: $status $text");
    check(!str_contains($text, 'WARNING'), "-t printed a warning: $text");
    echo "configuration test ok\n";

    $proc = startPacked($a, $env, $work, 'a');
    $master = proc_get_status($proc)['pid'];
    waitFor(fn () => str_contains($logText(), 'ready to handle connections'), 'ready (a): ' . @file_get_contents("$work/a.err") . $logText());
    $dirs = glob("$work/state/php-fpm-ng-app-*/*");
    check(count($dirs) === 1, 'one state directory expected: ' . implode(' ', $dirs));
    $stateA = $dirs[0];

    foreach ([$base => 'classic'] as $port => $executor) {
        [$s, $b] = get($port, '/');
        check($s === 200 && $b === 'A mem=77M precision=14 name=/public/index.php self=/public/index.php', "$executor /: $s $b");
        [$s, $b] = get($port, '/inc');
        check($s === 200 && $b === 'lib-A', "$executor /inc: $s $b");
        [$s, $b] = get($port, '/res');
        check($s === 200 && $b === 'resource-A', "$executor /res: $s $b");
    }
    echo "app ok\n";
    waitFor(fn () => is_file("$work/sup.out"), 'supervisor output');
    check(file_get_contents("$work/sup.out") === "lib-A 77M\n", 'supervisor: ' . file_get_contents("$work/sup.out"));
    echo "supervisor ok\n";

    /* 2. boundary: only the front controller answers. */
    file_put_contents("$work/host/note.txt", 'HOST-FILE-SECRET');
    foreach (["$work/host/note.txt", '/../../../etc/passwd', '/%2e%2e/%2e%2e/etc/passwd', '/public/../lib/lib.php', '/lib/lib.php', '/app.phar',
              '/fpm.conf', '/php.ini', '/res/data.txt', '//etc/passwd', '/public/index.php/../../lib/lib.php'] as $path) {
        foreach ([$base] as $port) {
            [$s, $b] = get($port, $path);
            check($b !== '' && str_starts_with($b, 'route '), "$path via $port: $s " . substr($b, 0, 80));
            check(!str_contains($b, 'root:') && !str_contains($b, 'HOST-FILE-SECRET') && !str_contains($b, 'function lib_version') && !str_contains($b, 'memory_limit'), "$path leaked content");
        }
    }
    echo "boundary ok\n";

    /* 3. the state directory. */
    clearstatcache();
    $euid = (int) trim(shell_exec('id -u'));
    $parent = dirname($stateA);
    check(fileowner($parent) === $euid && (fileperms($parent) & 0777) === 0700, 'parent not private: ' . decoct(fileperms($parent) & 0777));
    check(fileowner($stateA) === $euid && (fileperms($stateA) & 0777) === 0700, 'state dir not private');
    check(strlen(basename($stateA)) === 64 && preg_match('/^[0-9a-f]{64}$/', basename($stateA)) === 1, 'state dir is not named by the payload digest');
    $names = array_map('basename', glob("$stateA/{,.}*", GLOB_BRACE));
    sort($names);
    check($names === ['.', '..', 'app.phar', 'fpm.conf', 'php.ini'], 'state dir content: ' . implode(',', $names));
    foreach (glob("$stateA/*") as $f) {
        check((fileperms($f) & 0222) === 0 && fileowner($f) === $euid, basename($f) . ' is writable or not ours');
    }
    [, $keys] = get($base, '/opcache');
    check(str_contains($keys, "file=phar://$stateA/app.phar/public/index.php\n") && str_contains($keys, 'enabled=1'), "script identity is not the state directory:\n$keys");
    echo "state dir ok\n";
    $evidenceA = $keys;

    /* 4. log reopen and re-exec keep the app. */
    check(rename($errLog, "$work/err.log.1"), 'rename log');
    proc_terminate($proc, 10);
    waitFor(fn () => str_contains($logText(), 're-opened'), 'log reopen');
    echo "reopen ok\n";
    proc_terminate($proc, 12);
    waitFor(fn () => substr_count($logText(), 'ready to handle connections') >= 1 && str_contains($logText(), 'reloading'), 'reload');
    [$s, $b] = waitFor(fn () => ($r = get($base, '/inc'))[0] === 200 ? $r : false, 'serving after reload');
    check($b === 'lib-A' && proc_get_status($proc)['pid'] === $master && proc_get_status($proc)['running'], 'reload changed the app or the master pid');
    check(glob("$work/state/php-fpm-ng-app-*/*") === [$stateA], 'reload created another state directory');
    echo "reload ok\n";

    /* 5. documented upgrade: a repack renamed over the executable, then SIGUSR2. */
    $b2 = "$work/app-b";
    fpmng_pack($binary, $work, fpmng_mini_phar(appFiles('B')), $ini, appConf($work, $base), $b2);
    check(rename($b2, $a), 'rename the repack over the executable');
    proc_terminate($proc, 12);
    $r = waitFor(fn () => ($r = get($base, '/inc'))[1] === 'lib-B' ? $r : false, 'the repacked application after SIGUSR2');
    foreach ([$base => 'classic'] as $port => $executor) {
        [, $b] = get($port, '/');
        check(str_starts_with($b, 'B '), "$executor still serves old code: $b");
        [, $b] = get($port, '/res');
        check($b === 'resource-B', "$executor resource: $b");
    }
    $dirs = glob("$work/state/php-fpm-ng-app-*/*");
    sort($dirs);
    check(count($dirs) === 2 && in_array($stateA, $dirs, true), 'two state directories expected: ' . implode(' ', $dirs));
    $stateB = array_values(array_diff($dirs, [$stateA]))[0];
    [, $keys] = get($base, '/opcache');
    check(str_contains($keys, "file=phar://$stateB/app.phar/public/index.php\n") && !str_contains($keys, $stateA), "script identity after the upgrade:\n$keys");
    waitFor(fn () => str_contains((string) file_get_contents("$work/sup.out"), 'lib-B'), 'the supervisor runs the new script');
    echo "upgrade ok\n";
    $norm = fn (string $t) => preg_replace(['#[0-9a-f]{64}#', '#' . preg_quote($work, '#') . '#', '#app-\d+/#'], ['<digest>', '<work>', 'app-<euid>/'], $t);
    echo "cache status before the upgrade:\n", $norm($evidenceA), "\nafter:\n", $norm($keys), "\n";

    /* 6. graceful stop. */
    proc_terminate($proc, 15);
    waitFor(fn () => !proc_get_status($proc)['running'], 'exit after SIGTERM');
    check(str_contains($logText(), 'exiting, bye-bye'), 'no clean exit line');
    proc_close($proc);
    $proc = null;
    echo "stop ok\n";

    /* 7. refusals: every one fails before "ready", names the cause, never falls back. */
    $good = appFiles('C');
    $cases = [
        'entry not in the PHAR' => [$good, appConf($work, $base, 'fpmng-app://public/missing.php'), $ini, 'missing.php'],
        'entry with ..' => [$good, appConf($work, $base, 'fpmng-app://public/../lib/lib.php'), $ini, 'empty, absolute, or contains'],
        'unknown directive in fpm.conf' => [$good, appConf($work, $base, 'fpmng-app://public/index.php', 'no_such_directive = 1'), $ini, 'no_such_directive'],
        'no extension=phar in php.ini' => [$good, appConf($work, $base), "; nothing\n", "extension=phar"],
        'missing extension' => [$good, appConf($work, $base), $ini . "extension=fpmng_no_such_extension\n", 'fpmng_no_such_extension'],
    ];
    foreach ($cases as $name => [$files, $conf, $inifile, $needle]) {
        $exe = "$work/refused";
        @unlink($exe);
        @unlink($errLog);
        fpmng_pack($binary, $work, fpmng_mini_phar($files), $inifile, $conf, $exe);
        [$status, $text] = runToEnd([$exe], $env);
        check($status !== 0, "$name: exit status 0");
        check(str_contains($text . $logText(), $needle), "$name: no message with '$needle': $text");
        check(!str_contains($text . $logText(), 'ready to handle connections'), "$name: claimed to be ready");
        check(get($base, '/')[0] === 0, "$name: something is listening");
    }
    echo "config refusals ok\n";

    /* The binary's own reader: damaged payloads. */
    $whole = file_get_contents($exe);
    $size = strlen($whole);
    $damaged = [
        'flipped byte in the data' => substr_replace($whole, chr(ord($whole[$size - 200]) ^ 1), $size - 200, 1),
        'flipped byte in the record magic' => substr_replace($whole, chr(ord($whole[$size - 72]) ^ 1), $size - 72, 1),
        'truncated by one byte' => substr($whole, 0, $size - 1),
        'truncated inside the data' => substr($whole, 0, $size - 150),
    ];
    foreach ($damaged as $name => $bytes) {
        file_put_contents("$work/damaged", $bytes);
        chmod("$work/damaged", 0755);
        @unlink($errLog);
        [$status, $text] = runToEnd(["$work/damaged"], $env);
        check($status !== 0, "$name: started (status $status): $text");
        check(preg_match('/php-fpm-ng: cannot run the packed application|payload/i', $text) === 1, "$name: no message: $text");
        check(!str_contains($text . $logText(), 'ready to handle connections'), "$name: claimed to be ready");
        check(get($base, '/')[0] === 0, "$name: something is listening");
    }
    echo "damaged payload refusals ok\n";

    /* An unpacked binary does not know fpmng-app://. */
    file_put_contents("$work/plain.conf", appConf($work, $base));
    [$status, $text] = runToEnd([$binary, '-n', '-y', "$work/plain.conf", '-t'], ['PATH' => getenv('PATH')]);
    check($status !== 0 && str_contains($text, 'fpmng-app://'), "unpacked binary: $status $text");
    echo "unpacked ok\n";

    /* A stale FPMNG_PACK_INJECTED in the environment must not eat the operator's
     * arguments of any binary: only the exact injected -c/-y pair is stripped. */
    [$status, $text] = runToEnd([$binary, '-n', '-y', "$work/plain.conf", '-t', '-F'], ['PATH' => getenv('PATH'), 'FPMNG_PACK_INJECTED' => '4']);
    check($status !== 0 && str_contains($text, 'fpmng-app://'), "stale FPMNG_PACK_INJECTED: $status $text");
    echo "stale marker ok\n";
} finally {
    if (is_resource($proc)) {
        /* SIGTERM, not SIGKILL: the master takes its pool children down with it. */
        proc_terminate($proc, 15);
        waitFor(fn () => !proc_get_status($proc)['running'], 'the master to stop', 15);
        proc_close($proc);
    }
    rmtree($work);
}
?>
--EXPECT--
configuration test ok
app ok
supervisor ok
boundary ok
state dir ok
reopen ok
reload ok
upgrade ok
cache status before the upgrade:
file=phar://<work>/state/php-fpm-ng-app-<euid>/<digest>/app.phar/public/index.php
cached=0
enabled=1
after:
file=phar://<work>/state/php-fpm-ng-app-<euid>/<digest>/app.phar/public/index.php
cached=0
enabled=1
stop ok
config refusals ok
damaged payload refusals ok
unpacked ok
stale marker ok

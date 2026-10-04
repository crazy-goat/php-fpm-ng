--TEST--
fpm-ng: `php-fpm-ng serve` runs an app without a configuration file (default, --direct, --worker) and --print-config passes -t (issue #728)
--SKIPIF--
<?php
include "fpmng-skipif.inc";
if (PHP_OS_FAMILY !== 'Linux') die('skip requires Linux');
?>
--FILE--
<?php

require_once "tester.inc";

/* Issue #728. `serve` builds the configuration itself, so the tests start the
 * real binary with no -y and ask it three things:
 *   - default (gateway -> fastcgi on a private unix socket): the front
 *     controller, a second .php script that must run directly, a static file;
 *   - --direct (http-direct, classic): the front controller and the static file;
 *     a .php path goes to the front controller there (docs/http-direct.md);
 *   - --worker index.php (http-direct, worker executor): the worker answers;
 * and that Ctrl-C (SIGINT) ends it with status 0 and removes its temporary
 * directory, that -y is refused, and that the --print-config text passes -t. */

function check(bool $condition, string $message): void
{
    if (!$condition) {
        throw new RuntimeException($message);
    }
}
/* check() throws instead of exiting, so the finally blocks below still stop the server a failed run started. */
set_exception_handler(function (Throwable $e): void {
    echo 'FAIL: ' . $e->getMessage() . "\n";
});

$binary = FPM\Tester::findExecutable();
$base = (int) (getenv('FPMNG_SERVE_TEST_PORT') ?: 28180 + 200 * (int) getenv('TEST_PHP_WORKER') + (int) getenv('FPMNG_PHPT_PORT_SHIFT'));
$work = sys_get_temp_dir() . '/fpmng-728-serve';
$tmp = "$work/tmp";
$app = "$work/app";

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

rmtree($work);
mkdir($tmp, 0777, true);
mkdir("$app/public", 0777, true);
file_put_contents("$app/public/index.php", '<?php echo "index:", $_SERVER["REQUEST_URI"];');
file_put_contents("$app/public/second.php", '<?php echo "second";');
file_put_contents("$app/public/style.css", "body{color:red}\n");
file_put_contents("$app/public/worker.php", <<<'PHP'
<?php
$notify = fpmng_worker_notify_stream();
$watcher = fpmng_worker_event_create(FPMNG_WORKER_READ, $notify, function () use ($notify): void {
    fread($notify, 65536);
    while (($id = fpmng_worker_next_request()) !== null) {
        fpmng_worker_respond($id, 200, ['Content-Type' => 'text/plain'], 'worker');
    }
});
fpmng_worker_event_enable($watcher);
while (!fpmng_worker_may_exit()) {
    fpmng_worker_loop(true);
}
PHP);

function get(int $port, string $path): array
{
    $fp = @stream_socket_client("tcp://127.0.0.1:$port", $errno, $error, 2);
    if (!$fp) {
        return [0, '', ''];
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
    return [(int) ($m[1] ?? 0), $head, $body];
}

/** Starts `serve`; returns [process, pipes]. Output goes to files, so a full pipe never blocks the server. */
function serve(string $binary, array $args, string $cwd, string $tmp, string $tag)
{
    $spec = [0 => ['file', '/dev/null', 'r'], 1 => ['file', "$tmp/$tag.out", 'w'], 2 => ['file', "$tmp/$tag.err", 'w']];
    $proc = proc_open(array_merge([$binary, 'serve', '-n'], $args), $spec, $pipes, $cwd, ['TMPDIR' => $tmp, 'PATH' => getenv('PATH')]);
    check(is_resource($proc), "$tag: proc_open failed");
    return $proc;
}

function waitUp(int $port, $proc, string $tag): void
{
    $end = microtime(true) + 15;
    while (microtime(true) < $end) {
        check(proc_get_status($proc)['running'], "$tag: serve exited early");
        if (get($port, '/')[0] === 200) {
            return;
        }
        usleep(50000);
    }
    check(false, "$tag: nothing listening on $port");
}

function stop($proc, string $tmp, string $tag): void
{
    proc_terminate($proc, 2); /* SIGINT, what Ctrl-C sends */
    $end = microtime(true) + 15;
    while (proc_get_status($proc)['running'] && microtime(true) < $end) {
        usleep(50000);
    }
    check(!proc_get_status($proc)['running'], "$tag: still running 15 s after SIGINT");
    check(glob("$tmp/php-fpm-ng-serve-*") === [], "$tag: the temporary directory is still there: " . implode(' ', glob("$tmp/php-fpm-ng-serve-*")));
    proc_close($proc);
}

/* 1. default mode: gateway -> fastcgi */
$port = $base;
$proc = serve($binary, ['--root', "$app/public", '--listen', "127.0.0.1:$port", '--workers', '2'], $work, $tmp, 'default');
try {
    waitUp($port, $proc, 'default');
    [$s, $h, $b] = get($port, '/');
    check($s === 200 && $b === 'index:/', "default /: $s " . var_export($b, true));
    [$s, , $b] = get($port, '/second.php');
    check($s === 200 && $b === 'second', "default /second.php must run directly: $s " . var_export($b, true));
    [$s, $h, $b] = get($port, '/style.css');
    check($s === 200 && $b === "body{color:red}\n" && stripos($h, 'text/css') !== false, "default static file: $s " . var_export($h, true));
    [$s, , $b] = get($port, '/no/such/path?x=1');
    check($s === 200 && $b === 'index:/no/such/path?x=1', "default front controller fallback: $s " . var_export($b, true));
    $log = file_get_contents("$tmp/default.err");
    check(strpos($log, 'GET /second.php') !== false, 'default: no access log line on stderr: ' . var_export($log, true));
    /* the reload recipe of docs/guides/dev-server.md: the pid from the start message, SIGUSR2, still serving */
    check(preg_match('/master pid (\d+), pid file (\S+)/', $log, $m) === 1, 'default: no pid in the start message: ' . var_export($log, true));
    check(trim((string) @file_get_contents($m[2])) === $m[1], "default: pid file $m[2] does not hold {$m[1]}");
    $kill = proc_open(['kill', '-USR2', $m[1]], [], $pipes);
    check(proc_close($kill) === 0, 'default: kill -USR2 failed');
    usleep(500000);
    waitUp($port, $proc, 'default after reload');
    [$s, , $b] = get($port, '/second.php');
    check($s === 200 && $b === 'second', "default after SIGUSR2: $s " . var_export($b, true));
    echo "default: ok\n";
} finally {
    stop($proc, $tmp, 'default');
}
echo "default stopped: ok\n";

/* 2. --direct: http-direct, classic executor. The default root is public/ of the working directory. */
$port = $base + 1;
$proc = serve($binary, ['--direct', '--listen', (string) $port, '--workers', '1'], $app, $tmp, 'direct');
try {
    waitUp($port, $proc, 'direct');
    [$s, , $b] = get($port, '/');
    check($s === 200 && $b === 'index:/', "direct /: $s " . var_export($b, true));
    [$s, , $b] = get($port, '/second.php');
    check($s === 200 && $b === 'index:/second.php', "direct /second.php goes to the front controller: $s " . var_export($b, true));
    [$s, $h, $b] = get($port, '/style.css');
    check($s === 200 && $b === "body{color:red}\n" && stripos($h, 'text/css') !== false, "direct static file: $s " . var_export($h, true));
    echo "direct: ok\n";
} finally {
    stop($proc, $tmp, 'direct');
}

/* 3. --worker: implies --direct */
$port = $base + 2;
$proc = serve($binary, ['--worker', 'worker.php', '--root', "$app/public", '--listen', "127.0.0.1:$port", '--workers', '1'], $work, $tmp, 'worker');
try {
    waitUp($port, $proc, 'worker');
    [$s, , $b] = get($port, '/anything');
    check($s === 200 && $b === 'worker', "worker /anything: $s " . var_export($b, true));
    echo "worker: ok\n";
} finally {
    stop($proc, $tmp, 'worker');
}

/* 4. refusals */
function run(string $binary, array $args, ?array $env = null): array
{
    $proc = proc_open(array_merge([$binary], $args, ['-n']), [1 => ['pipe', 'w'], 2 => ['redirect', 1]], $pipes, null, $env);
    $out = stream_get_contents($pipes[1]);
    return [proc_close($proc), $out];
}
[$rc, $out] = run($binary, ['serve', '-y', "$tmp/x.conf"]);
check($rc === 64 && strpos($out, 'cannot be combined with a configuration file') !== false, "-y: $rc " . var_export($out, true));
[$rc, $out] = run($binary, ['serve', '--root', "$work/missing"]);
check($rc === 64 && strpos($out, 'not a directory') !== false, "missing root: $rc " . var_export($out, true));
[$rc, $out] = run($binary, ['serve', '--worker', 'nope.php', '--root', "$app/public"]);
check($rc === 64 && strpos($out, 'worker script is not a file') !== false, "missing worker: $rc " . var_export($out, true));
[$rc, $out] = run($binary, ['serve', '--bogus']);
check($rc === 64 && strpos($out, 'unknown option') !== false, "unknown option: $rc " . var_export($out, true));
echo "refusals: ok\n";

/* 5. --print-config passes -t, in every mode */
foreach ([[], ['--direct'], ['--worker', 'worker.php']] as $i => $mode) {
    [$rc, $text] = run($binary, array_merge(['serve', '--root', "$app/public", '--workers', '3', '--print-config'], $mode));
    check($rc === 0 && strpos($text, 'pool.type') !== false && strpos($text, 'max_children = 3') !== false, "print-config #$i: $rc " . var_export($text, true));
    file_put_contents("$tmp/printed$i.conf", $text);
    [$rc, $out] = run($binary, ['-t', '-y', "$tmp/printed$i.conf"]);
    check($rc === 0 && strpos($out, 'test is successful') !== false, "-t on print-config #$i: $rc " . var_export($out, true));
}
echo "print-config: ok\n";

/* 6. a root with spaces, parentheses and & is written quoted: serve it, and print it for -t */
$odd = "$work/app (old) & new";
mkdir("$odd/public", 0777, true);
file_put_contents("$odd/public/index.php", '<?php echo "odd";');
$port = $base + 3;
$proc = serve($binary, ['--root', "$odd/public", '--listen', "127.0.0.1:$port", '--workers', '1'], $work, $tmp, 'odd');
try {
    waitUp($port, $proc, 'odd');
    [$s, , $b] = get($port, '/');
    check($s === 200 && $b === 'odd', "odd root: $s " . var_export($b, true));
} finally {
    stop($proc, $tmp, 'odd');
}
foreach ([[], ['--direct'], ['--worker', 'index.php']] as $i => $mode) {
    [$rc, $text] = run($binary, array_merge(['serve', '--root', "$odd/public", '--print-config'], $mode));
    check($rc === 0 && strpos($text, "chdir = \"$odd/public\"") !== false, "odd print-config #$i: $rc " . var_export($text, true));
    file_put_contents("$tmp/odd$i.conf", $text);
    [$rc, $out] = run($binary, ['-t', '-y', "$tmp/odd$i.conf"]);
    check($rc === 0 && strpos($out, 'test is successful') !== false, "-t on odd print-config #$i: $rc " . var_export($out, true));
}
echo "odd root: ok\n";

/* 7. characters the file cannot carry are refused */
foreach (['a"b', 'a\\b', 'a${x}b', 'a$poolb'] as $name) {
    mkdir("$work/$name", 0777, true);
    [$rc, $out] = run($binary, ['serve', '--root', "$work/$name", '--print-config']);
    check($rc === 64 && strpos($out, 'cannot be written into a configuration file') !== false, "bad root $name: $rc " . var_export($out, true));
}
echo "refused characters: ok\n";

/* 8. a listen address that is taken: exit 1 with a message, no server without a listener */
$held = stream_socket_server('tcp://127.0.0.1:' . ($base + 4), $errno, $error);
check($held !== false, "cannot hold the port: $error");
[$rc, $out] = run($binary, ['serve', '--root', "$app/public", '--listen', (string) ($base + 4)], ['TMPDIR' => $tmp, 'PATH' => getenv('PATH')]);
check($rc === 1 && strpos($out, 'cannot listen on 127.0.0.1:' . ($base + 4)) !== false, "busy port: $rc " . var_export($out, true));
fclose($held);
check(glob("$tmp/php-fpm-ng-serve-*") === [], 'busy port left a temporary directory');
echo "busy port: ok\n";

/* 9. a relative TMPDIR is made absolute: the socket and pid paths must not depend on the master's prefix */
$port = $base + 5;
$spec = [0 => ['file', '/dev/null', 'r'], 1 => ['file', "$tmp/rel.out", 'w'], 2 => ['file', "$tmp/rel.err", 'w']];
$proc = proc_open([$binary, 'serve', '-n', '--root', "$app/public", '--listen', (string) $port, '--workers', '1'], $spec, $pipes, $work, ['TMPDIR' => 'tmp', 'PATH' => getenv('PATH')]);
check(is_resource($proc), 'rel: proc_open failed');
try {
    waitUp($port, $proc, 'rel');
    /* back to back on a one-worker pool: a second gateway process would hold a connection the only worker never serves (503) */
    for ($i = 0; $i < 8; $i++) {
        [$s, , $b] = get($port, '/second.php');
        check($s === 200 && $b === 'second', "relative TMPDIR, request $i: $s " . var_export($b, true));
    }
} finally {
    stop($proc, $tmp, 'rel');
}
echo "relative TMPDIR: ok\n";
?>
--CLEAN--
<?php
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
rmtree(sys_get_temp_dir() . '/fpmng-728-serve');
?>
--EXPECT--
default: ok
default stopped: ok
direct: ok
worker: ok
refusals: ok
print-config: ok
odd root: ok
refused characters: ok
busy port: ok
relative TMPDIR: ok

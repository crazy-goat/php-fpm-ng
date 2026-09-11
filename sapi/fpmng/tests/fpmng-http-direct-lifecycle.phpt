--TEST--
fpm-ng: direct HTTP retains FPM recycling, supervision, concurrency and graceful reload
--SKIPIF--
<?php
include "skipif.inc";
if (PHP_OS_FAMILY !== 'Linux') die('skip requires Linux /proc');
?>
--FILE--
<?php
require_once "tester.inc";
require_once "fpmng-tester.inc";
function verify(bool $ok, string $message): void { if (!$ok) throw new RuntimeException($message); }
function waitFor(callable $condition, string $label): void
{
    $end = microtime(true) + 8;
    do {
        if ($condition()) return;
        usleep(10000);
    } while (microtime(true) < $end);
    throw new RuntimeException("timed out: $label");
}
function startRequest(string $addr, string $query = '')
{
    $fp = stream_socket_client("tcp://$addr", $errno, $error, 5);
    stream_set_timeout($fp, 8);
    fwrite($fp, "GET /?$query HTTP/1.1\r\nHost: test\r\nConnection: close\r\n\r\n");
    return $fp;
}
function finishRequest($fp): string
{
    $raw = stream_get_contents($fp);
    fclose($fp);
    return explode("\r\n\r\n", $raw, 2)[1] ?? '';
}
function getPidHttp(string $addr): int { return (int) finishRequest(startRequest($addr)); }
function children(int $master): array
{
    $text = trim(file_get_contents("/proc/$master/task/$master/children"));
    return $text === '' ? [] : array_map('intval', preg_split('/\s+/', $text));
}
$root = __DIR__;
$script = '/fpmng-http-direct-lifecycle-front-' . getmypid() . '.php';
$marker = $root . '/fpmng-http-direct-lifecycle-marker-' . getmypid();
file_put_contents($root . $script, '<?php $marker = ' . var_export($marker, true) . ';' . <<<'PHP'
if (isset($_GET['hold'])) {
    file_put_contents($marker, (string) getmypid());
    usleep(400000);
}
if (isset($_GET['timeout'])) {
    file_put_contents($marker, (string) getmypid());
    sleep(10);
}
if (isset($_GET['barrier'])) {
    file_put_contents($marker . $_GET['barrier'], (string) getmypid());
    $end = microtime(true) + 4;
    while ((!is_file($marker . 'a') || !is_file($marker . 'b')) && microtime(true) < $end) usleep(1000);
    if (!is_file($marker . 'a') || !is_file($marker . 'b')) { http_response_code(500); echo 'barrier failed'; return; }
}
echo getmypid();
PHP);
$port = (int) (getenv('FPMNG_DIRECT_TEST_PORT') ?: 28054) + 1;
$addr = "127.0.0.1:$port";
$cfg = <<<CFG
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
process_control_timeout = 3
[direct]
listen = $addr
pool.type = http-direct
pm = static
pm.max_children = 1
pm.max_requests = 2
chdir = $root
http.front_controller = $script
request_terminate_timeout = 2
CFG;
$tester = new FPM\Tester($cfg, '<?php');
$closed = false;
try {
    $tester->start();
    fpmng_expect_log_start_notices($tester);
    $pid1 = getPidHttp($addr);
    verify($pid1 > 0 && getPidHttp($addr) === $pid1, 'first two requests not in same worker');
    $pid2 = getPidHttp($addr);
    verify($pid2 > 0 && $pid2 !== $pid1, 'pm.max_requests did not replace worker');
    waitFor(fn() => children($tester->getPid()) === [$pid2], 'one child, no gateway');
    $tester->signal('KILL', $pid2);
    $pid3 = getPidHttp($addr);
    verify($pid3 > 0 && $pid3 !== $pid2, 'crash replacement');
    echo "recycle/crash/no-gateway: ok\n";

    $cfg = str_replace('pm.max_requests = 2', 'pm.max_requests = 0', $cfg);
    $tester->reload($cfg);
    waitFor(fn() => !in_array($pid3, children($tester->getPid()), true), 'reload replacement');
    $fp = startRequest($addr, 'hold=1');
    waitFor(fn() => is_file($marker), 'active request before reload');
    $active = (int) file_get_contents($marker);
    $tester->reload();
    verify((int) finishRequest($fp) === $active, 'reload truncated active response');
    waitFor(fn() => !in_array($active, children($tester->getPid()), true), 'old worker drained');
    verify(getPidHttp($addr) !== $active, 'reload did not replace worker');
    unlink($marker);
    echo "graceful-reload: ok\n";

    $fp = startRequest($addr, 'timeout=1');
    waitFor(fn() => is_file($marker), 'timeout request started');
    $timedOut = (int) file_get_contents($marker);
    verify(finishRequest($fp) === '', 'master timeout did not interrupt PHP');
    verify(getPidHttp($addr) !== $timedOut, 'timeout worker not replaced');
    unlink($marker);
    echo "master-timeout: ok\n";

    $old = children($tester->getPid());
    $tester->reload(str_replace('pm.max_children = 1', 'pm.max_children = 2', $cfg));
    waitFor(fn() => count(children($tester->getPid())) === 2 && !array_intersect($old, children($tester->getPid())), 'two workers');
    $a = startRequest($addr, 'barrier=a');
    // The shared listener can batch accepts into one event loop. Establish that
    // one worker is executing PHP before testing the other worker's progress.
    waitFor(fn() => is_file($marker . 'a'), 'first concurrent request started');
    $b = startRequest($addr, 'barrier=b');
    $pa = (int) finishRequest($a);
    $pb = (int) finishRequest($b);
    verify($pa > 0 && $pb > 0 && $pa !== $pb, 'requests not executed concurrently in separate children');
    echo "two-child-concurrency: ok\n";

    $fp = startRequest($addr, 'hold=1');
    waitFor(fn() => is_file($marker), 'active request before stop');
    $active = (int) file_get_contents($marker);
    $tester->signal('QUIT');
    verify((int) finishRequest($fp) === $active, 'graceful stop truncated request');
    $tester->close();
    $closed = true;
    echo "graceful-stop: ok\n";
} finally {
    if (!$closed) {
        $tester->terminate();
        $tester->close();
    }
    foreach ([$root . $script, $marker, $marker . 'a', $marker . 'b'] as $path) @unlink($path);
}
echo "Done\n";
?>
--EXPECT--
recycle/crash/no-gateway: ok
graceful-reload: ok
master-timeout: ok
two-child-concurrency: ok
graceful-stop: ok
Done
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>

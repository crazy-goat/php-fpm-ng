--TEST--
fpm-ng: a continuously reused keep-alive connection survives a SIGUSR1 retire (issue #311)
--SKIPIF--
<?php
include "skipif.inc";
if (PHP_OS_FAMILY !== 'Linux') die('skip requires Linux /proc');
?>
--FILE--
<?php
require_once "tester.inc";
require_once "fpmng-operator.inc";

/* Issue #311. fpmng-http-direct-retire.phpt (issue #65) already covers a
 * request that is in flight the instant SIGUSR1 arrives, and one connection
 * held open with nothing in flight on it -- both single exchanges. What it
 * does not cover is the shape build/benchmark-http-direct-pm.py's retire
 * workload found at --idle 0: a client that never pauses between an answer
 * and its next request on the same keep-alive connection, all the way
 * through the retire. That is exactly the client the old code answered with
 * Connection: close on every response while merely fpm_direct_retiring was
 * true (see the comment on fpm_direct_last_request() in fpm_http_direct.c):
 * evhttp's need_close teardown (evhttp_send_done(), http.c) frees the
 * connection the instant that response is written, with no chance to notice
 * a next request already on the wire, and the client's own send() for it had
 * already succeeded -- so it read a reset instead of a reply. Measured on the
 * test box before the fix: every response given while retiring cost that
 * connection its next request, not a rare race.
 *
 * This test drives one connection with no gaps for longer than
 * http.read_timeout, signals a retire partway through, and requires every
 * single exchange to complete -- not most of them. */
function verify(bool $ok, string $message): void
{
    if (!$ok) {
        throw new RuntimeException($message);
    }
}

function connect(int $port)
{
    for ($i = 0; $i < 50; $i++) {
        $fp = @stream_socket_client("tcp://127.0.0.1:$port", $errno, $error, 5);
        if ($fp) {
            stream_set_timeout($fp, 10);
            return $fp;
        }
        usleep(100000);
    }
    throw new RuntimeException("connect $port: $error");
}

/* Throws rather than returning a soft failure: a dropped exchange here is
 * exactly the closed_without_response the benchmark counts, and one is one
 * too many for this test to shrug off as a sample. */
function fetch($fp, string $path): array
{
    fwrite($fp, "GET $path HTTP/1.1\r\nHost: test\r\n\r\n");
    $line = fgets($fp);
    if ($line === false || $line === '') {
        throw new RuntimeException('connection closed without a response');
    }
    if (!preg_match('#^HTTP/1\.1 (\d+) #', $line, $m)) {
        throw new RuntimeException('bad status line: ' . var_export($line, true));
    }
    $status = (int) $m[1];
    $length = 0;
    $close = false;
    while (($line = fgets($fp)) !== false && $line !== "\r\n") {
        if (stripos($line, 'Content-Length:') === 0) {
            $length = (int) trim(substr($line, 15));
        }
        if (stripos($line, 'Connection:') === 0 && stripos($line, 'close') !== false) {
            $close = true;
        }
    }
    $body = '';
    while (strlen($body) < $length) {
        $chunk = fread($fp, $length - strlen($body));
        if ($chunk === false || $chunk === '') {
            throw new RuntimeException('connection closed mid-body (closed_without_response)');
        }
        $body .= $chunk;
    }
    return [$status, $body, $close];
}

function workers(string $ops, string $path): array
{
    $body = fpmng_operator_body($ops, $path . '?json&full');
    $decoded = json_decode($body, true);
    verify(is_array($decoded), "$path?json&full is not JSON: $body");
    $out = [];
    foreach ($decoded['workers'] as $row) {
        if ($row['live']) {
            $out[$row['pid']] = $row;
        }
    }
    return $out;
}

function until(callable $done, float $seconds, string $what)
{
    $deadline = microtime(true) + $seconds;
    do {
        $value = $done();
        if ($value !== null) {
            return $value;
        }
        usleep(20000);
    } while (microtime(true) < $deadline);
    throw new RuntimeException("timed out waiting for $what");
}

$root = sys_get_temp_dir() . '/fpmng-retire-continuous-' . getmypid();
@mkdir($root);
file_put_contents($root . '/front.php', '<?php echo getmypid();');

$base = (int) (getenv('FPMNG_DIRECT_TEST_PORT') ?: 28054);
$port = $base + 61;
$ops = '127.0.0.1:' . ($base + 62);
$cfg = <<<CFG
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
[direct]
listen = 127.0.0.1:$port
pool.type = http-direct
pm = static
pm.max_children = 2
chdir = $root
http.front_controller = /front.php
pm.status_path = /status
pm.status_listen = $ops
; Short enough to keep the test quick, long enough that a handful of
; back-to-back local exchanges land comfortably inside one grant of it.
http.read_timeout = 700
CFG;

$tester = new FPM\Tester($cfg, '<?php');
try {
    $tester->start();
    $tester->expectLogStartNotices();

    until(function () use ($ops) {
        $w = workers($ops, '/status');
        return count($w) === 2 ? $w : null;
    }, 15, 'both children to appear on the status page');
    echo "pool started: ok\n";

    $conn = connect($port);
    [$status, $pid] = fetch($conn, '/');
    verify($status === 200, "warm-up request: $status");
    $victim = (int) $pid;
    echo "connection pinned to child $victim: ok\n";

    /* Back to back, no sleep between an answer and the next request -- the
     * shape --idle 0 puts every stream connection in during the retire
     * workload. Retire partway through so exchanges land both before and
     * after fpm_direct_retiring becomes true, and keep going well past
     * http.read_timeout so the fix has to actually keep extending the
     * deadline on every completed response rather than merely surviving one
     * lucky window. */
    $exchanges = 0;
    $signalled = false;
    $end = microtime(true) + 2.5;
    while (microtime(true) < $end) {
        [$status, $body] = fetch($conn, '/');
        verify($status === 200, "exchange $exchanges: status $status");
        verify((int) $body === $victim, "exchange $exchanges answered by " . var_export($body, true) .
            ", not the pinned child $victim");
        $exchanges++;
        if (!$signalled && $exchanges === 5) {
            $tester->signal('USR1', $victim);
            $signalled = true;
        }
    }
    verify($signalled, 'the loop ended before the retire signal was sent');
    verify($exchanges > 20, "only $exchanges exchanges completed in 2.5s -- suspiciously few");
    fclose($conn);
    echo "no exchange was dropped: ok ($exchanges exchanges)\n";

    /* The child still leaves once the client stops giving it a reason to
     * stay: this is not a drain that got stuck open by the fix. */
    until(function () use ($ops, $victim) {
        $w = workers($ops, '/status');
        return count($w) === 2 && !isset($w[$victim]) ? $w : null;
    }, 15, "child $victim to exit and be replaced");
    echo "retired child exited and was replaced: ok\n";

    echo "Done\n";
} finally {
    $tester->terminate();
    $tester->close();
    @unlink($root . '/front.php');
    @rmdir($root);
}
?>
--EXPECTF--
pool started: ok
connection pinned to child %d: ok
no exchange was dropped: ok (%d exchanges)
retired child exited and was replaced: ok
Done
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>

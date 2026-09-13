--TEST--
fpm-ng: pm.max_requests drains a direct HTTP child instead of severing its other held connections (issue #313)
--SKIPIF--
<?php
include "skipif.inc";
if (PHP_OS_FAMILY !== 'Linux') die('skip requires Linux /proc');
?>
--FILE--
<?php
require_once "tester.inc";
require_once "fpmng-operator.inc";

/* Issue #313. fpm_direct_retire() used to answer pm.max_requests the same way
 * an external SIGQUIT does -- fpm_direct_stopping -- which fpm_direct_tick_body()
 * checks first and, once pending responses hit zero, tears down every
 * connection the child holds (fpm_http_direct_conns_free()/evhttp_free()) with
 * no regard for whether they were idle-but-open rather than answered. A second,
 * keep-alive connection accepted onto the same child and never sent a request
 * on yet would be severed along with it. The fix routes pm.max_requests through
 * the same fpm_direct_retiring drain fpm_direct_retire_now() already gives a
 * SIGUSR1 retirement (issue #65): stop accepting new connections, but keep
 * answering the ones already open until they finish or http.read_timeout runs
 * out. This test holds a connection open with no request in flight on it,
 * trips pm.max_requests on a sibling connection, and checks the held one is
 * still answerable afterwards rather than reset. */
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

function fetch($fp, string $path): array
{
    fwrite($fp, "GET $path HTTP/1.1\r\nHost: test\r\n\r\n");
    $line = fgets($fp);
    if (!$line || !preg_match('#^HTTP/1\.1 (\d+) #', $line, $m)) {
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
            throw new RuntimeException('short body');
        }
        $body .= $chunk;
    }
    return [$status, $body, $close];
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

$root = sys_get_temp_dir() . '/fpmng-max-requests-drain-' . getmypid();
@mkdir($root);
file_put_contents($root . '/front.php', '<?php echo getmypid();');

$base = (int) (getenv('FPMNG_DIRECT_TEST_PORT') ?: 28054);
$port = $base + 41;
$ops = '127.0.0.1:' . ($base + 42);
$cfg = <<<CFG
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
[direct]
listen = 127.0.0.1:$port
pool.type = http-direct
pm = static
pm.max_children = 1
pm.max_requests = 1
chdir = $root
http.front_controller = /front.php
pm.status_path = /status
pm.status_listen = $ops
; The bound on how long the held-but-idle connection below has to still be
; answered once this child starts draining.
http.read_timeout = 3000
CFG;

$tester = new FPM\Tester($cfg, '<?php');
try {
    $tester->start();
    $tester->expectLogStartNotices();

    /* Accepted first, kept idle: no request sent on it while the sibling
     * below trips pm.max_requests. */
    $held = connect($port);

    /* This is the request that reaches pm.max_requests = 1 and, before the
     * fix, would have collapsed straight to fpm_direct_stopping and torn
     * $held down with it. */
    $fp = connect($port);
    [$status, $pid, $close] = fetch($fp, '/');
    fclose($fp);
    verify($status === 200, "triggering request: $status");
    verify((int) $pid > 1, "triggering request answered with no pid: $pid");
    verify($close, 'the request that reached pm.max_requests did not say Connection: close');
    echo "pm.max_requests reached: ok\n";

    /* The status page must show the child retiring, not gone: a child that
     * hit fpm_direct_stopping instead would already have exited by now. */
    until(function () use ($ops, $pid) {
        $body = fpmng_operator_body($ops, '/status?json&full');
        $decoded = json_decode($body, true);
        verify(is_array($decoded), "status is not JSON: $body");
        foreach ($decoded['workers'] ?? [] as $row) {
            if (($row['pid'] ?? null) == $pid && ($row['retiring'] ?? 0) === 1) {
                return true;
            }
        }
        return null;
    }, 2, 'the child to show as retiring rather than already gone');
    echo "retiring, not stopped: ok\n";

    /* The connection that was only ever held open, never used, is still
     * answerable: the drain kept it, rather than evhttp_free() severing it
     * the instant pending responses hit zero. */
    [$status2, $body2, $close2] = fetch($held, '/');
    fclose($held);
    verify($status2 === 200, "held connection after max_requests: $status2");
    verify((int) $body2 === (int) $pid, "held connection answered by a different child: $body2 vs $pid");
    verify($close2, 'the retiring child answered the held connection without Connection: close');
    echo "held connection drained, not dropped: ok\n";

    /* And the master still puts a replacement in the slot once the drain
     * ends. */
    until(function () use ($ops, $pid) {
        $body = fpmng_operator_body($ops, '/status?json&full');
        $decoded = json_decode($body, true);
        verify(is_array($decoded), "status is not JSON: $body");
        foreach ($decoded['workers'] ?? [] as $row) {
            if (($row['pid'] ?? null) == $pid) {
                return null;
            }
        }
        return count($decoded['workers'] ?? []) === 1 ? true : null;
    }, 15, 'the retired child to exit and be replaced');
    echo "replaced: ok\n";

    echo "Done\n";
} finally {
    $tester->terminate();
    $tester->close();
    @unlink($root . '/front.php');
    @rmdir($root);
}
?>
--EXPECT--
pm.max_requests reached: ok
retiring, not stopped: ok
held connection drained, not dropped: ok
replaced: ok
Done
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>

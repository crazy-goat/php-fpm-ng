--TEST--
fpm-ng: direct HTTP bounds the first request in time and the connections per worker (issue #61)
--SKIPIF--
<?php include "skipif.inc"; ?>
--FILE--
<?php
require_once "tester.inc";

/* Issue #61. Three policies, one pool each, because each of them is only
 * observable when the others are off:
 *
 * - [deadline] http.read_timeout as an absolute budget for the first request.
 *   evhttp_set_timeout_tv() alone is an idle timeout that every arriving byte
 *   resets, so before this issue a client trickling one byte at a time held a
 *   direct connection for as long as it liked. Measured on the poligon
 *   2026-09-11 against http.read_timeout = 3000: one byte every 2 s held the
 *   connection 56 s and was then served normally.
 * - [total] http.max_connections, with one child, so "this worker is full"
 *   and "the pool is full" are the same statement.
 * - [client] http.max_connections_per_client, likewise with one child: the
 *   limit is per worker, and a second child would double it.
 *
 * The pools are separate for a second reason too: the deadline pool needs a
 * short http.read_timeout to be testable, and the two limit pools need a long
 * one, or the idle connections they are asked to hold would be closed by the
 * idle timeout before the limit was ever reached.
 */

$root = sys_get_temp_dir() . '/fpmng-conn-limits-' . getmypid();
@mkdir($root);
file_put_contents($root . '/front.php', "<?php echo 'ok';");

$base = (int) (getenv('FPMNG_DIRECT_TEST_PORT') ?: 28054);
$deadlinePort = $base + 18;
$totalPort = $base + 19;
$clientPort = $base + 20;

/* Long enough that a loaded CI machine does not trip it by scheduling delay
 * alone, short enough that the test does not wait a visible amount of time. */
$readTimeoutMs = 1000;

$cfg = <<<CFG
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
[deadline]
listen = 127.0.0.1:$deadlinePort
pool.type = http-direct
pm = static
pm.max_children = 2
chdir = $root
http.front_controller = /front.php
http.read_timeout = $readTimeoutMs
[total]
listen = 127.0.0.1:$totalPort
pool.type = http-direct
pm = static
pm.max_children = 1
chdir = $root
http.front_controller = /front.php
http.read_timeout = 30000
http.max_connections = 2
[client]
listen = 127.0.0.1:$clientPort
pool.type = http-direct
pm = static
pm.max_children = 1
chdir = $root
http.front_controller = /front.php
http.read_timeout = 30000
; The total cap is required alongside the per-client one, and is set well above
; it here so that what this pool refuses is only ever the per-client rule.
http.max_connections = 16
http.max_connections_per_client = 2
CFG;

function expect(string $what, $actual, $expected): void
{
    if ($actual !== $expected) {
        throw new RuntimeException("$what: expected " . var_export($expected, true) .
            ', got ' . var_export($actual, true));
    }
}

function connect(int $port)
{
    $fp = stream_socket_client("tcp://127.0.0.1:$port", $errno, $error, 5);
    if (!$fp) {
        throw new RuntimeException("connect to $port: $error ($errno)");
    }
    stream_set_timeout($fp, 5);
    return $fp;
}

/* Status line only: every response in this test is either a whole small reply
 * or nothing at all. Returns '' when the peer closed without one. */
function statusLine($fp, float $seconds): string
{
    $read = [$fp];
    $write = $except = null;
    if (stream_select($read, $write, $except, (int) $seconds, (int) (($seconds - (int) $seconds) * 1e6)) < 1) {
        return 'TIMEOUT';
    }
    $line = fgets($fp);
    return $line === false ? '' : rtrim($line, "\r\n");
}

/* Writes one byte at a time until the peer goes away, and reports how long
 * that took. The pause is deliberately shorter than http.read_timeout: the
 * point of the deadline is exactly that resetting the idle timeout forever no
 * longer keeps the connection. */
function dripUntilDropped($fp, string $bytes, float $pause): float
{
    $started = microtime(true);
    foreach (str_split($bytes) as $byte) {
        /* A dropped connection is not always visible on the first write after
         * it: the first one leaves the local buffer and only draws the RST. */
        if (@fwrite($fp, $byte) !== 1 || feof($fp)) {
            return microtime(true) - $started;
        }
        usleep((int) ($pause * 1e6));
    }
    return -1.0;
}

$tester = new FPM\Tester($cfg, '<?php');
$closed = false;
try {
    $tester->start();
    $tester->expectLogStartNotices();

    /* 1. A trickled request line. Bounded by the deadline, not by the pause. */
    $fp = connect($deadlinePort);
    $held = dripUntilDropped($fp, "GET /app HTTP/1.1\r\nHost: t\r\n\r\n", $readTimeoutMs / 2000);
    fclose($fp);
    if ($held < 0 || $held > 4 * $readTimeoutMs / 1000) {
        throw new RuntimeException("slow header: held for {$held}s");
    }
    echo "slow header dropped: ok\n";

    /* 2. A complete header and a trickled body. The deadline covers the whole
     * of the first request, which is what fpm_conf.h has always said
     * http.read_timeout means. */
    $fp = connect($deadlinePort);
    fwrite($fp, "POST /app HTTP/1.1\r\nHost: t\r\nContent-Length: 20\r\n\r\n");
    $held = dripUntilDropped($fp, str_repeat('x', 20), $readTimeoutMs / 2000);
    fclose($fp);
    if ($held < 0 || $held > 4 * $readTimeoutMs / 1000) {
        throw new RuntimeException("slow body: held for {$held}s");
    }
    echo "slow body dropped: ok\n";

    /* 3. Ordinary keep-alive is untouched: the deadline belongs to the first
     * request, and a connection that answers one keeps serving. */
    $fp = connect($deadlinePort);
    for ($i = 0; $i < 3; $i++) {
        fwrite($fp, "GET /app HTTP/1.1\r\nHost: t\r\n\r\n");
        expect("keep-alive request $i", statusLine($fp, 5), 'HTTP/1.1 200 OK');
        while (($line = fgets($fp)) !== false && $line !== "\r\n") {
        }
        fread($fp, 2);
    }
    fclose($fp);
    echo "keep-alive unaffected: ok\n";

    /* 4. http.max_connections. The third connection is neither answered nor
     * refused: it stays in the accept queue, which is the documented refusal
     * for a limit whose listening socket is shared with sibling children. */
    $held = [];
    for ($i = 0; $i < 2; $i++) {
        $held[$i] = connect($totalPort);
        fwrite($held[$i], "GET /app HTTP/1.1\r\nHost: t\r\n\r\n");
        expect("within limit $i", statusLine($held[$i], 5), 'HTTP/1.1 200 OK');
    }
    $third = connect($totalPort);
    fwrite($third, "GET /app HTTP/1.1\r\nHost: t\r\n\r\n");
    expect('over the limit', statusLine($third, 1), 'TIMEOUT');
    echo "total limit reached: ok\n";

    /* And it is a limit, not a wall: the slot the first connection releases is
     * the slot the queued one gets, without a reconnect. */
    fclose($held[0]);
    expect('after a slot freed', statusLine($third, 5), 'HTTP/1.1 200 OK');
    echo "total limit releases: ok\n";

    /* 5. http.max_connections_per_client. Every connection in this test comes
     * from 127.0.0.1, so the cap applies to all of them. Refused at the
     * earliest point the peer is knowable, which is after the accept: the
     * client sees a closed connection rather than a response. */
    $perClient = [];
    for ($i = 0; $i < 2; $i++) {
        $perClient[$i] = connect($clientPort);
        fwrite($perClient[$i], "GET /app HTTP/1.1\r\nHost: t\r\n\r\n");
        expect("within client cap $i", statusLine($perClient[$i], 5), 'HTTP/1.1 200 OK');
    }
    $extra = connect($clientPort);
    fwrite($extra, "GET /app HTTP/1.1\r\nHost: t\r\n\r\n");
    /* Two accepted shapes, and which one a run gets is libevent's scheduling.
     * The connection is judged either by the pickup pass -- a zero-delay timer
     * armed when it was accepted, which closes it without a word -- or, if the
     * request callback got there first, by a 503 with Connection: close. The
     * pool cannot make that ordering deterministic (see fpm_direct_handle),
     * and both outcomes are the same refusal: the connection is closed and no
     * PHP ran. Asserting one of them would be asserting the scheduling. */
    $refusal = statusLine($extra, 5);
    if ($refusal !== '' && !str_starts_with($refusal, 'HTTP/1.1 503 ')) {
        throw new RuntimeException('over the client cap: got ' . var_export($refusal, true));
    }
    echo "per-client cap enforced: ok\n";

    /* 6. Draining must not wait for connections the policy is holding or
     * refusing. At this point the [total] pool has one served connection open
     * and the [client] pool two, and both pools have a queued connection the
     * limits will not let them take. */
    $tester->signal('QUIT');
    /* proc_close() blocks until the master is gone, which is the assertion:
     * a drain that waited for a connection the policy is holding or refusing
     * would never return and the test would time out. The same idiom as the
     * graceful-stop case of fpmng-http-direct-lifecycle.phpt -- kill -0 is not
     * usable here, because the master is this process's own child and stays a
     * reapable zombie, alive as far as a signal is concerned, until
     * proc_close() collects it. */
    $tester->close();
    $closed = true;
    echo "graceful drain: ok\n";

    echo "Done\n";
} finally {
    if (!$closed) {
        $tester->terminate();
        $tester->close();
    }
    @unlink($root . '/front.php');
    @rmdir($root);
}
?>
--EXPECT--
slow header dropped: ok
slow body dropped: ok
keep-alive unaffected: ok
total limit reached: ok
total limit releases: ok
per-client cap enforced: ok
graceful drain: ok
Done
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>

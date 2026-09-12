--TEST--
fpm-ng: SIGUSR1 retires one direct HTTP child without dropping a request (issue #65)
--SKIPIF--
<?php
include "skipif.inc";
if (PHP_OS_FAMILY !== 'Linux') die('skip requires Linux /proc');
?>
--FILE--
<?php
require_once "tester.inc";
require_once "fpmng-operator.inc";

/* Issue #65. Retiring is one child stepping out of a pool that keeps running,
 * which is why it is not SIGQUIT: SIGQUIT is the pool winding down and answers
 * 503 to whatever arrives, while a retiring child keeps serving the
 * connections it already holds and only stops taking new ones. What this test
 * has to show is that the difference is real -- that a request in flight is
 * finished rather than dropped, that the client is told the connection ends
 * (Connection: close, so a keep-alive client moves to a sibling of its own
 * accord), and that the master puts a replacement back. */
$root = sys_get_temp_dir() . '/fpmng-retire-' . getmypid();
@mkdir($root);
file_put_contents($root . '/front.php', <<<'PHP'
<?php
if (isset($_GET['sleep'])) {
    usleep((int) $_GET['sleep'] * 1000);
}
echo getmypid();
PHP);
$base = (int) (getenv('FPMNG_DIRECT_TEST_PORT') ?: 28054);
$port = $base + 31;
$solo = $base + 32;
/* The status pages of both pools, on one operator listener (issue #275): the
 * page is no longer answered by the child being retired, which is what makes
 * "the retiring child is still on the page" an observation about the pool
 * rather than about whether that one child still has an event loop. */
$ops = '127.0.0.1:' . ($base + 33);
$cfg = <<<CFG
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
[rolling]
listen = 127.0.0.1:$port
pool.type = http-direct
pm = static
pm.max_children = 2
chdir = $root
http.front_controller = /front.php
pm.status_path = /status
pm.status_listen = $ops
[solo]
listen = 127.0.0.1:$solo
pool.type = http-direct
pm = static
pm.max_children = 1
chdir = $root
http.front_controller = /front.php
pm.status_path = /solo-status
pm.status_listen = $ops
; Also the bound on how long a retiring child waits for the connections it
; holds, which is what this pool exercises: the reader keeps one open.
http.read_timeout = 1000
CFG;

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

/* One framed message, so a keep-alive connection stays usable. Returns the
 * status, the body and whether the server said it is closing -- that header is
 * half of what makes a retire load-balancer-friendly, so the test reads it. */
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

/* One request on a connection of its own, which is what a client behind a load
 * balancer does and what the rolling phase below has to keep succeeding. */
function once(int $port, string $path): array
{
    $fp = connect($port);
    try {
        return fetch($fp, $path);
    } finally {
        fclose($fp);
    }
}

/* The per-child rows of ?full, keyed by pid. Only live slots: a slot whose
 * child has gone prints its totals but no pid. */
function workers(string $path): array
{
    global $ops;

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

$tester = new FPM\Tester($cfg, '<?php');
try {
    $tester->start();
    $tester->expectLogStartNotices();

    /* 1. The pid is on the status row, because it is the address of the
     * signal: without it an operator reading a slot would have to go looking
     * in ps and guess which child it just read about. */
    $rows = until(function () {
        $w = workers('/status');
        return count($w) === 2 ? $w : null;
    }, 15, 'both children to appear on the status page');
    foreach ($rows as $pid => $row) {
        verify($pid > 1, "row has no pid: " . json_encode($row));
        verify($row['retiring'] === 0, "a fresh child is retiring: " . json_encode($row));
    }
    echo "pids on the page: ok\n";

    /* 2. A request in flight is finished, not dropped. The slow request pins
     * one child inside PHP -- the classic executor runs the script in evhttp's
     * request callback, so that child's event loop is not running -- and the
     * retire signal is delivered while it is there. If retiring were the 503
     * gate that stopping is, or if it exited on the first tick after the
     * signal, this response would be truncated or refused. */
    $slow = connect($port);
    fwrite($slow, "GET /?sleep=700 HTTP/1.1\r\nHost: test\r\n\r\n");
    $busy = until(function () {
        foreach (workers('/status') as $pid => $row) {
            if ($row['active requests'] > 0) {
                return $pid;
            }
        }
        return null;
    }, 15, 'the slow request to show as active on a child');
    $tester->signal('USR1', $busy);
    $line = fgets($slow);
    verify(substr($line, 0, 12) === 'HTTP/1.1 200', "slow request: " . var_export($line, true));
    $length = 0;
    $close = false;
    while (($line = fgets($slow)) !== false && $line !== "\r\n") {
        if (stripos($line, 'Content-Length:') === 0) {
            $length = (int) trim(substr($line, 15));
        }
        if (stripos($line, 'Connection:') === 0 && stripos($line, 'close') !== false) {
            $close = true;
        }
    }
    $body = '';
    while (strlen($body) < $length) {
        $chunk = fread($slow, $length - strlen($body));
        if ($chunk === false || $chunk === '') {
            break;
        }
        $body .= $chunk;
    }
    fclose($slow);
    verify((int) $body === $busy, "the slow request was answered by $body, not by the retiring $busy");
    verify($close, 'a retiring child answered without Connection: close');
    echo "in-flight request finished: ok\n";

    /* 3. And the master puts a replacement in that slot. Nothing new had to be
     * written for this: a child that exits on its own is replaced unless the
     * master marked it for idle_kill, so retiring deliberately does not touch
     * that flag. */
    until(function () use ($busy) {
        $w = workers('/status');
        return count($w) === 2 && !isset($w[$busy]) ? $w : null;
    }, 15, "child $busy to exit and be replaced");
    echo "replaced: ok\n";

    /* 4. Retiring all of them, one at a time, with a client that never stops:
     * a rolling restart of the pool with no failed request. One at a time is
     * the contract -- the siblings are what keeps the pool answering while one
     * child drains, so the test waits for the replacement before signalling
     * the next. Each request opens its own connection, which is what a client
     * behind a load balancer does. */
    $served = 0;
    for ($round = 0; $round < 4; $round++) {
        $before = workers('/status');
        $victim = array_key_first($before);
        $tester->signal('USR1', $victim);
        /* Every answer during the drain, not a sample of them: one 503 here
         * would be a deploy dropping a customer's request. */
        until(function () use ($port, $victim, &$served) {
            [$status, $body] = once($port, '/');
            verify($status === 200, "a request was answered $status during a rolling retire");
            verify((int) $body > 1, "a request came back with no pid: $body");
            $served++;
            $w = workers('/status');
            return count($w) === 2 && !isset($w[$victim]) ? $w : null;
        }, 15, "child $victim to be retired and replaced");
    }
    verify($served >= 4, "the rolling phase served only $served requests");
    echo "rolling restart: ok\n";

    /* 5. The flag is on the page while it is true. Needs a child that cannot
     * leave at once, so this is the single-child pool with the reader holding
     * a connection open: the child stays until that connection ends or until
     * http.read_timeout is up. A second signal changes nothing -- a deploy
     * script that retries is not one that shortens the drain it is waiting
     * for. */
    $reader = connect($solo);
    [$status, $pid] = fetch($reader, '/');
    verify($status === 200, "solo pool: $status");
    $tester->signal('USR1', (int) $pid);
    $tester->signal('USR1', (int) $pid);
    /* A request the retiring child does answer, on the connection it is
     * draining: the answer arrives and it says the connection ends, which is
     * how a keep-alive client is moved to a sibling of its own accord. */
    [$status, , $close] = fetch($reader, '/');
    verify($status === 200, "solo request during retire: $status");
    verify($close, 'a retiring child answered without Connection: close');
    $body = fpmng_operator_body($ops, '/solo-status?json&full');
    $decoded = json_decode($body, true);
    verify(is_array($decoded), "solo status is not JSON: $body");
    verify($decoded['retiring children'] === 1,
        'retiring children: ' . var_export($decoded['retiring children'], true) . "\n$body");
    verify($decoded['workers'][0]['retiring'] === 1, "the row does not say retiring\n$body");
    fclose($reader);
    echo "retiring is visible: ok\n";

    /* 6. And it goes away with the child, rather than being inherited by the
     * replacement that takes the same slot. */
    until(function () use ($pid) {
        $w = workers('/solo-status');
        return count($w) === 1 && !isset($w[(int) $pid]) && $w[array_key_first($w)]['retiring'] === 0 ? $w : null;
    }, 15, 'the solo child to be replaced by one that is not retiring');
    echo "replacement is not retiring: ok\n";

    echo "Done\n";
} finally {
    $tester->terminate();
    $tester->close();
    @unlink($root . '/front.php');
    @rmdir($root);
}
?>
--EXPECT--
pids on the page: ok
in-flight request finished: ok
replaced: ok
rolling restart: ok
retiring is visible: ok
replacement is not retiring: ok
Done
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>

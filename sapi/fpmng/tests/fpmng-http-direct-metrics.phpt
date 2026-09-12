--TEST--
fpm-ng: direct HTTP status reports per-connection counters that move with the load (issue #64)
--SKIPIF--
<?php include "skipif.inc"; ?>
--FILE--
<?php
require_once "tester.inc";
require_once "fpmng-operator.inc";

/* Issue #64. Every assertion here is about a counter MOVING under a load this
 * test produced, not about a field being present: a status page whose numbers
 * are wrong is worse than one that is missing, because it is believed.
 *
 * The page is read from the operator listener, not from the pool's own: issue
 * #275 moved it there, and nothing about the numbers changed in the move. What
 * did change is that the reader is no longer a client of the pool it reads --
 * the connections and requests below are the ones this test made on purpose,
 * with none of the scrape's own mixed in.
 *
 * http.max_connections = 1 on [metrics] is the lever that makes the test
 * deterministic rather than a policy under test here. A child that holds a
 * connection is at capacity, so fpm_http_direct_conns_may_accept() keeps it out
 * of accept (issue #53's gate) and every further connection lands on a
 * different child -- which is what gives the per-child rows below two children
 * with something on each.
 *
 * [recycle] is separate because its pm.max_requests would recycle the child
 * holding the connection this test keeps open in the middle of everything
 * else. */
$root = sys_get_temp_dir() . '/fpmng-metrics-' . getmypid();
@mkdir($root);
file_put_contents($root . '/front.php', <<<'PHP'
<?php
if (isset($_GET['sleep'])) {
    usleep((int) $_GET['sleep'] * 1000);
}
echo 'php';
PHP);
$base = (int) (getenv('FPMNG_DIRECT_TEST_PORT') ?: 28054);
$port = $base + 21;
$recycle = $base + 22;
$ops = '127.0.0.1:' . ($base + 23);
$cfg = <<<CFG
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
[metrics]
listen = 127.0.0.1:$port
pool.type = http-direct
pm = static
pm.max_children = 2
http.max_connections = 1
chdir = $root
http.front_controller = /front.php
pm.status_path = /status
pm.status_listen = $ops
; Short enough that a connection which sends nothing is dropped inside this
; test's patience, long enough that a connection this test means to hold open
; between two reads of the page is not dropped under it.
http.read_timeout = 1500
[recycle]
listen = 127.0.0.1:$recycle
pool.type = http-direct
pm = static
pm.max_children = 1
pm.max_requests = 2
chdir = $root
http.front_controller = /front.php
; The same operator listener as [metrics], on a path of its own: two pools may
; share one listener as long as the triple (address, port, path) is unique
; (issue #273, point 7), and a test that reads both pages is the cheapest place
; to keep saying so.
pm.status_path = /recycle-status
pm.status_listen = $ops
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

/* One framed message per call, so a keep-alive connection stays usable. */
function fetch($fp, string $path): array
{
    fwrite($fp, "GET $path HTTP/1.1\r\nHost: test\r\n\r\n");
    $line = fgets($fp);
    if (!$line || !preg_match('#^HTTP/1\.1 (\d+) #', $line, $m)) {
        throw new RuntimeException('bad status line: ' . var_export($line, true));
    }
    $status = (int) $m[1];
    $length = 0;
    while (($line = fgets($fp)) !== false && $line !== "\r\n") {
        if (stripos($line, 'Content-Length:') === 0) {
            $length = (int) trim(substr($line, 15));
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
    $GLOBALS['last_page'] = $body;
    return [$status, $body];
}

/* The text page is a field per line; this is what a tool reading it would do.
 * The pool-wide block is printed before the per-child rows, so on `?full` this
 * still reads the pool's value. */
function field(string $body, string $name): int
{
    if (!preg_match('/^' . preg_quote($name, '/') . ': *(\d+)$/m', $body, $m)) {
        throw new RuntimeException("field missing: $name\n$body");
    }
    return (int) $m[1];
}

/* Polls rather than sleeps: every gauge here is published from the worker's
 * 10 ms tick, so for one tick "not yet" and "never" look the same. */
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
    /* With the page it last read: a gauge that did not move is only
     * diagnosable against the numbers that were there instead, and a bare
     * "timed out" turns every flake into a re-run. */
    throw new RuntimeException("timed out waiting for $what\nlast page:\n" .
        ($GLOBALS['last_page'] ?? '(none)'));
}

$tester = new FPM\Tester($cfg, '<?php');
try {
    $tester->start();
    $tester->expectLogStartNotices();

    /* One connection per read, on the operator listener: that server answers
     * one request and closes. */
    $page = function (string $query = '') use ($ops): string {
        $body = fpmng_operator_body($ops, '/status' . $query);
        $GLOBALS['last_page'] = $body;
        return $body;
    };
    $body = $page();

    /* 1. The schema marker and the fields behind it. A tool that finds a
     * version it does not know can stop; one that finds no version at all has
     * to guess from the fields. */
    expect('schema', field($body, 'direct schema'), 1);
    foreach (['live connections', 'pending responses', 'refused acl', 'refused capacity',
              'refused connections', 'timed out connections', 'rejected responses'] as $name) {
        field($body, $name);
    }
    echo "schema: ok\n";

    /* 2. Connections. One connection held open by this test is one live
     * connection on the page; a second one lands on the other child (see
     * http.max_connections above) and raises both counters. */
    $held = connect($port);
    fetch($held, '/app');
    $body = until(function () use ($page) {
        $b = $page();
        return field($b, 'live connections') >= 1 ? $b : null;
    }, 15, 'the held connection to show up as a live connection');
    $accepted = field($body, 'accepted conn');
    $other = connect($port);
    fetch($other, '/app');
    until(function () use ($page, $accepted) {
        $b = $page('?full');
        return field($b, 'accepted conn') > $accepted && field($b, 'live connections') >= 2 ? $b : null;
    }, 15, 'the second connection to be counted');
    fclose($other);
    /* And the gauge comes back down: a live count that only grows is a total
     * wearing a gauge's name. */
    until(function () use ($page) {
        $b = $page();
        return field($b, 'live connections') === 1 ? $b : null;
    }, 15, 'the second connection to be released');
    echo "connections: ok\n";

    /* 3. A slow request is visible as an active request WHILE it runs. This is
     * the counter that cannot be published from the tick -- the child running
     * the script is not running its event loop -- so it is written on the
     * request path itself, and this check is what says so. */
    $slow = connect($port);
    fwrite($slow, "GET /app?sleep=1000 HTTP/1.1\r\nHost: test\r\n\r\n");
    until(function () use ($page) {
        $b = $page();
        return field($b, 'active requests') >= 1 ? $b : null;
    }, 15, 'the slow request to show as active');
    echo "active during php: ok\n";
    $line = fgets($slow);
    expect('slow request finished', substr($line, 0, 12), 'HTTP/1.1 200');
    fclose($slow);
    until(function () use ($page) {
        $b = $page();
        return field($b, 'active requests') === 0 ? $b : null;
    }, 15, 'the slow request to stop being active');
    echo "active after php: ok\n";

    /* 4. A connection that never sends a request is dropped by the first
     * request deadline from issue #61, and that drop has its own counter: an
     * operator who sees connections disappear has to be able to tell this
     * reason from a client that simply went away. */
    $silent = connect($port);
    until(function () use ($page) {
        $b = $page();
        return field($b, 'timed out connections') >= 1 ? $b : null;
    }, 10, 'the silent connection to time out');
    fclose($silent);
    echo "timed out: ok\n";

    /* 5. Refusals are counted apart by reason, and "refused requests" stays
     * the sum it has always been. This load refused nothing, so all of them
     * are zero -- what the assertion is really about is that they are separate
     * fields: a page that reported only the sum would make "nothing was
     * refused" indistinguishable from "everything was refused for a reason you
     * cannot see". The ACL counter is exercised by
     * fpmng-http-direct-operator.phpt, on a pool that refuses everyone. */
    $body = $page();
    expect('refused acl', field($body, 'refused acl'), 0);
    expect('refused capacity', field($body, 'refused capacity'), 0);
    expect('refused requests', field($body, 'refused requests'), 0);
    expect('refused connections', field($body, 'refused connections'), 0);
    echo "refusals separated: ok\n";

    /* 6. The per-child rows. This is what makes the accept distribution
     * measurable from the status page alone -- issue #53's fairness finding
     * needed an external harness, and a pool-wide sum cannot show it at all.
     * Two children, so two rows, and their accepted connections have to add up
     * to the pool's. */
    $body = $page('?full');
    preg_match_all('/^slot: *(\d+)$/m', $body, $m);
    expect('slots', count($m[1]), 2);
    preg_match_all('/^accepted conn: *(\d+)$/m', $body, $rows);
    $sum = 0;
    foreach (array_slice($rows[1], 1) as $n) {
        $sum += (int) $n;
    }
    expect('per-child accepted adds up', $sum, (int) $rows[1][0]);
    echo "per-child rows: ok\n";

    /* 7. JSON stays JSON with the rows in it: a monitoring tool reads this
     * page, and a page that is only valid in one of its two modes breaks the
     * day someone asks for detail. */
    [$status, $headers, $body] = fpmng_operator_fetch($ops, '/status?json&full');
    expect('json status', $status, 200);
    expect('json type', $headers['content-type'], 'application/json');
    $decoded = json_decode($body, true);
    if (!is_array($decoded)) {
        throw new RuntimeException("status?json&full is not JSON\n$body");
    }
    expect('json schema', $decoded['direct schema'], 1);
    expect('json workers', count($decoded['workers']), 2);
    foreach (['slot', 'accepted conn', 'live connections', 'timed out connections'] as $key) {
        if (!array_key_exists($key, $decoded['workers'][0])) {
            throw new RuntimeException("per-child JSON row is missing $key\n$body");
        }
    }
    echo "json: ok\n";

    /* 8. Totals survive a child recycled by pm.max_requests. [recycle] runs one
     * child that is replaced every two requests, and the replacement inherits
     * the dead one's scoreboard slot: if it zeroed the totals in that slot, the
     * pool's accepted connections would go backwards in the middle of a
     * monitoring series, which is the one thing nobody can alert on. Each
     * request gets a fresh connection, so accepted conn has to reach the number
     * of connections this loop made and never drop. */
    /* Retried, because the child being recycled is the point: a connection
     * accepted by a child that is on its way out is closed without an answer,
     * and that is correct behaviour, not a failure to assert against. */
    $once = function (string $path) use ($recycle) {
        for ($try = 0; $try < 20; $try++) {
            $fp = connect($recycle);
            try {
                [, $b] = fetch($fp, $path);
                return $b;
            } catch (RuntimeException $e) {
                usleep(100000);
            } finally {
                if (is_resource($fp)) {
                    fclose($fp);
                }
            }
        }
        throw new RuntimeException("no answer from the recycling pool for $path");
    };
    $seen = 0;
    for ($i = 0; $i < 6; $i++) {
        $once('/app');
        /* The page of the OTHER pool on the same operator listener, so this
         * also says that two pools sharing one listener are told apart by
         * their paths. */
        $b = fpmng_operator_body($ops, '/recycle-status');
        $now = field($b, 'accepted conn');
        if ($now < $seen) {
            throw new RuntimeException("accepted conn went backwards: $seen -> $now\n$b");
        }
        $seen = $now;
    }
    /* Six connections, one per /app: the scrape is no longer one of them. */
    if ($seen < 6) {
        throw new RuntimeException("accepted conn did not survive recycling: $seen");
    }
    echo "recycling: ok\n";

    echo "Done\n";
} finally {
    if (isset($held) && is_resource($held)) {
        fclose($held);
    }
    $tester->terminate();
    $tester->close();
    @unlink($root . '/front.php');
    @rmdir($root);
}
?>
--EXPECT--
schema: ok
connections: ok
active during php: ok
active after php: ok
timed out: ok
refusals separated: ok
per-child rows: ok
json: ok
recycling: ok
Done
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>

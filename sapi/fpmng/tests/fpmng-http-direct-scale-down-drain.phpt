--TEST--
fpm-ng: the master grants a scaled-down direct HTTP child its http.read_timeout before SIGKILL (issue #310)
--SKIPIF--
<?php
include "skipif.inc";
if (PHP_OS_FAMILY !== 'Linux') die('skip requires Linux /proc');
?>
--FILE--
<?php
require_once "tester.inc";
require_once "fpmng-operator.inc";

/* Issue #310. fpm_pctl_kill_idle_child() is how the master shrinks a
 * pm = dynamic/ondemand pool back down when it has more idle children than
 * pm.max_spare_servers wants: it used to jump straight to FPM_PCTL_KILL
 * (SIGKILL) the very next idle-server-maintenance pass, about a second later,
 * with no regard for whether the child was still legitimately draining a
 * connection under its own http.read_timeout. This test holds two idle-but-open
 * connections (no request in flight on either, which is exactly the state a
 * spare child is in when the master decides to retire it), lets the pool
 * shrink to one, and checks that whichever child gets picked is retired with
 * SIGUSR1 rather than killed outright, keeps answering the connection it
 * holds, is not respawned once it exits, and that its sibling is untouched.
 *
 * pool.type = http-direct enforces pm = static in production; the fix's own
 * code path is otherwise unreachable, so this test asks the binary to relax
 * that one check for itself via FPMNG_TEST_ALLOW_NONSTATIC_DIRECT, which is
 * read from the environment rather than from anything a config file lever
 * could set. */
putenv('FPMNG_TEST_ALLOW_NONSTATIC_DIRECT=1');

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

$root = sys_get_temp_dir() . '/fpmng-scale-down-drain-' . getmypid();
@mkdir($root);
file_put_contents($root . '/front.php', '<?php echo getmypid();');

$base = (int) (getenv('FPMNG_DIRECT_TEST_PORT') ?: 28054);
$port = $base + 51;
$ops = '127.0.0.1:' . ($base + 52);
$cfg = <<<CFG
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
[direct]
listen = 127.0.0.1:$port
pool.type = http-direct
pm = dynamic
pm.max_children = 2
pm.start_servers = 2
pm.min_spare_servers = 1
pm.max_spare_servers = 1
chdir = $root
http.front_controller = /front.php
pm.status_path = /status
pm.status_listen = $ops
; The bound the scaled-down child gets before the master escalates to
; SIGKILL: comfortably longer than one idle-server-maintenance pass
; (~1 second), which is exactly the margin the old code did not grant.
http.read_timeout = 3000
CFG;

$tester = new FPM\Tester($cfg, '<?php');
try {
    $tester->start();
    $tester->expectLogStartNotices();

    until(function () use ($ops) {
        $w = workers($ops, '/status');
        return count($w) === 2 ? $w : null;
    }, 15, 'both children to appear on the status page');
    echo "two children started: ok\n";

    /* Both connections accepted and answered once, then left open with no
     * further request on them -- an idle-but-open connection, which is what
     * a spare child the master is about to retire is holding. Written back
     * to back, before either response is read, so both children (both
     * already blocked in accept()) get a chance to take one each. */
    $connA = connect($port);
    $connB = connect($port);
    fwrite($connA, "GET / HTTP/1.1\r\nHost: test\r\n\r\n");
    fwrite($connB, "GET / HTTP/1.1\r\nHost: test\r\n\r\n");
    [$statusA, $pidA, $closeA] = fetch($connA, '/');
    [$statusB, $pidB, $closeB] = fetch($connB, '/');
    verify($statusA === 200 && $statusB === 200, "warm-up requests: $statusA / $statusB");
    verify(!$closeA && !$closeB, 'a fresh child answered its warm-up request with Connection: close');
    verify((int) $pidA !== (int) $pidB, "both warm-up requests landed on the same child: $pidA");
    $pids = [(int) $pidA => $connA, (int) $pidB => $connB];
    echo "both children answered: ok\n";

    /* The pool wants one spare, not two: the master picks one of these two
     * idle children to retire on its own, with no signal from this test. */
    $victim = until(function () use ($ops, $pids) {
        foreach (workers($ops, '/status') as $pid => $row) {
            if (isset($pids[$pid]) && $row['retiring'] === 1) {
                return $pid;
            }
        }
        return null;
    }, 15, 'the master to pick one idle child to retire');
    $survivor = array_key_first(array_diff_key($pids, [$victim => true]));
    echo "one child picked to retire: ok\n";

    /* The connection the retiring child holds, with no request ever having
     * been in flight when it was picked, is still answered rather than
     * reset: the same "drain, don't sever" contract issue #313 gave
     * pm.max_requests, now reached from the master's own scale-down instead
     * of from the child itself. */
    [$status2, $body2, $close2] = fetch($pids[$victim], '/');
    verify($status2 === 200, "held connection on the retiring child: $status2");
    verify((int) $body2 === $victim, "held connection answered by a different child: $body2 vs $victim");
    verify($close2, 'the retiring child answered without Connection: close');
    fclose($pids[$victim]);
    echo "held connection drained, not dropped: ok\n";

    /* And it is not respawned once it exits: a scale-down putting a
     * replacement back in the slot it just freed would defeat the whole
     * point of shrinking the pool. */
    until(function () use ($ops, $victim) {
        $w = workers($ops, '/status');
        return count($w) === 1 && !isset($w[$victim]) ? $w : null;
    }, 15, 'the retired child to exit without being replaced');
    echo "retired without being replaced: ok\n";

    /* The sibling was never touched. */
    $rows = workers($ops, '/status');
    verify(isset($rows[$survivor]), "the surviving child $survivor is gone");
    verify($rows[$survivor]['retiring'] === 0, "the surviving child is retiring too");
    [$status3, $body3, $close3] = fetch($pids[$survivor], '/');
    fclose($pids[$survivor]);
    verify($status3 === 200 && !$close3, "the surviving child's held connection was disturbed: $status3");
    verify((int) $body3 === $survivor, "the surviving connection answered by a different child: $body3 vs $survivor");
    echo "sibling untouched: ok\n";

    echo "Done\n";
} finally {
    $tester->terminate();
    $tester->close();
    @unlink($root . '/front.php');
    @rmdir($root);
}
?>
--EXPECT--
two children started: ok
both children answered: ok
one child picked to retire: ok
held connection drained, not dropped: ok
retired without being replaced: ok
sibling untouched: ok
Done
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>

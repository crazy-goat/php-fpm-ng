--TEST--
fpm-ng: a direct child that is idle and has served requests still obeys SIGUSR1 and SIGQUIT (issue #256)
--SKIPIF--
<?php
include "skipif.inc";
if (PHP_OS_FAMILY !== 'Linux') die('skip requires Linux /proc');
?>
--FILE--
<?php
require_once "tester.inc";

/* Issue #256. fpmng-http-direct-retire.phpt signals a child that is INSIDE a
 * request, and that path works for a reason that does not generalise: while
 * PHP runs, SIGG(active) is 1 and zend_signal_handler_defer() reaches the
 * handler this SAPI installed. An idle child is the opposite case and it is
 * the one that matters -- a scale-down, a deploy and fpm_pctl_kill_idle_child()
 * all signal a child precisely because it is idle.
 *
 * It also has to be a child that has served at least two requests. The first
 * zend_signal_activate() captures whatever sigaction() left installed, so one
 * request still behaves; from the second on, activate() restores the snapshot
 * zend_signal_init() took, and before the fix that snapshot was the one
 * fpm_signals_init_child() made BEFORE this SAPI installed anything -- SIGQUIT
 * = fpm's sig_soft_quit, which nothing in the direct loop reads, and SIGUSR1 =
 * SIG_DFL, which kills the child and every connection it holds.
 *
 * So: three requests, then idle, then the signal. Both pools are
 * pm.max_children = 1, because the assertion is about one named child and a
 * sibling would answer for it. */
$root = sys_get_temp_dir() . '/fpmng-idlesig-' . getmypid();
@mkdir($root);
file_put_contents($root . '/front.php', '<?php echo getmypid();');
$base = (int) (getenv('FPMNG_DIRECT_TEST_PORT') ?: 28054);
$retire = $base + 41;
$stop = $base + 42;
$cfg = <<<CFG
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
[retire]
listen = 127.0.0.1:$retire
pool.type = http-direct
pm = static
pm.max_children = 1
chdir = $root
http.front_controller = /front.php
pm.status_path = /status
; Long enough that the drain is ended by this test closing the connection and
; not by the read timeout: a timeout would hide a child that never retired.
http.read_timeout = 30000
[stop]
listen = 127.0.0.1:$stop
pool.type = http-direct
pm = static
pm.max_children = 1
chdir = $root
http.front_controller = /front.php
pm.status_path = /status
http.read_timeout = 30000
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

/* One framed message, so the connection stays usable for the next one. */
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

/* Three requests on one connection, and the pid that answered all of them. The
 * connection is left open on purpose: it is what the child has to keep serving
 * after SIGUSR1, and what proves the child is still there. */
function warm($fp): int
{
    $pid = null;
    for ($i = 0; $i < 3; $i++) {
        [$status, $body, $close] = fetch($fp, '/');
        verify($status === 200, "warm-up request $i: $status");
        verify(!$close, "the server closed a keep-alive connection during warm-up");
        if ($pid === null) {
            $pid = (int) $body;
        }
        verify((int) $body === $pid, "warm-up request $i was answered by $body, not $pid");
    }
    verify($pid > 1, "no pid from the warm-up");
    return $pid;
}

$tester = new FPM\Tester($cfg, '<?php');
try {
    $tester->start();
    $tester->expectLogStartNotices();

    /* 1. SIGUSR1 to an idle child that has served three requests. The child
     * must still be alive, must say so on the status page, and must say it is
     * retiring -- before the fix it was gone, and this fetch() threw on a
     * closed connection instead. */
    $held = connect($retire);
    $pid = warm($held);
    $tester->signal('USR1', $pid);
    $decoded = until(function () use ($held) {
        [$status, $body, $close] = fetch($held, '/status?json&full');
        verify($status === 200, "status on the held connection: $status");
        $decoded = json_decode($body, true);
        verify(is_array($decoded), "status is not JSON: $body");
        return $decoded['retiring children'] === 1 ? $decoded : null;
    }, 15, 'the idle child to report itself retiring');
    verify($decoded['workers'][0]['pid'] === $pid,
        "a different child answered: " . json_encode($decoded['workers'][0]));
    echo "idle child retires on SIGUSR1: ok\n";

    /* And the drain ends when this connection does, rather than at the read
     * timeout: the child leaves and the master puts a replacement in the slot. */
    fclose($held);
    until(function () use ($retire, $pid) {
        $fp = @stream_socket_client("tcp://127.0.0.1:$retire", $errno, $error, 5);
        if (!$fp) {
            return null;
        }
        try {
            stream_set_timeout($fp, 10);
            [$status, $body] = fetch($fp, '/');
            return ($status === 200 && (int) $body !== $pid) ? true : null;
        } catch (RuntimeException $e) {
            return null;
        } finally {
            fclose($fp);
        }
    }, 15, "the retired child $pid to be replaced");
    echo "drain ends with the connection: ok\n";

    /* 2. SIGQUIT to an idle child that has served three requests. This is the
     * signal fpm_pctl_kill_idle_child() sends and the one a pool-wide graceful
     * stop starts with, so the child has to act on it rather than leave the
     * master to escalate. Before the fix it reached fpm's sig_soft_quit, the
     * direct loop never saw a thing, and the same child kept answering. */
    $quit = connect($stop);
    $victim = warm($quit);
    fclose($quit);
    $tester->signal('QUIT', $victim);
    until(function () use ($stop, $victim) {
        $fp = @stream_socket_client("tcp://127.0.0.1:$stop", $errno, $error, 5);
        if (!$fp) {
            return null;
        }
        try {
            stream_set_timeout($fp, 10);
            [$status, $body] = fetch($fp, '/');
            return ($status === 200 && (int) $body !== $victim) ? true : null;
        } catch (RuntimeException $e) {
            return null;
        } finally {
            fclose($fp);
        }
    }, 15, "the child $victim to stop on SIGQUIT and be replaced");
    echo "idle child stops on SIGQUIT: ok\n";

    echo "Done\n";
} finally {
    $tester->terminate();
    $tester->close();
    @unlink($root . '/front.php');
    @rmdir($root);
}
?>
--EXPECT--
idle child retires on SIGUSR1: ok
drain ends with the connection: ok
idle child stops on SIGQUIT: ok
Done
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>

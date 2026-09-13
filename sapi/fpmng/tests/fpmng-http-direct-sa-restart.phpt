--TEST--
fpm-ng: a retire signal does not break the syscall the script is blocked in (issue #259)
--SKIPIF--
<?php
include "skipif.inc";
if (PHP_OS_FAMILY !== 'Linux') die('skip requires a FIFO and kill(1)');
?>
--FILE--
<?php
require_once "tester.inc";

/* Issue #259. The child installs its SIGQUIT and SIGUSR1 handlers with
 * SA_RESTART on purpose -- in this executor PHP runs inside evhttp's request
 * callback, so a signal aimed at a busy child lands while the script is blocked
 * in read() or write() on a database, cache or HTTP socket. Every
 * php_request_startup() takes the flag back off: zend_signal_activate()
 * reinstalls an action for every signal in zend_sigs[] with SA_SIGINFO alone
 * (Zend/zend_signal.c:305).
 *
 * The test blocks a request in fread() on a FIFO and retires its child twice
 * while it is in there. Twice, not once, and that is the whole shape of the
 * test: main/streams/plain_wrapper.c:459 retries an EINTR'd read exactly once
 * by itself, so a single signal is absorbed by PHP whether the fix is present
 * or not. The second interruption falls through to the "TODO: Should this be
 * treated as a proper error" branch (:470) and fread() returns false. With the
 * flag restored the kernel restarts the read and the script never sees EINTR.
 *
 * A FIFO rather than a socket because php's socket layer loops on EINTR around
 * poll() itself (main/streams/xp_socket.c:145-156) and would hide the bug --
 * which is also why this is a narrow exposure in production and not a
 * every-request one. */
function verify(bool $ok, string $message): void
{
    if (!$ok) {
        throw new RuntimeException($message);
    }
}

$root = sys_get_temp_dir() . '/fpmng-sa-restart-' . getmypid();
@mkdir($root);
$fifo = $root . '/fifo';
exec('mkfifo ' . escapeshellarg($fifo), $out, $status);
verify($status === 0, "mkfifo failed with status $status");

file_put_contents($root . '/front.php', '<?php $dir = ' . var_export($root, true) . ';' . <<<'PHP'
if (($_SERVER['REQUEST_URI'] ?? '/') === '/warm') {
    echo 'warm ', getmypid();
    return;
}
/* The pid file is the handshake: it is written before the blocking open, so
 * the driver knows which child to signal and that it is about to block. */
file_put_contents($dir . '/pid', getmypid());
$h = fopen($dir . '/fifo', 'r');
echo 'read=', var_export(fread($h, 5), true);
PHP);

$base = (int) (getenv('FPMNG_DIRECT_TEST_PORT') ?: 28054);
$port = $base + 34;
$cfg = <<<CFG
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
[direct]
listen = 127.0.0.1:$port
pool.type = http-direct
pm = static
pm.max_children = 1
chdir = $root
http.front_controller = /front.php
; Longer than this test takes: the child must be stopped by the FIFO, not by a
; deadline that would end the request for an unrelated reason.
http.read_timeout = 30000
CFG;

function connect(int $port)
{
    for ($i = 0; $i < 50; $i++) {
        $fp = @stream_socket_client("tcp://127.0.0.1:$port", $errno, $error, 5);
        if ($fp) {
            stream_set_timeout($fp, 15);
            return $fp;
        }
        usleep(100000);
    }
    throw new RuntimeException("connect $port: $error");
}

function body($fp): string
{
    $response = '';
    $deadline = microtime(true) + 15;
    while (!feof($fp) && microtime(true) < $deadline) {
        $chunk = fread($fp, 8192);
        if ($chunk === false) {
            break;
        }
        $response .= $chunk;
    }
    $split = strpos($response, "\r\n\r\n");
    verify($split !== false, 'no response: ' . var_export($response, true));
    return substr($response, $split + 4);
}

$tester = new FPM\Tester($cfg, '<?php');
try {
    $tester->start();
    $tester->expectLogStartNotices();

    /* A warm request first, so the child has been through php_request_startup()
     * at least once before the one that matters. Not strictly needed -- the
     * flag is already gone during the very first request -- but it is the state
     * a child in a running pool is actually in. */
    $warm = connect($port);
    fwrite($warm, "GET /warm HTTP/1.1\r\nHost: test\r\nConnection: close\r\n\r\n");
    verify(str_starts_with(body($warm), 'warm '), 'the warm request did not reach the front controller');
    fclose($warm);
    @unlink($root . '/pid');

    /* 'r+' rather than 'w': opening the write end of a FIFO for writing blocks
     * until a reader shows up, and a test that hangs when the fix regresses is
     * worse than one that fails. */
    $writer = fopen($fifo, 'r+');
    verify($writer !== false, 'could not open the FIFO');

    $blocking = connect($port);
    fwrite($blocking, "GET /block HTTP/1.1\r\nHost: test\r\nConnection: close\r\n\r\n");
    $deadline = microtime(true) + 15;
    do {
        $pid = (int) @file_get_contents($root . '/pid');
        usleep(20000);
    } while (!$pid && microtime(true) < $deadline);
    verify($pid > 1, 'the blocking request never reached the front controller');
    /* The pid file is written just before the open; give the child the moment
     * it needs to get from there into fread(). */
    usleep(300000);

    $tester->signal('USR1', $pid);
    usleep(50000);
    $tester->signal('USR1', $pid);
    usleep(200000);

    fwrite($writer, 'hello');
    fflush($writer);
    echo body($blocking), "\n";
    fclose($blocking);
    fclose($writer);

    echo "Done\n";
} finally {
    $tester->terminate();
    $tester->close();
    @unlink($root . '/front.php');
    @unlink($root . '/pid');
    @unlink($fifo);
    @rmdir($root);
}
?>
--EXPECT--
read='hello'
Done
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>

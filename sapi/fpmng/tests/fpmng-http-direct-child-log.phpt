--TEST--
fpm-ng: a direct child's lifecycle NOTICEs reach error_log without catch_workers_output (issue #260)
--SKIPIF--
<?php
include "skipif.inc";
?>
--FILE--
<?php
require_once "tester.inc";

/* Issue #260. Upstream FPM takes the error_log away from a child on the
 * premise that the master narrates everything worth narrating
 * (fpm_stdio_init_child(): close(error_log_fd) + zlog_set_fd(-1)), so a child's
 * zlog() goes to STDERR_FILENO, which under the default
 * catch_workers_output = no is /dev/null.
 *
 * That premise does not hold for a direct pool. The child owns the accept
 * socket, so the child is the only process that knows it has stopped
 * accepting -- retiring (issue #65) is narrated by the child or by nobody. The
 * pool type now takes the log channel of issue #121
 * (fpm_pool_type_s.child_logs_via_master), so the line lands in error_log as an
 * ordinary master line with "(child N)" appended.
 *
 * catch_workers_output is deliberately NOT set here: needing it was the bug,
 * and its documented purpose is capturing application output, not the
 * daemon's own lifecycle. */
function verify(bool $ok, string $message): void
{
    if (!$ok) {
        throw new RuntimeException($message);
    }
}

$root = sys_get_temp_dir() . '/fpmng-child-log-' . getmypid();
@mkdir($root);
file_put_contents($root . '/front.php', <<<'PHP'
<?php
echo getmypid();
PHP);

$base = (int) (getenv('FPMNG_DIRECT_TEST_PORT') ?: 28054);
$port = $base + 35;
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
http.read_timeout = 1000
CFG;

function once(int $port): string
{
    for ($i = 0; $i < 50; $i++) {
        $fp = @stream_socket_client("tcp://127.0.0.1:$port", $errno, $error, 5);
        if ($fp) {
            break;
        }
        usleep(100000);
        $fp = null;
    }
    verify($fp !== null, "connect $port: $error");
    stream_set_timeout($fp, 10);
    fwrite($fp, "GET / HTTP/1.1\r\nHost: test\r\nConnection: close\r\n\r\n");
    $response = '';
    while (!feof($fp)) {
        $chunk = fread($fp, 8192);
        if ($chunk === false || $chunk === '') {
            break;
        }
        $response .= $chunk;
    }
    fclose($fp);
    $split = strpos($response, "\r\n\r\n");
    verify($split !== false, 'no response: ' . var_export($response, true));
    return substr($response, $split + 4);
}

$tester = new FPM\Tester($cfg, '<?php');
try {
    $tester->start();
    $tester->expectLogStartNotices();

    $pid = (int) once($port);
    verify($pid > 1, "the request came back with no pid: $pid");

    $tester->signal('USR1', $pid);
    /* The line the issue was opened about, and the pid in it: the channel is
     * per pool, so without it an operator reading the message would have to
     * guess which child produced it. */
    $tester->expectLogNotice(
        "child $pid is retiring: no new connections, finishing the ones it holds.*\\(child $pid\\)",
        'direct'
    );
    echo "retire notice in error_log: ok\n";

    echo "Done\n";
} finally {
    $tester->terminate();
    $tester->close();
    @unlink($root . '/front.php');
    @rmdir($root);
}
?>
--EXPECT--
retire notice in error_log: ok
Done
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>

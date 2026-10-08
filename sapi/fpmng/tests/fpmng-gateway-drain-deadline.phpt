--TEST--
fpm-ng: a stuck gateway client is cut at process_control_timeout and the reload still finishes (issue #641)
--SKIPIF--
<?php
include "fpmng-skipif.inc";
if (PHP_OS_FAMILY !== 'Linux') die('skip requires Linux /proc');
fpmng_skip_if_pool_type_unsupported('gateway');
?>
--FILE--
<?php

require_once "tester.inc";

/* Issue #641 phase 1, the other half of the drain: a client that never reads
 * must not hold the reload open forever. The gateway stops accepting, closes
 * what is idle, and waits for the response still in flight only until
 * process_control_timeout; the master SIGKILLs at the same deadline. The
 * reload therefore finishes about process_control_timeout after SIGUSR2 (not
 * at once, which is what the old SIGTERM did) and the stuck client is cut.
 *
 * "The reload finished" is measured on the gateway PROCESS being replaced:
 * until the master's execvp() the old gateway and its old workers still
 * answer, so "a request answered" would pass at once. */

const BODY = 8 * 1024 * 1024;
const PCT = 3;
$pct = PCT;

$root = sys_get_temp_dir() . '/fpmng-gw-drain-deadline-' . getmypid();
@mkdir($root, 0700, true);
file_put_contents("$root/app.php", <<<'PHP'
<?php
$n = (int) ($_GET['n'] ?? 0);
$body = str_repeat('A', $n);
header('Content-Type: application/octet-stream');
header('Content-Length: ' . strlen($body));
echo $body;
PHP);

$cfg = <<<EOT
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
log_level = notice
process_control_timeout = $pct
[gw]
pool.type = gateway
listen = {{ADDR[http]}}
chdir = $root
http.gateways = 1
http.response_buffer = 0
http.route[app] = /
[app]
pool.type = fastcgi
listen = {{ADDR}}
chdir = $root
pm = static
pm.max_children = 2
EOT;

/* The gateway process whose parent is $master, via the setproctitle line
 * fpm_http_gateway_run() installs. 0 when none exists. */
function gateway_pid(int $master): int
{
    foreach (glob('/proc/[0-9]*/stat') as $stat) {
        $raw = @file_get_contents($stat);
        if ($raw === false || !preg_match('/^(\d+) \((.*)\) \S (\d+) /s', $raw, $m)) {
            continue;
        }
        if ((int) $m[3] !== $master) {
            continue;
        }
        $cmd = @file_get_contents(dirname($stat) . '/cmdline');
        if ($cmd !== false && str_contains($cmd, 'http gateway gw')) {
            return (int) $m[1];
        }
    }
    return 0;
}

function read_all($fp, int $seconds): string
{
    stream_set_timeout($fp, $seconds);
    $out = '';
    while (!feof($fp)) {
        $chunk = fread($fp, 65536);
        if ($chunk === false) {
            break;
        }
        if ($chunk === '') {
            $meta = stream_get_meta_data($fp);
            if ($meta['timed_out'] || feof($fp)) {
                break;
            }
            continue;
        }
        $out .= $chunk;
    }
    return $out;
}

$tester = new FPM\Tester($cfg, '<?php');
try {
    /* forceStderr = false so the gateway's own drain lines land in the
     * error_log file this test reads; -O would send them to the pipe. */
    $tester->start([], false);
    $tester->switchLogSource('{{FILE:LOG}}');
    $tester->expectLogStartNotices();
    $master = $tester->getPid();
    $http = $tester->getAddr('ipv4', '[http]');

    $oldGw = gateway_pid($master);
    if ($oldGw === 0) {
        throw new RuntimeException('the gateway process was not found');
    }

    $stuck = stream_socket_client("tcp://$http", $errno, $error, 5);
    if (!$stuck) {
        throw new RuntimeException("connect: $error");
    }
    fwrite($stuck, "GET /app.php?n=" . BODY . " HTTP/1.1\r\nHost: test\r\n\r\n");
    /* Let the worker answer and the gateway buffer the body for the client
     * that is deliberately not reading. */
    usleep(800000);

    $t0 = microtime(true);
    $tester->signal('USR2');

    /* The new gateway process appears only after the old one has drained and
     * the master has re-exec'd. Detecting the replacement (a different pid)
     * rather than "the old pid is gone" also survives pid reuse. */
    $deadline = microtime(true) + 20;
    $newGw = 0;
    while (microtime(true) < $deadline) {
        $pid = gateway_pid($master);
        if ($pid !== 0 && $pid !== $oldGw) {
            $newGw = $pid;
            break;
        }
        usleep(50000);
    }
    $elapsed = microtime(true) - $t0;
    if ($newGw === 0) {
        throw new RuntimeException('the old gateway was never replaced');
    }
    /* The drain must have waited for the deadline, not cut at once. */
    if ($elapsed < PCT - 1.0) {
        throw new RuntimeException(sprintf('the gateway was replaced in %.2fs, well before process_control_timeout=%ds: it was not drained', $elapsed, PCT));
    }
    if ($elapsed > PCT + 8.0) {
        throw new RuntimeException(sprintf('the gateway held on for %.2fs, far past process_control_timeout=%ds', $elapsed, PCT));
    }
    echo "old gateway drained at the deadline: ok\n";

    /* The reload finished: the new gateway serves a fresh request. */
    $fresh = @stream_socket_client("tcp://$http", $errno, $error, 2);
    if (!$fresh) {
        throw new RuntimeException("the new gateway did not accept: $error");
    }
    fwrite($fresh, "GET /app.php?n=16 HTTP/1.1\r\nHost: test\r\nConnection: close\r\n\r\n");
    $r = read_all($fresh, 5);
    fclose($fresh);
    if (!str_contains($r, 'HTTP/1.1 200') || !str_contains($r, str_repeat('A', 16))) {
        throw new RuntimeException("the new generation did not serve\n$r");
    }
    echo "reload finished and serves: ok\n";

    /* The stuck client was cut: its response is truncated and the socket is
     * closed. Reading now cannot unblock the (dead) old gateway. */
    $response = read_all($stuck, 10);
    fclose($stuck);
    $split = strpos($response, "\r\n\r\n");
    $body = $split === false ? $response : substr($response, $split + 4);
    if (strlen($body) >= BODY) {
        throw new RuntimeException('the stuck client received the whole body; it was not cut');
    }
    echo "stuck client cut: ok\n";

    /* The deadline is visible in the log: the gateway's own "stopped waiting
     * ... after process_control_timeout", or the master's SIGKILL line when
     * the gateway lost the race to its own deadline. */
    $log = (string) @file_get_contents($tester->getPrefixedFile(FPM\Tester::FILE_EXT_LOG_ERR));
    if (!str_contains($log, 'after process_control_timeout')
            && !str_contains($log, 'did not drain within process_control_timeout')) {
        throw new RuntimeException("the deadline was never logged\n$log");
    }
    echo "deadline enforced: ok\n";

    echo "Done\n";
} finally {
    $tester->terminate();
    $tester->close();
    @unlink("$root/app.php");
    @rmdir($root);
}
?>
--EXPECT--
old gateway drained at the deadline: ok
reload finished and serves: ok
stuck client cut: ok
deadline enforced: ok
Done
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>

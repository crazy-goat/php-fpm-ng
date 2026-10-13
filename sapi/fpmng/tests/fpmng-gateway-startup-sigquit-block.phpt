--TEST--
fpm-ng: SIGQUIT is blocked in the gateway startup window, so a stop/reload signal is not discarded (issue #641)
--SKIPIF--
<?php
include "fpmng-skipif.inc";
if (PHP_OS_FAMILY !== 'Linux') die('skip requires Linux /proc');
fpmng_skip_if_pool_type_unsupported('gateway');
?>
--FILE--
<?php

require_once "tester.inc";

/* Issue #641 round 2: the gateway is forked in fpm_http_gateway_spawn()
 * without fpm_signals_child_block() (that path is only for struct fpm_child_s
 * children, fpm_children.c:569) and the master's mask is empty by then
 * (fpm_signals_init_main() unblocks everything), so nothing had SIGQUIT
 * blocked before the evsignal was installed. The moved fpm_signals_unblock()
 * was then a no-op and a SIGQUIT delivered in the window was discarded by
 * SIG_IGN, so the gateway never drained and was SIGKILLed at the deadline.
 *
 * To observe the window deterministically, http.access_log is a FIFO with no
 * reader: the gateway blocks in open() (fpm_http_access_log_open()) after it
 * has blocked SIGQUIT and before it installs the evsignal and lifts the block.
 * SigBlk must contain SIGQUIT (bit 2 = 0x4) while blocked, and lose it once a
 * reader opens the FIFO and startup finishes. This is exactly the check that
 * would have caught the round-2 no-op. */

$root = sys_get_temp_dir() . '/fpmng-gw-sigquit-block-' . getmypid();
@mkdir($root, 0700, true);
$fifo = "$root/access.fifo";
exec('mkfifo ' . escapeshellarg($fifo), $out, $status);
if ($status !== 0) {
    throw new RuntimeException("mkfifo failed with status $status");
}

$cfg = <<<EOT
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
log_level = notice
process_control_timeout = 0
[gw]
pool.type = gateway
listen = {{ADDR[http]}}
chdir = $root
http.gateways = 1
http.access_log = $fifo
http.route[app] = /
[app]
pool.type = fastcgi
listen = {{ADDR}}
chdir = $root
pm = static
pm.max_children = 1
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

function sigblk(int $pid): int
{
    $status = (string) @file_get_contents("/proc/$pid/status");
    if (preg_match('/^SigBlk:\s*([0-9a-f]+)/mi', $status, $m)) {
        return (int) hexdec($m[1]);
    }
    return -1;
}

$tester = new FPM\Tester($cfg, '<?php');
try {
    $tester->start([], false);

    /* The gateway blocks in open() before the master finishes starting, so wait
     * for the pid file rather than reading it at once. */
    $pidFile = $tester->getPrefixedFile(FPM\Tester::FILE_EXT_PID);
    $deadline = microtime(true) + 10;
    while (microtime(true) < $deadline && !is_file($pidFile)) {
        usleep(20000);
    }
    $master = (int) @file_get_contents($pidFile);
    if ($master <= 0) {
        throw new RuntimeException('the master pid was not written');
    }

    $deadline = microtime(true) + 10;
    $gw = 0;
    while (microtime(true) < $deadline) {
        $gw = gateway_pid($master);
        if ($gw !== 0) {
            break;
        }
        usleep(20000);
    }
    if ($gw === 0) {
        throw new RuntimeException('the gateway process was not found');
    }

    $blocked = sigblk($gw);
    if (($blocked & 0x4) === 0) {
        throw new RuntimeException(sprintf('SIGQUIT is not blocked in the gateway startup window: SigBlk %s', dechex($blocked)));
    }
    echo "SIGQUIT blocked during startup: ok\n";

    /* Release the gateway: pair its blocking open(O_WRONLY) with a reader, then
     * the late fpm_signals_unblock() must clear the bit. */
    $reader = fopen($fifo, 'r');
    if (!$reader) {
        throw new RuntimeException('cannot open the access-log FIFO for reading');
    }
    $deadline = microtime(true) + 10;
    while (microtime(true) < $deadline && (sigblk($gw) & 0x4) !== 0) {
        usleep(20000);
    }
    if ((sigblk($gw) & 0x4) !== 0) {
        throw new RuntimeException(sprintf('SIGQUIT is still blocked after startup: SigBlk %s', dechex(sigblk($gw))));
    }
    echo "SIGQUIT unblocked after startup: ok\n";

    fclose($reader);
    echo "Done\n";
} finally {
    $tester->terminate();
    $tester->close();
    @unlink($fifo);
    @rmdir($root);
}
?>
--EXPECT--
SIGQUIT blocked during startup: ok
SIGQUIT unblocked after startup: ok
Done
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>

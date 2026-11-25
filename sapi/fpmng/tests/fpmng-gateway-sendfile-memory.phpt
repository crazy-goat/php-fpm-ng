--TEST--
fpm-ng: a 200 MB file sent through X-Accel-Redirect to a slow reader does not grow the gateway's resident memory (#649)
--SKIPIF--
<?php
include "fpmng-skipif.inc";
if (PHP_OS_FAMILY !== 'Linux') die('skip requires Linux /proc');
fpmng_skip_if_pool_type_unsupported('gateway');
?>
--FILE--
<?php
require_once "tester.inc";

/* The gateway sends a served file in windows, and reads the next one only
 * after the client has taken the previous one. Its resident memory therefore
 * stays near its idle value during the transfer. The old code mapped the whole
 * file (evbuffer_add_file()), so VmRSS grew by the file's size as the client
 * read it: about 425 MB for a 500 MB file in the review of #649. The bound
 * here is 64 MB of VmRSS, peak over the transfer, measured on the gateway
 * process from /proc/<pid>/status. */
const FILE_SIZE = 200 * 1024 * 1024;
const LIMIT_KB = 64 * 1024;

$base = sys_get_temp_dir() . '/fpmng-sendfile-mem-' . getmypid();
$root = $base . '/protected';
@mkdir($root, 0777, true);
$big = $root . '/big.bin';
$fh = fopen($big, 'wb');
$block = str_repeat('m', 1024 * 1024);
for ($i = 0; $i < FILE_SIZE / (1024 * 1024); $i++) {
    fwrite($fh, $block);
}
fclose($fh);

file_put_contents($base . '/send.php', '<?php
header("X-Accel-Redirect: " . $_GET["r"]);
echo "worker body";
');

$cfg = <<<EOT
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
[gw]
pool.type = gateway
listen = {{ADDR[http]}}
chdir = $base
http.gateways = 1
http.front_controller = /send.php
http.sendfile_root = $root
http.route[web] = /
[web]
pool.type = fastcgi
listen = {{ADDR}}
chdir = $base
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

/* VmRSS of a process in kB, or -1 when it cannot be read. */
function vm_rss_kb(int $pid): int
{
    $status = @file_get_contents("/proc/$pid/status");
    return ($status !== false && preg_match('/^VmRSS:\s+(\d+) kB/m', $status, $m)) ? (int) $m[1] : -1;
}

$tester = new FPM\Tester($cfg, '<?php');
try {
    $tester->start();
    $tester->expectLogStartNotices();
    $master = $tester->getPid();
    $http = $tester->getAddr('ipv4', '[http]');
    $host = preg_replace('/:\d+$/', '', $http);
    $port = (int) substr($http, strrpos($http, ':') + 1);

    $gw = 0;
    for ($i = 0; $i < 100 && $gw === 0; $i++) {
        $gw = gateway_pid($master);
        usleep(20000);
    }
    if ($gw === 0) {
        throw new RuntimeException('gateway process not found');
    }
    $idle = vm_rss_kb($gw);

    $sock = fsockopen($host, $port, $errno, $errstr, 5);
    if ($sock === false) {
        throw new RuntimeException("connect failed: $errstr ($errno)");
    }
    stream_set_timeout($sock, 10);
    fwrite($sock, "GET /?r=%2Fbig.bin HTTP/1.1\r\nHost: test\r\nConnection: close\r\n\r\n");

    $head = '';
    while (($line = fgets($sock)) !== false && $line !== "\r\n") {
        $head .= $line;
    }
    if (!str_starts_with($head, 'HTTP/1.1 200 ')) {
        throw new RuntimeException("unexpected status line: " . strtok($head, "\r\n"));
    }
    if (!preg_match('/^Content-Length: ' . FILE_SIZE . '\r$/mi', $head)) {
        throw new RuntimeException('Content-Length is not the file size');
    }

    /* A slow reader: 64 KB, then a short pause, so the transfer takes a few
     * seconds and the gateway is sampled while the body is still being sent. */
    $peak = $idle;
    $got = 0;
    while ($got < FILE_SIZE) {
        $chunk = fread($sock, 64 * 1024);
        if ($chunk === false || $chunk === '') {
            break;
        }
        $got += strlen($chunk);
        $peak = max($peak, vm_rss_kb($gw));
        usleep(1000);
    }
    fclose($sock);

    if ($got !== FILE_SIZE) {
        throw new RuntimeException("body is $got bytes, expected " . FILE_SIZE);
    }
    if ($peak > LIMIT_KB) {
        throw new RuntimeException("gateway peak VmRSS $peak kB (idle $idle kB) is above the " . LIMIT_KB . ' kB bound');
    }
    echo "memory bounded: ok\n";
    echo "Done\n";
} finally {
    $tester->terminate();
    $tester->close();
    @unlink($big);
    @unlink($base . '/send.php');
    @rmdir($root);
    @rmdir($base);
}
?>
--EXPECT--
memory bounded: ok
Done
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>

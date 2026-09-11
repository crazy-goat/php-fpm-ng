--TEST--
FPM http gateway: http.read_timeout cuts off a client that trickles its request (task 031)
--SKIPIF--
<?php
include "fpmng-skipif.inc";
fpmng_skip_if_pool_type_unsupported('http');
?>
--FILE--
<?php

require_once "tester.inc";

// Task 031, acceptance criterion 1: a client sending one byte at a time,
// never going fully idle, must be cut off within a bounded, documented time
// -- http.read_timeout. The deadline is one budget for the whole client-side
// read of the first request on a connection (headers + body), armed at
// accept and disarmed when the request is fully assembled; see
// struct fpm_http_read_deadline_s in fpm_http.c.

$docroot = __DIR__;

$config = <<<EOT
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
process_control_timeout = 5
[read]
listen = {{ADDR[fastcgi]}}
pool.type = http
pm = static
pm.max_children = 1
chdir = $docroot
http.listen = {{ADDR[http]}}
http.read_timeout = 2000
EOT;

$tester = new FPM\Tester($config, '<?php echo "ok";');
$tester->start();
$tester->expectLogStartNotices();

// The docroot is the tests directory (chdir above); the worker script is the
// tester's own source file, addressed by basename -- the pattern
// http-tls-chain.phpt already uses.
$script = '/' . basename($tester->makeSourceFile());

$httpAddr = $tester->getAddr('ipv4', '[http]');
[$host, $port] = explode(':', $httpAddr);

// Baseline: a normal request answers 200, proving the listener is up before
// we start the trickle (otherwise the assertions below could pass for the
// wrong reason -- a gateway that never came up also never answers).
$fp = fsockopen($host, (int) $port, $errno, $errstr, 5);
if (!$fp) {
    echo "FAIL: baseline connect: $errstr ($errno)\n";
    exit(1);
}
fwrite($fp, "GET $script HTTP/1.1\r\nHost: $host\r\nConnection: close\r\n\r\n");
$baseline = '';
while (!feof($fp)) {
    $baseline .= fgets($fp);
}
fclose($fp);
if (!str_starts_with($baseline, 'HTTP/1.1 200') && !str_starts_with($baseline, 'HTTP/1.0 200')) {
    echo "FAIL: baseline request did not return 200:\n$baseline\n";
    exit(1);
}

// Trickle: send one header byte every 100 ms. With read_timeout = 2000 ms the
// gateway must close the connection roughly two seconds after the request
// started, long before the (never completed) 60-second header block.
$fp = fsockopen($host, (int) $port, $errno, $errstr, 5);
if (!$fp) {
    echo "FAIL: trickle connect: $errstr ($errno)\n";
    exit(1);
}
stream_set_blocking($fp, false);

$request = "GET $script HTTP/1.1\r\nHost: $host\r\nContent-Length: 1\r\n\r\n";
$sent = 0;
$start = microtime(true);
$closed = false;

while (microtime(true) - $start < 20) {
    // Watch for EOF (the gateway closing on the deadline) in parallel with
    // the trickle: once the deadline fires, writes may keep succeeding
    // (kernel buffers, RST not yet arrived), so a write-only loop would
    // never notice.
    $read = [$fp];
    $write = $except = null;
    if (stream_select($read, $write, $except, 0, 100000) > 0) {
        $data = @fread($fp, 8192);
        if ($data === '' || $data === false) {
            $closed = true; // EOF: the gateway cut us off
            break;
        }
    }
    if ($sent < strlen($request)) {
        $n = @fwrite($fp, $request[$sent]);
        if ($n === 1) {
            $sent++;
        }
    }
}
$elapsed = microtime(true) - $start;
fclose($fp);

if (!$closed) {
    echo "FAIL: trickling client was still connected after 20 s\n";
    exit(1);
}
if ($elapsed > 10) {
    echo sprintf("FAIL: cut off too late, %.1f s (read_timeout = 2000 ms)\n", $elapsed);
    exit(1);
}
if ($elapsed < 1) {
    echo sprintf("FAIL: cut off suspiciously early, %.1f s -- wrong reason?\n", $elapsed);
    exit(1);
}

$tester->terminate();
$tester->expectLogTerminatingNotices();
$tester->close();

?>
Done
--EXPECT--
Done
--CLEAN--
<?php
require_once "tester.inc";
FPM\Tester::clean();
?>

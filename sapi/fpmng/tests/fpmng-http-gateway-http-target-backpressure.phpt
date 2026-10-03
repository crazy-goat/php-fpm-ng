--TEST--
FPM http gateway: a non-reading client slows an http-direct target down too (issue #596)
--SKIPIF--
<?php
include "fpmng-skipif.inc";
fpmng_skip_if_pool_type_unsupported('gateway');
fpmng_skip_if_pool_type_unsupported('http-direct');
?>
--FILE--
<?php

require_once "tester.inc";

/* The HTTP/1.1 client transport (fpm_http_client.c) feeds the same
 * fpm_http_stdout() as the FastCGI one, so it gets the same flow control. Same
 * measurement as fpmng-http-gateway-backpressure.phpt: the target's script
 * counts the MiB it has produced in a file; a client that does not read must
 * leave the count well short of the end, and one that then reads must get the
 * body byte-exact. The route is a pool.type = http-direct target. Neither
 * backpressure test name may be a prefix of the other: Tester::clean() removes
 * files by name prefix. */

const MIB = 1048576;
const TOTAL = 128;
/* The limit is 1 MiB; the kernel buffers on three hops add a few MiB (6-7
 * observed). 16 MiB still fails a gateway that keeps tens of MiB per client. */
const STALL_MAX = 16;

$docroot = __DIR__ . '/fpmng-backpressure-http-docroot';
@mkdir($docroot, 0700, true);
file_put_contents($docroot . '/index.php', <<<'PHP'
<?php
$mib = (int) $_GET['mib'];
$progress = $_GET['p'];
header('Content-Type: application/octet-stream');
for ($i = 0; $i < $mib; $i++) {
    echo str_repeat(chr(65 + $i % 26), 1048576);
    file_put_contents($progress, (string) ($i + 1));
}
PHP);

$config = <<<EOT
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
[gw]
pool.type = gateway
listen = {{ADDR[http]}}
chdir = $docroot
http.gateways = 1
http.response_buffer = 1M
http.route[direct] = /
operator.status_listen = {{ADDR[operator]}}
[direct]
listen = {{ADDR[direct]}}
pm = static
pm.max_children = 1
pool.type = http-direct
chdir = $docroot
http.front_controller = /index.php
; Without streaming the direct pool holds the whole response until the script
; ends, and the script would finish whatever the gateway does.
http.stream = yes
; The gateway pausing makes the target block in its write, and that blocked
; time counts against this total budget (default 10000 ms); the test holds the
; client back for longer than that. See fpmng-http-gateway-stream-budget.phpt.
http.stream_write_timeout = 120000
EOT;

$tester = new FPM\Tester($config, '<?php echo "unused";');
$tester->start();
$tester->expectLogStartNotices();
[$host, $port] = explode(':', $tester->getAddr('ipv4', '[http]'));

function progress(string $file): int
{
    return (int) @file_get_contents($file);
}

function settle(string $file): int
{
    $last = -1;
    $since = microtime(true);
    while (microtime(true) - $since < 1.5) {
        $now = progress($file);
        if ($now !== $last) {
            $last = $now;
            $since = microtime(true);
        }
        usleep(100000);
    }
    return $last;
}

function open($host, $port, string $pf)
{
    $ctx = stream_context_create(['socket' => ['so_rcvbuf' => 4096]]);
    $fp = stream_socket_client("tcp://$host:$port", $errno, $errstr, 5, STREAM_CLIENT_CONNECT, $ctx);
    if (!$fp) {
        echo "FAIL: connect: $errstr ($errno)\n";
        exit(1);
    }
    $q = http_build_query(['mib' => TOTAL, 'p' => $pf]);
    fwrite($fp, "GET /index.php?$q HTTP/1.0\r\nHost: $host\r\n\r\n");
    return $fp;
}

$tmp = sys_get_temp_dir() . '/fpmng-backpressure-http-p-' . getmypid();

/* [paused] + [resume] */
@unlink("$tmp-a");
$fp = open($host, $port, "$tmp-a");
$stalled = settle("$tmp-a");
if ($stalled < 1 || $stalled > STALL_MAX) {
    echo "FAIL: [paused] target is at $stalled of " . TOTAL . " MiB with the client not reading\n";
    exit(1);
}
echo "paused: held back\n";

$expected = hash_init('md5');
for ($i = 0; $i < TOTAL; $i++) {
    hash_update($expected, str_repeat(chr(65 + $i % 26), MIB));
}
$got = hash_init('md5');
$head = '';
$bodyBytes = 0;
stream_set_timeout($fp, 20);
while (!feof($fp)) {
    $data = fread($fp, 262144);
    if ($data === false || ($data === '' && stream_get_meta_data($fp)['timed_out'])) {
        echo "FAIL: [resume] read stalled after $bodyBytes bytes\n";
        exit(1);
    }
    if ($head !== null) {
        $head .= $data;
        $pos = strpos($head, "\r\n\r\n");
        if ($pos === false) {
            continue;
        }
        $data = substr($head, $pos + 4);
        $head = null;
    }
    hash_update($got, $data);
    $bodyBytes += strlen($data);
}
fclose($fp);
if ($bodyBytes !== TOTAL * MIB || hash_final($got) !== hash_final($expected)) {
    echo "FAIL: [resume] body is $bodyBytes bytes, want " . TOTAL * MIB . " byte-exact\n";
    exit(1);
}
echo "resume: byte-exact\n";

/* [closed]: the client leaves while the upstream is paused; the target must
 * still finish its script, so the paused read was resumed and drained. */
@unlink("$tmp-b");
$fp = open($host, $port, "$tmp-b");
$stalled = settle("$tmp-b");
if ($stalled < 1 || $stalled > STALL_MAX) {
    echo "FAIL: [closed] target is at $stalled MiB before the disconnect\n";
    exit(1);
}
fclose($fp);
$start = microtime(true);
while (progress("$tmp-b") !== TOTAL && microtime(true) - $start < 20) {
    usleep(100000);
}
if (progress("$tmp-b") !== TOTAL) {
    echo "FAIL: [closed] target stuck at " . progress("$tmp-b") . " MiB after the client left\n";
    exit(1);
}
echo "closed: drained\n";

@unlink("$tmp-a");
@unlink("$tmp-b");
$tester->terminate();
$tester->expectLogTerminatingNotices();
$tester->close();

?>
Done
--EXPECT--
paused: held back
resume: byte-exact
closed: drained
Done
--CLEAN--
<?php
require_once "tester.inc";
@unlink(__DIR__ . '/fpmng-backpressure-http-docroot/index.php');
@rmdir(__DIR__ . '/fpmng-backpressure-http-docroot');
FPM\Tester::clean();
?>

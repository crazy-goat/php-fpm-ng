--TEST--
FPM http gateway: flow control spends a streaming target's stream_write_timeout; response_buffer = 0 does not (issue #596)
--SKIPIF--
<?php
include "fpmng-skipif.inc";
fpmng_skip_if_pool_type_unsupported('gateway');
fpmng_skip_if_pool_type_unsupported('http-direct');
?>
--FILE--
<?php

require_once "tester.inc";

/* Pins the documented interaction (docs/gateway.md): while a gateway holds the
 * response back from a slow client, the streaming http-direct target is
 * blocked in its write, and that time counts against its total
 * http.stream_write_timeout budget. Two gateways share one target:
 *   - [limited]   http.response_buffer = 1M: a client that does not read for
 *     longer than the budget gets a cut response (fewer bytes than the body);
 *   - [unlimited] http.response_buffer = 0: the gateway takes everything, the
 *     target never blocks, and the same slow client gets the whole body.
 * If the default ever changes so that the first case no longer truncates, this
 * test and the documentation must be revisited together. */

const MIB = 1048576;
const TOTAL = 128;

$docroot = __DIR__ . '/fpmng-stream-budget-docroot';
@mkdir($docroot, 0700, true);
file_put_contents($docroot . '/index.php', <<<'PHP'
<?php
header('Content-Type: application/octet-stream');
for ($i = 0; $i < (int) $_GET['mib']; $i++) {
    echo str_repeat(chr(65 + $i % 26), 1048576);
}
PHP);

$config = <<<EOT
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
[limited]
pool.type = gateway
listen = {{ADDR[limited]}}
chdir = $docroot
http.gateways = 1
http.response_buffer = 1M
http.route[direct] = /
[unlimited]
pool.type = gateway
listen = {{ADDR[unlimited]}}
chdir = $docroot
http.gateways = 1
http.response_buffer = 0
http.route[direct] = /
[direct]
listen = {{ADDR[direct]}}
pm = static
pm.max_children = 2
pool.type = http-direct
chdir = $docroot
http.front_controller = /index.php
http.stream = yes
; Short on purpose: the client below does not read for longer than this.
http.stream_write_timeout = 2000
EOT;

$tester = new FPM\Tester($config, '<?php echo "unused";');
$tester->start();
$tester->expectLogStartNotices();

function slowGet(string $addr): int
{
    [$host, $port] = explode(':', $addr);
    $ctx = stream_context_create(['socket' => ['so_rcvbuf' => 4096]]);
    $fp = stream_socket_client("tcp://$host:$port", $errno, $errstr, 5, STREAM_CLIENT_CONNECT, $ctx);
    if (!$fp) {
        echo "FAIL: connect: $errstr ($errno)\n";
        exit(1);
    }
    fwrite($fp, "GET /index.php?mib=" . TOTAL . " HTTP/1.0\r\nHost: $host\r\n\r\n");
    sleep(5); /* longer than http.stream_write_timeout = 2000 */
    stream_set_timeout($fp, 20);
    $head = '';
    $body = 0;
    while (!feof($fp)) {
        $data = fread($fp, 262144);
        if ($data === false || ($data === '' && stream_get_meta_data($fp)['timed_out'])) {
            break;
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
        $body += strlen($data);
    }
    fclose($fp);
    return $body;
}

$got = slowGet($tester->getAddr('ipv4', '[limited]'));
echo $got > 0 && $got < TOTAL * MIB ? "limited: cut\n" : "FAIL: [limited] got $got of " . TOTAL * MIB . " bytes\n";

$got = slowGet($tester->getAddr('ipv4', '[unlimited]'));
echo $got === TOTAL * MIB ? "unlimited: complete\n" : "FAIL: [unlimited] got $got of " . TOTAL * MIB . " bytes\n";

$tester->terminate();
$tester->expectLogTerminatingNotices();
$tester->close();

?>
Done
--EXPECTF--
limited: cut
unlimited: complete
Done
--CLEAN--
<?php
require_once "tester.inc";
@unlink(__DIR__ . '/fpmng-stream-budget-docroot/index.php');
@rmdir(__DIR__ . '/fpmng-stream-budget-docroot');
FPM\Tester::clean();
?>

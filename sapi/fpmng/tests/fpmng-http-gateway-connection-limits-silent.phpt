--TEST--
FPM http gateway: a connection that never completes its request frees its http.max_connections slot (issue #686)
--SKIPIF--
<?php
include "fpmng-skipif.inc";
fpmng_skip_if_pool_type_unsupported('gateway');
?>
--FILE--
<?php

require_once "tester.inc";

/* Issue #686: http.max_connections = 3 on a gateway with http.read_timeout =
 * 1000. Three clients send an incomplete request and stay open. The gateway
 * closes each one when the first-request deadline expires, but the peer never
 * sends the byte or FIN that the EOF watcher waits for. The slots must still
 * come back: a fourth, complete request is answered. The 400 path, which has
 * no deadline at all, is in fpmng-http-gateway-connection-limits-malformed.phpt. */

$cfg = <<<EOT
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
process_control_timeout = 5
[gw]
pool.type = gateway
listen = {{ADDR[http]}}
chdir = __DIR__
http.gateways = 1
http.read_timeout = 1000
http.keepalive_timeout = 10000
http.max_connections = 3
http.route[app] = /
[app]
pool.type = fastcgi
listen = {{ADDR[app]}}
pm = static
pm.max_children = 4
chdir = __DIR__
EOT;
$cfg = str_replace('__DIR__', __DIR__, $cfg);

$tester = new FPM\Tester($cfg, '<?php echo "ok";');
$tester->start();
$tester->expectLogStartNotices();

$script = '/' . basename($tester->makeSourceFile());
$httpAddr = $tester->getAddr('ipv4', '[http]');
[$host, $port] = explode(':', $httpAddr);

function connect(string $host, string $port)
{
    $fp = stream_socket_client("tcp://$host:$port", $errno, $errstr, 5);
    if (!$fp) {
        echo "FAIL: connect: $errstr ($errno)\n";
        exit(1);
    }
    return $fp;
}

/* The first line of the next response, 'EOF' when the peer closed the
 * socket, or null when nothing arrives in $max seconds. */
function statusLine($fp, float $max): ?string
{
    $start = microtime(true);
    $buf = '';
    stream_set_blocking($fp, false);
    while (microtime(true) - $start < $max) {
        $r = [$fp];
        $w = $e = null;
        if (stream_select($r, $w, $e, 0, 50000) > 0) {
            $data = @fread($fp, 8192);
            if ($data === '' || $data === false) {
                return 'EOF';
            }
            $buf .= $data;
            $nl = strpos($buf, "\r\n");
            if ($nl !== false) {
                return substr($buf, 0, $nl);
            }
        }
    }
    return null;
}

/* A complete request on a new connection, which must get a 200 while the
 * silent connections are still open. */
function completeRequestAnswered(string $host, string $port, string $script): void
{
    $fp = connect($host, $port);
    fwrite($fp, "GET $script HTTP/1.1\r\nHost: $host\r\n\r\n");
    $line = statusLine($fp, 3);
    fclose($fp);
    if ($line === null || !str_starts_with($line, 'HTTP/1.1 200')) {
        echo "FAIL: the fourth client answered: " . var_export($line, true) . "\n";
        exit(1);
    }
}

/* Three clients send a request line and go quiet. */
$silent = [];
for ($i = 0; $i < 3; $i++) {
    $silent[$i] = connect($host, $port);
    fwrite($silent[$i], "GET $script HTTP/1.1\r\n");
}
foreach ($silent as $i => $fp) {
    $line = statusLine($fp, 3);
    if ($line !== 'EOF') {
        echo "FAIL: client $i got " . var_export($line, true) . ", want EOF at http.read_timeout\n";
        exit(1);
    }
}
echo "silent: 3 closed at http.read_timeout\n";
completeRequestAnswered($host, $port, $script);
echo "silent: fourth request answered\n";

foreach ($silent as $fp) {
    fclose($fp);
}

$tester->terminate();
$tester->expectLogTerminatingNotices();
$tester->close();

?>
Done
--EXPECT--
silent: 3 closed at http.read_timeout
silent: fourth request answered
Done
--CLEAN--
<?php
require_once "tester.inc";
FPM\Tester::clean();
?>

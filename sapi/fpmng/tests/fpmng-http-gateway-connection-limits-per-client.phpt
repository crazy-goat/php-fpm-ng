--TEST--
FPM http gateway: http.max_connections_per_client refuses a client's extra connection (issue #686)
--SKIPIF--
<?php
include "fpmng-skipif.inc";
fpmng_skip_if_pool_type_unsupported('gateway');
?>
--FILE--
<?php

require_once "tester.inc";

/* Issue #686: http.max_connections_per_client = 2 on a gateway. Every client
 * here is 127.0.0.1, so the per-client count is the whole test. The third
 * connection is refused: it is closed before it is served, or it is answered
 * 503 and closed. It is never answered 200. When one of the first two leaves,
 * the next connection from the same address is served. */

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
http.read_timeout = 3000
http.keepalive_timeout = 10000
http.max_connections = 8
http.max_connections_per_client = 2
http.route[app] = /
[app]
pool.type = fastcgi
listen = {{ADDR[app]}}
pm = static
pm.max_children = 2
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

/* The first line of the next response, or null when none arrives in $max
 * seconds. "EOF" when the peer closes first. */
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

/* True when the peer closes within $max seconds, whatever it sent before. */
function closedWithin($fp, float $max): bool
{
    $start = microtime(true);
    stream_set_blocking($fp, false);
    while (microtime(true) - $start < $max) {
        $r = [$fp];
        $w = $e = null;
        if (stream_select($r, $w, $e, 0, 100000) > 0) {
            $data = @fread($fp, 8192);
            if ($data === '' || $data === false) {
                return true;
            }
        }
    }
    return false;
}

$request = "GET $script HTTP/1.1\r\nHost: $host\r\n\r\n";

/* [first] two connections from one address are served. */
$a = connect($host, $port);
fwrite($a, $request);
$b = connect($host, $port);
fwrite($b, $request);
foreach (['a' => $a, 'b' => $b] as $name => $fp) {
    $line = statusLine($fp, 3);
    if ($line === null || !str_starts_with($line, 'HTTP/1.1 200')) {
        echo "FAIL: [first] client $name answered: " . var_export($line, true) . "\n";
        exit(1);
    }
}
echo "first: 2 served\n";

/* [third] the third connection from the same address is refused. */
$c = connect($host, $port);
fwrite($c, $request);
$line = statusLine($c, 3);
if ($line === null) {
    echo "FAIL: [third] no answer and no close for the refused connection\n";
    exit(1);
}
if ($line !== 'EOF' && !str_starts_with($line, 'HTTP/1.1 503')) {
    echo "FAIL: [third] refused connection answered: $line\n";
    exit(1);
}
if (!closedWithin($c, 3)) {
    echo "FAIL: [third] refused connection is still open after its answer\n";
    exit(1);
}
fclose($c);
echo "third: refused\n";

/* [freed] one of the first two leaves; the next connection is served. */
fclose($a);
usleep(500000);
$d = connect($host, $port);
fwrite($d, $request);
$line = statusLine($d, 3);
if ($line === null || !str_starts_with($line, 'HTTP/1.1 200')) {
    echo "FAIL: [freed] connection after a client left answered: " . var_export($line, true) . "\n";
    exit(1);
}
echo "freed: served\n";
fclose($b);
fclose($d);

$tester->terminate();
$tester->expectLogTerminatingNotices();
$tester->close();

?>
Done
--EXPECT--
first: 2 served
third: refused
freed: served
Done
--CLEAN--
<?php
require_once "tester.inc";
FPM\Tester::clean();
?>

--TEST--
FPM http gateway: http.max_connections caps the connections one gateway process holds (issue #686)
--SKIPIF--
<?php
include "fpmng-skipif.inc";
fpmng_skip_if_pool_type_unsupported('gateway');
?>
--FILE--
<?php

require_once "tester.inc";
require_once "fpmng-operator.inc";

/* Issue #686: http.max_connections = 3 on a gateway. Ten clients connect and
 * send a request at once. Exactly three may be served. The other seven wait in
 * the listen backlog: they are not accepted, so connections_open stays at 3.
 * When one served client leaves, exactly one waiting client is accepted and
 * served. Every number below is a count of responses that arrived, so a gate
 * that overshoots by one accept batch cannot pass for the cap. */

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
http.max_connections = 3
http.route[app] = /
operator.status_listen = {{ADDR[operator]}}
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
$operator = $tester->getListen('{{ADDR[operator]}}');
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

/* True when the socket has bytes, or EOF, waiting to be read. */
function readable($fp): bool
{
    $r = [$fp];
    $w = $e = null;
    return stream_select($r, $w, $e, 0, 0) > 0;
}

/* The first line of the next response, or null when none arrives in $max
 * seconds. */
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

function connectionsOpen(string $operator): int
{
    $decoded = json_decode(fpmng_operator_body($operator, '/status'), true, flags: JSON_THROW_ON_ERROR);
    foreach ($decoded['pools'] as $row) {
        if (!array_key_exists('target', $row) && ($row['name'] ?? '') === 'gw') {
            return (int) $row['connections_open'];
        }
    }
    echo "FAIL: no pool row for gw\n";
    exit(1);
}

$request = "GET $script HTTP/1.1\r\nHost: $host\r\n\r\n";

/* [burst] ten clients, three may be served. */
$clients = [];
for ($i = 0; $i < 10; $i++) {
    $clients[$i] = connect($host, $port);
    fwrite($clients[$i], $request);
}
usleep(1500000);
$served = [];
$waiting = [];
foreach ($clients as $i => $fp) {
    if (readable($fp)) {
        $served[] = $i;
    } else {
        $waiting[] = $i;
    }
}
if (count($served) !== 3) {
    echo sprintf("FAIL: [burst] %d clients served, want 3 (http.max_connections = 3)\n", count($served));
    exit(1);
}
foreach ($served as $i) {
    $line = statusLine($clients[$i], 2);
    if ($line === null || !str_starts_with($line, 'HTTP/1.1 200')) {
        echo "FAIL: [burst] served client $i answered: " . var_export($line, true) . "\n";
        exit(1);
    }
}
$open = connectionsOpen($operator);
if ($open !== 3) {
    echo "FAIL: [burst] connections_open is $open, want 3\n";
    exit(1);
}
echo "burst: 3 served\n";
echo "open: 3\n";

/* [freed] one served client leaves: one waiting client may now be accepted. */
fclose($clients[$served[0]]);
unset($clients[$served[0]]);
usleep(1500000);
$more = [];
foreach ($waiting as $i) {
    if (readable($clients[$i])) {
        $more[] = $i;
    }
}
if (count($more) !== 1) {
    echo sprintf("FAIL: [freed] %d waiting clients served after one left, want 1\n", count($more));
    exit(1);
}
$line = statusLine($clients[$more[0]], 2);
if ($line === null || !str_starts_with($line, 'HTTP/1.1 200')) {
    echo "FAIL: [freed] the accepted client answered: " . var_export($line, true) . "\n";
    exit(1);
}
if (connectionsOpen($operator) !== 3) {
    echo "FAIL: [freed] connections_open is not 3 after the slot was reused\n";
    exit(1);
}
echo "freed: 1 more served\n";

foreach ($clients as $fp) {
    fclose($fp);
}

$tester->terminate();
$tester->expectLogTerminatingNotices();
$tester->close();

?>
Done
--EXPECT--
burst: 3 served
open: 3
freed: 1 more served
Done
--CLEAN--
<?php
require_once "tester.inc";
FPM\Tester::clean();
?>

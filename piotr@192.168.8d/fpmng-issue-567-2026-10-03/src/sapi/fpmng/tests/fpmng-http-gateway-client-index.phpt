--TEST--
fpm-ng: the gateway client index keeps local, proxied, disconnected, and churned keep-alive connections exact (issue #490)
--SKIPIF--
<?php
include "fpmng-skipif.inc";
fpmng_skip_if_pool_type_unsupported('gateway');
?>
--FILE--
<?php
require_once "tester.inc";
require_once "fpmng-operator.inc";

function check(bool $condition, string $message): void
{
    if (!$condition) throw new RuntimeException($message);
}

function connectGateway(string $http)
{
    $fp = @stream_socket_client("tcp://$http", $errno, $error, 5);
    check((bool) $fp, "gateway connect: $error");
    stream_set_timeout($fp, 5);
    return $fp;
}

function sendRequest($fp, string $path): void
{
    check(fwrite($fp, "GET $path HTTP/1.1\r\nHost: gateway\r\n\r\n") !== false,
        "write $path failed");
}

function readResponse($fp, string $path): array
{
    $line = fgets($fp);
    check($line && preg_match('#^HTTP/1\.1 (\d+) #', $line, $m),
        "$path status: " . var_export($line, true));
    $headers = [];
    while (($line = fgets($fp)) !== false && $line !== "\r\n") {
        [$name, $value] = explode(':', $line, 2);
        $headers[strtolower($name)] = trim($value);
    }
    $length = isset($headers['content-length']) ? (int) $headers['content-length'] : 0;
    $body = '';
    while (strlen($body) < $length) {
        $chunk = fread($fp, $length - strlen($body));
        if ($chunk === false || $chunk === '') {
            break;
        }
        $body .= $chunk;
    }
    check(strlen($body) === $length, "$path short body: " . strlen($body) . " of $length");
    return [(int) $m[1], $body];
}

function request($fp, string $path): array
{
    sendRequest($fp, $path);
    return readResponse($fp, $path);
}

function metric(string $metrics, string $pattern): ?int
{
    return preg_match($pattern, $metrics, $m) ? (int) $m[1] : null;
}

function waitForGauge(string $operator, string $pattern, int $expected, string $what): void
{
    $deadline = microtime(true) + 10;
    $value = null;
    do {
        $metrics = fpmng_operator_body($operator, '/metrics');
        $value = metric($metrics, $pattern);
        if ($value === $expected) {
            return;
        }
        usleep(20000);
    } while (microtime(true) < $deadline);
    throw new RuntimeException("$what: expected $expected, got " . var_export($value, true)
        . "\n" . $GLOBALS['fpmng_operator_last_page']);
}

$docroot = sys_get_temp_dir() . '/fpmng-gw-client-index-' . getmypid();
@mkdir($docroot, 0700, true);
file_put_contents($docroot . '/index.php', <<<'PHP'
<?php
$uri = $_SERVER['REQUEST_URI'] ?? '/';
if (str_starts_with($uri, '/app/slow')) {
    sleep(3);
}
$body = 'app:' . $uri;
header('Content-Type: text/plain');
header('Content-Length: ' . strlen($body));
echo $body;
PHP);

$config = <<<CFG
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
[gw]
pool.type = gateway
listen = {{ADDR[http]}}
chdir = $docroot
http.gateways = 1
http.read_timeout = 0
http.idle_timeout = 100
http.front_controller = /index.php
http.route[app] = /app
ping.path = /ping
ping.response = pong
operator.metrics_listen = {{ADDR[operator]}}
[app]
pool.type = fastcgi
listen = {{ADDR[app]}}
pm = static
pm.max_children = 1
chdir = $docroot
catch_workers_output = yes
php_admin_value[max_execution_time] = 0
php_admin_value[display_errors] = 0
CFG;

$connections = [];
$hold = null;
$tester = new FPM\Tester($config, '<?php echo "unused";');
try {
    $tester->start();
    $tester->expectLogStartNotices();
    $http = $tester->getAddr('ipv4', '[http]');
    $operator = $tester->getListen('{{ADDR[operator]}}');
    $connectionsOpen = '/fpmng_gateway_connections_open\{pool="gw"\} (\d+)/';
    $upstreamsUsed = '/fpmng_gateway_upstreams_used\{pool="gw",target="app"\} (\d+)/';

    /* Enough sockets to force at least one index growth, all kept alive after
     * one local request. Sequential establishment also makes the first socket
     * deterministic rather than dependent on accept scheduling. */
    $count = 64;
    for ($i = 0; $i < $count; $i++) {
        $connections[$i] = connectGateway($http);
        [$status, $body] = request($connections[$i], '/ping');
        check($status === 200 && $body === 'pong', "initial ping $i: $status $body");
    }
    waitForGauge($operator, $connectionsOpen, $count, 'initial connection index');
    echo "indexed-and-gauge: ok\n";

    /* The same node serves local and proxied answers on an established
     * connection; neither completion may add or remove it. */
    [$status, $body] = request($connections[0], '/ping');
    check($status === 200 && $body === 'pong', 'reused ping failed');
    [$status, $body] = request($connections[0], '/app/one');
    check($status === 200 && $body === 'app:/app/one', 'reused proxy request failed');
    [$status, $body] = request($connections[0], '/ping');
    check($status === 200 && $body === 'pong', 'ping after proxy failed');
    waitForGauge($operator, $connectionsOpen, $count, 'keep-alive local/proxy reuse');
    echo "keepalive-local-and-proxied: ok\n";

    /* Remove a middle bucket node, then churn several positions. Replacements
     * must insert into the exact surviving links and the gauge must return to
     * its expected value after every close/open pair. */
    foreach ([17, 1, 30, 63, 0, 17] as $index) {
        fclose($connections[$index]);
        unset($connections[$index]);
        waitForGauge($operator, $connectionsOpen, $count - 1, "close index $index");
        $connections[$index] = connectGateway($http);
        [$status, $body] = request($connections[$index], '/ping');
        check($status === 200 && $body === 'pong', "replacement $index failed");
        waitForGauge($operator, $connectionsOpen, $count, "replacement index $index");
    }
    echo "out-of-order-close-and-reuse: ok\n";

    /* A live proxied request whose client disappears must detach the request
     * without leaking either the client node or its upstream reservation. */
    $hold = connectGateway($http);
    sendRequest($hold, '/app/slow');
    waitForGauge($operator, $upstreamsUsed, 1, 'in-flight upstream');
    fclose($hold);
    $hold = null;
    waitForGauge($operator, $connectionsOpen, $count, 'in-flight client disconnect');
    waitForGauge($operator, $upstreamsUsed, 0, 'disconnected upstream release');
    [$status, $body] = request($connections[0], '/app/after-disconnect');
    check($status === 200 && $body === 'app:/app/after-disconnect',
        "request after disconnect: $status $body");
    echo "inflight-disconnect-detached: ok\n";

    foreach ($connections as $fp) {
        fclose($fp);
    }
    $connections = [];
    waitForGauge($operator, $connectionsOpen, 0, 'all client closes');
    echo "all-connections-removed: ok\n";
} finally {
    if (is_resource($hold)) {
        fclose($hold);
    }
    foreach ($connections as $fp) {
        if (is_resource($fp)) {
            fclose($fp);
        }
    }
    $tester->terminate();
    $tester->expectLogTerminatingNotices();
    $tester->close();
    @unlink($docroot . '/index.php');
    @rmdir($docroot);
}
echo "Done\n";
?>
--EXPECT--
indexed-and-gauge: ok
keepalive-local-and-proxied: ok
out-of-order-close-and-reuse: ok
inflight-disconnect-detached: ok
all-connections-removed: ok
Done
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>

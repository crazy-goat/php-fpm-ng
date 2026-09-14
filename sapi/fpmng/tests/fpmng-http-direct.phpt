--TEST--
fpm-ng: direct HTTP classic requests, headers, bodies, isolation, limits and keep-alive
--SKIPIF--
<?php include "skipif.inc"; ?>
--FILE--
<?php
require_once "tester.inc";

function check(bool $condition, string $message): void
{
    if (!$condition) throw new RuntimeException($message);
}
function response($fp, bool $head = false): array
{
    $status = fgets($fp);
    check($status !== false, 'missing status');
    $headers = [];
    while (($line = fgets($fp)) !== false && $line !== "\r\n") {
        [$key, $value] = explode(':', trim($line), 2);
        $headers[strtolower($key)][] = trim($value);
    }
    $body = '';
    $length = $head ? 0 : (int) ($headers['content-length'][0] ?? 0);
    while (strlen($body) < $length) {
        $part = fread($fp, $length - strlen($body));
        check($part !== false && $part !== '', 'short body');
        $body .= $part;
    }
    return [$status, $headers, $body];
}
function connectDirect(string $address)
{
    $fp = stream_socket_client("tcp://$address", $errno, $error, 5);
    stream_set_timeout($fp, 5);
    return $fp;
}

$root = __DIR__;
$script = '/fpmng-http-direct-front-' . getmypid() . '.php';
file_put_contents($root . $script, <<<'PHP'
<?php
class PerRequest { public static int $n = 0; }
if (isset($_GET['fatal'])) { missing_function(); }
if (isset($_GET['large'])) { echo str_repeat('x', 8 * 1024 * 1024 + 1); return; }
if (isset($_GET['shutdown'])) {
    register_shutdown_function(function () { echo 'shutdown'; });
    echo 'before-'; return;
}
http_response_code(201);
header('Content-Type: application/json');
header('X-Repeat: first', false);
header('X-Repeat: second', false);
header('Content-Length: 999999');
echo json_encode([
    'n' => ++PerRequest::$n,
    'get' => $_GET, 'post' => $_POST, 'cookie' => $_COOKIE,
    'raw' => file_get_contents('php://input'),
    'server' => $_SERVER,
    'headers' => getallheaders(),
    'alias' => apache_request_headers(),
    'env' => getenv('REQUEST_URI'),
    'finish' => function_exists('fastcgi_finish_request'),
    'conn' => fpm_connection_info(),
]);
PHP);
$port = (int) (getenv('FPMNG_DIRECT_TEST_PORT') ?: 28054);
$cfg = <<<CFG
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
[direct]
listen = 127.0.0.1:$port
pool.type = http-direct
pool.executor = classic
pm = static
pm.max_children = 1
chdir = $root
http.front_controller = $script
http.max_body = 1k
http.read_timeout = 1000
php_admin_value[display_errors] = 0
CFG;
$tester = new FPM\Tester($cfg, '<?php');
try {
    $tester->start();
    $tester->expectLogStartNotices();
    $fp = connectDirect("127.0.0.1:$port");
    $prevAge = -1.0;
    for ($i = 0; $i < 3; $i++) {
        $body = 'field=value' . $i;
        $uri = '/arbitrary/../route?q=' . $i;
        fwrite($fp, "POST $uri HTTP/1.1\r\nHost: example.test\r\nContent-Type: application/x-www-form-urlencoded\r\nContent-Length: " . strlen($body) . "\r\nCookie: token=abc\r\nX-Probe: custom\r\nProxy: attacker\r\nAuthorization: Basic " . base64_encode('user:pass') . "\r\n\r\n$body");
        [$status, $headers, $raw] = response($fp);
        check(str_contains($status, ' 201 '), $status);
        check($headers['x-repeat'] === ['first', 'second'], 'repeated response headers');
        $data = json_decode($raw, true, flags: JSON_THROW_ON_ERROR);
        check($data['n'] === 1, 'class static leaked');
        check($data['get']['q'] === (string) $i && $data['post']['field'] === 'value' . $i, 'query/form mismatch');
        check($data['raw'] === $body && $data['cookie']['token'] === 'abc', 'body/cookie mismatch');
        check($data['server']['REQUEST_URI'] === $uri && $data['env'] === $uri, 'request URI mismatch');
        check($data['server']['SCRIPT_NAME'] === $script && $data['server']['SCRIPT_FILENAME'] === $root . $script, 'client selected a script');
        check($data['server']['HTTP_X_PROBE'] === 'custom' && !isset($data['server']['HTTP_PROXY']), 'header mapping/httpoxy');
        check($data['server']['PHP_AUTH_USER'] === 'user' && $data['server']['PHP_AUTH_PW'] === 'pass', 'basic auth');
        check($data['headers']['X-Probe'] === 'custom' && $data['alias'] === $data['headers'], 'header functions');
        check(!$data['finish'], 'unsafe FastCGI function exposed');
        /* fpm_connection_info() (issue #62): plain transport, no TLS/client
         * cert keys, and the request count/age track THIS connection, never
         * resetting across keep-alive requests on it. */
        $conn = $data['conn'];
        check($conn['transport'] === 'plain', 'plain transport: ' . var_export($conn['transport'], true));
        check(!array_key_exists('tls_protocol', $conn) && !array_key_exists('client_cert_verified', $conn),
            'plain connection exposed TLS/client-cert keys');
        check($conn['peer_addr'] === '127.0.0.1', 'peer_addr: ' . var_export($conn['peer_addr'], true));
        check(is_int($conn['peer_port']) && $conn['peer_port'] > 0, 'peer_port: ' . var_export($conn['peer_port'], true));
        check($conn['requests'] === $i + 1, "requests should be " . ($i + 1) . ", got " . var_export($conn['requests'], true));
        check(is_float($conn['age']) && $conn['age'] >= $prevAge, 'age did not advance: ' . var_export($conn['age'], true));
        $prevAge = $conn['age'];
    }
    fwrite($fp, "HEAD / HTTP/1.1\r\nHost: example.test\r\n\r\n");
    [$status, $headers] = response($fp, true);
    check(str_contains($status, ' 201 '), 'HEAD status');
    fwrite($fp, "GET /?shutdown=1 HTTP/1.1\r\nHost: example.test\r\n\r\n");
    check(response($fp)[2] === 'before-shutdown', 'shutdown or HEAD framing');
    fwrite($fp, "GET /?large=1 HTTP/1.1\r\nHost: example.test\r\n\r\n");
    check(str_contains(response($fp)[0], ' 500 '), 'output limit');
    fwrite($fp, "GET /?fatal=1 HTTP/1.1\r\nHost: example.test\r\n\r\n");
    check(str_contains(response($fp)[0], ' 500 '), 'fatal error status');
    fwrite($fp, "GET /?shutdown=1 HTTP/1.1\r\nHost: example.test\r\nConnection: close\r\n\r\n");
    check(response($fp)[2] === 'before-shutdown', 'worker did not recover');
    fclose($fp);
    echo "requests/keep-alive/isolation: ok\n";

    $fp = connectDirect("127.0.0.1:$port");
    fwrite($fp, "POST / HTTP/1.1\r\nHost: test\r\nTransfer-Encoding: chunked\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\n3\r\nabc\r\n0\r\n\r\n");
    $data = json_decode(response($fp)[2], true, flags: JSON_THROW_ON_ERROR);
    check($data['raw'] === 'abc' && $data['server']['CONTENT_LENGTH'] === '3', 'chunked request');
    /* Isolation (issue #62): a brand new connection on the very worker that
     * just served 3 requests on the previous one starts back at requests=1,
     * not 4 -- nothing about the old connection's fpm_connection_info()
     * state leaked into this one. */
    check($data['conn']['requests'] === 1, 'requests leaked across connections: ' . var_export($data['conn']['requests'], true));
    fclose($fp);
    $fp = connectDirect("127.0.0.1:$port");
    fwrite($fp, "POST / HTTP/1.1\r\nHost: test\r\nContent-Length: 2048\r\nConnection: close\r\n\r\n" . str_repeat('x', 2048));
    check(str_contains(response($fp)[0], ' 413 '), 'body limit');
    fclose($fp);
    $fp = connectDirect("127.0.0.1:$port");
    fwrite($fp, "GET / HTTP/1.1\r\nHost: test\r\nX-Large: " . str_repeat('x', 65536) . "\r\n\r\n");
    check(str_contains(response($fp)[0], ' 4'), 'header limit');
    fclose($fp);
    $fp = connectDirect("127.0.0.1:$port");
    fwrite($fp, "GET / HTTP/1.1\r\nHost:");
    stream_get_contents($fp);
    check(!stream_get_meta_data($fp)['timed_out'] && feof($fp), 'incomplete request did not expire');
    fclose($fp);
    echo "limits/chunked: ok\n";
} finally {
    $tester->terminate();
    $tester->close();
    unlink($root . $script);
}
echo "Done\n";
?>
--EXPECT--
requests/keep-alive/isolation: ok
limits/chunked: ok
Done
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>

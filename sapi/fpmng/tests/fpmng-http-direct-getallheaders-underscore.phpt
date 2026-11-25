--TEST--
fpm-ng: http-direct getallheaders() and apache_request_headers() omit header names that contain "_" (issue #628)
--SKIPIF--
<?php
include "fpmng-skipif.inc";
fpmng_skip_if_pool_type_unsupported('http-direct');
?>
--FILE--
<?php
require_once "tester.inc";

function check(bool $condition, string $message): void
{
    if (!$condition) throw new RuntimeException($message);
}

function request(string $addr, string $raw): array
{
    $fp = stream_socket_client("tcp://$addr", $errno, $error, 5);
    check($fp !== false, "connect to $addr failed: $error");
    stream_set_timeout($fp, 5);
    fwrite($fp, $raw);
    $status = (string) fgets($fp);
    $headers = [];
    while (($line = fgets($fp)) !== false && $line !== "\r\n") {
        [$key, $value] = explode(':', trim($line), 2);
        $headers[strtolower($key)][] = trim($value);
    }
    /* The script sets no Content-Length, so read to EOF (every request asks
     * for Connection: close) and unchunk when the response says it is chunked. */
    $raw = (string) stream_get_contents($fp);
    fclose($fp);
    if (in_array('chunked', $headers['transfer-encoding'] ?? [], true)) {
        $body = '';
        while ($raw !== '') {
            [$size, $raw] = explode("\r\n", $raw, 2);
            $size = (int) hexdec(trim($size));
            if ($size === 0) break;
            $body .= substr($raw, 0, $size);
            $raw = substr($raw, $size + 2);
        }
        return [$status, $body];
    }
    return [$status, $raw];
}

/* Issue #628: the underscore rule of issue #595 drops "X_Custom" from $_SERVER
 * HTTP_* keys and from the gateway's FastCGI parameters, but on classic
 * HTTP-direct getallheaders() and apache_request_headers() read the raw request
 * headers and still returned it. Both functions must now omit such a name, the
 * same way $_SERVER does. Each function is asserted on its own: the alias is
 * registered separately (sapi/fpmng/fpm/fpm_http_direct.c, the names[] loop). */

$root = sys_get_temp_dir() . '/fpmng-direct-getallheaders-' . getmypid();
@mkdir($root, 0700, true);

file_put_contents("$root/env.php", <<<'PHP'
<?php
echo json_encode([
    'all' => getallheaders(),
    'alias' => apache_request_headers(),
    'server' => $_SERVER,
]);
PHP);

$cfg = <<<EOT
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
[direct]
pool.type = http-direct
listen = {{ADDR[http]}}
chdir = $root
pm = static
pm.max_children = 1
http.front_controller = /env.php
http.read_timeout = 10000
php_admin_value[display_errors] = 0
EOT;

$tester = new FPM\Tester($cfg, file_get_contents("$root/env.php"));
$tester->start();
$tester->expectLogStartNotices();
$direct = $tester->getAddr('ipv4', '[http]');

/* "X_Custom" is the only spelling of that name, so no key can collide with it.
 * "X-Proxy-Control" rides along to show the request still reaches the script
 * and that an ordinary header is kept. */
[$status, $body] = request($direct, "GET /env.php HTTP/1.1\r\nHost: h.test\r\n"
    . "X_Custom: v\r\nX-Proxy-Control: kept\r\nConnection: close\r\n\r\n");
check(str_contains($status, ' 200 '), "underscore request status: $status");
$data = json_decode($body, true);
check(is_array($data), "underscore request body: $body");

foreach (['all', 'alias'] as $fn) {
    $names = array_map('strtolower', array_keys($data[$fn]));
    check(!in_array('x_custom', $names, true) && !in_array('x-custom', $names, true),
        "$fn() returned the header with an underscore: $body");
    check(($data[$fn]['X-Proxy-Control'] ?? '') === 'kept',
        "$fn() dropped an ordinary header: $body");
}
check(!isset($data['server']['HTTP_X_CUSTOM']), '$_SERVER got HTTP_X_CUSTOM: ' . $body);
echo "underscore-dropped-getallheaders: ok\n";

/* The collision case from issue #595, seen through the function: "X_Real_IP"
 * would share its CGI key with "X-Real-IP". The function must keep the dashed
 * name and not add the underscore one. */
[$status, $body] = request($direct, "GET /env.php HTTP/1.1\r\nHost: h.test\r\n"
    . "X-Real-IP: 10.0.0.1\r\nX_Real_IP: 6.6.6.6\r\nConnection: close\r\n\r\n");
check(str_contains($status, ' 200 '), "collision request status: $status");
$data = json_decode($body, true);
check(is_array($data), "collision request body: $body");
foreach (['all', 'alias'] as $fn) {
    check(($data[$fn]['X-Real-IP'] ?? '') === '10.0.0.1', "$fn() lost X-Real-IP: $body");
    check(!isset($data[$fn]['X_Real_IP']), "$fn() returned X_Real_IP: $body");
}
echo "underscore-not-listed-beside-dash: ok\n";

$tester->terminate();
$tester->expectLogTerminatingNotices();
$tester->close();

@unlink("$root/env.php");
@rmdir($root);

?>
Done
--EXPECT--
underscore-dropped-getallheaders: ok
underscore-not-listed-beside-dash: ok
Done
--CLEAN--
<?php
require_once "tester.inc";
FPM\Tester::clean();
?>

--TEST--
fpm-ng: http-direct drops a header name with a character outside [A-Za-z0-9-] from $_SERVER and getallheaders() (issue #1002)
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

/* Issue #1002: the mapping to HTTP_* keys (sapi/fpmng/fpm/fpm_http_header_cgi.c)
 * copied '.', ' ' and '[' verbatim. PHP then rewrites '.' and ' ' to '_' and
 * reads '[' as an array index, so "X.Real.IP" overwrote HTTP_X_REAL_IP and
 * "X-Forwarded-For[]" turned HTTP_X_FORWARDED_FOR into an array. Such a name
 * must now get no key, and getallheaders() must not list it either: the
 * gateway's getallheaders() does not show it (fpmng-http-gateway-header-name-charset.phpt). */

$root = sys_get_temp_dir() . '/fpmng-direct-header-name-charset-' . getmypid();
@mkdir($root, 0700, true);

file_put_contents("$root/env.php", <<<'PHP'
<?php
echo json_encode([
    'all' => getallheaders(),
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

/* The dotted name is sent after the dashed one: the order in which a
 * collision would win in $_SERVER. */
[$status, $body] = request($direct, "GET /env.php HTTP/1.1\r\nHost: h.test\r\n"
    . "X-Real-IP: 10.0.0.1\r\nX.Real.IP: 1.2.3.4\r\nConnection: close\r\n\r\n");
check(str_contains($status, ' 200 '), "dotted name status: $status");
$data = json_decode($body, true);
check(is_array($data), "dotted name body: $body");
check(($data['server']['HTTP_X_REAL_IP'] ?? '') === '10.0.0.1',
    'X.Real.IP overrode X-Real-IP in $_SERVER: ' . $body);
check(!array_key_exists('X.Real.IP', $data['all']), 'getallheaders() listed X.Real.IP: ' . $body);
echo "dot-dropped: ok\n";

/* "X-Forwarded-For" is a string when it arrives alone. The "[]" spelling
 * must not turn it into an array. */
[$status, $body] = request($direct, "GET /env.php HTTP/1.1\r\nHost: h.test\r\n"
    . "X-Forwarded-For: 10.0.0.1\r\nX-Forwarded-For[]: 6.6.6.6\r\nConnection: close\r\n\r\n");
check(str_contains($status, ' 200 '), "bracket name status: $status");
$data = json_decode($body, true);
check(is_array($data), "bracket name body: $body");
check(($data['server']['HTTP_X_FORWARDED_FOR'] ?? null) === '10.0.0.1',
    'X-Forwarded-For[] changed HTTP_X_FORWARDED_FOR in $_SERVER: ' . $body);
check(!array_key_exists('X-Forwarded-For[]', $data['all']), 'getallheaders() listed X-Forwarded-For[]: ' . $body);
echo "bracket-dropped: ok\n";

/* A space is rewritten to '_' by PHP, so "X Custom" would land on
 * HTTP_X_CUSTOM. The name has no spelling of its own to lose to. */
[$status, $body] = request($direct, "GET /env.php HTTP/1.1\r\nHost: h.test\r\n"
    . "X Custom: 6.6.6.6\r\nX-Kept: kept\r\nConnection: close\r\n\r\n");
check(str_contains($status, ' 200 '), "space name status: $status");
$data = json_decode($body, true);
check(is_array($data), "space name body: $body");
check(!isset($data['server']['HTTP_X_CUSTOM']), 'a name with a space reached $_SERVER: ' . $body);
check(!array_key_exists('X Custom', $data['all']), 'getallheaders() listed "X Custom": ' . $body);
check(($data['server']['HTTP_X_KEPT'] ?? '') === 'kept', 'http-direct dropped an ordinary header: ' . $body);
echo "space-dropped: ok\n";

/* An empty name would map to the bare key "HTTP_". It must get no key, and
 * getallheaders() must not list it either. */
[$status, $body] = request($direct, "GET /env.php HTTP/1.1\r\nHost: h.test\r\n"
    . ": emptyname\r\nX-Kept: kept\r\nConnection: close\r\n\r\n");
check(str_contains($status, ' 200 '), "empty name status: $status");
$data = json_decode($body, true);
check(is_array($data), "empty name body: $body");
check(!array_key_exists('HTTP_', $data['server']), 'an empty header name reached $_SERVER as HTTP_: ' . $body);
check(!array_key_exists('', $data['all']), 'getallheaders() listed an empty name: ' . $body);
check(($data['server']['HTTP_X_KEPT'] ?? '') === 'kept', 'http-direct dropped an ordinary header: ' . $body);
echo "empty-dropped: ok\n";

$tester->terminate();
$tester->expectLogTerminatingNotices();
$tester->close();

@unlink("$root/env.php");
@rmdir($root);

?>
Done
--EXPECT--
dot-dropped: ok
bracket-dropped: ok
space-dropped: ok
empty-dropped: ok
Done
--CLEAN--
<?php
require_once "tester.inc";
FPM\Tester::clean();
?>

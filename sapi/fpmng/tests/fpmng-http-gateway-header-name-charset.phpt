--TEST--
fpm-ng: the HTTP gateway drops a header name with a character outside [A-Za-z0-9-] before the FastCGI worker (issue #1002)
--SKIPIF--
<?php
include "fpmng-skipif.inc";
fpmng_skip_if_pool_type_unsupported('gateway');
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
    /* The gateway chunks a PHP response (no Content-Length from the script),
     * so read to EOF -- every request here asks for Connection: close -- and
     * unchunk when it says it did. */
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

/* Issue #1002: the gateway's mapping to FastCGI parameter names copied '.',
 * ' ' and '[' verbatim, so the worker got "HTTP_X.REAL.IP" and
 * "HTTP_X-FORWARDED-FOR[]". The worker's getallheaders() reads those
 * parameters back, so it shows what the gateway sent. Such a name must not
 * reach the worker at all. */

$root = sys_get_temp_dir() . '/fpmng-gateway-header-name-charset-' . getmypid();
@mkdir($root, 0700, true);

/* getallheaders() shows the FastCGI parameters the gateway sent; $_SERVER shows
 * what PHP registered from them. The test checks both: a dropped name must be
 * absent from each, and the dashed name must keep its value in $_SERVER. */
file_put_contents("$root/env.php", <<<'PHP'
<?php
echo json_encode([
    'headers' => array_change_key_case(getallheaders(), CASE_LOWER),
    'server' => $_SERVER,
]);
PHP);

$cfg = <<<EOT
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
[gw]
pool.type = gateway
listen = {{ADDR[http]}}
chdir = $root
http.front_controller = /env.php
; One gateway process: the target is pm.max_children = 1, so a second gateway
; would race for its single upstream connection and answer 503. Not asserted.
http.gateways = 1
http.route[web] = /
[web]
pool.type = fastcgi
listen = {{ADDR}}
chdir = $root
pm = static
pm.max_children = 1
EOT;

$tester = new FPM\Tester($cfg, file_get_contents("$root/env.php"));
$tester->start();
$tester->expectLogStartNotices();
$http = $tester->getAddr('ipv4', '[http]');

/* On the gateway path the header the client sends first wins a collision in
 * the worker's $_SERVER (the opposite of http-direct). With the dashed name
 * first, an unfixed build still showed HTTP_X_REAL_IP = 10.0.0.1, so this
 * check could not fail; measured in the round 2 review of #1002. The dotted
 * and bracketed names are therefore sent first. Why the order is reversed was
 * not established. */
[$status, $body] = request($http, "GET /env.php HTTP/1.1\r\nHost: h.test\r\n"
    . "X.Real.IP: 1.2.3.4\r\nX-Real-IP: 10.0.0.1\r\nConnection: close\r\n\r\n");
check(str_contains($status, ' 200 '), "dotted name status: $status");
$data = json_decode($body, true);
check(is_array($data), "dotted name body: $body");
check(($data['server']['HTTP_X_REAL_IP'] ?? '') === '10.0.0.1',
    'X.Real.IP overrode X-Real-IP in the worker $_SERVER: ' . $body);
check(($data['headers']['x-real-ip'] ?? '') === '10.0.0.1', 'X.Real.IP overrode X-Real-IP: ' . $body);
check(!array_key_exists('x.real.ip', $data['headers']), 'the worker received X.Real.IP: ' . $body);
echo "dot-dropped: ok\n";

[$status, $body] = request($http, "GET /env.php HTTP/1.1\r\nHost: h.test\r\n"
    . "X-Forwarded-For[]: 6.6.6.6\r\nX-Forwarded-For: 10.0.0.1\r\nConnection: close\r\n\r\n");
check(str_contains($status, ' 200 '), "bracket name status: $status");
$data = json_decode($body, true);
check(is_array($data), "bracket name body: $body");
check(($data['server']['HTTP_X_FORWARDED_FOR'] ?? null) === '10.0.0.1',
    'X-Forwarded-For[] changed HTTP_X_FORWARDED_FOR in the worker $_SERVER: ' . $body);
check(($data['headers']['x-forwarded-for'] ?? null) === '10.0.0.1', 'X-Forwarded-For[] changed X-Forwarded-For: ' . $body);
check(!array_key_exists('x-forwarded-for[]', $data['headers']), 'the worker received X-Forwarded-For[]: ' . $body);
echo "bracket-dropped: ok\n";

[$status, $body] = request($http, "GET /env.php HTTP/1.1\r\nHost: h.test\r\n"
    . "X Custom: 6.6.6.6\r\nX-Kept: kept\r\nConnection: close\r\n\r\n");
check(str_contains($status, ' 200 '), "space name status: $status");
$data = json_decode($body, true);
check(is_array($data), "space name body: $body");
check(!isset($data['server']['HTTP_X_CUSTOM']), 'a name with a space reached the worker $_SERVER: ' . $body);
check(!array_key_exists('x custom', $data['headers']), 'the worker received a name with a space: ' . $body);
check(($data['headers']['x-kept'] ?? '') === 'kept', 'gateway dropped an ordinary header: ' . $body);
echo "space-dropped: ok\n";

[$status, $body] = request($http, "GET /env.php HTTP/1.1\r\nHost: h.test\r\n"
    . ": emptyname\r\nX-Kept: kept\r\nConnection: close\r\n\r\n");
check(str_contains($status, ' 200 '), "empty name status: $status");
$data = json_decode($body, true);
check(is_array($data), "empty name body: $body");
check(!array_key_exists('HTTP_', $data['server']), 'an empty header name reached the worker $_SERVER as HTTP_: ' . $body);
check(($data['headers']['x-kept'] ?? '') === 'kept', 'gateway dropped an ordinary header: ' . $body);
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

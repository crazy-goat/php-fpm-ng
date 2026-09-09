--TEST--
fpm-ng: the HTTP gateway refuses "Proxy" and bounds a request header name like HTTP-direct (issue #115)
--SKIPIF--
<?php include "skipif.inc"; ?>
--FILE--
<?php
require_once "tester.inc";

/* The gateway (sapi/fpmng/fpm/fpm_http.c) and HTTP-direct
 * (sapi/fpmng/fpm/fpm_http_direct_request.c) derive HTTP_* keys from the same
 * mapping since issue #109, but used to disagree on *which* headers get a key
 * and how long a name may be. This test pins the two rules the gateway gained
 * in issue #115; the HTTP-direct side of the same behaviour is checked by
 * fpmng-http-direct-request-parity.phpt. */

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

$root = sys_get_temp_dir() . '/fpmng-gateway-header-name-' . getmypid();
@mkdir($root, 0700, true);

/* getallheaders() reads the FastCGI parameters the gateway sent
 * (sapi/fpm/fpm_main.c PHP_FUNCTION(apache_request_headers), via
 * fcgi_loadenv), so it shows what the gateway put on the wire. $_SERVER does
 * not: core deletes HTTP_PROXY there on its own (main/php_variables.c
 * check_http_proxy()), which would make this test pass without the gateway
 * doing anything. */
file_put_contents("$root/env.php", <<<'PHP'
<?php
echo json_encode(array_change_key_case(getallheaders(), CASE_LOWER));
PHP);

$cfg = <<<EOT
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
[web]
listen = {{ADDR}}
chdir = $root
pm = static
pm.max_children = 1
pool.type = http
http.listen = {{ADDR[http]}}
http.front_controller = /env.php
EOT;

$tester = new FPM\Tester($cfg, file_get_contents("$root/env.php"));
$tester->start();
$tester->expectLogStartNotices();
$http = $tester->getAddr('ipv4', '[http]');

/* httpoxy (CVE-2016-5385): "Proxy" must not reach the worker as HTTP_PROXY at
 * all. X-Proxy-Control rides along to show the loop still passes everything
 * else -- an exclusion that swallowed the whole request would pass otherwise. */
[$status, $body] = request($http, "GET /env.php HTTP/1.1\r\nHost: h.test\r\n"
    . "Proxy: attacker\r\nX-Proxy-Control: kept\r\nConnection: close\r\n\r\n");
check(str_contains($status, ' 200 '), "proxy request status: $status");
$headers = json_decode($body, true);
check(is_array($headers), "proxy request body: $body");
check(!isset($headers['proxy']), 'gateway forwarded the Proxy header: ' . $body);
check(($headers['x-proxy-control'] ?? '') === 'kept', 'gateway dropped an ordinary header: ' . $body);
echo "proxy-excluded: ok\n";

/* The bound is on the name, and it is the same 1024 HTTP-direct enforces
 * (FPM_HTTP_HEADER_NAME_MAX). One byte over is a 400, not a served request
 * with the header silently missing. */
$name = 'X-' . str_repeat('a', 1022);
[$status, $body] = request($http, "GET /env.php HTTP/1.1\r\nHost: h.test\r\n"
    . "$name: v\r\nConnection: close\r\n\r\n");
check(str_contains($status, ' 200 '), "1024-byte header name status: $status");
$headers = json_decode($body, true);
check(is_array($headers) && ($headers[strtolower($name)] ?? '') === 'v',
    'a 1024-byte header name did not arrive: ' . $body);
echo "header-name-at-limit: ok\n";

[$status] = request($http, "GET /env.php HTTP/1.1\r\nHost: h.test\r\n"
    . 'X-' . str_repeat('a', 1100) . ": v\r\nConnection: close\r\n\r\n");
check(str_contains($status, ' 400 '), "over-long header name status: $status");
echo "header-name-over-limit: ok\n";

$tester->terminate();
$tester->expectLogTerminatingNotices();
$tester->close();

@unlink("$root/env.php");
@rmdir($root);

?>
Done
--EXPECT--
proxy-excluded: ok
header-name-at-limit: ok
header-name-over-limit: ok
Done
--CLEAN--
<?php
require_once "tester.inc";
FPM\Tester::clean();
?>

--TEST--
fpm-ng: the 308 and the ACME 404 on http.plain_listen carry no http.response_header[] (the 404 has the page), the TLS listener does, and libevent's 413 carries neither the headers nor the page (issue #650)
--SKIPIF--
<?php
include "fpmng-skipif.inc";
fpmng_skip_if_pool_type_unsupported('gateway');
fpmng_skip_if_no_acme();
if (!function_exists('openssl_x509_parse')) {
    die('skip requires the openssl extension');
}
if (trim((string) shell_exec('command -v openssl 2>/dev/null')) === '') {
    die('skip requires the openssl CLI to generate a test certificate');
}
$probe = new FPM\Tester(<<<'EOT'
[global]
error_log = {{FILE:LOG}}
[gw]
pool.type = gateway
listen = {{ADDR[http]}}
chdir = /tmp
http.tls_cert = /nonexistent-cert.pem
http.tls_key = /nonexistent-key.pem
http.route[unconfined] = /
[unconfined]
pool.type = fastcgi
listen = {{ADDR}}
pm = static
pm.max_children = 1
chdir = /tmp
EOT, '<?php');
$messages = $probe->testConfig(true, null, false, false);
FPM\Tester::clean();
foreach ((array) $messages as $message) {
    if (str_contains($message, 'built with TLS support')) {
        die('skip php-fpm-ng built without TLS support (configure without --enable-fpmng-tls, issue #280)');
    }
}
?>
--FILE--
<?php
require_once "tester.inc";

/* Issue #650. RFC 6797 7.2: an HSTS host must not send the header over a
 * non-secure transport, so the plain companion's replies carry none of the
 * configured headers. The 413 for a body over http.max_body is written by
 * libevent before the gateway callback runs (#882): it carries neither the
 * headers nor the page, and the key 413 is refused by -t for that reason. */

function run(string $cmd): void
{
    exec($cmd . ' 2>&1', $output, $code);
    if ($code !== 0) {
        throw new RuntimeException("COMMAND FAILED: $cmd\n" . implode("\n", $output));
    }
}

function fetch(string $scheme, string $addr, string $request): string
{
    $ctx = stream_context_create(['ssl' => ['verify_peer' => false, 'verify_peer_name' => false]]);
    $fp = stream_socket_client("$scheme://$addr", $errno, $error, 5, STREAM_CLIENT_CONNECT, $ctx);
    if (!$fp) {
        throw new RuntimeException("connect: $error");
    }
    stream_set_timeout($fp, 5);
    fwrite($fp, $request);
    $raw = '';
    while (!feof($fp)) {
        $chunk = fread($fp, 8192);
        if ($chunk === false || $chunk === '') {
            break;
        }
        $raw .= $chunk;
    }
    fclose($fp);
    return $raw;
}

function check(bool $ok, string $what, string $raw): void
{
    if (!$ok) {
        throw new RuntimeException("$what\n$raw");
    }
}

$root = sys_get_temp_dir() . '/fpmng-gw-resp-header-plain-' . getmypid();
@mkdir($root, 0700, true);
run("openssl req -x509 -newkey rsa:2048 -nodes -days 2 -sha256 "
    . "-subj /CN=t -keyout $root/tls.key -out $root/tls.crt");
file_put_contents("$root/404.html", '<h1>nothing here</h1>');
file_put_contents("$root/index.php", '<?php echo "x";');

$cfg = <<<EOT
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
[gw]
pool.type = gateway
listen = {{ADDR[https]}}
chdir = $root
http.plain_listen = {{ADDR[plain]}}
http.gateways = 1
http.front_controller = /index.php
http.tls_cert = $root/tls.crt
http.tls_key = $root/tls.key
http.max_body = 1K
http.route[web] = /
http.response_header[Strict-Transport-Security] = "max-age=31536000; includeSubDomains"
http.error_page[404] = $root/404.html
[web]
pool.type = fastcgi
listen = {{ADDR}}
chdir = $root
pm = static
pm.max_children = 1
EOT;

$tester = new FPM\Tester($cfg, '<?php echo "unused";');
try {
    $tester->start();
    $tester->expectLogStartNotices();
    $https = $tester->getAddr('ipv4', '[https]');
    $plain = $tester->getAddr('ipv4', '[plain]');

    $raw = fetch('tcp', $plain, "GET /x HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n");
    check(str_starts_with($raw, 'HTTP/1.1 308'), 'plain: not a 308', $raw);
    check(stripos($raw, 'Strict-Transport-Security') === false, 'plain: the 308 carries HSTS', $raw);
    echo "plain 308: no configured header\n";

    /* The ACME 404 on the plain listener goes through the shared error path, so
     * the 404 page applies by the documented rule, and the headers do not. */
    $raw = fetch('tcp', $plain, "GET /.well-known/acme-challenge/nothing HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n");
    check(str_starts_with($raw, 'HTTP/1.1 404'), 'plain: ACME fetch is not a 404', $raw);
    check(stripos($raw, 'Strict-Transport-Security') === false, 'plain: the ACME 404 carries HSTS', $raw);
    check(str_contains($raw, '<h1>nothing here</h1>'), 'plain: the ACME 404 has no page', $raw);
    echo "plain ACME 404: page, no configured header\n";

    $raw = fetch('ssl', $https, "GET /x HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n");
    check(str_starts_with($raw, 'HTTP/1.1 200'), 'https: not a 200', $raw);
    check(substr_count(strtolower($raw), 'strict-transport-security: max-age=31536000; includesubdomains') === 1, 'https: HSTS not once', $raw);
    echo "https 200: configured header once\n";

    $body = str_repeat('a', 4096);
    $raw = fetch('ssl', $https, "POST /x HTTP/1.1\r\nHost: t\r\nConnection: close\r\nContent-Length: " . strlen($body) . "\r\n\r\n$body");
    check(str_starts_with($raw, 'HTTP/1.1 413'), 'https: not a 413', $raw);
    check(stripos($raw, 'Strict-Transport-Security') === false, '413 carries HSTS', $raw);
    check(!str_contains($raw, 'nothing here'), '413 carries the 404 page', $raw);
    echo "413 from libevent: no headers\n";
} finally {
    $tester->terminate();
    $tester->close();
    foreach (glob("$root/*") as $f) @unlink($f);
    @rmdir($root);
}
?>
--EXPECT--
plain 308: no configured header
plain ACME 404: page, no configured header
https 200: configured header once
413 from libevent: no headers
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>

--TEST--
fpm-ng: the plain :80 companion redirects an absolute-form request-target as origin-form on the target's authority (issue #534, RFC 9112 3.2.2)
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

/* Before #534 "GET http://h/x" on http.plain_listen produced a Location of
 * "https://" + Host + "http://h/x". */

function run(string $cmd): void
{
    exec($cmd . ' 2>&1', $output, $code);
    if ($code !== 0) {
        throw new RuntimeException("COMMAND FAILED: $cmd\n" . implode("\n", $output));
    }
}

function location(string $addr, string $target): string
{
    $fp = stream_socket_client("tcp://$addr", $errno, $error, 5);
    if (!$fp) {
        throw new RuntimeException("connect: $error");
    }
    stream_set_timeout($fp, 5);
    fwrite($fp, "GET $target HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n");
    $raw = stream_get_contents($fp);
    fclose($fp);
    $status = preg_match('#^HTTP/\S+ (\d+)#', (string) $raw, $m) ? $m[1] : 'none';
    $loc = preg_match('#^Location: (.*?)\r$#mi', (string) $raw, $m) ? $m[1] : '(none)';
    return "$status $loc";
}

$root = sys_get_temp_dir() . '/fpmng-gw-absform-plain-' . getmypid();
@mkdir($root, 0700, true);
run("openssl req -x509 -newkey rsa:2048 -nodes -days 2 -sha256 "
    . "-subj /CN=t -keyout $root/tls.key -out $root/tls.crt");
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
http.route[web] = /
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
    $plain = $tester->getAddr('ipv4', '[plain]');
    echo "origin: " . location($plain, '/x?a=1') . "\n";
    echo "absolute: " . location($plain, 'http://other.example:81/x?a=1') . "\n";
    echo "no path: " . location($plain, 'http://other.example:81') . "\n";
    echo "scheme only: " . location($plain, 'http:/x?a=1') . "\n";
    echo "network path: " . location($plain, '//h/x') . "\n";
    echo "ipv6: " . location($plain, 'http://[::1]:81/x') . "\n";
} finally {
    $tester->terminate();
    $tester->close();
    @unlink("$root/index.php");
    @unlink("$root/tls.key");
    @unlink("$root/tls.crt");
    @rmdir($root);
}
?>
--EXPECT--
origin: 308 https://t/x?a=1
absolute: 308 https://other.example/x?a=1
no path: 308 https://other.example/
scheme only: 308 https://t/x?a=1
network path: 308 https://t//h/x
ipv6: 308 https://[::1]/x
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>

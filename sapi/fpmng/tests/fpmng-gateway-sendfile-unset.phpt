--TEST--
fpm-ng: with http.sendfile_root unset, X-Accel-Redirect and X-Sendfile are forwarded like any other header (#649)
--SKIPIF--
<?php
include "fpmng-skipif.inc";
fpmng_skip_if_pool_type_unsupported('gateway');
?>
--FILE--
<?php
require_once "tester.inc";
$base = sys_get_temp_dir() . '/fpmng-sendfile-unset-' . getmypid();
@mkdir($base, 0777, true);

/* No http.sendfile_root: the gateway does not act on the redirect headers.
 * The client gets them with the worker's body. */
file_put_contents($base . '/send.php', '<?php
header("X-Accel-Redirect: /etc/hostname");
header("X-Sendfile: /etc/hostname");
echo "worker body";
');

$cfg = <<<EOT
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
[gw]
pool.type = gateway
listen = {{ADDR[http]}}
chdir = $base
http.gateways = 1
http.front_controller = /send.php
http.route[web] = /
[web]
pool.type = fastcgi
listen = {{ADDR}}
chdir = $base
pm = static
pm.max_children = 1
EOT;

function header_value(array $headers, string $name): ?string
{
    foreach ($headers as $line) {
        if (stripos($line, $name . ':') === 0) {
            return trim(substr($line, strlen($name) + 1));
        }
    }
    return null;
}

$tester = new FPM\Tester($cfg, '<?php');
try {
    $tester->start();
    $tester->expectLogStartNotices();
    $http = $tester->getAddr('ipv4', '[http]');
    $ctx = stream_context_create(['http' => ['timeout' => 5, 'ignore_errors' => true]]);
    $body = @file_get_contents("http://$http/", false, $ctx);
    $headers = $http_response_header ?? [];

    if (!str_starts_with($headers[0] ?? '', 'HTTP/1.1 200 ')) {
        throw new RuntimeException('status: ' . ($headers[0] ?? 'none'));
    }
    if ($body !== 'worker body') {
        throw new RuntimeException('body: ' . var_export($body, true));
    }
    foreach (['X-Accel-Redirect', 'X-Sendfile'] as $name) {
        if (header_value($headers, $name) !== '/etc/hostname') {
            throw new RuntimeException("$name not forwarded: " . var_export(header_value($headers, $name), true));
        }
    }
    echo "unset: headers and worker body forwarded: ok\n";
    echo "Done\n";
} finally {
    $tester->terminate();
    $tester->close();
    @unlink($base . '/send.php');
    @rmdir($base);
}
?>
--EXPECT--
unset: headers and worker body forwarded: ok
Done
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>

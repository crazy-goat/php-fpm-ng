--TEST--
fpm-ng: a FastCGI worker death leaves the chunked gateway reply unterminated (issue #637)
--SKIPIF--
<?php
include "fpmng-skipif.inc";
fpmng_skip_if_pool_type_unsupported('gateway');
?>
--FILE--
<?php

require_once "tester.inc";

$root = sys_get_temp_dir() . '/fpmng-fastcgi-mid-body-' . getmypid();
@mkdir($root, 0700, true);
$script = $root . '/partial.php';
file_put_contents($script, <<<'PHP'
<?php
header('Content-Type: text/plain');
header('X-Fpmng-Test-Pad: 12chars');
echo str_repeat('x', 4097);
flush();
usleep(3000000);
PHP);

$config = <<<EOT
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
[gw]
pool.type = gateway
listen = {{ADDR[http]}}
chdir = $root
http.route[web] = /
[web]
pool.type = fastcgi
listen = {{ADDR}}
chdir = $root
pm = static
pm.max_children = 1
request_terminate_timeout = 1
EOT;

$tester = new FPM\Tester($config, '<?php echo "unused";');
try {
    @unlink($tester->getPrefixedFile(FPM\Tester::FILE_EXT_LOG_ERR));
    $tester->start([], false);
    $tester->switchLogSource('{{FILE:LOG}}');
    $tester->expectLogStartNotices();
    $http = $tester->getAddr('ipv4', '[http]');
    $parts = explode(':', $http);
    $fp = stream_socket_client("tcp://{$parts[0]}:{$parts[1]}", $errno, $error, 5);
    if (!$fp) throw new RuntimeException("connect: $error");
    stream_set_timeout($fp, 6);
    fwrite($fp, "GET /partial.php HTTP/1.1\r\nHost: test\r\nConnection: close\r\n\r\n");
    $response = '';
    while (!feof($fp)) {
        $part = fread($fp, 8192);
        if ($part === false || $part === '') break;
        $response .= $part;
    }
    $meta = stream_get_meta_data($fp);
    fclose($fp);
    $headEnd = strpos($response, "\r\n\r\n");
    $body = $headEnd === false ? '' : substr($response, $headEnd + 4);
    if (!str_contains(strtolower($response), 'transfer-encoding: chunked')
        || !str_contains($response, str_repeat('x', 4097))
        || str_contains($body, "0\r\n\r\n")
        || ($meta['timed_out'] ?? false)) {
        throw new RuntimeException('chunked response was not an EOF-terminated partial body: ' . substr($response, 0, 500));
    }
    $errorLog = $tester->getPrefixedFile(FPM\Tester::FILE_EXT_LOG_ERR);
    $log = (string) @file_get_contents($errorLog);
    if (!preg_match('/failed after the response head was sent/', $log)) {
        throw new RuntimeException('the FastCGI upstream did not fail after the response head');
    }
    echo "chunked: truncated\n";
} finally {
    $tester->terminate();
    $tester->close();
    @unlink($script);
    @rmdir($root);
}
?>
--EXPECT--
chunked: truncated
--CLEAN--
<?php
require_once "tester.inc";
FPM\Tester::clean();
?>

--TEST--
fpm-ng: a gateway 504 access-log line counts its plain-text body (issue #1085)
--SKIPIF--
<?php
include "fpmng-skipif.inc";
fpmng_skip_if_pool_type_unsupported('gateway');
?>
--FILE--
<?php

require_once "tester.inc";

/* Issue #1085: the gateway's own timeout replies used evhttp_send_error(),
 * whose HTML page could not be read back for the access log. The 504 now
 * carries a known plain-text body and the log counts it. */
$docroot = sys_get_temp_dir() . '/fpmng-504-access-log-' . getmypid();
@mkdir($docroot, 0700, true);
file_put_contents("$docroot/silent.php", '<?php usleep(10 * 1000 * 1000);');

$config = <<<EOT
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
[gw]
pool.type = gateway
listen = {{ADDR[http]}}
chdir = $docroot
http.gateways = 1
http.upstream_read_timeout = 1500
http.access_log = {{FILE:LOG:ACC}}
http.route[web] = /
[web]
pool.type = fastcgi
listen = {{ADDR:UDS}}
pm = static
pm.max_children = 1
chdir = $docroot
EOT;

$tester = new FPM\Tester($config);
$tester->start();
$tester->expectLogStartNotices();

$fp = stream_socket_client("tcp://" . $tester->getAddr('ipv4', '[http]'), $errno, $error, 5);
stream_set_timeout($fp, 10);
fwrite($fp, "GET /silent.php HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n");
$reply = stream_get_contents($fp);
fclose($fp);

$body = substr($reply, strpos($reply, "\r\n\r\n") + 4);
if (!str_contains($body, 'Gateway Timeout')) {
    echo "FAIL: 504 body missing: " . var_export($reply, true) . "\n";
    exit(1);
}

$accessLog = $tester->getPrefixedFile(FPM\Tester::FILE_EXT_LOG_ACC);
$deadline = microtime(true) + 10;
$content = '';
do {
    $content = (string) @file_get_contents($accessLog);
    if (str_contains($content, '"/silent.php HTTP/1.1" 504 ')) {
        break;
    }
    usleep(100000);
} while (microtime(true) < $deadline);

if (!preg_match('/"GET \/silent\.php HTTP\/1\.1" 504 16 /', $content)) {
    echo "FAIL: 504 did not count its body in the access log:\n$content\n";
    exit(1);
}
echo "504 body counted: ok\n";

@unlink("$docroot/silent.php");
@rmdir($docroot);

$tester->terminate();
$tester->expectLogTerminatingNotices();
$tester->close();
echo "Done\n";
?>
--EXPECT--
504 body counted: ok
Done
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>

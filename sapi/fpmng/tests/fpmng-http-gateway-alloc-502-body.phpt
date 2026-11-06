--TEST--
fpm-ng: a gateway whose 502 body cannot be allocated still ends the 502 reply cleanly (issue #967)
--SKIPIF--
<?php
include "fpmng-skipif.inc";
fpmng_skip_if_no_fault_inject();
fpmng_skip_if_pool_type_unsupported('gateway');
?>
--FILE--
<?php
require_once "tester.inc";

/* Issue #967. An invalid upstream Status sends a 502 (issue #594), and that
 * 502 has a text body built in an evbuffer. The fault point http_reply_502_body
 * makes evbuffer_new() fail there, as an out-of-memory would. Before the fix
 * evbuffer_add_printf(NULL, ...) killed the gateway on SIGSEGV. Now the 502 has
 * no body: the reply is a chunked 502 with an empty body that ends normally,
 * and the WARNING for the invalid Status is still logged. */

function check(bool $condition, string $message): void
{
    if (!$condition) throw new RuntimeException($message);
}

function fetch(string $addr, string $path): string
{
    $fp = stream_socket_client("tcp://$addr", $errno, $error, 5);
    check($fp !== false, "connect to $addr failed: $error");
    stream_set_timeout($fp, 5);
    fwrite($fp, "GET $path HTTP/1.1\r\nHost: alloc.test\r\nConnection: close\r\n\r\n");
    $reply = stream_get_contents($fp);
    fclose($fp);
    return (string) $reply;
}

function file_wait(string $file, string $pattern, int $seconds = 10): string
{
    $deadline = microtime(true) + $seconds;
    do {
        $content = (string) @file_get_contents($file);
        if (preg_match($pattern, $content)) return $content;
        usleep(100000);
    } while (microtime(true) < $deadline);
    return $content;
}

$root = sys_get_temp_dir() . '/fpmng-gateway-alloc-502-body-' . getmypid();
@mkdir($root, 0700, true);
file_put_contents("$root/s.php", <<<'PHP'
<?php
header('Status: abc');
echo "body";
PHP);

$cfg = <<<EOT
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
[gw]
pool.type = gateway
listen = {{ADDR[http]}}
chdir = $root
http.gateways = 1
http.access_log = {{FILE:LOG:ACC}}
http.route[web] = /
[web]
pool.type = fastcgi
listen = {{ADDR}}
chdir = $root
pm = static
pm.max_children = 2
EOT;

$tester = new FPM\Tester($cfg);
@unlink($tester->getPrefixedFile(FPM\Tester::FILE_EXT_LOG_ERR));
$tester->start([], false, false, [], [], array_merge(getenv(), [
    'FPMNG_FAULT' => 'http_reply_502_body',
]));
$tester->switchLogSource('{{FILE:LOG}}');
$tester->expectLogStartNotices();
$http = $tester->getAddr('ipv4', '[http]');

$reply = fetch($http, '/s.php');
check(str_starts_with($reply, 'HTTP/1.1 502 Bad Gateway'),
    'no 502: ' . var_export(strtok($reply, "\r\n"), true));
$body = substr($reply, (int) strpos($reply, "\r\n\r\n") + 4);
check($body === "0\r\n\r\n", 'the 502 is not a clean empty chunked reply: ' . var_export($body, true));
echo "502 without body: ok\n";

$errors = file_wait($tester->getPrefixedFile(FPM\Tester::FILE_EXT_LOG_ERR), "/invalid Status 'abc'/");
check(preg_match("/WARNING: \[pool gw\] http: upstream sent invalid Status 'abc'/", $errors) === 1,
    "no WARNING for the invalid Status:\n$errors");
echo "warning: ok\n";

$tester->terminate();
$tester->expectLogTerminatingNotices();
$tester->close();

@unlink("$root/s.php");
@rmdir($root);
?>
Done
--EXPECT--
502 without body: ok
warning: ok
Done
--CLEAN--
<?php
require_once "tester.inc";
FPM\Tester::clean();
?>

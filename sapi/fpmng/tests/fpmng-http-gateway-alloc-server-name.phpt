--TEST--
fpm-ng: a gateway answers 503 and forwards nothing when it cannot copy the Host header (issue #967)
--SKIPIF--
<?php
include "fpmng-skipif.inc";
fpmng_skip_if_no_fault_inject();
fpmng_skip_if_pool_type_unsupported('gateway');
?>
--FILE--
<?php
require_once "tester.inc";

/* Issue #967. fpm_http_build_request() copies the Host header for
 * SERVER_NAME with strdup(). The fault point http_request_server_name makes
 * that copy fail, as an out-of-memory would. Before the fix strrchr(NULL, ':')
 * killed the gateway on SIGSEGV. Now the request is not forwarded to the
 * worker: the client gets a 503, and one WARNING names the pool. A missing
 * SERVER_NAME is not an option: frameworks build absolute URLs from it. */

function check(bool $condition, string $message): void
{
    if (!$condition) throw new RuntimeException($message);
}

function fetch(string $addr, string $path): string
{
    $fp = stream_socket_client("tcp://$addr", $errno, $error, 5);
    check($fp !== false, "connect to $addr failed: $error");
    stream_set_timeout($fp, 5);
    fwrite($fp, "GET $path HTTP/1.1\r\nHost: alloc.test:8080\r\nConnection: close\r\n\r\n");
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

$root = sys_get_temp_dir() . '/fpmng-gateway-alloc-server-name-' . getmypid();
@mkdir($root, 0700, true);
file_put_contents("$root/s.php", <<<'PHP'
<?php
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
    'FPMNG_FAULT' => 'http_request_server_name',
]));
$tester->switchLogSource('{{FILE:LOG}}');
$tester->expectLogStartNotices();
$http = $tester->getAddr('ipv4', '[http]');

$reply = fetch($http, '/s.php');
check(str_starts_with($reply, 'HTTP/1.1 503 Service Unavailable'),
    'no 503 for a Host that could not be copied: ' . var_export(strtok($reply, "\r\n"), true));
check(!str_contains($reply, 'body'), 'the worker answered a request with no SERVER_NAME');
echo "503: ok\n";

$errors = file_wait($tester->getPrefixedFile(FPM\Tester::FILE_EXT_LOG_ERR), '/cannot copy the Host header/');
check(preg_match('/WARNING: \[pool gw\] http: cannot copy the Host header/', $errors) === 1,
    "no WARNING that names the pool:\n$errors");
check(!preg_match('/killed by signal|respawning/', $errors), "the gateway crashed:\n$errors");
echo "warning: ok\n";

$tester->terminate();
$tester->expectLogTerminatingNotices();
$tester->close();

@unlink("$root/s.php");
@rmdir($root);
?>
Done
--EXPECT--
503: ok
warning: ok
Done
--CLEAN--
<?php
require_once "tester.inc";
FPM\Tester::clean();
?>

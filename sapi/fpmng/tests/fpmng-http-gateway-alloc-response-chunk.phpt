--TEST--
fpm-ng: a gateway cuts a reply short, without its terminating chunk, when it cannot buffer a body chunk (issue #967)
--SKIPIF--
<?php
include "fpmng-skipif.inc";
fpmng_skip_if_no_fault_inject();
fpmng_skip_if_pool_type_unsupported('gateway');
?>
--FILE--
<?php
require_once "tester.inc";

/* Issue #967. fpm_http_response_chunk() wraps every body piece in an evbuffer.
 * The fault point http_reply_chunk makes evbuffer_new() fail, as an
 * out-of-memory would. Before the fix evbuffer_add(NULL, ...) killed the
 * gateway on SIGSEGV. Now the piece cannot be sent, so the reply is cut the way
 * issue #533 cuts a lost reply: the connection closes with no terminating chunk
 * that would make a truncated body look complete. One WARNING names the pool. */

function check(bool $condition, string $message): void
{
    if (!$condition) throw new RuntimeException($message);
}

function fetch(string $addr, string $path, ?bool &$timed_out = null): string
{
    $fp = stream_socket_client("tcp://$addr", $errno, $error, 5);
    check($fp !== false, "connect to $addr failed: $error");
    stream_set_timeout($fp, 5);
    fwrite($fp, "GET $path HTTP/1.1\r\nHost: alloc.test\r\nConnection: close\r\n\r\n");
    $reply = stream_get_contents($fp);
    $timed_out = stream_get_meta_data($fp)['timed_out'];
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

$root = sys_get_temp_dir() . '/fpmng-gateway-alloc-response-chunk-' . getmypid();
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
    'FPMNG_FAULT' => 'http_reply_chunk',
]));
$tester->switchLogSource('{{FILE:LOG}}');
$tester->expectLogStartNotices();
$http = $tester->getAddr('ipv4', '[http]');

$reply = fetch($http, '/s.php', $timed_out);
/* A reply that is cut lets the gateway close the connection. A connection
 * that stays open until the 5 s stream timeout returns '' too, so the test
 * also requires that the read did not time out. */
check(!$timed_out, 'the connection stayed open until the read timed out: the reply was not cut');
/* The head may be lost too: shutdown() drops the output libevent has not
 * written yet (issue #635), and the head is written in the same pass as the
 * lost piece. Either way the client gets no complete reply. */
check($reply === '' || str_starts_with($reply, 'HTTP/1.1 200 OK'),
    'the cut reply is neither empty nor a 200 head: ' . var_export($reply, true));
check(!str_contains($reply, 'body'), 'the body reached the client');
check(!str_ends_with($reply, "0\r\n\r\n"), 'the cut reply ends with its terminating chunk: ' . var_export($reply, true));
echo "cut without terminator: ok\n";

$errors = file_wait($tester->getPrefixedFile(FPM\Tester::FILE_EXT_LOG_ERR), '/cannot buffer a response chunk/');
check(preg_match('/WARNING: \[pool gw\] http: cannot buffer a response chunk/', $errors) === 1,
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
cut without terminator: ok
warning: ok
Done
--CLEAN--
<?php
require_once "tester.inc";
FPM\Tester::clean();
?>

--TEST--
fpm-ng: a gateway answers 502 for an invalid upstream "Status:" instead of putting it on the wire (issue #594)
--SKIPIF--
<?php
include "fpmng-skipif.inc";
fpmng_skip_if_pool_type_unsupported('gateway');
?>
--FILE--
<?php
require_once "tester.inc";

/* fpm_http_start_reply() used to run atoi() over the upstream's CGI "Status:"
 * and hand the int to libevent: "abc" became "HTTP/1.1 0 ...", 99999 and -5
 * went out as they were, and a 1xx became the final answer with its body
 * dropped, so a keep-alive client waited for a response that never came. Only
 * 200..599 (fpm_http_direct_status_final()) may become the status line now;
 * anything else is a 502, a WARNING and a 502 in the access log. */

function check(bool $condition, string $message): void
{
    if (!$condition) throw new RuntimeException($message);
}

/* Raw request on a keep-alive connection. Returns the status line, or
 * 'TIMEOUT' when nothing arrived. With the old code a 1xx status
 * arrived here as the final answer. */
function status_line(string $addr, string $path): string
{
    $fp = stream_socket_client("tcp://$addr", $errno, $error, 5);
    check($fp !== false, "connect to $addr failed: $error");
    stream_set_timeout($fp, 5);
    fwrite($fp, "GET $path HTTP/1.1\r\nHost: status.test\r\n\r\n");
    $line = fgets($fp);
    fclose($fp);
    return $line === false ? 'TIMEOUT' : rtrim($line);
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

$root = sys_get_temp_dir() . '/fpmng-gateway-upstream-status-' . getmypid();
@mkdir($root, 0700, true);
file_put_contents("$root/s.php", <<<'PHP'
<?php
header('Status: ' . $_GET['v']);
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
/* A file, not the -O stderr pipe: the WARNING is read back from error_log. */
@unlink($tester->getPrefixedFile(FPM\Tester::FILE_EXT_LOG_ERR));
$tester->start([], false);
$tester->switchLogSource('{{FILE:LOG}}');
$tester->expectLogStartNotices();
$http = $tester->getAddr('ipv4', '[http]');

foreach (['abc' => 'abc', 'big' => '99999', 'neg' => '-5', 'huge' => '99999999999',
          'info' => '103', 'cont' => '100', 'low' => '199', 'high' => '600', 'empty' => ''] as $name => $v) {
    $line = status_line($http, '/s.php?v=' . rawurlencode($v) . "&n=$name");
    check($line === 'HTTP/1.1 502 Bad Gateway', "Status '$v': " . var_export($line, true));
    echo "$name: 502\n";
}

/* Issue #605: control bytes in the reason phrase are rejected like a bad
 * code (502), not forwarded to the client -- the reason goes on the wire
 * verbatim, so an interior CR would split the status line. PHP's own
 * header() refuses CR, LF and NUL (measured on PHP 8.5.10: "new line
 * detected" / "NUL bytes" warnings), so through this PHP worker only bytes
 * like 0x01 and 0x7f arrive; a non-PHP FastCGI upstream can also send CR
 * and NUL, which the same C check rejects by scanning the whole value
 * ('\n' never arrives: the header block is split on it). */
foreach (['ctrl-soh' => "404 B\x01ad", 'ctrl-del' => "404 B\x7fad"] as $name => $v) {
    $line = status_line($http, '/s.php?v=' . rawurlencode($v) . "&n=$name");
    check($line === 'HTTP/1.1 502 Bad Gateway', "reason '$name': " . var_export($line, true));
    echo "$name: 502\n";
}

/* Controls: a valid status, with and without a reason, passes through. */
$line = status_line($http, '/s.php?v=' . rawurlencode('404 Gone') . '&n=ctl');
check($line === 'HTTP/1.1 404 Gone', 'control 404 Gone: ' . var_export($line, true));
echo "control-reason: ok\n";
/* Issue #605: bytes >= 0x80 are not controls -- a UTF-8 reason passes
 * through verbatim. This pins the check against over-rejecting (e.g. a
 * future switch to iscntrl(), which in some locales rejects high bytes).
 * The \xc3\xa9 bytes survive PHP's header() untouched (probed on PHP
 * 8.5.10: they arrive on the wire as-is, unlike CR/LF/NUL which header()
 * refuses). */
$line = status_line($http, '/s.php?v=' . rawurlencode("404 Caf\xc3\xa9") . '&n=ctl3');
check($line === "HTTP/1.1 404 Caf\xc3\xa9", 'control utf8 reason: ' . var_export($line, true));
echo "control-utf8: ok\n";
$line = status_line($http, '/s.php?v=599&n=ctl2');
check(str_starts_with($line, 'HTTP/1.1 599 '), 'control 599: ' . var_export($line, true));
echo "control-bare: ok\n";

$errors = file_wait($tester->getPrefixedFile(FPM\Tester::FILE_EXT_LOG_ERR), '/invalid Status .99999999999./');
check(preg_match("/WARNING: \[pool gw\] http: upstream sent invalid Status 'abc'/", $errors) === 1, 'no WARNING for abc');
check(preg_match("/invalid Status '103'/", $errors) === 1, 'no WARNING for 103');
check(preg_match("/invalid Status '404 B\x01ad'/", $errors) === 1, 'no WARNING for control reason');
echo "warning: ok\n";

$acc = file_wait($tester->getPrefixedFile(FPM\Tester::FILE_EXT_LOG_ACC), '/n=ctl2/');
foreach (['abc', 'big', 'neg', 'huge', 'info', 'ctrl-soh', 'ctrl-del'] as $name) {
    check(preg_match('/n=' . $name . '[^\n]* 502 /', $acc) === 1, "access log: no 502 for $name");
}
echo "access-log: ok\n";

$tester->terminate();
$tester->expectLogTerminatingNotices();
$tester->close();

@unlink("$root/s.php");
@rmdir($root);

?>
Done
--EXPECT--
abc: 502
big: 502
neg: 502
huge: 502
info: 502
cont: 502
low: 502
high: 502
empty: 502
ctrl-soh: 502
ctrl-del: 502
control-reason: ok
control-utf8: ok
control-bare: ok
warning: ok
access-log: ok
Done
--CLEAN--
<?php
require_once "tester.inc";
FPM\Tester::clean();
?>

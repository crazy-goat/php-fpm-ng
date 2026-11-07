--TEST--
fpm-ng: http.access_log reports zero bytes for a HEAD reply (issue #1086)
--SKIPIF--
<?php
include "fpmng-skipif.inc";
fpmng_skip_if_pool_type_unsupported('gateway');
?>
--FILE--
<?php

require_once "tester.inc";

/* Issue #1086: libevent sends no body for HEAD replies, so the access log
 * must report zero body bytes for a proxied HEAD even when the upstream
 * yields a body. A GET to the same endpoint still reports its 5 bytes. */
$docroot = sys_get_temp_dir() . '/fpmng-head-access-log-' . getmypid();
@mkdir($docroot, 0700, true);
file_put_contents($docroot . '/head.php', '<?php echo "hello";');

$config = <<<EOT
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
[gw]
pool.type = gateway
listen = {{ADDR[http]}}
chdir = $docroot
http.gateways = 1
http.front_controller = /head.php
http.access_log = {{FILE:LOG:ACC}}
http.route[web] = /
[web]
pool.type = fastcgi
listen = {{ADDR}}
chdir = $docroot
pm = static
pm.max_children = 2
EOT;

$tester = new FPM\Tester($config, '<?php echo "unused";');
$tester->start();
$tester->expectLogStartNotices();

$httpAddr = $tester->getAddr('ipv4', '[http]');

function rawRequest(string $addr, string $method): string
{
    $fp = stream_socket_client("tcp://$addr", $errno, $error, 5);
    if ($fp === false) {
        throw new RuntimeException("connect failed: $error");
    }
    stream_set_timeout($fp, 5);
    fwrite($fp, "$method /head.php HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n");
    $reply = stream_get_contents($fp);
    fclose($fp);
    return (string) $reply;
}

function fileWait(string $file, string $pattern, int $seconds = 10): string
{
    $deadline = microtime(true) + $seconds;
    do {
        $content = (string) @file_get_contents($file);
        if (preg_match($pattern, $content)) {
            return $content;
        }
        usleep(100000);
    } while (microtime(true) < $deadline);

    return $content;
}

function check(bool $condition, string $message): void
{
    if (!$condition) throw new RuntimeException($message);
}

$headReply = rawRequest($httpAddr, 'HEAD');
check(str_contains($headReply, "HTTP/1.1 200"), 'HEAD status: ' . strtok($headReply, "\r\n"));
$getReply = rawRequest($httpAddr, 'GET');
check(str_contains($getReply, "HTTP/1.1 200"), 'GET status: ' . strtok($getReply, "\r\n"));
check(str_contains($getReply, "hello"), 'GET body missing');

$accessLog = $tester->getPrefixedFile(FPM\Tester::FILE_EXT_LOG_ACC);
$content = fileWait($accessLog, '/"GET \/head\.php HTTP\/1\.1" 200 5 /');

$headLine = null;
$getLine = null;
foreach (explode("\n", $content) as $line) {
    if (str_contains($line, '"HEAD /head.php HTTP/1.1"')) {
        $headLine = $line;
    }
    if (str_contains($line, '"GET /head.php HTTP/1.1"')) {
        $getLine = $line;
    }
}

check($headLine !== null, "HEAD access log line missing:\n$content");
check($getLine !== null, "GET access log line missing:\n$content");
check(preg_match('/"HEAD \/head\.php HTTP\/1\.1" 200 0 /', $headLine) === 1,
    "HEAD logged nonzero bytes: $headLine");
check(preg_match('/"GET \/head\.php HTTP\/1\.1" 200 5 /', $getLine) === 1,
    "GET did not log 5 bytes: $getLine");
echo "head bytes zero, get bytes five: ok\n";

$tester->terminate();
$tester->expectLogTerminatingNotices();
$tester->close();

@unlink("$docroot/head.php");
@rmdir($docroot);
echo "Done\n";
?>
--EXPECT--
head bytes zero, get bytes five: ok
Done
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>

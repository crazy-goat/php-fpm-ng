--TEST--
fpm-ng: a gateway reply that the FastCGI worker ends after a flush and a pause is finished once, and the gateway keeps serving (HTTP/1.0, HEAD, Connection: close and keep-alive)
--SKIPIF--
<?php
include "fpmng-skipif.inc";
fpmng_skip_if_pool_type_unsupported('gateway');
?>
--FILE--
<?php

require_once "tester.inc";

/* The script writes and flushes, then pauses before it ends. The FastCGI
 * END_REQUEST arrives in a later read, after the client has taken the bytes
 * that were already flushed, so the gateway ends the reply with nothing queued.
 * A reply that is not chunked is then ended at once, and for a closing
 * connection the connection goes with it. The gateway must log each request
 * once with its status and byte count, and must serve the next request.
 * flush-cl.php sets Content-Length, so an HTTP/1.1 reply is not chunked. The
 * keep-alive round sends two requests on one HTTP/1.1 connection. The second
 * asks for Connection: close and is read to EOF, so each request has ended on
 * the gateway before the next step. */
$docroot = sys_get_temp_dir() . '/fpmng-gw-flush-then-end-' . getmypid();
@mkdir($docroot, 0700, true);
file_put_contents($docroot . '/flush.php', '<?php echo "a"; flush(); usleep(200000);');
file_put_contents($docroot . '/flush-cl.php', '<?php header("Content-Length: 1"); echo "a"; flush(); usleep(200000);');
file_put_contents($docroot . '/ok.php', '<?php echo "ok";');

$config = <<<EOT
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
[gw]
pool.type = gateway
listen = {{ADDR[http]}}
chdir = $docroot
http.access_log = {{FILE:LOG:ACC}}
http.access_format = json
; One gateway process: the target is pm.max_children = 1, so a second gateway
; would race for its single upstream connection and answer 503.
http.gateways = 1
http.route[web] = /
[web]
pool.type = fastcgi
listen = {{ADDR}}
chdir = $docroot
pm = static
pm.max_children = 1
EOT;

/* A raw request: the client reads to the end of the reply, which a closing
 * connection ends. A timeout is a failure, not an empty reply. */
function exchange(string $addr, string $request): string
{
    $fp = stream_socket_client("tcp://$addr", $errno, $errstr, 10);
    if ($fp === false) {
        throw new RuntimeException("cannot connect: $errstr");
    }
    stream_set_timeout($fp, 10);
    fwrite($fp, $request);
    $response = stream_get_contents($fp);
    $meta = stream_get_meta_data($fp);
    fclose($fp);
    if ($meta['timed_out']) {
        throw new RuntimeException('timed out before the reply ended');
    }
    return $response;
}

/* One reply with a Content-Length, read from a connection that stays open.
 * A timeout is a failure, not an empty reply. */
function readReply($fp): string
{
    $head = '';
    do {
        $line = fgets($fp);
        if ($line === false) {
            $meta = stream_get_meta_data($fp);
            throw new RuntimeException($meta['timed_out'] ? 'timed out before the reply head ended' : 'connection closed before the reply head ended');
        }
        $head .= $line;
    } while ($line !== "\r\n");
    if (!preg_match('{^Content-Length: (\d+)\r\n}mi', $head, $m)) {
        throw new RuntimeException('reply has no Content-Length: ' . $head);
    }
    return $head . fread($fp, (int) $m[1]);
}

$tester = new FPM\Tester($config, '<?php echo "unused";');
try {
    $tester->start();
    $tester->expectLogStartNotices();
    $httpAddr = $tester->getAddr('ipv4', '[http]');

    /* The bytes of a HEAD reply are 0: the gateway sends no body for HEAD. */
    $cases = [
        ["GET /flush.php HTTP/1.0\r\nHost: test\r\n\r\n", 'a', 1],
        ["GET /flush-cl.php HTTP/1.1\r\nHost: test\r\nConnection: close\r\n\r\n", 'a', 1],
        ["HEAD /flush-cl.php HTTP/1.1\r\nHost: test\r\nConnection: close\r\n\r\n", '', 0],
    ];
    $rounds = 5;
    $expectedLog = [];

    foreach ($cases as [$request, $body, $bytes]) {
        [$method, $uri, $protocol] = explode(' ', strtok($request, "\r\n"));
        for ($i = 0; $i < $rounds; $i++) {
            $response = exchange($httpAddr, $request);
            $end = strpos($response, "\r\n\r\n");
            if ($end === false || !preg_match('{^HTTP/1\.[01] 200 OK\r\n}', $response)) {
                throw new RuntimeException("$method $uri: no 200 reply: " . substr($response, 0, 300));
            }
            if (substr($response, $end + 4) !== $body) {
                throw new RuntimeException("$method $uri: unexpected body: " . substr($response, $end + 4, 300));
            }
            $expectedLog[] = [$method, $uri, $protocol, 200, $bytes];
        }
    }

    $fp = stream_socket_client("tcp://$httpAddr", $errno, $errstr, 10);
    if ($fp === false) {
        throw new RuntimeException("cannot connect: $errstr");
    }
    stream_set_timeout($fp, 10);
    /* Request 0 keeps the connection open and is read by its Content-Length.
     * Request 1 asks for Connection: close and is read to EOF. The gateway
     * closes the connection only after it has ended that request, so the step
     * after this one starts with the gateway idle. */
    for ($i = 0; $i < 2; $i++) {
        if ($i === 0) {
            fwrite($fp, "GET /flush-cl.php HTTP/1.1\r\nHost: test\r\n\r\n");
            $response = readReply($fp);
        } else {
            fwrite($fp, "GET /flush-cl.php HTTP/1.1\r\nHost: test\r\nConnection: close\r\n\r\n");
            $response = stream_get_contents($fp);
            if (stream_get_meta_data($fp)['timed_out']) {
                throw new RuntimeException("keep-alive request $i: timed out before the reply ended");
            }
        }
        $end = strpos($response, "\r\n\r\n");
        if ($end === false || !preg_match('{^HTTP/1\.1 200 OK\r\n}', $response) || substr($response, $end + 4) !== 'a') {
            throw new RuntimeException("keep-alive request $i: no 200 reply: " . substr($response, 0, 300));
        }
        $expectedLog[] = ['GET', '/flush-cl.php', 'HTTP/1.1', 200, 1];
    }
    fclose($fp);

    $response = exchange($httpAddr, "GET /ok.php HTTP/1.1\r\nHost: test\r\nConnection: close\r\n\r\n");
    if (!preg_match('{^HTTP/1\.1 200 OK\r\n}', $response) || !str_contains($response, 'ok')) {
        throw new RuntimeException('gateway did not serve the next request: ' . substr($response, 0, 300));
    }
    $expectedLog[] = ['GET', '/ok.php', 'HTTP/1.1', 200, 2];

    $accessLog = $tester->getPrefixedFile(FPM\Tester::FILE_EXT_LOG_ACC);
    $deadline = microtime(true) + 10;
    do {
        $lines = array_values(array_filter(explode("\n", (string) @file_get_contents($accessLog)), fn($l) => $l !== ''));
        if (count($lines) >= count($expectedLog)) {
            break;
        }
        usleep(100000);
    } while (microtime(true) < $deadline);

    $got = [];
    foreach ($lines as $line) {
        $obj = json_decode($line, true);
        if (!is_array($obj)) {
            throw new RuntimeException("access log line is not JSON: $line");
        }
        $got[] = [$obj['method'], $obj['uri'], $obj['protocol'], $obj['status'], $obj['bytes']];
    }
    if ($got !== $expectedLog) {
        throw new RuntimeException("access log differs:\n" . implode("\n", $lines));
    }

    echo "replies: 18 of 18 with the expected status and body\n";
    echo "access log: one line per request, in order, with status and bytes\n";
    echo "gateway: still serving\n";
} finally {
    $tester->terminate();
    $tester->close();
    @unlink($docroot . '/flush.php');
    @unlink($docroot . '/flush-cl.php');
    @unlink($docroot . '/ok.php');
    @rmdir($docroot);
}
?>
--EXPECT--
replies: 18 of 18 with the expected status and body
access log: one line per request, in order, with status and bytes
gateway: still serving
--CLEAN--
<?php
require_once "tester.inc";
FPM\Tester::clean();
?>

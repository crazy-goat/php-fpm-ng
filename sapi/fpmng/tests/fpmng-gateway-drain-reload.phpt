--TEST--
fpm-ng: SIGUSR2 reload drains a gateway request in flight instead of cutting it (issue #641)
--SKIPIF--
<?php
include "fpmng-skipif.inc";
fpmng_skip_if_pool_type_unsupported('gateway');
?>
--FILE--
<?php

require_once "tester.inc";

/* Issue #641 phase 1: a reload used to SIGTERM every gateway process in
 * fpm_http_cleanup(), and SIGTERM's default disposition killed the gateway
 * mid-response. The gateway now gets SIGQUIT, stops accepting, finishes the
 * response it already holds and exits within process_control_timeout.
 *
 * The failure this pins: the worker answers a large body quickly and the
 * gateway buffers it for a client that is not reading yet. The worker is done,
 * so the master reaches the reload's exec immediately and, without the drain,
 * SIGTERMs the gateway with the body still unwritten -- the client sees a
 * truncated response. With the drain the client reads the whole body.
 *
 * http.response_buffer = 0 so the gateway reads the whole upstream response
 * (the worker exits) and only the gateway-to-client half is still in flight. */

const BODY = 8 * 1024 * 1024;

$root = sys_get_temp_dir() . '/fpmng-gw-drain-reload-' . getmypid();
@mkdir($root, 0700, true);
file_put_contents("$root/app.php", <<<'PHP'
<?php
$n = (int) ($_GET['n'] ?? 0);
header('Content-Type: application/octet-stream');
header('Content-Length: ' . $n);
echo str_repeat('A', $n);
PHP);

$cfg = <<<EOT
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
log_level = notice
process_control_timeout = 10
[gw]
pool.type = gateway
listen = {{ADDR[http]}}
chdir = $root
http.gateways = 1
http.response_buffer = 0
http.route[app] = /
[app]
pool.type = fastcgi
listen = {{ADDR}}
chdir = $root
pm = static
pm.max_children = 2
EOT;

function read_all($fp, int $seconds = 30): string
{
    stream_set_timeout($fp, $seconds);
    $out = '';
    while (!feof($fp)) {
        $chunk = fread($fp, 65536);
        if ($chunk === false) {
            break;
        }
        if ($chunk === '') {
            $meta = stream_get_meta_data($fp);
            if ($meta['timed_out'] || feof($fp)) {
                break;
            }
            continue;
        }
        $out .= $chunk;
    }
    return $out;
}

$tester = new FPM\Tester($cfg, '<?php');
try {
    $tester->start();
    $tester->expectLogStartNotices();
    $http = $tester->getAddr('ipv4', '[http]');
    [$host, $port] = explode(':', $http);

    $fp = stream_socket_client("tcp://$http", $errno, $error, 5);
    if (!$fp) {
        throw new RuntimeException("connect: $error");
    }
    fwrite($fp, "GET /app.php?n=" . BODY . " HTTP/1.1\r\nHost: test\r\nConnection: close\r\n\r\n");

    /* Let the worker produce the body and the gateway buffer it in the client's
     * output buffer; the client is deliberately not reading yet. */
    usleep(800000);

    $tester->signal('USR2');
    /* Give the master time to exec and (without the fix) SIGTERM the gateway. */
    usleep(800000);

    $response = read_all($fp, 30);
    fclose($fp);

    $split = strpos($response, "\r\n\r\n");
    if ($split === false) {
        throw new RuntimeException('no response headers; got ' . strlen($response) . ' bytes');
    }
    $head = substr($response, 0, $split);
    $body = substr($response, $split + 4);
    if (!preg_match('#^HTTP/1\.1 200#', $head)) {
        throw new RuntimeException("status was not 200:\n$head");
    }
    if (strlen($body) !== BODY) {
        throw new RuntimeException('the reload cut the response: got ' . strlen($body) . ' of ' . BODY . ' bytes');
    }
    echo "in-flight gateway response completed across the reload: ok\n";

    /* The reload finished: a fresh connection is served by the new generation. */
    $deadline = microtime(true) + 20;
    $served = false;
    do {
        $fresh = @stream_socket_client("tcp://$http", $e2, $m2, 2);
        if ($fresh) {
            stream_set_timeout($fresh, 5);
            fwrite($fresh, "GET /app.php?n=16 HTTP/1.1\r\nHost: test\r\nConnection: close\r\n\r\n");
            $r = read_all($fresh, 5);
            fclose($fresh);
            if (str_contains($r, 'HTTP/1.1 200') && str_contains($r, str_repeat('A', 16))) {
                $served = true;
                break;
            }
        }
        usleep(100000);
    } while (microtime(true) < $deadline);
    if (!$served) {
        throw new RuntimeException('the new generation never served a fresh request');
    }
    echo "new generation serves after the drain: ok\n";

    echo "Done\n";
} finally {
    $tester->terminate();
    $tester->close();
    @unlink("$root/app.php");
    @rmdir($root);
}
?>
--EXPECT--
in-flight gateway response completed across the reload: ok
new generation serves after the drain: ok
Done
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>

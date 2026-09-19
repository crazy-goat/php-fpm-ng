--TEST--
fpm-ng: http.route[] HTTP transport does not let a 1xx interim response complete the exchange (issue #451)
--SKIPIF--
<?php
include "fpmng-skipif.inc";
fpmng_skip_if_pool_type_unsupported('http');
fpmng_skip_if_pool_type_unsupported('http-direct');
?>
--FILE--
<?php

require_once "tester.inc";

function check(bool $condition, string $message): void
{
    if (!$condition) throw new RuntimeException($message);
}

/* Issue #451: the target's 103 Early Hints took the transport's no_body
 * completion path, became the client's FINAL answer, and the real 200 behind
 * it arrived with no request to attach to -- a hanging client and a silent
 * log. An interim head is now discarded parser-side; the client sees the
 * final response. (The gateway does not relay interims; that is asserted
 * here too -- the first and only message is the 200.) */

$root = sys_get_temp_dir() . '/fpmng-route-1xx-' . getmypid();
@mkdir($root, 0700, true);

file_put_contents("$root/worker.php", <<<'PHP'
<?php
$notify = fpmng_worker_notify_stream();

$watcher = fpmng_worker_event_create(FPMNG_WORKER_READ, $notify, function () use ($notify): void {
    fread($notify, 65536);
    while (($id = fpmng_worker_next_request()) !== null) {
        fpm_send_early_hints(['Link' => '</style.css>; rel=preload'], $id);
        fpmng_worker_respond($id, 200, ['X-Kind' => 'final'], 'A-final-body');
    }
});
fpmng_worker_event_enable($watcher);
while (!fpmng_worker_may_exit()) {
    fpmng_worker_loop(true);
}
PHP);

$config = <<<EOT
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
[web]
listen = {{ADDR}}
pm = static
pm.max_children = 2
pool.type = http
http.gateways = 1
http.listen = {{ADDR[http]}}
http.route[d] = /d

[d]
listen = {{ADDR[d]}}
pm = static
pm.max_children = 1
pool.type = http-direct
pool.executor = worker
chdir = $root
http.front_controller = /worker.php
http.read_timeout = 10000
php_admin_value[max_execution_time] = 0
EOT;

function readMessage($fp): array
{
    $line = fgets($fp);
    if (!$line || !preg_match('#^HTTP/1\.(\d) (\d+) ([^\r\n]*)\r\n$#', $line, $m)) {
        throw new RuntimeException('bad status line: ' . var_export($line, true));
    }
    $status = (int) $m[2];
    $headers = [];
    while (($line = fgets($fp)) !== false && $line !== "\r\n") {
        [$k, $v] = explode(':', $line, 2);
        $headers[strtolower(trim($k))] = trim($v);
    }
    $body = '';
    if ($status >= 100 && $status < 200) {
        return [$status, $headers, $body];
    }
    if (($headers['transfer-encoding'] ?? '') === 'chunked') {
        while (true) {
            $size = hexdec(trim((string) fgets($fp)));
            if ($size === 0) {
                fgets($fp);
                break;
            }
            $chunk = '';
            while (strlen($chunk) < $size) {
                $part = fread($fp, $size - strlen($chunk));
                if ($part === false || $part === '') throw new RuntimeException('short chunk');
                $chunk .= $part;
            }
            $body .= $chunk;
            fgets($fp);
        }
        return [$status, $headers, $body];
    }
    $length = (int) ($headers['content-length'] ?? 0);
    while (strlen($body) < $length) {
        $part = fread($fp, $length - strlen($body));
        if ($part === false || $part === '') throw new RuntimeException('short body');
        $body .= $part;
    }
    return [$status, $headers, $body];
}

$tester = new FPM\Tester($config, '<?php echo "unused";');
try {
    $tester->start();
    $tester->expectLogStartNotices();
    $http = $tester->getAddr('ipv4', '[http]');

    $fp = stream_socket_client("tcp://$http", $errno, $error, 5);
    if (!$fp) throw new RuntimeException("connect: $error");
    stream_set_timeout($fp, 10);
    fwrite($fp, "GET /d/index.php HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n");

    /* The one and only message: the final 200. Before the fix the first
     * message here was a terminal 103 and nothing ever followed. */
    [$status, $headers, $body] = readMessage($fp);
    check($status === 200, 'the first message is the FINAL response, status ' . $status);
    check(($headers['x-kind'] ?? '') === 'final', 'final response headers: ' . var_export($headers, true));
    check($body === 'A-final-body', 'final body intact: ' . var_export($body, true));
    echo "interim-not-terminal: ok\n";
    fclose($fp);
} finally {
    $tester->terminate();
    $tester->close();
    @unlink("$root/worker.php");
    @rmdir($root);
}
echo "Done\n";
?>
--EXPECT--
interim-not-terminal: ok
Done
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>

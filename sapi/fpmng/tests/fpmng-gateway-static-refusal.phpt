--TEST--
fpm-ng: a gateway http.static refusal 404 logs its 10 body bytes, and a HEAD refusal has no body and leaves the keep-alive connection in step (#882)
--SKIPIF--
<?php
include "fpmng-skipif.inc";
fpmng_skip_if_pool_type_unsupported('gateway');
?>
--FILE--
<?php
require_once "tester.inc";

/* Issue #882: fpm_http_static_refuse() logged a fixed 10 for every method and
 * sent the body "Not Found\n" for HEAD too. libevent writes the body of a HEAD
 * reply (http.c:628-633, 2.1.12) and the reply has no Content-Length, so on a
 * keep-alive connection the client read the body as the start of the next
 * response. The same function serves the http-direct pool type: see
 * fpmng-http-direct-static-refusal.phpt. */
$base = sys_get_temp_dir() . '/fpmng-gw-static-refuse-' . getmypid();
@mkdir($base);
file_put_contents($base . '/app.css', 'body{color:red}');
file_put_contents($base . '/.env.css', 'SECRET');
file_put_contents($base . '/front.php', '<?php echo "php";');
$cfg = <<<EOT
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
[gw]
pool.type = gateway
listen = {{ADDR[http]}}
chdir = $base
http.gateways = 1
http.static = 1
http.front_controller = /front.php
http.route[web] = /
http.access_log = {{FILE:LOG:ACC}}
[web]
pool.type = fastcgi
listen = {{ADDR}}
chdir = $base
pm = static
pm.max_children = 1
EOT;

/* One keep-alive connection: the refusal reply to HEAD must have no body, and
 * the next response on the connection must start where the head of the first
 * one ended. Returns [status line, header lines] and reads no body. */
function readHead($fp): array
{
    $status = fgets($fp);
    if ($status === false) {
        throw new RuntimeException('no status line');
    }
    $headers = [];
    while (($line = fgets($fp)) !== false && $line !== "\r\n") {
        $headers[] = rtrim($line);
    }
    return [rtrim($status), $headers];
}

function expect(string $what, $actual, $expected): void
{
    if ($actual !== $expected) {
        throw new RuntimeException("$what: expected " . var_export($expected, true) .
            ', got ' . var_export($actual, true));
    }
}

/* The access log lines of the requests so far; waits until there are $count. */
function accessLines(string $file, int $count): array
{
    $deadline = microtime(true) + 10;
    do {
        $lines = array_values(array_filter(explode("\n", (string) @file_get_contents($file)), 'strlen'));
        if (count($lines) >= $count) {
            break;
        }
        usleep(100000);
    } while (microtime(true) < $deadline);
    return $lines;
}

$tester = new FPM\Tester($cfg, '<?php');
try {
    $tester->start();
    $tester->expectLogStartNotices();
    $http = $tester->getAddr('ipv4', '[http]');
    $log = $tester->getPrefixedFile(FPM\Tester::FILE_EXT_LOG_ACC);
    /* run-tests.php retries a failed test that uses usleep() in the same
     * tree, and the retry appends to the same access log. Read only the lines
     * of this attempt, so a retry reports its own result (#882 review R3-1). */
    $base = count(accessLines($log, 0));

    $fp = stream_socket_client("tcp://$http", $errno, $error, 5);
    stream_set_timeout($fp, 5);

    /* GET: the refusal has a body, and the log counts it. */
    fwrite($fp, "GET /.env.css HTTP/1.1\r\nHost: t\r\n\r\n");
    [$status, $headers] = readHead($fp);
    expect('GET status', $status, 'HTTP/1.1 404 Not Found');
    expect('GET content-length', in_array('Content-Length: 10', $headers, true), true);
    expect('GET body', fread($fp, 10), "Not Found\n");
    echo "GET refusal: body Not Found\n";

    /* HEAD: no body, and the next request on the same connection is parsed
     * from its own first byte. Before issue #882 the body of a HEAD reply went on
     * the wire with no Content-Length, and "Not Found" was read as the start of
     * the next response. */
    fwrite($fp, "HEAD /.env.css HTTP/1.1\r\nHost: t\r\n\r\n");
    [$status] = readHead($fp);
    expect('HEAD status', $status, 'HTTP/1.1 404 Not Found');
    fwrite($fp, "GET /app.css HTTP/1.1\r\nHost: t\r\n\r\n");
    [$status, $headers] = readHead($fp);
    expect('GET after HEAD status', $status, 'HTTP/1.1 200 OK');
    expect('GET after HEAD content-length', in_array('Content-Length: 15', $headers, true), true);
    expect('GET after HEAD body', fread($fp, 15), 'body{color:red}');
    echo "HEAD refusal: no body, next response parsed\n";

    $lines = array_slice(accessLines($log, $base + 3), $base);
    expect('access log lines', count($lines), 3);
    $got = array_map(static fn ($l) => preg_match('/"(\\S+) \\S+ [^"]*" (\\d{3}) (\\d+) /', $l, $m) ? $m[1] . ' ' . $m[2] . ' ' . $m[3] : $l, $lines);
    expect('access log', $got, ['GET 404 10', 'HEAD 404 0', 'GET 200 15']);
    echo "access log: GET 404 10, HEAD 404 0\n";

    echo "Done\n";
} finally {
    $tester->terminate();
    $tester->close();
    @unlink($base . '/app.css');
    @unlink($base . '/.env.css');
    @unlink($base . '/front.php');
    @rmdir($base);
}
?>
--EXPECT--
GET refusal: body Not Found
HEAD refusal: no body, next response parsed
access log: GET 404 10, HEAD 404 0
Done

--TEST--
fpm-ng: an http-direct http.static refusal 404 logs its 10 body bytes, and a HEAD refusal has no body and leaves the keep-alive connection in step (#882)
--SKIPIF--
<?php include "skipif.inc"; ?>
--FILE--
<?php
require_once "tester.inc";

/* Issue #882: the same fpm_http_static_refuse() as the gateway's, see
 * fpmng-gateway-static-refusal.phpt, which also checks the bytes field of the
 * access log. An http-direct access.format has no directive for the response
 * body bytes (%M is memory, fpm_http_direct_access_log.c), so this test checks
 * the wire and the status of each line only. */
$base = sys_get_temp_dir() . '/fpmng-direct-static-refuse-' . getmypid();
@mkdir($base);
file_put_contents($base . '/app.css', 'body{color:red}');
file_put_contents($base . '/.env.css', 'SECRET');
file_put_contents($base . '/front.php', '<?php echo "php";');
$cfg = <<<CFG
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
[direct]
listen = {{ADDR[http]}}
pool.type = http-direct
pm = static
pm.max_children = 1
chdir = $base
http.front_controller = /front.php
http.static = yes
access.log = {{FILE:LOG:ACC}}
access.format = "%m|%s"
CFG;

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
    $got = array_map(static fn ($l) => preg_match('/^(\\w+)\\|(\\d{3})$/', $l, $m) ? $m[1] . ' ' . $m[2] : $l, $lines);
    expect('access log', $got, ['GET 404', 'HEAD 404', 'GET 200']);
    echo "access log: one line per request\n";

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
access log: one line per request
Done

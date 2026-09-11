--TEST--
fpm-ng: direct HTTP answers ping/status, writes access.log and honours listen.allowed_clients
--SKIPIF--
<?php include "skipif.inc"; ?>
--FILE--
<?php
require_once "tester.inc";
$root = sys_get_temp_dir() . '/fpmng-operator-' . getmypid();
@mkdir($root);
$access = $root . '/access.log';
$refusedLog = $root . '/refused.log';
file_put_contents($root . '/front.php', <<<'PHP'
<?php
header('X-Marker: mark');
echo 'php:' . $_SERVER['REQUEST_URI'];
PHP);
$base = (int) (getenv('FPMNG_DIRECT_TEST_PORT') ?: 28054);
$port = $base + 16;
$denied = $base + 17;
/* %r is the path and %q the query string, exactly as in a fastcgi pool, so the
 * same access.format an operator already has keeps producing the same line
 * here. %{milli}d, %M and %{...}o are in the format because they are the
 * specifiers that have to come from somewhere other than the request line:
 * the scoreboard slot this child filled, and the response headers. */
$cfg = <<<CFG
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
[direct]
listen = 127.0.0.1:$port
pool.type = http-direct
pm = static
pm.max_children = 2
chdir = $root
http.front_controller = /front.php
ping.path = /ping
ping.response = alive
pm.status_path = /status
access.log = $access
access.format = "%R %m %r%Q%q %s %{milli}d %M %{X-Marker}o"
access.suppress_path[] = /ping
[denied]
listen = 127.0.0.1:$denied
pool.type = http-direct
pm = static
pm.max_children = 1
chdir = $root
http.front_controller = /front.php
listen.allowed_clients = 192.0.2.1
access.log = $refusedLog
access.format = "%R %m %r %s %l"
CFG;

/* One framed message per call, so a keep-alive connection stays usable. */
function fetch($fp, string $path, string $method = 'GET'): array
{
    fwrite($fp, "$method $path HTTP/1.1\r\nHost: test\r\n\r\n");
    $line = fgets($fp);
    if (!$line || !preg_match('#^HTTP/1\.1 (\d+) #', $line, $m)) {
        throw new RuntimeException('bad status line: ' . var_export($line, true));
    }
    $status = (int) $m[1];
    $headers = [];
    $length = 0;
    while (($line = fgets($fp)) !== false && $line !== "\r\n") {
        [$name, $value] = explode(':', $line, 2);
        $headers[strtolower($name)] = trim($value);
        if (strcasecmp($name, 'Content-Length') === 0) {
            $length = (int) trim($value);
        }
    }
    $body = '';
    while (strlen($body) < $length) {
        $chunk = fread($fp, $length - strlen($body));
        if ($chunk === false || $chunk === '') {
            throw new RuntimeException('short body');
        }
        $body .= $chunk;
    }
    return [$status, $headers, $body];
}

function expect(string $what, $actual, $expected): void
{
    if ($actual !== $expected) {
        throw new RuntimeException("$what: expected " . var_export($expected, true) .
            ', got ' . var_export($actual, true));
    }
}

$tester = new FPM\Tester($cfg, '<?php');
try {
    $tester->start();
    $tester->expectLogStartNotices();
    $fp = stream_socket_client("tcp://127.0.0.1:$port", $errno, $error, 5);
    stream_set_timeout($fp, 5);

    [$status, $headers, $body] = fetch($fp, '/ping');
    expect('ping status', $status, 200);
    expect('ping body', $body, 'alive');
    expect('ping type', $headers['content-type'], 'text/plain');
    /* A monitoring answer a proxy may cache is a monitoring answer that lies. */
    expect('ping cache', $headers['cache-control'], 'no-cache, no-store, must-revalidate, max-age=0');
    echo "ping: ok\n";

    [$status, , $body] = fetch($fp, '/app?x=1');
    expect('php status', $status, 200);
    expect('php body', $body, 'php:/app?x=1');
    echo "php: ok\n";

    [$status, $headers, $body] = fetch($fp, '/status');
    expect('status status', $status, 200);
    expect('status type', $headers['content-type'], 'text/plain');
    foreach (['pool:', 'process manager:', 'start since:', 'accepted conn:', 'idle processes:',
              'active processes:', 'total processes:', 'max children reached:', 'requests:',
              'non-php requests:', 'refused requests:', 'active requests:', 'memory peak:'] as $field) {
        if (!str_contains($body, $field)) {
            throw new RuntimeException("status field missing: $field\n$body");
        }
    }
    if (!preg_match('/^pool: +direct$/m', $body)) {
        throw new RuntimeException("status pool:\n$body");
    }
    if (!preg_match('/^process manager: +static$/m', $body)) {
        throw new RuntimeException("status pm:\n$body");
    }
    /* One PHP request has ended by now, and the status page itself is not one:
     * the counters have to tell those two apart, which is the whole reason a
     * direct pool keeps its own. */
    if (!preg_match('/^requests: +([1-9]\d*)$/m', $body)) {
        throw new RuntimeException("status requests:\n$body");
    }
    if (!preg_match('/^non-php requests: +([1-9]\d*)$/m', $body)) {
        throw new RuntimeException("status non-php requests:\n$body");
    }
    echo "status: ok\n";

    [$status, $headers, $body] = fetch($fp, '/status?json');
    expect('json status', $status, 200);
    expect('json type', $headers['content-type'], 'application/json');
    $decoded = json_decode($body, true, flags: JSON_THROW_ON_ERROR);
    expect('json pool', $decoded['pool'], 'direct');
    expect('json pm', $decoded['process manager'], 'static');
    if (!is_int($decoded['accepted conn']) || $decoded['accepted conn'] < 1) {
        throw new RuntimeException('json accepted conn: ' . var_export($decoded['accepted conn'], true));
    }
    echo "status json: ok\n";

    /* "/statuses" is not "/status": the path is matched whole, so a longer URL
     * that starts with it belongs to the application. */
    [, , $body] = fetch($fp, '/statuses');
    expect('prefix is not the endpoint', $body, 'php:/statuses');
    echo "whole path: ok\n";

    fclose($fp);

    $refused = stream_socket_client("tcp://127.0.0.1:$denied", $errno, $error, 5);
    if (!$refused) {
        throw new RuntimeException("connect: $error");
    }
    stream_set_timeout($refused, 5);
    /* The TCP connection is accepted -- the list is enforced on the request,
     * where libevent first knows who the peer is -- and the request is not. */
    [$status] = fetch($refused, '/app');
    expect('denied', $status, 403);
    fclose($refused);
    echo "allowed_clients: ok\n";

    /* The children write the log; give the last line time to land. */
    $lines = [];
    for ($i = 0; $i < 50 && count($lines) < 4; $i++) {
        usleep(100000);
        $lines = array_values(array_filter(explode("\n", (string) @file_get_contents($access))));
    }
    /* ping is in access.suppress_path, so it must not be here at all; /app is
     * a PHP request and /statuses another; the 403 belongs to the other pool,
     * which has its own. */
    foreach ($lines as $line) {
        if (str_contains($line, '/ping')) {
            throw new RuntimeException("suppressed path logged: $line");
        }
    }
    if (!preg_match('#^127\.0\.0\.1 GET /app\?x=1 200 [0-9]+\.[0-9]{3} [0-9]+ mark$#', $lines[0] ?? '')) {
        throw new RuntimeException('php line: ' . var_export($lines, true));
    }
    /* The status page never runs PHP, so %M has nothing to report and %{...}o
     * reads the headers this module set: a non-PHP line is still a line, with
     * the fields that do not apply left at zero rather than carried over from
     * whatever the child served last. */
    if (!preg_match('#^127\.0\.0\.1 GET /status 200 [0-9]+\.[0-9]{3} 0 -$#', $lines[1] ?? '')) {
        throw new RuntimeException('local line: ' . var_export($lines, true));
    }
    echo "access log: ok\n";

    /* A refusal is a response too, and the pool that refused it is the only
     * one that can record it: it never reached PHP. */
    $refusedLines = [];
    for ($i = 0; $i < 50 && $refusedLines === []; $i++) {
        usleep(100000);
        $refusedLines = array_values(array_filter(explode("\n", (string) @file_get_contents($refusedLog))));
    }
    expect('refusal line', $refusedLines[0] ?? '', '127.0.0.1 GET /app 403 0');
    echo "refusal logged: ok\n";

    echo "Done\n";
} finally {
    $tester->terminate();
    $tester->close();
    @unlink($root . '/front.php');
    @unlink($access);
    @unlink($refusedLog);
    @rmdir($root);
}
?>
--EXPECT--
ping: ok
php: ok
status: ok
status json: ok
whole path: ok
allowed_clients: ok
access log: ok
refusal logged: ok
Done
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>

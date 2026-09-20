--TEST--
fpm-ng: direct HTTP serves static files itself, opt-in, without a PHP request
--SKIPIF--
<?php include "skipif.inc"; ?>
--FILE--
<?php
require_once "tester.inc";
$root = sys_get_temp_dir() . '/fpmng-static-' . getmypid();
$outside = sys_get_temp_dir() . '/fpmng-static-outside-' . getmypid();
@mkdir($root);
@mkdir($root . '/assets');
@mkdir($root . '/assets/.hidden');
@mkdir($outside);
file_put_contents($outside . '/secret.css', 'OUTSIDE');
file_put_contents($root . '/assets/app.css', 'body{color:red}');
file_put_contents($root . '/assets/.hidden/app.css', 'HIDDEN');
file_put_contents($root . '/.env', 'DB_PASSWORD=hunter2');
file_put_contents($root . '/.backup.css', 'DOTFILE');
file_put_contents($root . '/archive.bin', 'UNKNOWN-EXTENSION');
symlink($outside . '/secret.css', $root . '/escape.css');
/* The front controller counts its own invocations into a file, because a direct
 * pool has no operator.status_path to ask (it is refused there). "PHP never ran" is
 * what "no PHP request" means from the outside, and this is how the test sees
 * it. */
file_put_contents($root . '/front.php', <<<'PHP'
<?php
$counter = __DIR__ . '/count';
$n = (int) @file_get_contents($counter) + 1;
file_put_contents($counter, (string) $n);
echo 'php:' . $n . ':' . $_SERVER['REQUEST_URI'];
PHP);
$port = (int) (getenv('FPMNG_DIRECT_TEST_PORT') ?: 28054) + 15;
$cfg = <<<CFG
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
[direct]
listen = 127.0.0.1:$port
pool.type = http-direct
pm = static
pm.max_children = 1
pm.max_requests = 12
chdir = $root
http.front_controller = /front.php
http.static = yes
CFG;

/* Reads exactly one framed message: the status line, the headers, and a body
 * bounded by Content-Length, so a keep-alive connection stays usable. */
function fetch($fp, string $path, string $method = 'GET', array $extra = []): array
{
    $request = "$method $path HTTP/1.1\r\nHost: test\r\n";
    foreach ($extra as $name => $value) {
        $request .= "$name: $value\r\n";
    }
    fwrite($fp, $request . "\r\n");
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
    if ($method !== 'HEAD' && $status !== 304) {
        while (strlen($body) < $length) {
            $chunk = fread($fp, $length - strlen($body));
            if ($chunk === false || $chunk === '') {
                throw new RuntimeException('short body');
            }
            $body .= $chunk;
        }
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

    [$status, $headers, $body] = fetch($fp, '/assets/app.css');
    expect('status', $status, 200);
    expect('body', $body, 'body{color:red}');
    expect('content-type', $headers['content-type'], 'text/css; charset=UTF-8');
    expect('content-length', $headers['content-length'], '15');
    if (!preg_match('/^[A-Z][a-z]{2}, \d{2} [A-Z][a-z]{2} \d{4} \d{2}:\d{2}:\d{2} GMT$/', $headers['last-modified'] ?? '')) {
        throw new RuntimeException('last-modified: ' . var_export($headers['last-modified'] ?? null, true));
    }
    echo "served: ok\n";

    [$status] = fetch($fp, '/assets/app.css', extra: ['If-None-Match' => $headers['etag']]);
    expect('if-none-match', $status, 304);
    [$status] = fetch($fp, '/assets/app.css', extra: ['If-Modified-Since' => $headers['last-modified']]);
    expect('if-modified-since', $status, 304);
    [$status] = fetch($fp, '/assets/app.css', extra: ['If-None-Match' => '"stale"']);
    expect('stale etag', $status, 200);
    echo "conditional: ok\n";

    [$status, $headers, $body] = fetch($fp, '/assets/app.css', 'HEAD');
    expect('head status', $status, 200);
    expect('head content-length', $headers['content-length'], '15');
    expect('head body', $body, '');
    echo "head: ok\n";

    /* Refused outright: the file is there and this module would otherwise have
     * served it, so anything but a refusal is the leak. */
    foreach (['/.backup.css', '/assets/.hidden/app.css', '/escape.css'] as $path) {
        [$status] = fetch($fp, $path);
        expect("refused $path", $status, 404);
    }
    echo "refused: ok\n";

    /* Not this module's business: PHP decides, and says so by answering. */
    $php = 0;
    /* '/.env' is in this list on purpose: a dot-segment is refused only where
     * this module would have served the file. An extension it has no type for
     * was the application's to route before http.static was turned on, and
     * turning it on must not make the URL disappear. */
    foreach (['/../../etc/passwd', '/%2e%2e/%2e%2e/etc/passwd', '/archive.bin', '/.env',
              '/front.php', '/assets/', '/assets/missing.css'] as $path) {
        [$status, , $body] = fetch($fp, $path);
        expect("php answered $path", $status, 200);
        expect("php body $path", $body, 'php:' . ++$php . ':' . $path);
    }
    [$status, , $body] = fetch($fp, '/assets/app.css', 'POST');
    expect('post body', $body, 'php:' . ++$php . ':/assets/app.css');
    echo "handed to php: ok\n";

    /* Eleven static hits against pm.max_requests = 12: a static reply must not
     * retire the child, or this connection would have been closed long ago and
     * the PHP request after it could not be served on it. */
    for ($i = 0; $i < 11; $i++) {
        [$status] = fetch($fp, '/assets/app.css');
        expect('repeat', $status, 200);
    }
    [$status, , $body] = fetch($fp, '/whatever');
    expect('php still there', $body, 'php:' . ++$php . ':/whatever');
    expect('php request count', (int) file_get_contents($root . '/count'), $php);
    echo "no request accounted: ok\n";

    fclose($fp);
    echo "Done\n";
} finally {
    $tester->terminate();
    $tester->close();
    @unlink($root . '/escape.css');
    @unlink($root . '/assets/.hidden/app.css');
    @unlink($root . '/assets/app.css');
    @unlink($root . '/.env');
    @unlink($root . '/.backup.css');
    @unlink($root . '/archive.bin');
    @unlink($root . '/front.php');
    @unlink($root . '/count');
    @unlink($outside . '/secret.css');
    @rmdir($root . '/assets/.hidden');
    @rmdir($root . '/assets');
    @rmdir($root);
    @rmdir($outside);
}
?>
--EXPECT--
served: ok
conditional: ok
head: ok
refused: ok
handed to php: ok
no request accounted: ok
Done
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>

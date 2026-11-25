--TEST--
fpm-ng: http-direct Cache-Control by path prefix on static files (http.static.cache_control, issue #648)
--SKIPIF--
<?php include "skipif.inc"; ?>
--FILE--
<?php
require_once "tester.inc";
$root = sys_get_temp_dir() . '/fpmng-cc-' . getmypid();
@mkdir($root);
@mkdir($root . '/assets');
@mkdir($root . '/assets/vendor');
file_put_contents($root . '/assets/app.css', 'body{color:red}');
file_put_contents($root . '/assets/vendor/lib.js', 'var lib=1;');
file_put_contents($root . "/archive.bin", "UNKNOWN-EXTENSION");
file_put_contents($root . '/robots.txt', 'User-agent: *');
file_put_contents($root . '/front.php', '<?php echo "php:", $_SERVER["REQUEST_URI"];');
$port = 28123 + 200 * (int) getenv('TEST_PHP_WORKER') + (int) getenv('FPMNG_PHPT_PORT_SHIFT');

function config(int $port, string $root, string $extra): string
{
    return <<<CFG
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
[direct]
listen = 127.0.0.1:$port
pool.type = http-direct
pm = static
pm.max_children = 1
chdir = $root
http.front_controller = /front.php
http.static = yes
$extra
CFG;
}

/* One framed reply: status, headers, and a body bounded by Content-Length. A
 * HEAD reply and a 304 have no body on the wire, whatever their length says. */
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

function withPool(string $cfg, int $port, callable $body): void
{
    $tester = new FPM\Tester($cfg, '<?php');
    try {
        $tester->start();
        $tester->expectLogStartNotices();
        $fp = stream_socket_client("tcp://127.0.0.1:$port", $errno, $error, 5);
        stream_set_timeout($fp, 5);
        $body($fp);
        fclose($fp);
    } finally {
        $tester->terminate();
        $tester->close();
    }
}

/* The value is quoted: an unquoted ini value cannot hold '='. The longest
 * matching prefix wins, so /assets/vendor/ overrides /assets/ for its files. */
$rules = <<<INI
http.static.cache_control["/assets/"] = "public, max-age=3600"
http.static.cache_control["/assets/vendor/"] = "public, max-age=31536000, immutable"
INI;

try {
    withPool(config($port, $root, $rules), $port, function ($fp) {
        [$status, $headers, $body] = fetch($fp, '/assets/app.css');
        expect('app status', $status, 200);
        expect('app body', $body, 'body{color:red}');
        expect('app cache-control', $headers['cache-control'] ?? null, 'public, max-age=3600');
        $etag = $headers['etag'];
        $modified = $headers['last-modified'];
        echo "prefix: ok\n";

        [$status, $headers] = fetch($fp, '/assets/vendor/lib.js');
        expect('vendor status', $status, 200);
        expect('vendor cache-control (longest prefix)', $headers['cache-control'] ?? null,
            'public, max-age=31536000, immutable');
        echo "longest prefix: ok\n";

        /* No rule matches: the reply carries no Cache-Control at all, and PHP's
         * own answer is not touched. */
        [$status, $headers, $body] = fetch($fp, '/robots.txt');
        expect('unmatched static status', $status, 200);
        expect('unmatched static cache-control', $headers['cache-control'] ?? null, null);
        [$status, $headers, $body] = fetch($fp, '/archive.bin');
        expect('php answers a file type static does not serve', $status, 200);
        expect('php body', $body, 'php:/archive.bin');
        expect('php cache-control', $headers['cache-control'] ?? null, null);
        echo "unmatched: ok\n";

        /* A 304 repeats the Cache-Control of the 200 it stands for. */
        [$status, $headers] = fetch($fp, '/assets/app.css', 'GET', ['If-None-Match' => $etag]);
        expect('304 status', $status, 304);
        expect('304 cache-control', $headers['cache-control'] ?? null, 'public, max-age=3600');
        echo "304: ok\n";

        [$status, $headers, $body] = fetch($fp, '/assets/app.css', 'HEAD');
        expect('head status', $status, 200);
        expect('head cache-control', $headers['cache-control'] ?? null, 'public, max-age=3600');
        expect('head body', $body, '');
        echo "head: ok\n";

        [$status, $headers, $body] = fetch($fp, '/assets/app.css', 'GET', ['Range' => 'bytes=0-4']);
        expect('range status without http.static.ranges', $status, 200);
        expect('range reply cache-control', $headers['cache-control'] ?? null, 'public, max-age=3600');
        expect('range reply body', $body, 'body{color:red}');
        echo "range reply: ok\n";

        [$status, $headers, $body] = fetch($fp, '/assets/app.css', 'GET', ['If-Modified-Since' => $modified]);
        expect('if-modified-since status', $status, 304);
        expect('if-modified-since cache-control', $headers['cache-control'] ?? null, 'public, max-age=3600');
        expect('if-modified-since body', $body, '');
        echo "if-modified-since: ok\n";
    });

    /* Without the directive the same files get no Cache-Control, so existing
     * deployments see no change. */
    withPool(config($port, $root, ''), $port, function ($fp) {
        [$status, $headers, $body] = fetch($fp, '/assets/app.css');
        expect('default off status', $status, 200);
        expect('default off body', $body, 'body{color:red}');
        expect('default off cache-control', $headers['cache-control'] ?? null, null);
        echo "default off: ok\n";
    });
    echo "Done\n";
} finally {
    @unlink($root . '/assets/app.css');
    @unlink($root . '/assets/vendor/lib.js');
    @unlink($root . "/archive.bin");
    @unlink($root . '/robots.txt');
    @unlink($root . '/front.php');
    @rmdir($root . '/assets/vendor');
    @rmdir($root . '/assets');
    @rmdir($root);
}
?>
--EXPECT--
prefix: ok
longest prefix: ok
unmatched: ok
304: ok
head: ok
range reply: ok
if-modified-since: ok
default off: ok
Done
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>

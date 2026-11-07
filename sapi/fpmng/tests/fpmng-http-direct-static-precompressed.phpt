--TEST--
fpm-ng: http-direct serves .br and .gz sidecars only inside the containment check (http.static.precompressed, issue #648)
--SKIPIF--
<?php include "skipif.inc"; ?>
--FILE--
<?php
require_once "tester.inc";
$root = sys_get_temp_dir() . '/fpmng-precomp-' . getmypid();
$outside = sys_get_temp_dir() . '/fpmng-precomp-outside-' . getmypid();
@mkdir($root);
@mkdir($root . '/assets');
@mkdir($outside);
file_put_contents($root . '/assets/app.css', 'body{color:red}');
file_put_contents($root . '/assets/app.css.br', 'BR-BODY');
file_put_contents($root . '/assets/app.css.gz', 'GZ-BODY');
file_put_contents($root . '/assets/only.js', 'console.log(1)');
file_put_contents($root . '/assets/only.js.gz', 'GZ-ONLY');
file_put_contents($root . '/assets/ghost.css.br', 'GHOST');
file_put_contents($root . '/assets/linked.css', 'LINKED-IDENTITY');
file_put_contents($root . '/assets/plain.css', 'PLAIN-IDENTITY');
file_put_contents($outside . '/secret.br', 'OUTSIDE-BR');
file_put_contents($outside . '/secret.gz', 'OUTSIDE-GZ');
symlink($outside . '/secret.br', $root . '/assets/linked.css.br');
symlink($outside . '/secret.gz', $root . '/assets/plain.css.gz');
file_put_contents($root . '/front.php', '<?php echo "php:", $_SERVER["REQUEST_URI"];');
$port = 28121 + 200 * (int) getenv('TEST_PHP_WORKER') + (int) getenv('FPMNG_PHPT_PORT_SHIFT');

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

try {
    withPool(config($port, $root, 'http.static.precompressed = yes'), $port, function ($fp) {
        $br = ['Accept-Encoding' => 'br, gzip'];

        [$status, $headers, $body] = fetch($fp, '/assets/app.css', 'GET', $br);
        expect('br status', $status, 200);
        expect('br body', $body, 'BR-BODY');
        expect('br content-encoding', $headers['content-encoding'] ?? null, 'br');
        expect('br content-type is the original', $headers['content-type'], 'text/css; charset=UTF-8');
        expect('br vary', $headers['vary'] ?? null, 'Accept-Encoding');
        if (!str_ends_with($headers['etag'], '-br"')) {
            throw new RuntimeException('br etag: ' . $headers['etag']);
        }
        $brEtag = $headers['etag'];
        echo "br: ok\n";

        [$status, $headers, $body] = fetch($fp, '/assets/app.css', 'GET', ['Accept-Encoding' => 'gzip']);
        expect('gzip body', $body, 'GZ-BODY');
        expect('gzip content-encoding', $headers['content-encoding'] ?? null, 'gzip');
        echo "gzip: ok\n";

        [, $headers, $body] = fetch($fp, '/assets/app.css', 'GET', ['Accept-Encoding' => 'br;q=0, gzip']);
        expect('q=0 refuses br', $body, 'GZ-BODY');
        expect('q=0 content-encoding', $headers['content-encoding'] ?? null, 'gzip');
        [, $headers, $body] = fetch($fp, '/assets/app.css', 'GET', ['Accept-Encoding' => 'br;q=0.000, gzip;q=0']);
        expect('both q=0 gives identity', $body, 'body{color:red}');
        expect('both q=0 has no content-encoding', $headers['content-encoding'] ?? null, null);
        echo "q-values: ok\n";

        [, $headers, $body] = fetch($fp, '/assets/app.css', 'GET', ['Accept-Encoding' => '*']);
        expect('wildcard does not select a sidecar', $body, 'body{color:red}');
        [, $headers, $body] = fetch($fp, '/assets/app.css');
        expect('no Accept-Encoding gives identity', $body, 'body{color:red}');
        expect('identity reply still varies', $headers['vary'] ?? null, 'Accept-Encoding');
        $identityEtag = $headers['etag'];
        if (str_ends_with($identityEtag, '-br"') || str_ends_with($identityEtag, '-gzip"')) {
            throw new RuntimeException('identity etag carries a coding: ' . $identityEtag);
        }
        echo "negotiation: ok\n";

        [$status, , $body] = fetch($fp, '/assets/only.js', 'GET', $br);
        expect('missing br falls back to gzip', $body, 'GZ-ONLY');
        echo "fallback: ok\n";

        [$status, $headers] = fetch($fp, '/assets/app.css', 'GET', ['Accept-Encoding' => 'br', 'If-None-Match' => $brEtag]);
        expect('conditional br', $status, 304);
        expect('304 keeps vary', $headers['vary'] ?? null, 'Accept-Encoding');
        [$status, , $body] = fetch($fp, '/assets/app.css', 'GET', ['Accept-Encoding' => 'br', 'If-None-Match' => $identityEtag]);
        expect('identity etag does not match the br representation', $status, 200);
        expect('after a mismatch the br body is sent', $body, 'BR-BODY');
        echo "conditional: ok\n";

        [$status, $headers, $body] = fetch($fp, '/assets/app.css', 'HEAD', $br);
        expect('head status', $status, 200);
        expect('head content-length is the sidecar', $headers['content-length'], '7');
        expect('head body', $body, '');
        expect('head content-encoding', $headers['content-encoding'] ?? null, 'br');
        echo "head: ok\n";

        /* A sidecar is not served on its own: with no original there is nothing
         * to be the representation of, so the request goes to PHP. */
        [, , $body] = fetch($fp, '/assets/ghost.css', 'GET', $br);
        expect('sidecar without original', $body, 'php:/assets/ghost.css');

        /* A sidecar symlink that leaves the root is refused as the original would
         * be, and the original is still served, as identity. */
        [, $headers, $body] = fetch($fp, '/assets/linked.css', 'GET', $br);
        expect('outside br sidecar is not served', $body, 'LINKED-IDENTITY');
        expect('outside br sidecar: no content-encoding', $headers['content-encoding'] ?? null, null);
        [, , $body] = fetch($fp, '/assets/plain.css', 'GET', ['Accept-Encoding' => 'gzip']);
        expect('outside gzip sidecar is not served', $body, 'PLAIN-IDENTITY');
        echo "containment: ok\n";
    });

    /* The same root with the option left out: the sidecars are on disk and
     * must not change the answer. */
    withPool(config($port, $root, ''), $port, function ($fp) {
        [$status, $headers, $body] = fetch($fp, '/assets/app.css', 'GET', ['Accept-Encoding' => 'br, gzip']);
        expect('default off: status', $status, 200);
        expect('default off: identity body', $body, 'body{color:red}');
        expect('default off: no content-encoding', $headers['content-encoding'] ?? null, null);
        expect('default off: no vary', $headers['vary'] ?? null, null);
        echo "default off: ok\n";
    });
    echo "Done\n";
} finally {
    @unlink($root . '/assets/app.css');
    @unlink($root . '/assets/app.css.br');
    @unlink($root . '/assets/app.css.gz');
    @unlink($root . '/assets/only.js');
    @unlink($root . '/assets/only.js.gz');
    @unlink($root . '/assets/ghost.css.br');
    @unlink($root . '/assets/linked.css');
    @unlink($root . '/assets/plain.css');
    @unlink($root . '/assets/linked.css.br');
    @unlink($root . '/assets/plain.css.gz');
    @unlink($root . '/front.php');
    @unlink($outside . '/secret.br');
    @unlink($outside . '/secret.gz');
    @rmdir($root . '/assets');
    @rmdir($root);
    @rmdir($outside);
}
?>
--EXPECT--
br: ok
gzip: ok
q-values: ok
negotiation: ok
fallback: ok
conditional: ok
head: ok
containment: ok
default off: ok
Done
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>

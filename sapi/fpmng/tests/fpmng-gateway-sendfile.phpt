--TEST--
fpm-ng: gateway serves X-Accel-Redirect and X-Sendfile files from http.sendfile_root and refuses the rest (#649)
--SKIPIF--
<?php
include "fpmng-skipif.inc";
fpmng_skip_if_pool_type_unsupported('gateway');
?>
--FILE--
<?php
require_once "tester.inc";
$base = sys_get_temp_dir() . '/fpmng-sendfile-' . getmypid();
$root = $base . '/protected';
@mkdir($root, 0777, true);
$payload = 'protected payload';
file_put_contents($root . '/file.txt', $payload);
file_put_contents($root . '/empty.txt', '');
file_put_contents($root . '/.hidden', 'dot');
file_put_contents($root . '/x.php', '<?php echo "source";');
file_put_contents($base . '/secret.txt', 'outside the root');
symlink($base . '/secret.txt', $root . '/link.txt');
/* A directory symlink in the middle of the name: the file is outside the root
 * through it, and the realpath() containment must refuse it. */
@mkdir($base . '/outside-dir', 0777, true);
file_put_contents($base . '/outside-dir/s.txt', 'outside through a directory');
symlink($base . '/outside-dir', $root . '/mid');
exec('mkfifo ' . escapeshellarg($root . '/pipe.txt'), $out, $status);
if ($status !== 0) {
    throw new RuntimeException("mkfifo failed with status $status");
}
/* A file larger than the usual write buffer, so the body is sent in pieces.
 * The gateway must not read it into memory (evbuffer_add_file()). */
$large = str_repeat('0123456789abcdef', 1024 * 1024);
file_put_contents($root . '/large.bin', $large);

/* The FastCGI worker answers with the header named by the query string and a
 * body of its own. A reply that the gateway serves from the file never shows
 * the worker body, and a header the gateway did not act on shows up in the
 * response. With p=1 the worker also sends headers that a served file keeps
 * (Content-Disposition, Cache-Control, Set-Cookie, Content-Type) and a
 * Content-Length, Content-Encoding and Accept-Ranges that describe its own body
 * or its own ranges, which the gateway drops. */
file_put_contents($base . '/send.php', '<?php
if (isset($_GET["r"])) { header("X-Accel-Redirect: " . $_GET["r"]); }
if (isset($_GET["s"])) { header("X-Sendfile: " . $_GET["s"]); }
if (isset($_GET["p"])) {
    header("Content-Type: application/pdf");
    header("Content-Disposition: attachment; filename=\"report.pdf\"");
    header("Cache-Control: private");
    header("Set-Cookie: a=b");
    header("Content-Length: 11");
    header("Content-Encoding: gzip");
    header("Accept-Ranges: bytes");
}
echo "worker body";
');

$cfg = <<<EOT
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
[gw]
pool.type = gateway
listen = {{ADDR[http]}}
chdir = $base
http.gateways = 1
http.front_controller = /send.php
http.sendfile_root = $root
http.route[web] = /
[web]
pool.type = fastcgi
listen = {{ADDR}}
chdir = $base
pm = static
pm.max_children = 1
EOT;

/* Returns the status, the response headers as "name: value" lines, and the
 * body. A timeout gives status 0 and body false. The timeout is short on
 * purpose: a blocked gateway fails the test in seconds. */
function httpReq(string $method, string $url, int $timeout = 5): array
{
    $ctx = stream_context_create(['http' => ['method' => $method, 'timeout' => $timeout, 'ignore_errors' => true]]);
    $body = @file_get_contents($url, false, $ctx);
    $headers = $http_response_header ?? [];
    $status = preg_match('#^HTTP/1\.[01] (\d{3}) #', $headers[0] ?? '', $m) ? (int) $m[1] : 0;
    return [$status, $headers, $body];
}

function header_value(array $headers, string $name): ?string
{
    foreach ($headers as $line) {
        if (stripos($line, $name . ':') === 0) {
            return trim(substr($line, strlen($name) + 1));
        }
    }
    return null;
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
    $http = $tester->getAddr('ipv4', '[http]');
    $url = fn(string $query) => "http://$http/?" . $query;

    /* Served from the root: the body is the file, the length is the file's,
     * and the redirect header is gone. */
    [$status, $headers, $body] = httpReq('GET', $url('r=' . urlencode('/file.txt')));
    expect('accel status', $status, 200);
    expect('accel body', $body, $payload);
    expect('accel length', header_value($headers, 'Content-Length'), (string) strlen($payload));
    expect('accel header stripped', header_value($headers, 'X-Accel-Redirect'), null);
    /* A Content-Length reply, not a chunked one: the length is known up front. */
    expect('accel not chunked', header_value($headers, 'Transfer-Encoding'), null);
    echo "x-accel-redirect serves the file: ok\n";

    /* The headers the worker sent for the download reach the client. The
     * worker's Content-Length is dropped, and the file's length is sent. */
    [$status, $headers, $body] = httpReq('GET', $url('r=' . urlencode('/file.txt') . '&p=1'));
    expect('kept status', $status, 200);
    expect('kept body', $body, $payload);
    expect('kept content-type', header_value($headers, 'Content-Type'), 'application/pdf');
    expect('kept content-disposition', header_value($headers, 'Content-Disposition'), 'attachment; filename="report.pdf"');
    expect('kept cache-control', header_value($headers, 'Cache-Control'), 'private');
    expect('kept set-cookie', header_value($headers, 'Set-Cookie'), 'a=b');
    expect('upstream length dropped', header_value($headers, 'Content-Length'), (string) strlen($payload));
    /* The file is sent raw and the gateway answers no ranges, so the worker's
     * Content-Encoding and Accept-Ranges must not reach the client. */
    expect('upstream content-encoding dropped', header_value($headers, 'Content-Encoding'), null);
    expect('upstream accept-ranges dropped', header_value($headers, 'Accept-Ranges'), null);
    echo "upstream headers pass through: ok\n";

    /* X-Sendfile names an absolute path, which must lie inside the root. */
    [$status, $headers, $body] = httpReq('GET', $url('s=' . urlencode($root . '/file.txt')));
    expect('sendfile status', $status, 200);
    expect('sendfile body', $body, $payload);
    expect('sendfile header stripped', header_value($headers, 'X-Sendfile'), null);
    echo "x-sendfile serves the file: ok\n";

    /* Refused names: 404, the worker body never shows, and the gateway
     * keeps serving afterwards. */
    $refused = [
        'traversal' => 'r=' . urlencode('/../secret.txt'),
        'absolute outside' => 's=' . urlencode($base . '/secret.txt'),
        'symlink escape' => 'r=' . urlencode('/link.txt'),
        'dot-segment' => 'r=' . urlencode('/.hidden'),
        'php script' => 'r=' . urlencode('/x.php'),
        'fifo' => 'r=' . urlencode('/pipe.txt'),
        'missing' => 'r=' . urlencode('/missing.txt'),
        'relative sendfile' => 's=' . urlencode('file.txt'),
        'symlink in a middle directory' => 'r=' . urlencode('/mid/s.txt'),
    ];
    foreach ($refused as $label => $query) {
        [$status, $headers, $body] = httpReq('GET', $url($query));
        expect("$label status", $status, 404);
        expect("$label body", $body, "Not Found\n");
        expect("$label accel header stripped", header_value($headers, 'X-Accel-Redirect'), null);
        expect("$label sendfile header stripped", header_value($headers, 'X-Sendfile'), null);
        expect("$label no worker headers", header_value($headers, 'Content-Disposition'), null);
        echo "$label refused: ok\n";
    }
    /* The FIFO must not block the event loop: a regular file still answers. */
    [$status, , $body] = httpReq('GET', $url('r=' . urlencode('/file.txt')));
    expect('after refusals', [$status, $body], [200, $payload]);
    echo "gateway serves after refusals: ok\n";

    /* A zero-length file: the reply is empty, with Content-Length 0. */
    [$status, $headers, $body] = httpReq('GET', $url('r=' . urlencode('/empty.txt')));
    expect('empty status', $status, 200);
    expect('empty body', $body, '');
    expect('empty length', header_value($headers, 'Content-Length'), '0');
    echo "empty file: ok\n";

    /* HEAD: the length of the file, and no body. */
    [$status, $headers, $body] = httpReq('HEAD', $url('r=' . urlencode('/file.txt')));
    expect('head status', $status, 200);
    expect('head length', header_value($headers, 'Content-Length'), (string) strlen($payload));
    expect('head body', $body, '');
    echo "head: ok\n";

    /* A large file arrives whole and byte for byte. */
    [$status, $headers, $body] = httpReq('GET', $url('r=' . urlencode('/large.bin')), 30);
    expect('large status', $status, 200);
    expect('large length', header_value($headers, 'Content-Length'), (string) strlen($large));
    expect('large content', hash('sha256', (string) $body), hash('sha256', $large));
    echo "large file: ok\n";
    echo "Done\n";
} finally {
    $tester->terminate();
    $tester->close();
    foreach (glob($root . '/*') ?: [] as $f) {
        @unlink($f);
    }
    @unlink($base . '/secret.txt');
    @unlink($base . '/outside-dir/s.txt');
    @rmdir($base . '/outside-dir');
    @unlink($base . '/send.php');
    @unlink($root . '/.hidden');
    @rmdir($root);
    @rmdir($base);
}
?>
--EXPECT--
x-accel-redirect serves the file: ok
upstream headers pass through: ok
x-sendfile serves the file: ok
traversal refused: ok
absolute outside refused: ok
symlink escape refused: ok
dot-segment refused: ok
php script refused: ok
fifo refused: ok
missing refused: ok
relative sendfile refused: ok
symlink in a middle directory refused: ok
gateway serves after refusals: ok
empty file: ok
head: ok
large file: ok
Done
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>

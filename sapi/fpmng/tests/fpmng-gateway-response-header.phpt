--TEST--
fpm-ng: http.response_header[] reaches every reply the gateway sends, once, and replaces the application's header of the same name; http.error_page[] replaces the gateway's own error bodies (issue #650)
--SKIPIF--
<?php
include "fpmng-skipif.inc";
fpmng_skip_if_pool_type_unsupported('gateway');
?>
--FILE--
<?php
require_once "tester.inc";

/* Issue #650. The gateway adds the configured headers where it adds
 * X-Request-Id, and every place that clears the output headers adds them back.
 * Each reply below is one such place. The header must be on the wire exactly
 * once: a second copy would be the application's own, which the gateway drops
 * (the configured value wins), or one the clear-and-add path added twice. */

$base = sys_get_temp_dir() . '/fpmng-gw-resp-header-' . getmypid();
$fcSock = $base . '-dead.sock';
@mkdir("$base/pages", 0700, true);
@mkdir("$base/prot", 0700, true);
file_put_contents("$base/app.css", 'body{color:red}');
file_put_contents("$base/.env.css", 'SECRET');
file_put_contents("$base/prot/file.txt", 'protected payload');
file_put_contents("$base/pages/404.html", '<h1>nothing here</h1>');
file_put_contents("$base/pages/502.json", '{"error":"bad gateway"}');
file_put_contents("$base/pages/503.html", '<h1>busy</h1>');
file_put_contents("$base/front.php", <<<'PHP'
<?php
switch ($_GET['m'] ?? '') {
    case 'hsts':
        header('Strict-Transport-Security: max-age=1');
        header('Strict-Transport-Security: max-age=2', false);
        header('X-Frame-Options: SAMEORIGIN');
        echo 'app body';
        break;
    case 'badstatus':
        header('Status: abc');
        echo 'body';
        break;
    case 'accel':
        header('X-Accel-Redirect: ' . $_GET['r']);
        echo 'worker body';
        break;
    case 'app404':
        http_response_code(404);
        echo 'app not found';
        break;
    case 'hold':
        /* Pins the worker until the test releases it: $_GET['f'] is both the
         * "started" marker and, with ".release", the release file. Bounded at
         * 30 s so a failed test cannot leave the worker busy for ever. */
        touch($_GET['f']);
        for ($i = 0; $i < 300 && !file_exists($_GET['f'] . '.release'); $i++) {
            usleep(100000);
        }
        echo 'held';
        break;
    default:
        echo 'ok';
}
PHP);

$cfg = <<<EOT
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
process_control_timeout = 5
[gw]
pool.type = gateway
listen = {{ADDR[http]}}
chdir = $base
http.gateways = 1
http.static = 1
http.front_controller = /front.php
http.sendfile_root = $base/prot
http.access_log = {{FILE:LOG:ACC}}
ping.path = /ping
http.ready_path = /ready
http.upstream_read_timeout = 4000
http.route[web] = /api
http.route[hang] = /hang
http.route[slow] = /slow
http.route[dead] = /dead
http.response_header[Strict-Transport-Security] = "max-age=31536000; includeSubDomains"
http.response_header[X-Frame-Options] = DENY
http.error_page[404] = $base/pages/404.html
http.error_page[502] = $base/pages/502.json
http.error_page[503] = $base/pages/503.html
[web]
pool.type = fastcgi
listen = {{ADDR[web]}}
chdir = $base
pm = static
pm.max_children = 2
[slow]
pool.type = fastcgi
listen = {{ADDR[slow]}}
chdir = $base
pm = static
pm.max_children = 1
[hang]
pool.type = fastcgi
listen = {{ADDR[hang]}}
chdir = $base
pm = static
pm.max_children = 1
[dead]
pool.type = fastcgi
listen = $fcSock
chdir = $base
pm = static
pm.max_children = 1
EOT;

function fail(string $what, string $extra = ''): void
{
    throw new RuntimeException($what . ($extra === '' ? '' : "\n$extra"));
}

/* Sends one request with Connection: close and returns the parsed reply. */
function req(string $addr, string $method, string $target, array $extra = []): array
{
    $fp = stream_socket_client("tcp://$addr", $errno, $error, 5);
    if (!$fp) {
        fail("connect: $error");
    }
    stream_set_timeout($fp, 10);
    fwrite($fp, "$method $target HTTP/1.1\r\nHost: t\r\nConnection: close\r\n" . implode('', array_map(static fn ($h) => "$h\r\n", $extra)) . "\r\n");
    $raw = (string) stream_get_contents($fp);
    fclose($fp);
    return parse($raw);
}

function parse(string $raw): array
{
    [$head, $body] = array_pad(explode("\r\n\r\n", $raw, 2), 2, '');
    $lines = explode("\r\n", $head);
    $status = preg_match('#^HTTP/1\.[01] (\d{3})#', $lines[0], $m) ? (int) $m[1] : 0;
    $headers = [];
    foreach (array_slice($lines, 1) as $line) {
        if (str_contains($line, ':')) {
            [$k, $v] = explode(':', $line, 2);
            $headers[strtolower($k)][] = trim($v);
        }
    }
    if (($headers['transfer-encoding'][0] ?? '') === 'chunked') {
        $out = '';
        while ($body !== '' && ($n = strpos($body, "\r\n")) !== false) {
            $size = hexdec(substr($body, 0, $n));
            if ($size === 0) {
                break;
            }
            $out .= substr($body, $n + 2, $size);
            $body = substr($body, $n + 2 + $size + 2);
        }
        $body = $out;
    }
    return ['status' => $status, 'headers' => $headers, 'body' => $body];
}

const HTML = 'text/html; charset=UTF-8';
const HSTS = 'max-age=31536000; includeSubDomains';

/* The two configured headers are on the reply once each, with the configured value. */
function expectHeaders(string $what, array $r, int $status): void
{
    if ($r['status'] !== $status) {
        fail("$what: status {$r['status']}, want $status", json_encode($r));
    }
    if (($r['headers']['strict-transport-security'] ?? null) !== [HSTS]) {
        fail("$what: Strict-Transport-Security is not the configured value, once", json_encode($r['headers']));
    }
    if (($r['headers']['x-frame-options'] ?? null) !== ['DENY']) {
        fail("$what: X-Frame-Options is not the configured value, once", json_encode($r['headers']));
    }
}

function expectBody(string $what, array $r, string $body, ?string $type): void
{
    if ($r['body'] !== $body) {
        fail("$what: body " . var_export($r['body'], true) . ', want ' . var_export($body, true));
    }
    if ($type !== null && ($r['headers']['content-type'][0] ?? '') !== $type) {
        fail("$what: Content-Type " . var_export($r['headers']['content-type'] ?? null, true) . ", want $type");
    }
    if ((int) ($r['headers']['content-length'][0] ?? -1) !== strlen($body) && !isset($r['headers']['transfer-encoding'])) {
        fail("$what: Content-Length " . var_export($r['headers']['content-length'] ?? null, true));
    }
}

/* "METHOD URI STATUS BYTES" of the access log lines so far, polled until every
 * wanted tuple is there or 10 s pass. */
function accessFound(string $file, array $wants): array
{
    $deadline = microtime(true) + 10;
    do {
        $found = [];
        foreach (explode("\n", (string) @file_get_contents($file)) as $l) {
            if (preg_match('/"(\S+) (\S+) [^"]*" (\d{3}) (\d+) /', $l, $m)) {
                $found[] = "$m[1] $m[2] $m[3] $m[4]";
            }
        }
        if (!array_diff($wants, $found)) {
            break;
        }
        usleep(100000);
    } while (microtime(true) < $deadline);
    return $found;
}

$tester = new FPM\Tester($cfg, '<?php');
try {
    $tester->start();
    $tester->expectLogStartNotices();
    $http = $tester->getAddr('ipv4', '[http]');
    $acc = $tester->getPrefixedFile(FPM\Tester::FILE_EXT_LOG_ACC);
    $seen = [];

    /* FastCGI 200: the plain case. */
    $r = req($http, 'GET', '/api/x?m=ok');
    expectHeaders('fastcgi 200', $r, 200);
    expectBody('fastcgi 200', $r, 'ok', null);
    echo "fastcgi 200: headers once\n";

    /* The application's own header of the same name is dropped, all copies. */
    $r = req($http, 'GET', '/api/x?m=hsts');
    expectHeaders('app HSTS', $r, 200);
    expectBody('app HSTS', $r, 'app body', null);
    echo "app header replaced: configured value only\n";

    /* Static 200 and the 304 from its validator. */
    $r = req($http, 'GET', '/app.css');
    expectHeaders('static 200', $r, 200);
    expectBody('static 200', $r, 'body{color:red}', null);
    $etag = $r['headers']['etag'][0] ?? null;
    if ($etag === null) {
        fail('static 200: no ETag to revalidate with');
    }
    $r = req($http, 'GET', '/app.css', ["If-None-Match: $etag"]);
    expectHeaders('static 304', $r, 304);
    echo "static 200 and 304: headers once\n";

    /* Static refusal: the 404 page, kept connection, headers. */
    $r = req($http, 'GET', '/.env.css');
    expectHeaders('static refusal', $r, 404);
    expectBody('static refusal', $r, '<h1>nothing here</h1>', HTML);
    echo "static refusal 404: page and headers\n";

    /* X-Accel: the served file and the refusal. */
    $r = req($http, 'GET', '/api/x?m=accel&r=' . urlencode('/file.txt'));
    expectHeaders('accel 200', $r, 200);
    expectBody('accel 200', $r, 'protected payload', null);
    $r = req($http, 'GET', '/api/x?m=accel&r=' . urlencode('/missing.txt'));
    expectHeaders('accel 404', $r, 404);
    expectBody('accel 404', $r, '<h1>nothing here</h1>', HTML);
    echo "x-accel 200 and 404: headers once, page\n";

    /* Route miss: a 404 the gateway writes itself, and HEAD of it. */
    $r = req($http, 'GET', '/other');
    expectHeaders('route miss', $r, 404);
    expectBody('route miss', $r, '<h1>nothing here</h1>', HTML);
    $r = req($http, 'HEAD', '/other');
    expectHeaders('route miss HEAD', $r, 404);
    if ($r['body'] !== '' || ($r['headers']['content-type'][0] ?? '') !== HTML) {
        fail('route miss HEAD: want no body and the page type', json_encode($r));
    }
    echo "route miss 404: page, HEAD has no body\n";

    /* An application's own 404 passes through; the headers still apply. */
    $r = req($http, 'GET', '/api/x?m=app404');
    expectHeaders('app 404', $r, 404);
    expectBody('app 404', $r, 'app not found', null);
    echo "app 404: body untouched, headers once\n";

    /* Invalid upstream Status: 502, which the gateway writes mid-reply. */
    $r = req($http, 'GET', '/api/x?m=badstatus');
    expectHeaders('invalid Status 502', $r, 502);
    expectBody('invalid Status 502', $r, '{"error":"bad gateway"}', 'application/json');
    echo "invalid Status 502: page, headers once\n";

    /* Target gone: the connect fails. */
    @unlink($fcSock);
    $r = req($http, 'GET', '/dead/x');
    expectHeaders('dead target 502', $r, 502);
    expectBody('dead target 502', $r, '{"error":"bad gateway"}', 'application/json');
    echo "dead target 502: page, headers once\n";

    /* ping and ready are answered by the gateway, which sets its own
     * Cache-Control and Expires: the configured names are refused at -t, so
     * each header is on these replies once. */
    foreach (['/ping', '/ready'] as $probe) {
        $r = req($http, 'GET', $probe);
        expectHeaders("$probe 200", $r, 200);
        if (count($r['headers']['cache-control'] ?? []) !== 1 || count($r['headers']['expires'] ?? []) !== 1) {
            fail("$probe: Cache-Control or Expires not once", json_encode($r['headers']));
        }
    }
    echo "ping and ready: headers once\n";

    /* Pool full: pin the single worker until the test releases it. */
    $hold = "$base/hold-slow";
    $slow = stream_socket_client("tcp://$http", $errno, $error, 5);
    fwrite($slow, "GET /slow/x?m=hold&f=" . urlencode($hold) . " HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n");
    for ($i = 0; $i < 100 && !file_exists($hold); $i++) {
        usleep(100000);
    }
    if (!file_exists($hold)) {
        fail('the slow worker never started');
    }
    $r = req($http, 'GET', '/slow/y');
    expectHeaders('pool-full 503', $r, 503);
    expectBody('pool-full 503', $r, '<h1>busy</h1>', HTML);
    if (($r['headers']['retry-after'] ?? null) !== ['1']) {
        fail('pool-full 503: Retry-After is not the gateway value, once', json_encode($r['headers']));
    }
    $r = req($http, 'HEAD', '/slow/y');
    if ($r['status'] !== 503 || $r['body'] !== '' || ($r['headers']['content-type'][0] ?? '') !== HTML) {
        fail('pool-full 503 HEAD', json_encode($r));
    }
    echo "pool-full 503: page, headers once, Retry-After once\n";
    touch("$hold.release");
    stream_get_contents($slow);
    fclose($slow);

    /* 504: the worker holds past http.upstream_read_timeout. 504 is not a page
     * key, so the body is the gateway's own. */
    $hang = "$base/hold-hang";
    $r = req($http, 'GET', '/hang/x?m=hold&f=' . urlencode($hang));
    expectHeaders('504', $r, 504);
    expectBody('504', $r, "Gateway Timeout\n", 'text/plain');
    echo "504: headers once, no page\n";
    touch("$hang.release");

    /* The access log counts the bytes of each page, and 0 for HEAD. Wait for
     * exactly these lines; the 15 other requests are not asserted. */
    $wants = ['GET /.env.css 404 21', 'GET /other 404 21', 'HEAD /other 404 0', 'GET /dead/x 502 23',
        'GET /slow/y 503 13', 'HEAD /slow/y 503 0'];
    $found = accessFound($acc, $wants);
    foreach ($wants as $want) {
        if (!in_array($want, $found, true)) {
            fail("access log has no line '$want'", implode("\n", $found));
        }
    }
    echo "access log: page bytes, HEAD 0\n";
    echo "Done\n";
} finally {
    $tester->terminate();
    $tester->close();
    foreach (glob("$base/pages/*") as $f) @unlink($f);
    foreach (glob("$base/prot/*") as $f) @unlink($f);
    foreach (glob("$base/{,.}*.{css,php}", GLOB_BRACE) as $f) @unlink($f);
    @rmdir("$base/pages");
    @rmdir("$base/prot");
    @rmdir($base);
    @unlink($fcSock);
}
?>
--EXPECT--
fastcgi 200: headers once
app header replaced: configured value only
static 200 and 304: headers once
static refusal 404: page and headers
x-accel 200 and 404: headers once, page
route miss 404: page, HEAD has no body
app 404: body untouched, headers once
invalid Status 502: page, headers once
dead target 502: page, headers once
ping and ready: headers once
pool-full 503: page, headers once, Retry-After once
504: headers once, no page
access log: page bytes, HEAD 0
Done
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>

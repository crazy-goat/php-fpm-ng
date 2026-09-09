--TEST--
fpm-ng: the classic and worker HTTP-direct executors derive the same request (issue #74)
--SKIPIF--
<?php include "skipif.inc"; ?>
--FILE--
<?php
require_once "tester.inc";

/* Both executors build their CGI environment, refuse a malformed request and
 * drop the framing headers through one implementation
 * (sapi/fpmng/fpm/fpm_http_direct_request.c). This test is what notices if a
 * later change gives one of them a private copy again: it sends the SAME
 * request to both pools and compares what came back. */

function check(bool $condition, string $message): void
{
    if (!$condition) throw new RuntimeException($message);
}

function request(int $port, string $raw): array
{
    $fp = stream_socket_client("tcp://127.0.0.1:$port", $errno, $error, 5);
    check($fp !== false, "connect to $port failed: $error");
    stream_set_timeout($fp, 5);
    fwrite($fp, $raw);
    $status = (string) fgets($fp);
    $headers = [];
    while (($line = fgets($fp)) !== false && $line !== "\r\n") {
        [$key, $value] = explode(':', trim($line), 2);
        $headers[strtolower($key)][] = trim($value);
    }
    $body = '';
    $length = (int) ($headers['content-length'][0] ?? 0);
    while (strlen($body) < $length) {
        $part = fread($fp, $length - strlen($body));
        check($part !== false && $part !== '', 'short body');
        $body .= $part;
    }
    fclose($fp);
    return [$status, $headers, $body];
}

$root = sys_get_temp_dir() . '/fpmng-direct-parity-' . getmypid();
@mkdir($root, 0700, true);

/* The locale is set by the application, on purpose: issue #105. Whatever
 * LC_CTYPE the front controller (or, under the worker executor, the boot
 * script) chose must not change which HTTP_* key a header lands under. */
file_put_contents("$root/front.php", <<<'PHP'
<?php
$locale = setlocale(LC_ALL, 'tr_TR.UTF-8', 'tr_TR.utf8', 'tr_TR', 'az_AZ.UTF-8');
if (($_SERVER['PATH_INFO'] ?? '') === '/drop') {
    header('Content-Length: 999999');
    header('Connection: upgrade');
    header('X-Keep: yes');
    echo 'body';
    return;
}
$_SERVER['X_TEST_LOCALE'] = (string) $locale;
echo json_encode($_SERVER);
PHP);

/* No fibers and no driver: this test is about what the request looks like,
 * not about concurrency, so the handler answers inside the notify callback. */
file_put_contents("$root/worker.php", <<<'PHP'
<?php
$locale = (string) setlocale(LC_ALL, 'tr_TR.UTF-8', 'tr_TR.utf8', 'tr_TR', 'az_AZ.UTF-8');
$notify = fpmng_worker_notify_stream();
$watcher = fpmng_worker_event_create(FPMNG_WORKER_READ, $notify, function () use ($notify, $locale): void {
    fread($notify, 65536);
    while (($id = fpmng_worker_next_request()) !== null) {
        $env = fpmng_worker_request_env($id);
        if (($env['PATH_INFO'] ?? '') === '/drop') {
            fpmng_worker_respond($id, 200, [
                'Content-Length' => '999999',
                'Connection' => 'upgrade',
                'X-Keep' => 'yes',
            ], 'body');
            continue;
        }
        $env['X_TEST_LOCALE'] = $locale;
        fpmng_worker_respond($id, 200, ['Content-Type' => 'application/json'], json_encode($env));
    }
});
fpmng_worker_event_enable($watcher);
while (!fpmng_worker_may_exit()) {
    fpmng_worker_loop(true);
}
PHP);

$classicPort = (int) (getenv('FPMNG_DIRECT_PARITY_CLASSIC_PORT') ?: 28074);
$workerPort = (int) (getenv('FPMNG_DIRECT_PARITY_WORKER_PORT') ?: 28075);
$cfg = <<<CFG
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
[classic]
listen = 127.0.0.1:$classicPort
pool.type = http-direct
pool.executor = classic
pm = static
pm.max_children = 1
chdir = $root
http.front_controller = /front.php
http.read_timeout = 10000
http.max_body = 1M
php_admin_value[display_errors] = 0
[worker]
listen = 127.0.0.1:$workerPort
pool.type = http-direct
pool.executor = worker
pm = static
pm.max_children = 1
chdir = $root
http.front_controller = /worker.php
http.read_timeout = 10000
http.max_body = 1M
php_admin_value[max_execution_time] = 0
php_admin_value[display_errors] = 0
CFG;

$tester = new FPM\Tester($cfg, '<?php');
try {
    $tester->start();
    $tester->expectLogStartNotices();

    /* One request, byte for byte, to both pools. */
    $get = "GET /probe?q=1&r=2 HTTP/1.1\r\nHost: parity.test\r\n"
        . "X-Probe: custom\r\nProxy: attacker\r\nIf-Modified-Since: yesterday\r\n"
        . "Connection: close\r\n\r\n";
    $classic = json_decode(request($classicPort, $get)[2], true, flags: JSON_THROW_ON_ERROR);
    $worker = json_decode(request($workerPort, $get)[2], true, flags: JSON_THROW_ON_ERROR);

    /* Everything the transport derives from the request or from the pool,
     * minus what must differ: the two pools run different scripts on
     * different ports, and each names itself in SERVER_SOFTWARE. */
    $shared = [
        'REQUEST_METHOD', 'REQUEST_URI', 'QUERY_STRING', 'PATH_INFO',
        'DOCUMENT_ROOT', 'SERVER_PROTOCOL', 'GATEWAY_INTERFACE', 'SERVER_ADDR', 'SERVER_NAME',
        'CONTENT_LENGTH', 'CONTENT_TYPE', 'REMOTE_ADDR', 'HTTP_HOST', 'HTTP_X_PROBE',
        'HTTP_IF_MODIFIED_SINCE',
    ];
    foreach ($shared as $key) {
        check(array_key_exists($key, $classic), "classic is missing $key");
        check(array_key_exists($key, $worker), "worker is missing $key");
        check($classic[$key] === $worker[$key],
            "$key differs: " . var_export($classic[$key], true) . ' vs ' . var_export($worker[$key], true));
    }
    check($classic['REQUEST_URI'] === '/probe?q=1&r=2' && $classic['PATH_INFO'] === '/probe', 'URI parsing');
    /* Each pool names its own front controller, and names it the same way
     * twice — PHP_SELF used to exist on one executor only. */
    check($classic['SCRIPT_NAME'] === '/front.php' && $classic['PHP_SELF'] === '/front.php',
        'classic SCRIPT_NAME/PHP_SELF: ' . json_encode([$classic['SCRIPT_NAME'], $classic['PHP_SELF']]));
    check($worker['SCRIPT_NAME'] === '/worker.php' && $worker['PHP_SELF'] === '/worker.php',
        'worker SCRIPT_NAME/PHP_SELF: ' . json_encode([$worker['SCRIPT_NAME'], $worker['PHP_SELF']]));
    check(!isset($classic['HTTP_PROXY']) && !isset($worker['HTTP_PROXY']), 'httpoxy: Proxy was imported');
    check($worker['SERVER_SOFTWARE'] === 'php-fpm-ng/http-direct-worker'
        && $classic['SERVER_SOFTWARE'] === 'php-fpm-ng/http-direct', 'SERVER_SOFTWARE');
    echo "env-parity: ok\n";

    /* Issue #105: the HTTP_* key is derived with an explicit ASCII range, not
     * toupper(). Reproduced on 192.168.8.50 (glibc 2.43, php-8.5.9,
     * 2026-09-09) with the pre-fix binary on the WORKER leg below, whose boot
     * script sets the locale once: "If-Modified-Since" arrived as
     * HTTP_IF_MODiFiED_SiNCE, because the Turkish capital of 'i' is U+0130 and
     * does not fit the single-byte toupper() table.
     *
     * Both legs are checked, but only the worker leg can catch a regression:
     * on the classic executor ext/standard puts LC_ALL back to "C" at request
     * shutdown (basic_functions.c:448) and the next request's environment is
     * built before its script runs, so a corrupt key never becomes
     * observable there.
     *
     * And it can only catch it where a Turkish locale exists. The fpmng-phpt
     * CI job generates tr_TR.UTF-8 for exactly this test
     * (.github/workflows/build-matrix.yml); a machine without it -- including
     * 192.168.8.50, which carries C, C.utf8 and POSIX only -- runs the checks
     * against a locale where the pre-fix mapping was already correct, so they
     * pass either way. That is why the locale actually obtained is printed:
     * a green run that exercised nothing does not read the same as a real
     * one. */
    foreach (['classic' => $classic, 'worker' => $worker] as $name => $env) {
        check(($env['HTTP_IF_MODIFIED_SINCE'] ?? null) === 'yesterday',
            "$name lost HTTP_IF_MODIFIED_SINCE under locale "
                . var_export($env['X_TEST_LOCALE'], true) . ': '
                . json_encode(array_keys(array_filter($env,
                    fn ($k) => str_starts_with($k, 'HTTP_'), ARRAY_FILTER_USE_KEY))));
        foreach (array_keys($env) as $key) {
            check(!preg_match('/^HTTP_.*[a-z]/', $key),
                "$name derived a non-uppercase CGI key under locale "
                    . var_export($env['X_TEST_LOCALE'], true) . ": $key");
        }
    }
    echo 'locale-independent-cgi-keys: ok (worker locale: '
        . ($worker['X_TEST_LOCALE'] !== '' ? $worker['X_TEST_LOCALE'] : 'unavailable, check degenerate')
        . ")\n";

    /* The transport owns framing in both: an application-supplied
     * Content-Length and Connection never reach the wire, and a header that
     * is not about framing does. */
    $drop = "GET /drop HTTP/1.1\r\nHost: parity.test\r\nConnection: close\r\n\r\n";
    foreach (['classic' => $classicPort, 'worker' => $workerPort] as $name => $port) {
        [$status, $headers, $body] = request($port, $drop);
        check(str_contains($status, ' 200 '), "$name drop status: $status");
        check($body === 'body', "$name drop body: $body");
        check(($headers['content-length'] ?? []) === ['4'], "$name kept the application Content-Length: "
            . json_encode($headers['content-length'] ?? []));
        /* The client asked for a close, so libevent frames one; what must
         * not appear is the value the application set. */
        check(!in_array('upgrade', $headers['connection'] ?? [], true),
            "$name kept the application Connection header: " . json_encode($headers['connection'] ?? []));
        check(($headers['x-keep'] ?? []) === ['yes'], "$name dropped a header it should have sent");
    }
    echo "framing-parity: ok\n";

    /* A header name too long to become an HTTP_* key is refused by both, and
     * refused as a bad request rather than served with the header missing —
     * FPM_HTTP_HEADER_NAME_MAX is 1024. */
    $long = "GET / HTTP/1.1\r\nHost: parity.test\r\nX-" . str_repeat('a', 1100)
        . ": v\r\nConnection: close\r\n\r\n";
    foreach (['classic' => $classicPort, 'worker' => $workerPort] as $name => $port) {
        $status = request($port, $long)[0];
        check(str_contains($status, ' 400 '), "$name accepted an over-long header name: $status");
    }
    echo "header-name-limit: ok\n";
} finally {
    $tester->terminate();
    $tester->close();
    @unlink("$root/front.php");
    @unlink("$root/worker.php");
    @rmdir($root);
}
echo "Done\n";
?>
--EXPECTF--
env-parity: ok
locale-independent-cgi-keys: ok (worker locale: %s)
framing-parity: ok
header-name-limit: ok
Done
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>

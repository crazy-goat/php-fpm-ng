--TEST--
fpm-ng: HTTP-direct terminates TLS on both executors, reloads the certificate, and never falls back to plain (issue #55)
--SKIPIF--
<?php
include "skipif.inc";
if (!function_exists('openssl_x509_parse')) {
    die('skip requires the openssl extension');
}
if (trim((string) shell_exec('command -v openssl 2>/dev/null')) === '') {
    die('skip requires the openssl CLI to generate a test certificate');
}
/* Same probe shape as fpmng-http-tls-chain.phpt, but against pool.type =
 * http-direct: a build without libevent_openssl/OpenSSL refuses http.tls_cert
 * here with the same "built with TLS support" wording, which is the thing to
 * skip on -- not the absence of the ext/openssl the two lines above test. */
$probe = new FPM\Tester(<<<'EOT'
[global]
error_log = {{FILE:LOG}}
[unconfined]
listen = {{ADDR}}
pm = static
pm.max_children = 1
pool.type = http-direct
chdir = /tmp
http.front_controller = /nonexistent-front-controller.php
http.tls_cert = /nonexistent-cert.pem
http.tls_key = /nonexistent-key.pem
EOT, '<?php');
$messages = $probe->testConfig(true, null, false, false);
FPM\Tester::clean();
foreach ((array) $messages as $message) {
    if (str_contains($message, 'built with TLS support')) {
        die('skip php-fpm-ng built without TLS support (libevent_openssl and/or OpenSSL not found at build time)');
    }
}
?>
--FILE--
<?php
require_once "tester.inc";
require_once "fpmng-tester.inc";

function check(bool $condition, string $message): void
{
    if (!$condition) throw new RuntimeException($message);
}

function run(string $cmd): void
{
    exec($cmd . ' 2>&1', $output, $code);
    if ($code !== 0) {
        throw new RuntimeException("COMMAND FAILED: $cmd\n" . implode("\n", $output));
    }
}

$root = sys_get_temp_dir() . '/fpmng-direct-tls-' . getmypid();
@mkdir($root, 0700, true);

/* Two self-signed certificates, distinguishable by CN alone: the reload case
 * below asserts on which one a connection was served, and a CN is what a
 * client can read without trusting anything. */
foreach (['first', 'second'] as $cn) {
    run("openssl req -x509 -newkey rsa:2048 -nodes -days 2 -sha256 "
        . "-subj /CN=$cn.test -keyout $root/$cn.key -out $root/$cn.crt");
}
copy("$root/first.crt", "$root/serving.crt");
copy("$root/first.key", "$root/serving.key");

/* The classic executor's front controller and the worker executor's boot
 * script both answer with what the transport put in the environment: the two
 * CGI variables an application uses to know it is behind TLS. */
file_put_contents("$root/index.php", <<<'PHP'
<?php
echo 'scheme=', $_SERVER['REQUEST_SCHEME'] ?? '<unset>',
     ' https=', $_SERVER['HTTPS'] ?? '<unset>',
     ' port=', $_SERVER['SERVER_PORT'] ?? '<unset>';
PHP);
file_put_contents("$root/worker.php", <<<'PHP'
<?php
$notify = fpmng_worker_notify_stream();
$watcher = fpmng_worker_event_create(FPMNG_WORKER_READ, $notify, function () use ($notify): void {
    fread($notify, 65536);
    while (($id = fpmng_worker_next_request()) !== null) {
        $env = fpmng_worker_request_env($id);
        fpmng_worker_respond($id, 200, ['Content-Type' => 'text/plain'],
            'scheme=' . ($env['REQUEST_SCHEME'] ?? '<unset>')
            . ' https=' . ($env['HTTPS'] ?? '<unset>')
            . ' port=' . ($env['SERVER_PORT'] ?? '<unset>'));
    }
});
fpmng_worker_event_enable($watcher);
while (!fpmng_worker_may_exit()) {
    fpmng_worker_loop(true);
}
PHP);

$portA = (int) (getenv('FPMNG_DIRECT_TLS_PORT') ?: 28086);
$portB = $portA + 1;

/* verify_peer off: these are self-signed and the point is what the server
 * sends, not whether a CA chain validates. capture_peer_cert is what lets the
 * reload assertion below name the certificate actually served. */
function tlsContext(): mixed
{
    return stream_context_create(['ssl' => [
        'verify_peer' => false,
        'verify_peer_name' => false,
        'capture_peer_cert' => true,
        'SNI_enabled' => false,
    ]]);
}

function tlsConnect(int $port, float $timeout = 5.0)
{
    $client = @stream_socket_client("ssl://127.0.0.1:$port", $errno, $errstr, $timeout,
        STREAM_CLIENT_CONNECT, tlsContext());
    return $client ?: null;
}

/* One request on an already-connected stream, so the caller can decide
 * whether to reuse the connection -- which is the whole point of the
 * in-flight half of the reload assertion. */
function tlsRequest($client, int $port, string $path = '/'): string
{
    fwrite($client, "GET $path HTTP/1.1\r\nHost: 127.0.0.1:$port\r\nConnection: keep-alive\r\n\r\n");
    $response = '';
    $deadline = microtime(true) + 5.0;
    while (microtime(true) < $deadline) {
        $chunk = fread($client, 8192);
        if ($chunk === false || $chunk === '') {
            if (feof($client)) break;
            usleep(20000);
            continue;
        }
        $response .= $chunk;
        /* Bodies here are short and Content-Length'd; stop as soon as the
         * announced length has arrived rather than waiting for the timeout. */
        if (preg_match('/\r\nContent-Length: (\d+)\r\n/i', $response, $m)) {
            $split = strpos($response, "\r\n\r\n");
            if ($split !== false && strlen($response) - $split - 4 >= (int) $m[1]) {
                break;
            }
        }
    }
    return $response;
}

function servedCommonName($client): string
{
    $params = stream_context_get_params($client);
    if (!isset($params['options']['ssl']['peer_certificate'])) {
        return '<none>';
    }
    $parsed = openssl_x509_parse($params['options']['ssl']['peer_certificate']);
    return $parsed['subject']['CN'] ?? '<none>';
}

/* Retries: the master respawns children, and the first connection after a
 * start or a respawn can arrive before the child is listening again. */
function tlsGetBody(int $port, int $attempts = 50): string
{
    for ($i = 0; $i < $attempts; $i++) {
        $client = tlsConnect($port);
        if ($client) {
            $response = tlsRequest($client, $port);
            fclose($client);
            $split = strpos($response, "\r\n\r\n");
            if ($split !== false) {
                return substr($response, $split + 4);
            }
        }
        usleep(100000);
    }
    return '';
}

$config = <<<CFG
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
[direct]
listen = 127.0.0.1:$portA
pool.type = http-direct
pm = static
pm.max_children = 2
chdir = $root
http.front_controller = /index.php
http.read_timeout = 10000
http.max_body = 1M
http.tls_cert = $root/serving.crt
http.tls_key = $root/serving.key
http.tls_min_version = TLSv1.2
http.tls_reload_check = 1
php_admin_value[display_errors] = 0
[work]
listen = 127.0.0.1:$portB
pool.type = http-direct
pool.executor = worker
pm = static
pm.max_children = 1
chdir = $root
http.front_controller = /worker.php
http.read_timeout = 10000
http.max_body = 1M
http.tls_cert = $root/serving.crt
http.tls_key = $root/serving.key
php_admin_value[max_execution_time] = 0
php_admin_value[display_errors] = 0
CFG;

$tester = new FPM\Tester($config, '<?php');
try {
    $tester->start();
    fpmng_expect_log_start_notices($tester);

    /* 1. The classic executor: a real handshake, a real request, and the CGI
     * variables that tell the application it is behind TLS. */
    $body = tlsGetBody($portA);
    check($body === "scheme=https https=on port=$portA", 'classic over TLS: ' . var_export($body, true));
    echo "classic-serves-https: ok\n";

    /* 2. The worker executor, which builds the same environment from the same
     * code but owns its own loop -- issue #74's parity, now including TLS. */
    $body = tlsGetBody($portB);
    check($body === "scheme=https https=on port=$portB", 'worker over TLS: ' . var_export($body, true));
    echo "worker-serves-https: ok\n";

    /* 3. Plain HTTP on a TLS port is refused, not served. This is the failure
     * mode that must never be quiet: a pool configured as HTTPS answering
     * cleartext would make every $_SERVER['HTTPS'] check above a lie. */
    $plain = @file_get_contents("http://127.0.0.1:$portA/");
    check($plain === false || $plain === '', 'plain HTTP was served on a TLS port: ' . var_export($plain, true));
    echo "plain-http-is-refused: ok\n";

    /* 4. Reload. Swap the files the pool points at, keeping one connection
     * open across the swap: the open one must keep working on the certificate
     * it handshook with (in-flight requests finish on the old one), and a new
     * connection must get the new certificate without anything restarting. */
    $before = tlsConnect($portA);
    check($before !== null, 'could not open the pre-reload connection');
    check(servedCommonName($before) === 'first.test', 'pre-reload CN: ' . servedCommonName($before));
    $masterPid = $tester->getPid();

    /* Written through a temporary file: the master digests both files on its
     * tick (issue #71 replaced an mtime check with a content digest), and a
     * partially written PEM would be a candidate it rejects, not a reload. */
    foreach (['crt', 'key'] as $ext) {
        copy("$root/second.$ext", "$root/serving.$ext.tmp");
        rename("$root/serving.$ext.tmp", "$root/serving.$ext");
    }

    $after = '<none>';
    /* http.tls_reload_check = 1, plus the child's own adoption tick: 20 s is
     * far past both and still bounded, so a broken reload fails the test
     * instead of hanging the suite. */
    for ($i = 0; $i < 200; $i++) {
        $client = tlsConnect($portA);
        if ($client) {
            $after = servedCommonName($client);
            fclose($client);
            if ($after === 'second.test') break;
        }
        usleep(100000);
    }
    check($after === 'second.test', 'the certificate was not reloaded, still serving: ' . $after);
    echo "new-connections-get-the-new-certificate: ok\n";

    $response = tlsRequest($before, $portA);
    check(str_contains($response, 'scheme=https https=on'),
        'the connection open across the reload stopped working: ' . var_export($response, true));
    fclose($before);
    echo "in-flight-connection-survives-the-reload: ok\n";

    check($tester->getPid() === $masterPid,
        'the master restarted instead of reloading in place');
    echo "no-restart: ok\n";

    $tester->expectNoLogPattern('/ERROR:/', true);
} finally {
    $tester->terminate();
    $tester->close();
    array_map('unlink', glob("$root/*") ?: []);
    @rmdir($root);
}
echo "Done\n";
?>
--EXPECT--
classic-serves-https: ok
worker-serves-https: ok
plain-http-is-refused: ok
new-connections-get-the-new-certificate: ok
in-flight-connection-survives-the-reload: ok
no-restart: ok
Done
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>

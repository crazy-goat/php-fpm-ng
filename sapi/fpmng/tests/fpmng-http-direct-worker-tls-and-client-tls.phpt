--TEST--
fpm-ng: a worker pool serving TLS also opens its own outbound TLS client connection without one corrupting the other's OpenSSL state (issue #336)
--SKIPIF--
<?php
include "skipif.inc";
if (!function_exists('openssl_x509_parse')) {
    die('skip requires the openssl extension');
}
if (trim((string) shell_exec('command -v openssl 2>/dev/null')) === '') {
    die('skip requires the openssl CLI to generate a test certificate');
}
/* Same probe shape as fpmng-http-direct-tls.phpt: a build without
 * libevent_openssl/OpenSSL refuses http.tls_cert with "built with TLS
 * support", which is the thing to skip on. */
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
        die('skip php-fpm-ng built without TLS support (configure without --enable-fpmng-tls, issue #280)');
    }
}
if (trim((string) shell_exec('command -v openssl 2>/dev/null')) === '') {
    die('skip requires the openssl CLI');
}
exec(PHP_BINARY . ' -n -r ' . escapeshellarg('exit(extension_loaded("openssl") ? 0 : 1);'), $o, $st);
if ($st !== 0) {
    die('skip the -n CLI has no openssl (shared ext loaded via ini here)');
}
?>
--FILE--
<?php
require_once "tester.inc";

function check(bool $condition, string $message): void
{
    if (!$condition) throw new RuntimeException($message);
}

/* OpenSSL's error queue (ERR_get_error() et al.) is process-global, so this is
 * the one combination among issue #336's gaps with a plausible cross-talk: a
 * server-TLS worker pool (fpmng-http-direct-tls.phpt's [work] pool already
 * proves this in isolation) that, from within the SAME request handler, also
 * opens an outbound TLS client connection via the buffered-stream watcher
 * mechanism (fpmng-http-direct-worker-buffered-streams.phpt's pattern, in
 * isolation too). Neither existing test combines both TLS roles in the same
 * process. This test is deliberately simpler than an ideal fuzzed-interleaving
 * harness: it just repeats "answer a request over the server's own TLS
 * listener, then make a client-side TLS connection out to a small local TLS
 * origin and read a reply from it" a few times in the same worker process and
 * asserts every iteration succeeds cleanly, with no error leaking from one
 * role into the other. */
$root = sys_get_temp_dir() . '/fpmng-worker-tls-and-client-tls-' . getmypid();
@mkdir($root, 0700, true);

function run(string $cmd): void
{
    exec($cmd . ' 2>&1', $output, $code);
    if ($code !== 0) {
        throw new RuntimeException("COMMAND FAILED: $cmd\n" . implode("\n", $output));
    }
}

$descriptors = [0 => ['pipe', 'r'], 1 => ['pipe', 'w'], 2 => ['pipe', 'w']];
$origin = null;
$originPipes = [];
$tester = null;

try {
    /* The pool's own server certificate. */
    $keyFile = "$root/serving.key";
    $certFile = "$root/serving.crt";
    run('openssl req -x509 -newkey rsa:2048 -nodes '
        . '-keyout ' . escapeshellarg($keyFile) . ' -out ' . escapeshellarg($certFile)
        . ' -days 1 -subj "/CN=127.0.0.1"');

    /* A tiny outbound TLS origin the worker connects to as a CLIENT, mirroring
     * fpmng-http-direct-worker-buffered-streams.phpt: a self-signed cert of
     * its own (deliberately a DIFFERENT keypair from the pool's serving cert,
     * so any accidental reuse of state between the two roles would be
     * detectable), bound on port 0, echoing back whatever it is sent. */
    $originCert = "$root/origin.crt";
    $originKey = "$root/origin.key";
    run('openssl req -x509 -newkey rsa:2048 -nodes '
        . '-keyout ' . escapeshellarg($originKey) . ' -out ' . escapeshellarg($originCert)
        . ' -days 1 -subj "/CN=origin.test"');

    file_put_contents("$root/origin.php", <<<'PHP'
<?php
[$cert, $key] = [$argv[1], $argv[2]];
$ctx = stream_context_create(['ssl' => ['local_cert' => $cert, 'local_pk' => $key]]);
$server = @stream_socket_server('tls://127.0.0.1:0', $errno, $errstr,
    STREAM_SERVER_BIND | STREAM_SERVER_LISTEN, $ctx);
if (!$server) {
    fwrite(STDERR, "origin: bind failed: $errstr\n");
    exit(1);
}
echo stream_socket_get_name($server, false), "\n";
$stdin = fopen('php://stdin', 'r');
$open = [];
while (true) {
    $conn = @stream_socket_accept($server, 1);
    if ($conn === false) {
        $readable = [$stdin];
        $writable = $except = [];
        if (@stream_select($readable, $writable, $except, 0) > 0 && fgets($stdin) === false) {
            exit(0);
        }
        continue;
    }
    $line = fgets($conn);
    fwrite($conn, 'echo:' . $line);
    $open[] = $conn;
}
PHP);

    /* Array form, not a shell string: see fpmng-http-direct-worker-buffered-streams.phpt
     * (issue #89) for why a shell-string command leaks the origin process on
     * boxes where /bin/sh is dash. */
    $origin = proc_open([PHP_BINARY, '-n', "$root/origin.php", $originCert, $originKey],
        $descriptors, $originPipes);
    check(is_resource($origin), 'could not start the TLS origin');
    $addr = trim((string) fgets($originPipes[1]));
    if (!preg_match('/^127\.0\.0\.1:\d+$/', $addr)) {
        stream_set_blocking($originPipes[2], false);
        throw new RuntimeException('origin did not report an address: ' . var_export($addr, true)
            . ' / ' . stream_get_contents($originPipes[2]));
    }
    file_put_contents("$root/origin.addr", $addr);

    file_put_contents("$root/worker.php", <<<'PHP'
<?php
$notify = fpmng_worker_notify_stream();

function callOrigin(string $message): ?string
{
    $addr = trim((string) @file_get_contents(__DIR__ . '/origin.addr'));
    $ctx = stream_context_create(['ssl' => ['verify_peer' => false, 'verify_peer_name' => false]]);
    $stream = @stream_socket_client("tls://$addr", $errno, $errstr, 5,
        STREAM_CLIENT_CONNECT, $ctx);
    if (!$stream) {
        return null;
    }
    stream_set_blocking($stream, true);
    fwrite($stream, "$message\n");
    $reply = fgets($stream);
    fclose($stream);
    return $reply === false ? null : rtrim($reply, "\n");
}

function handle(int $id): void
{
    $env = fpmng_worker_request_env($id);
    $uri = $env['REQUEST_URI'] ?? '/';

    if (str_starts_with($uri, '/roundtrip')) {
        /* Server-side TLS for THIS request is entirely handled by evhttp
         * before the front controller ever runs -- the worker script only
         * ever sees plaintext HTTP internally, same as the classic executor's
         * TLS test. What this adds is the outbound client-TLS call happening
         * from inside the SAME handler invocation, on the SAME process, that
         * just answered (or is about to answer) over the server's TLS
         * listener. */
        $reply = callOrigin('hello-' . $id);
        if ($reply !== 'echo:hello-' . $id) {
            fpmng_worker_respond($id, 500, [], json_encode(['error' => 'bad echo', 'got' => $reply]));
            return;
        }
        fpmng_worker_respond($id, 200, ['Content-Type' => 'application/json'], json_encode(['ok' => true]));
        return;
    }

    fpmng_worker_respond($id, 200, [], 'hello from pid ' . getmypid());
}

$watcher = fpmng_worker_event_create(FPMNG_WORKER_READ, $notify, function () use ($notify): void {
    fread($notify, 65536);
    while (($id = fpmng_worker_next_request()) !== null) {
        handle($id);
    }
});
fpmng_worker_event_enable($watcher);
while (!fpmng_worker_may_exit()) {
    fpmng_worker_loop(true);
}
PHP);

    $port = (int) (getenv('FPMNG_DIRECT_WORKER_TLS_AND_CLIENT_TLS_PORT') ?: 28108);
    $config = <<<CFG
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
[work]
listen = 127.0.0.1:$port
pool.type = http-direct
pool.executor = worker
pm = static
pm.max_children = 1
chdir = $root
http.front_controller = /worker.php
http.tls_cert = $certFile
http.tls_key = $keyFile
http.read_timeout = 10000
catch_workers_output = yes
php_admin_value[max_execution_time] = 0
php_admin_value[display_errors] = 0
CFG;

    function fetchTls(int $port, string $path): array
    {
        $ctx = stream_context_create(['ssl' => ['verify_peer' => false, 'verify_peer_name' => false]]);
        $fp = stream_socket_client("tls://127.0.0.1:$port", $errno, $error, 10,
            STREAM_CLIENT_CONNECT, $ctx);
        if (!$fp) throw new RuntimeException("connect tls :$port: $error");
        stream_set_timeout($fp, 10);
        fwrite($fp, "GET $path HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n");
        $response = stream_get_contents($fp);
        fclose($fp);
        if (!preg_match('#^HTTP/1\.\d (\d+) #', $response, $m)) {
            throw new RuntimeException('bad status line: ' . var_export($response, true));
        }
        $split = strpos($response, "\r\n\r\n");
        $body = $split === false ? '' : substr($response, $split + 4);
        return [(int) $m[1], $body];
    }

    $tester = new FPM\Tester($config, '<?php');
    $tester->start();
    $tester->expectLogStartNotices();

    [$status, $body] = fetchTls($port, '/');
    check($status === 200, "hello over server TLS: $status");
    check(str_starts_with($body, 'hello from pid '), 'hello over server TLS: ' . var_export($body, true));
    echo "server-tls-serves: ok\n";

    /* Repeat the combined round trip a few times in the same worker process:
     * each iteration answers over the pool's OWN TLS listener while, inside
     * that same handler invocation, making an outbound TLS client connection
     * to the local origin. If OpenSSL's process-global error queue or any
     * other shared state leaked between the two roles, this would surface as
     * an intermittent failure across iterations rather than the first one. */
    for ($i = 0; $i < 5; $i++) {
        [$status, $body] = fetchTls($port, "/roundtrip?i=$i");
        $decoded = json_decode($body, true);
        check($status === 200, "roundtrip $i: status $status, body " . var_export($body, true));
        check(is_array($decoded) && ($decoded['ok'] ?? false) === true,
            "roundtrip $i: unexpected body " . var_export($body, true));
    }
    echo "server-tls-and-outbound-client-tls-coexist: ok\n";

    /* The worker (and its own server-TLS listener) is still healthy
     * afterwards. */
    [$status, $body] = fetchTls($port, '/');
    check($status === 200, "final hello over server TLS: $status");
    check(str_starts_with($body, 'hello from pid '), 'final hello over server TLS: ' . var_export($body, true));
    echo "worker-persists: ok\n";

    $tester->expectNoLogPattern('/ERROR:/', true);
} finally {
    if ($tester !== null) {
        $tester->terminate();
        $tester->close();
    }
    if (is_resource($origin)) {
        proc_terminate($origin);
        foreach ($originPipes as $pipe) {
            @fclose($pipe);
        }
        proc_close($origin);
    }
    foreach (glob("$root/*") ?: [] as $file) {
        @unlink($file);
    }
    @rmdir($root);
}
echo "Done\n";
?>
--EXPECT--
server-tls-serves: ok
server-tls-and-outbound-client-tls-coexist: ok
worker-persists: ok
Done
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>

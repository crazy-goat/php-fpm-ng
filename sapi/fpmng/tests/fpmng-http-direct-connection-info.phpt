--TEST--
fpm-ng: fpm_connection_info() reports transport/peer/TLS/client-certificate facts the worker's own socket can vouch for, and is refused where none of that is true (issue #62)
--SKIPIF--
<?php
include "fpmng-skipif.inc";
if (!function_exists('openssl_x509_parse')) {
    die('skip requires the openssl extension');
}
if (trim((string) shell_exec('command -v openssl 2>/dev/null')) === '') {
    die('skip requires the openssl CLI to generate test certificates');
}
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
?>
--FILE--
<?php
require_once "tester.inc";

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

$root = sys_get_temp_dir() . '/fpmng-direct-conninfo-' . getmypid();
@mkdir($root, 0700, true);

/* A CA, a server certificate the pools serve, a client certificate the CA
 * signed, and a client certificate nothing here trusts -- the minimum needed
 * to exercise http.tls_verify_client's three shapes (none/optional/require)
 * and both outcomes ("presented and trusted" / "presented and not"). */
run("openssl req -x509 -newkey rsa:2048 -nodes -days 2 -sha256 -subj /CN=ca.test -keyout $root/ca.key -out $root/ca.crt");
/* Signed by the same CA as the client certificate below, not self-signed:
 * that lets the openssl s_client cross-check use -CAfile ca.crt to validate
 * BOTH directions of the handshake, so "Verify return code: 0 (ok)" there
 * is a genuine confirmation of the same mutual trust PHP reports, not an
 * artifact of s_client trusting nothing. */
run("openssl req -newkey rsa:2048 -nodes -subj /CN=server.test -keyout $root/server.key -out $root/server.csr");
run("openssl x509 -req -in $root/server.csr -CA $root/ca.crt -CAkey $root/ca.key -CAcreateserial -days 2 -sha256 -out $root/server.crt");
run("openssl req -newkey rsa:2048 -nodes -subj /CN=client.test -keyout $root/client.key -out $root/client.csr");
run("openssl x509 -req -in $root/client.csr -CA $root/ca.crt -CAkey $root/ca.key -CAcreateserial -days 2 -sha256 -out $root/client.crt");
file_put_contents("$root/client.pem", file_get_contents("$root/client.crt") . file_get_contents("$root/client.key"));
run("openssl req -x509 -newkey rsa:2048 -nodes -days 2 -sha256 -subj /CN=untrusted.test -keyout $root/untrusted.key -out $root/untrusted.crt");
file_put_contents("$root/untrusted.pem", file_get_contents("$root/untrusted.crt") . file_get_contents("$root/untrusted.key"));

file_put_contents("$root/index.php", <<<'PHP'
<?php
header('Content-Type: application/json');
echo json_encode(fpm_connection_info());
PHP);

file_put_contents("$root/worker.php", <<<'PHP'
<?php
$notify = fpmng_worker_notify_stream();
$watcher = fpmng_worker_event_create(FPMNG_WORKER_READ, $notify, function () use ($notify): void {
    fread($notify, 65536);
    while (($id = fpmng_worker_next_request()) !== null) {
        fpmng_worker_respond($id, 200, ['Content-Type' => 'application/json'],
            json_encode(fpm_connection_info()));
    }
});
fpmng_worker_event_enable($watcher);
while (!fpmng_worker_may_exit()) {
    fpmng_worker_loop(true);
}
PHP);

file_put_contents("$root/gateway.php", <<<'PHP'
<?php
header('Content-Type: application/json');
echo json_encode(['exists' => function_exists('fpm_connection_info')]);
PHP);

function tlsContext(?string $localCertPem = null): mixed
{
    $opts = [
        'verify_peer' => false,
        'verify_peer_name' => false,
        'capture_peer_cert' => true,
        'SNI_enabled' => false,
    ];
    if ($localCertPem !== null) {
        $opts['local_cert'] = $localCertPem;
    }
    return stream_context_create(['ssl' => $opts]);
}

function tlsConnect(int $port, ?string $localCertPem = null, float $timeout = 5.0)
{
    $client = @stream_socket_client("ssl://127.0.0.1:$port", $errno, $errstr, $timeout,
        STREAM_CLIENT_CONNECT, tlsContext($localCertPem));
    return $client ?: null;
}

function tlsRequest($client, int $port): string
{
    fwrite($client, "GET / HTTP/1.1\r\nHost: 127.0.0.1:$port\r\nConnection: close\r\n\r\n");
    $response = '';
    $deadline = microtime(true) + 5.0;
    while (microtime(true) < $deadline) {
        /* A cert this pool rejects can tear the connection down mid-read
         * (post-handshake fatal alert in TLS 1.3, see tlsHandshakeRejected());
         * that surfaces here as an E_WARNING from fread(), which is exactly
         * the "no response" outcome this loop already handles below. */
        $chunk = @fread($client, 8192);
        if ($chunk === false || $chunk === '') {
            if (feof($client)) break;
            usleep(20000);
            continue;
        }
        $response .= $chunk;
    }
    return $response;
}

/* Retries: the master respawns children, and the first connection after a
 * start can arrive before the child is listening. */
function tlsGetInfo(int $port, ?string $localCertPem = null, int $attempts = 50): array|false|null
{
    for ($i = 0; $i < $attempts; $i++) {
        $client = tlsConnect($port, $localCertPem);
        if ($client) {
            $response = tlsRequest($client, $port);
            fclose($client);
            $split = strpos($response, "\r\n\r\n");
            if ($split !== false) {
                return json_decode(substr($response, $split + 4), true, flags: JSON_THROW_ON_ERROR);
            }
        }
        usleep(100000);
    }
    return null;
}

/* True once the handshake itself has had a chance to settle (so a rejection
 * case is not mistaken for "child not listening yet"). */
function tlsHandshakeRejected(int $port, ?string $localCertPem, int $attempts = 30): bool
{
    /* A rejected client certificate can fail the PHP stream in three
     * different ways depending on exactly when OpenSSL notices: outright
     * connect failure, a handshake-layer error surfaced through $errstr, or
     * (TLS 1.3, observed in practice) a handshake that completes on the
     * client's own side of the state machine before the server's
     * post-Finished fatal alert tears the connection down -- stream_socket_
     * client() then returns a "successful" resource that immediately reads
     * as EOF/closed with no bytes ever served. So: only ECONNREFUSED (the
     * pool's listener not up yet) is worth retrying; anything else -- a
     * false return, or a resource that produces no response -- is treated as
     * the rejection this helper is checking for. */
    for ($i = 0; $i < $attempts; $i++) {
        $client = @stream_socket_client("ssl://127.0.0.1:$port", $errno, $errstr, 2.0,
            STREAM_CLIENT_CONNECT, tlsContext($localCertPem));
        if (!$client) {
            if ($errno === 111) {
                usleep(100000);
                continue;
            }
            return true;
        }
        $response = tlsRequest($client, $port);
        fclose($client);
        if ($response === '') {
            return true;
        }
        return false;
    }
    return true;
}

$portNone = (int) (getenv('FPMNG_DIRECT_CONNINFO_PORT') ?: 28094);
$portOptional = $portNone + 1;
$portRequire = $portNone + 2;
$portWorker = $portNone + 3;
$fcgiListen = $portNone + 4;
$portGateway = $portNone + 5;

$config = <<<CFG
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
[none]
listen = 127.0.0.1:$portNone
pool.type = http-direct
pm = static
pm.max_children = 1
chdir = $root
http.front_controller = /index.php
http.read_timeout = 10000
http.tls_cert = $root/server.crt
http.tls_key = $root/server.key
php_admin_value[display_errors] = 0
[optional]
listen = 127.0.0.1:$portOptional
pool.type = http-direct
pm = static
pm.max_children = 1
chdir = $root
http.front_controller = /index.php
http.read_timeout = 10000
http.tls_cert = $root/server.crt
http.tls_key = $root/server.key
http.tls_verify_client = optional
http.tls_client_ca = $root/ca.crt
php_admin_value[display_errors] = 0
[require]
listen = 127.0.0.1:$portRequire
pool.type = http-direct
pm = static
pm.max_children = 1
chdir = $root
http.front_controller = /index.php
http.read_timeout = 10000
http.tls_cert = $root/server.crt
http.tls_key = $root/server.key
http.tls_verify_client = require
http.tls_client_ca = $root/ca.crt
php_admin_value[display_errors] = 0
[worker]
listen = 127.0.0.1:$portWorker
pool.type = http-direct
pool.executor = worker
pm = static
pm.max_children = 1
chdir = $root
http.front_controller = /worker.php
http.read_timeout = 10000
http.tls_cert = $root/server.crt
http.tls_key = $root/server.key
http.tls_verify_client = optional
http.tls_client_ca = $root/ca.crt
php_admin_value[max_execution_time] = 0
php_admin_value[display_errors] = 0
[gateway]
listen = 127.0.0.1:$fcgiListen
pool.type = http
pm = static
pm.max_children = 1
chdir = $root
http.listen = 127.0.0.1:$portGateway
php_admin_value[display_errors] = 0
CFG;

$tester = new FPM\Tester($config, '<?php');
try {
    $tester->start();
    $tester->expectLogStartNotices();

    /* 1. http.tls_verify_client = none (the default): TLS facts are there,
     * client-certificate keys are not present at all -- not null, absent --
     * because this pool never asked the client for one. */
    $info = tlsGetInfo($portNone);
    check($info !== null, 'no response from the [none] pool');
    check($info['transport'] === 'tls', 'transport: ' . var_export($info['transport'] ?? null, true));
    check(is_string($info['tls_protocol']) && str_starts_with($info['tls_protocol'], 'TLS'),
        'tls_protocol: ' . var_export($info['tls_protocol'] ?? null, true));
    check(is_string($info['tls_cipher']) && $info['tls_cipher'] !== '', 'tls_cipher missing');
    check(array_key_exists('tls_alpn', $info) && $info['tls_alpn'] === null, 'tls_alpn: ' . var_export($info['tls_alpn'] ?? '<absent>', true));
    check(array_key_exists('tls_sni', $info) && $info['tls_sni'] === null, 'tls_sni: ' . var_export($info['tls_sni'] ?? '<absent>', true));
    check(!array_key_exists('client_cert_verified', $info), 'verify_client=none still exposed client_cert_* keys');
    check($info['peer_addr'] === '127.0.0.1', 'peer_addr: ' . var_export($info['peer_addr'] ?? null, true));
    check($info['requests'] === 1, 'requests: ' . var_export($info['requests'] ?? null, true));
    echo "tls-none-no-client-cert-keys: ok\n";

    /* 2. http.tls_verify_client = optional, no client certificate presented:
     * the handshake completes (optional does not require one), and the
     * client_cert_* keys are present but null/false -- a script that checks
     * client_cert_verified without first checking the key exists still sees
     * a consistent, honest answer. */
    $info = tlsGetInfo($portOptional);
    check($info !== null, 'no response from the [optional] pool without a client cert');
    check($info['transport'] === 'tls', 'optional/no-cert transport');
    check($info['client_cert_verified'] === false, 'optional/no-cert verified: ' . var_export($info['client_cert_verified'] ?? null, true));
    check($info['client_cert_subject'] === null, 'optional/no-cert subject: ' . var_export($info['client_cert_subject'] ?? '<absent>', true));
    echo "tls-optional-no-client-cert: ok\n";

    /* 3. http.tls_verify_client = optional, a CA-signed client certificate
     * presented: verified, and every client_cert_* field populated from the
     * certificate the client actually sent -- not a pool default. */
    $info = tlsGetInfo($portOptional, "$root/client.pem");
    check($info !== null, 'no response from the [optional] pool with a valid client cert');
    check($info['client_cert_verified'] === true, 'optional/valid-cert verified: ' . var_export($info['client_cert_verified'] ?? null, true));
    check(str_contains($info['client_cert_subject'], 'CN=client.test'),
        'client_cert_subject: ' . var_export($info['client_cert_subject'] ?? null, true));
    check(str_contains($info['client_cert_issuer'], 'CN=ca.test'),
        'client_cert_issuer: ' . var_export($info['client_cert_issuer'] ?? null, true));
    check(preg_match('/^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}Z$/', $info['client_cert_not_before']) === 1,
        'client_cert_not_before shape: ' . var_export($info['client_cert_not_before'] ?? null, true));
    check(preg_match('/^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}Z$/', $info['client_cert_not_after']) === 1,
        'client_cert_not_after shape: ' . var_export($info['client_cert_not_after'] ?? null, true));

    /* Cross-check against an independent observation of the SAME certificate
     * file, and against a live openssl s_client handshake against the SAME
     * running pool -- so this is checking the actual connection, not just
     * that PHP can echo back a default. */
    $expectFingerprint = strtolower(str_replace(':', '', trim(
        preg_replace('/^.*=/', '', shell_exec("openssl x509 -in $root/client.crt -noout -fingerprint -sha256")))));
    check($info['client_cert_fingerprint_sha256'] === $expectFingerprint,
        "fingerprint mismatch: got {$info['client_cert_fingerprint_sha256']} want $expectFingerprint");

    $sOut = (string) shell_exec("printf 'GET / HTTP/1.0\\r\\nHost: x\\r\\n\\r\\n' | "
        . "openssl s_client -connect 127.0.0.1:$portOptional -cert $root/client.crt -key $root/client.key "
        . "-CAfile $root/ca.crt 2>&1");
    check(preg_match('/Verify return code:\s*0\s*\(ok\)/', $sOut) === 1,
        "openssl s_client did not confirm the server accepted our CA-signed cert:\n$sOut");
    check(preg_match('/Protocol\s*:\s*(\S+)/', $sOut, $m) === 1 && $m[1] === $info['tls_protocol'],
        "protocol mismatch between PHP ({$info['tls_protocol']}) and s_client ($sOut)");
    echo "tls-optional-valid-client-cert-cross-checked: ok\n";

    /* 4. A client certificate nothing here trusts is a handshake failure,
     * not "not verified" -- OpenSSL's default verify callback aborts on a
     * bad chain regardless of optional/require, and only the ABSENCE of a
     * certificate is what "optional" tolerates. */
    check(tlsHandshakeRejected($portOptional, "$root/untrusted.pem"),
        'untrusted client cert was accepted under verify_client=optional');
    echo "tls-optional-untrusted-client-cert-rejected: ok\n";

    /* 5. http.tls_verify_client = require, no client certificate: rejected. */
    check(tlsHandshakeRejected($portRequire, null),
        'no client cert was accepted under verify_client=require');
    echo "tls-require-no-client-cert-rejected: ok\n";

    /* 6. http.tls_verify_client = require, a CA-signed client certificate:
     * accepted and verified, same as case 3 but under the stricter mode. */
    $info = tlsGetInfo($portRequire, "$root/client.pem");
    check($info !== null, 'no response from the [require] pool with a valid client cert');
    check($info['client_cert_verified'] === true, 'require/valid-cert verified: ' . var_export($info['client_cert_verified'] ?? null, true));
    echo "tls-require-valid-client-cert-accepted: ok\n";

    /* 7. pool.executor = worker: fpm_connection_info() exists (a script
     * cannot mistake it for "not compiled in") but is the defined
     * "unsupported" answer -- false -- because the worker executor has no
     * single current connection to report on. */
    $info = tlsGetInfo($portWorker, "$root/client.pem");
    check($info === false, 'worker executor should answer false, got: ' . var_export($info, true));
    echo "worker-executor-unsupported: ok\n";

    /* 8. pool.type = http (the FastCGI gateway): fpm_connection_info() does
     * not exist at all -- the defined "unsupported" answer for a pool type
     * this API was never wired into, so function_exists() tells the truth
     * rather than opcache folding a call that would fatal. */
    $body = '';
    for ($i = 0; $i < 50 && $body === ''; $i++) {
        $body = (string) @file_get_contents("http://127.0.0.1:$portGateway/gateway.php");
        if ($body === '') usleep(100000);
    }
    check($body !== '', 'no response from the [gateway] pool');
    $data = json_decode($body, true, flags: JSON_THROW_ON_ERROR);
    check($data['exists'] === false, 'fpm_connection_info() exists on a non-direct pool');
    echo "non-direct-pool-unsupported: ok\n";

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
tls-none-no-client-cert-keys: ok
tls-optional-no-client-cert: ok
tls-optional-valid-client-cert-cross-checked: ok
tls-optional-untrusted-client-cert-rejected: ok
tls-require-no-client-cert-rejected: ok
tls-require-valid-client-cert-accepted: ok
worker-executor-unsupported: ok
non-direct-pool-unsupported: ok
Done
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>

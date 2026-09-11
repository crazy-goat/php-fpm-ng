--TEST--
FPM http gateway: TLS serves the full certificate chain from http.tls_cert (task 039)
--SKIPIF--
<?php
include "skipif.inc";
if (!function_exists('openssl_x509_parse')) {
    die('skip requires the openssl extension');
}
if (trim((string) shell_exec('command -v openssl 2>/dev/null')) === '') {
    die('skip requires the openssl CLI to generate a test certificate chain');
}
$probe = new FPM\Tester(<<<'EOT'
[global]
error_log = {{FILE:LOG}}
[unconfined]
listen = {{ADDR}}
pm = static
pm.max_children = 1
pool.type = http
http.listen = {{ADDR[probe]}}
http.tls_cert = /nonexistent-cert.pem
http.tls_key = /nonexistent-key.pem
EOT, '<?php');
$messages = $probe->testConfig(true, null, false, false);
FPM\Tester::clean();
$notBuilt = false;
foreach ((array) $messages as $message) {
    if (str_contains($message, 'built with TLS support')) {
        $notBuilt = true;
    }
}
if ($notBuilt) {
    die('skip php-fpm-ng built without TLS support (libevent_openssl and/or OpenSSL not found at build time)');
}
?>
--FILE--
<?php

require_once "tester.inc";
require_once "fpmng-tester.inc";

// Task 039: fpm_http_tls_ctx_new()/fpm_http_tls_check() used to install only
// the first PEM block from http.tls_cert (the leaf), silently dropping every
// intermediate that follows it in a fullchain.pem. This test builds a real
// 2-level CA (root -> intermediate -> leaf), points one pool at the full
// chain and another at a leaf-only certificate, and checks -- over a real
// TLS handshake, not just by reading the PEM -- exactly what each pool sends
// on the wire.

$certDir = sys_get_temp_dir() . '/' . basename(__FILE__, '.php') . '-certs';
@mkdir($certDir, 0700, true);

function run(string $cmd): void
{
    exec($cmd . ' 2>&1', $output, $code);
    if ($code !== 0) {
        echo "COMMAND FAILED: $cmd\n" . implode("\n", $output) . "\n";
        exit(1);
    }
}

// Root CA.
run("openssl genrsa -out $certDir/root.key 2048");
run("openssl req -x509 -new -key $certDir/root.key -sha256 -days 2 " .
    "-subj /CN=root.test -out $certDir/root.crt");

// Intermediate CA, signed by the root.
run("openssl genrsa -out $certDir/intermediate.key 2048");
run("openssl req -new -key $certDir/intermediate.key -subj /CN=intermediate.test " .
    "-out $certDir/intermediate.csr");
file_put_contents("$certDir/intermediate.ext", "basicConstraints=critical,CA:TRUE\nkeyUsage=critical,keyCertSign,cRLSign\n");
run("openssl x509 -req -in $certDir/intermediate.csr -CA $certDir/root.crt -CAkey $certDir/root.key " .
    "-CAcreateserial -days 2 -sha256 -extfile $certDir/intermediate.ext -out $certDir/intermediate.crt");

// Leaf, signed by the intermediate.
run("openssl genrsa -out $certDir/leaf.key 2048");
run("openssl req -new -key $certDir/leaf.key -subj /CN=leaf.test -out $certDir/leaf.csr");
file_put_contents("$certDir/leaf.ext", "basicConstraints=CA:FALSE\nkeyUsage=digitalSignature,keyEncipherment\n");
run("openssl x509 -req -in $certDir/leaf.csr -CA $certDir/intermediate.crt -CAkey $certDir/intermediate.key " .
    "-CAcreateserial -days 2 -sha256 -extfile $certDir/leaf.ext -out $certDir/leaf.crt");

file_put_contents(
    "$certDir/fullchain.pem",
    file_get_contents("$certDir/leaf.crt") . file_get_contents("$certDir/intermediate.crt")
);
copy("$certDir/leaf.crt", "$certDir/leafonly.pem");

function connectAndCollectChain(string $addr, ?string $caFile): ?array
{
    [$host, $port] = explode(':', $addr);
    $sslOptions = [
        'verify_peer'          => $caFile !== null,
        'verify_peer_name'     => false,
        'capture_peer_cert_chain' => true,
    ];
    if ($caFile !== null) {
        $sslOptions['cafile'] = $caFile;
    }
    $ctx = stream_context_create(['ssl' => $sslOptions]);
    $fp = @stream_socket_client(
        "ssl://$host:$port",
        $errno,
        $errstr,
        5,
        STREAM_CLIENT_CONNECT,
        $ctx
    );
    if (!$fp) {
        return null;
    }
    $chain = stream_context_get_options($ctx)['ssl']['peer_certificate_chain'] ?? [];
    fclose($fp);

    $subjects = [];
    foreach ($chain as $cert) {
        $parsed = openssl_x509_parse($cert);
        $subjects[] = $parsed['subject']['CN'] ?? '?';
    }

    return $subjects;
}

// --- Pool 1: fullchain.pem (leaf + intermediate) ---

$dir = __DIR__;
$cfg1 = <<<EOT
[global]
error_log = {{FILE:LOG}}
[unconfined]
listen = {{ADDR}}
pm = static
pm.max_children = 1
chdir = $dir
pool.type = http
http.listen = {{ADDR[chain]}}
http.tls_cert = $certDir/fullchain.pem
http.tls_key = $certDir/leaf.key
EOT;

$code = <<<'EOT'
<?php
echo "worker-hit\n";
EOT;

$tester1 = new FPM\Tester($cfg1, $code);
$tester1->start();
fpmng_expect_log_start_notices($tester1);

$httpAddr1 = $tester1->getAddr('ipv4', '[chain]');

// Not trusting the root: verification must fail without the chain being sent.
$noTrust = connectAndCollectChain($httpAddr1, null);
echo 'chain pool, no verify: ', count($noTrust ?? []), " cert(s)\n";
foreach ($noTrust ?? [] as $cn) {
    echo "  cn=$cn\n";
}

// Trusting only the root: verification must succeed BECAUSE the intermediate
// was sent along with the leaf.
$trusted = connectAndCollectChain($httpAddr1, "$certDir/root.crt");
echo 'chain pool, verify against root: ', ($trusted === null ? 'HANDSHAKE FAILED' : 'ok'), "\n";
foreach ($trusted ?? [] as $cn) {
    echo "  cn=$cn\n";
}

// The gateway is still an ordinary, working HTTP/TLS endpoint.
$response = file_get_contents("https://127.0.0.1:" . explode(':', $httpAddr1)[1] . '/' . basename($tester1->makeSourceFile()), false, stream_context_create([
    'ssl' => ['verify_peer' => false, 'verify_peer_name' => false],
]));
echo 'chain pool request body: ', var_export($response, true), "\n";

$tester1->terminate();
$tester1->expectLogTerminatingNotices();
$tester1->close();

// --- Pool 2: leaf-only certificate (regression: unchanged behaviour) ---

$cfg2 = <<<EOT
[global]
error_log = {{FILE:LOG}}
[unconfined]
listen = {{ADDR}}
pm = static
pm.max_children = 1
pool.type = http
http.listen = {{ADDR[leafonly]}}
http.tls_cert = $certDir/leafonly.pem
http.tls_key = $certDir/leaf.key
EOT;

$tester2 = new FPM\Tester($cfg2, $code);
$tester2->start();
fpmng_expect_log_start_notices($tester2);

$httpAddr2 = $tester2->getAddr('ipv4', '[leafonly]');
$leafOnly = connectAndCollectChain($httpAddr2, null);
echo 'leaf-only pool: ', count($leafOnly ?? []), " cert(s)\n";
foreach ($leafOnly ?? [] as $cn) {
    echo "  cn=$cn\n";
}

$tester2->terminate();
$tester2->expectLogTerminatingNotices();
$tester2->close();

// Cleanup.
foreach (glob("$certDir/*") as $f) {
    @unlink($f);
}
@rmdir($certDir);

?>
Done
--EXPECTF--
chain pool, no verify: 2 cert(s)
  cn=leaf.test
  cn=intermediate.test
chain pool, verify against root: ok
  cn=leaf.test
  cn=intermediate.test
chain pool request body: 'worker-hit
'
leaf-only pool: 1 cert(s)
  cn=leaf.test
Done
--CLEAN--
<?php
require_once "tester.inc";
FPM\Tester::clean();
// Belt-and-suspenders: --FILE-- already removes its own cert directory on the
// success path; this only mops up after an abnormal (e.g. timed out) run.
foreach (glob(sys_get_temp_dir() . '/http-tls-chain*-certs') as $dir) {
    foreach (glob("$dir/*") as $f) {
        @unlink($f);
    }
    @rmdir($dir);
}
?>

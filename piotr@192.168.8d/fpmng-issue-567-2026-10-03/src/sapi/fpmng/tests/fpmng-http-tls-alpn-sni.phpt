--TEST--
FPM http gateway: TLS advertises ALPN http/1.1 and selects a certificate by SNI (task 041)
--SKIPIF--
<?php
include "fpmng-skipif.inc";
fpmng_skip_if_pool_type_unsupported('gateway');
if (!function_exists('openssl_x509_parse')) {
    die('skip requires the openssl extension');
}
if (trim((string) shell_exec('command -v openssl 2>/dev/null')) === '') {
    die('skip requires the openssl CLI to generate test certificates and drive the handshakes');
}
if (trim((string) shell_exec('command -v timeout 2>/dev/null')) === '' && trim((string) shell_exec('command -v gtimeout 2>/dev/null')) === '') {
    die('skip requires a "timeout" (or "gtimeout") command so a stuck handshake cannot hang the test suite');
}
$probe = new FPM\Tester(<<<'EOT'
[global]
error_log = {{FILE:LOG}}
[gw]
pool.type = gateway
listen = {{ADDR[probe]}}
http.tls_cert = /nonexistent-cert.pem
http.tls_key = /nonexistent-key.pem
http.route[unconfined] = /
[unconfined]
pool.type = fastcgi
listen = {{ADDR}}
pm = static
pm.max_children = 1
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
    die('skip php-fpm-ng built without TLS support (configure without --enable-fpmng-tls, issue #280)');
}
?>
--FILE--
<?php

require_once "tester.inc";

// Task 041: ALPN (advertise http/1.1, reject a client offering only some
// other protocol) and SNI (one pool, two certificates, selected by
// servername; no SNI / an unrecognized name falls back to the default
// certificate). Both are verified over a REAL TLS handshake with the openssl
// CLI, not PHP's ssl:// wrapper -- that wrapper does not expose ALPN
// negotiation or let a client choose an unsupported protocol.

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

// timeout(1) is GNU coreutils; macOS dev boxes only have it via Homebrew
// (gtimeout) -- the SKIPIF above already required one of the two to exist.
$timeoutBin = trim((string) shell_exec('command -v timeout 2>/dev/null'));
if ($timeoutBin === '') {
    $timeoutBin = trim((string) shell_exec('command -v gtimeout 2>/dev/null'));
}

// Two self-signed leaf certificates -- this test is about ALPN/SNI
// selection, not chain building (that's task 039 / http-tls-chain.phpt).
run("openssl genrsa -out $certDir/default.key 2048");
run("openssl req -x509 -new -key $certDir/default.key -sha256 -days 2 " .
    "-subj /CN=default.test -out $certDir/default.crt");
run("openssl genrsa -out $certDir/other.key 2048");
run("openssl req -x509 -new -key $certDir/other.key -sha256 -days 2 " .
    "-subj /CN=other.test -out $certDir/other.crt");

// s_client is given </dev/null so it has nothing to send after the
// handshake, and $timeoutBin bounds a handshake that never completes (the
// rejected-ALPN case) so it cannot hang the test suite.
function sClient(string $timeoutBin, string $addr, array $extraArgs): array
{
    $args = implode(' ', array_map('escapeshellarg', $extraArgs));
    $cmd = "$timeoutBin 5 openssl s_client -connect " . escapeshellarg($addr) . " $args </dev/null 2>&1";
    exec($cmd, $output, $code);
    return [$code, implode("\n", $output)];
}

function alpnProtocol(string $timeoutBin, string $addr, string $alpn): array
{
    [$code, $out] = sClient($timeoutBin, $addr, ['-alpn', $alpn]);
    if (preg_match('/^ALPN protocol: (.+)$/m', $out, $m)) {
        return [$code, $m[1]];
    }
    return [$code, null];
}

function certSubjectCn(string $timeoutBin, string $addr, ?string
$servername): ?string
{
    $args = ['-showcerts'];
    if ($servername !== null) {
        $args[] = '-servername';
        $args[] = $servername;
    }
    [, $out] = sClient($timeoutBin, $addr, $args);
    if (!preg_match('/-----BEGIN CERTIFICATE-----.*?-----END CERTIFICATE-----/s', $out, $m)) {
        return null;
    }
    $parsed = openssl_x509_parse($m[0]);
    return $parsed['subject']['CN'] ?? null;
}

$dir = __DIR__;
$cfg = <<<EOT
[global]
error_log = {{FILE:LOG}}
[gw]
pool.type = gateway
listen = {{ADDR[gw]}}
chdir = $dir
http.tls_cert = $certDir/default.crt
http.tls_key = $certDir/default.key
http.tls_sni_cert = "  other.test : $certDir/other.crt : $certDir/other.key  "
http.route[unconfined] = /
[unconfined]
pool.type = fastcgi
listen = {{ADDR}}
pm = static
pm.max_children = 1
chdir = $dir
EOT;

$code = <<<'EOT'
<?php
echo "worker-hit\n";
EOT;

$tester = new FPM\Tester($cfg, $code);
$tester->start();
$tester->expectLogStartNotices();

$addr = $tester->getAddr('ipv4', '[gw]');

// --- ALPN ---

[, $negotiated] = alpnProtocol($timeoutBin, $addr, 'http/1.1');
echo 'ALPN http/1.1: ', var_export($negotiated, true), "\n";

// What the acceptance criterion actually requires is that the handshake
// fails (nonzero exit) and that no ALPN protocol was negotiated. The exact
// human-readable alert text ("no application protocol", etc.) is an
// implementation detail of the openssl CLI that varies across OpenSSL /
// LibreSSL versions and platforms -- e.g. this differs between the macOS
// LibreSSL s_client used during development and the OpenSSL 3.x s_client on
// CI's ubuntu-latest -- so it is not something to assert on.
[$bogusCode, $bogusNegotiated] = alpnProtocol($timeoutBin, $addr, 'some-bogus-protocol');
echo 'ALPN unsupported-only: exit=', $bogusCode,
    ', negotiated=', var_export($bogusNegotiated, true), "\n";

// A client sending no ALPN extension at all must still get a normal,
// working HTTPS response (today's, pre-041, behaviour).
$response = file_get_contents("https://127.0.0.1:" . explode(':', $addr)[1] . '/' . basename($tester->makeSourceFile()), false, stream_context_create([
    'ssl' => ['verify_peer' => false, 'verify_peer_name' => false],
]));
echo 'no-ALPN request body: ', var_export($response, true), "\n";

// --- SNI ---

echo 'SNI other.test: ', var_export(certSubjectCn($timeoutBin, $addr, 'other.test'), true), "\n";
echo 'SNI default.test: ', var_export(certSubjectCn($timeoutBin, $addr, 'default.test'), true), "\n";
echo 'SNI none: ', var_export(certSubjectCn($timeoutBin, $addr, null), true), "\n";
echo 'SNI unrecognized: ', var_export(certSubjectCn($timeoutBin, $addr, 'unknown.test'), true), "\n";

$tester->terminate();
$tester->expectLogTerminatingNotices();
$tester->close();

// Cleanup.
foreach (glob("$certDir/*") as $f) {
    @unlink($f);
}
@rmdir($certDir);

?>
Done
--EXPECTF--
ALPN http/1.1: 'http/1.1'
ALPN unsupported-only: exit=1, negotiated=NULL
no-ALPN request body: 'worker-hit
'
SNI other.test: 'other.test'
SNI default.test: 'default.test'
SNI none: 'default.test'
SNI unrecognized: 'default.test'
Done
--CLEAN--
<?php
require_once "tester.inc";
FPM\Tester::clean();
// Belt-and-suspenders: --FILE-- already removes its own cert directory on the
// success path; this only mops up after an abnormal (e.g. timed out) run.
foreach (glob(sys_get_temp_dir() . '/http-tls-alpn-sni*-certs') as $dir) {
    foreach (glob("$dir/*") as $f) {
        @unlink($f);
    }
    @rmdir($dir);
}
?>

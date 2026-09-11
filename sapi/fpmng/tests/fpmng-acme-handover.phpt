--TEST--
ACME: a certificate installed by the renewer reaches every gateway process, through task 040's mechanism and no other (issue #47)
--SKIPIF--
<?php
include "skipif.inc";
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
pool.type = http
http.listen = {{ADDR[probe]}}
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
require_once __DIR__ . '/../acme/state.php';

use FpmNg\Acme\State;

// Issue #47 criterion 3: after a successful renewal every gateway process
// serves the new certificate -- "this is 040's mechanism; this task must
// not grow a second one". So this test deliberately adds nothing: the
// renewer's only act is State::installCertificate(), the ordinary
// atomic write into the ACME state directory, and http.tls_cert points
// straight at that file. Everything after the rename is task 040's
// content-digest poll in the master plus the per-child adoption in
// fpm_http_tls_reload.c.
//
// The point of running with http.gateways = 4 is that each gateway builds
// its own SSL_CTX after fork(): a mechanism that reached only the master,
// or only the child that happened to be forked first, would show up here
// as a mixture of old and new serials rather than as a clean failure.

$root = sys_get_temp_dir() . '/' . basename(__FILE__, '.php') . '-' . getmypid();
$stateRoot = "$root/state";
@mkdir($stateRoot, 0700, true);

const DOMAIN = 'handover.test';

function run(string $cmd): void
{
    exec($cmd . ' 2>&1', $output, $code);
    if ($code !== 0) {
        echo "COMMAND FAILED: $cmd\n" . implode("\n", $output) . "\n";
        exit(1);
    }
}

function check(bool $ok, string $message): void
{
    if (!$ok) {
        echo "FAIL: $message\n";
    }
}

$state = new State($stateRoot);
$state->ensureDomainDir(DOMAIN);
$dir = $state->domainDir(DOMAIN);

// One key, two certificates. Reusing the key across a renewal is what the
// state layout does by default (state.php keeps privkey.pem), and it is
// also what keeps the pair on disk consistent at every instant: only one
// file changes, so the master can never digest a new chain against an old
// key.
run("openssl genrsa -out $dir/privkey.pem 2048");
chmod("$dir/privkey.pem", 0600);
run("openssl req -x509 -new -key $dir/privkey.pem -sha256 -days 2 " .
    "-set_serial 0x1111111111 -subj /CN=" . DOMAIN . " -out $root/first.pem");
run("openssl req -x509 -new -key $dir/privkey.pem -sha256 -days 2 " .
    "-set_serial 0x2222222222 -subj /CN=" . DOMAIN . " -out $root/renewed.pem");

// The certificate in effect before the renewal, installed the same way the
// renewer installs one.
$state->installCertificate(DOMAIN, (string) file_get_contents("$root/first.pem"), null);

/** The serial the gateway on this connection is serving, or null. */
function servedSerial(string $addr): ?string
{
    [$host, $port] = explode(':', $addr);
    $ctx = stream_context_create(['ssl' => [
        'verify_peer' => false,
        'verify_peer_name' => false,
        'capture_peer_cert' => true,
    ]]);
    $fp = @stream_socket_client("ssl://$host:$port", $errno, $errstr, 5, STREAM_CLIENT_CONNECT, $ctx);
    if (!$fp) {
        return null;
    }
    $cert = stream_context_get_options($ctx)['ssl']['peer_certificate'] ?? null;
    fclose($fp);
    if ($cert === null) {
        return null;
    }
    $parsed = openssl_x509_parse($cert);
    return $parsed['serialNumberHex'] ?? null;
}

/** @return array<string,int> serial => how many of $n connections saw it */
function serialsOver(string $addr, int $n): array
{
    $seen = [];
    for ($i = 0; $i < $n; $i++) {
        $serial = servedSerial($addr) ?? 'HANDSHAKE-FAILED';
        $seen[$serial] = ($seen[$serial] ?? 0) + 1;
    }
    return $seen;
}

/**
 * The serials this test uses are decimal digits, so PHP casts them to int
 * array keys; compare the keys as the strings they came back as.
 */
function distinctSerials(array $seen): array
{
    return array_map('strval', array_keys($seen));
}

$testDir = __DIR__;
$cfg = <<<EOT
[global]
error_log = {{FILE:LOG}}
[unconfined]
listen = {{ADDR}}
pm = static
pm.max_children = 1
chdir = $testDir
pool.type = http
http.listen = {{ADDR[tls]}}
http.gateways = 4
http.tls_cert = $dir/fullchain.pem
http.tls_key = $dir/privkey.pem
http.tls_reload_check = 1s
EOT;

$tester = new FPM\Tester($cfg, '<?php echo "worker\n";');
$tester->start();
$tester->expectLogStartNotices();

$addr = $tester->getAddr('ipv4', '[tls]');

/* Before the renewal: 40 fresh connections, spread across the four
 * gateways by SO_REUSEPORT, all serving the certificate that was on disk
 * when they forked. */
$before = serialsOver($addr, 40);
check(distinctSerials($before) === ['1111111111'],
    'serials before the renewal: ' . json_encode($before));
echo "all-gateways-serve-the-installed-certificate: ok\n";

/* The renewal. Nothing else happens: no signal, no reload, no new
 * directive -- just the atomic install the ACME client performs. */
$state->installCertificate(DOMAIN, (string) file_get_contents("$root/renewed.pem"), null);

/* http.tls_reload_check = 1s, so the master notices within about a second
 * and each gateway adopts on its own timer. Waiting for the first sighting
 * separately from the all-gateways assertion keeps a slow machine from
 * reading as a partial rollout. */
$deadline = microtime(true) + 30.0;
$appeared = false;
while (microtime(true) < $deadline) {
    if (servedSerial($addr) === '2222222222') {
        $appeared = true;
        break;
    }
    usleep(100000);
}
check($appeared, 'the renewed certificate was never served');

/* Now the part criterion 3 is actually about: not "a gateway picked it up"
 * but "every gateway did". Give the slowest adoption timer a moment, then
 * require 40 consecutive connections to agree. */
$after = [];
$deadline = microtime(true) + 30.0;
while (microtime(true) < $deadline) {
    $after = serialsOver($addr, 40);
    if (distinctSerials($after) === ['2222222222']) {
        break;
    }
    usleep(200000);
}
check(distinctSerials($after) === ['2222222222'],
    'serials after the renewal: ' . json_encode($after));
echo "every-gateway-serves-the-renewed-certificate: ok\n";

/* The pool is still an ordinary working endpoint afterwards -- a reload
 * that leaves the listener degraded would not be a successful renewal. */
$body = @file_get_contents(
    'https://' . $addr . '/' . basename($tester->makeSourceFile()),
    false,
    stream_context_create(['ssl' => ['verify_peer' => false, 'verify_peer_name' => false]])
);
check(trim((string) $body) === 'worker', 'request after the renewal: ' . var_export($body, true));
echo "pool-still-serves-requests: ok\n";

$tester->terminate();
$tester->expectLogTerminatingNotices();

/* No key material in the log, at any level (issue #49 criterion 7 starts
 * here). */
$log = (string) @file_get_contents($tester->getPrefixedFile('log'));
$key = (string) file_get_contents("$dir/privkey.pem");
/* Strip whatever PEM label this OpenSSL wrote -- 1.1.1 emits "BEGIN RSA
 * PRIVATE KEY", 3.x emits "BEGIN PRIVATE KEY". Matching only one of them
 * would leave the header in $keyBody, and a needle that starts with a
 * header line can never appear in a log, so the assertion would pass
 * whether or not key bytes leaked. */
$keyBody = trim(preg_replace('/-----(?:BEGIN|END)[^-]*-----|\s+/', '', $key));
check(strlen($keyBody) > 200, 'could not extract the key body to search for: ' . strlen($keyBody));
check(!str_contains($log, substr($keyBody, 40, 60)), 'the private key appeared in error_log');
echo "no-key-material-in-the-log: ok\n";

$tester->close();

foreach (glob("$dir/*") as $file) {
    @unlink($file);
}
@rmdir($dir);
foreach (glob("$root/*") as $file) {
    @unlink($file);
}
@rmdir($stateRoot);
@rmdir($root);

?>
Done
--EXPECT--
all-gateways-serve-the-installed-certificate: ok
every-gateway-serves-the-renewed-certificate: ok
pool-still-serves-requests: ok
no-key-material-in-the-log: ok
Done

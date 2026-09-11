--TEST--
fpm-ng: a certificate is obtained end to end over ACME HTTP-01, answered by the gateway, and not reordered while it is valid (issue #49)
--SKIPIF--
<?php
include "skipif.inc";
if (!extension_loaded('openssl')) {
    die('skip requires the openssl extension');
}
if (!function_exists('openssl_csr_new')) {
    die('skip requires an OpenSSL build with CSR support');
}
?>
--FILE--
<?php
require_once "tester.inc";

/* Issue #49 criteria 2, 3 and 4, proved the only way they can be: a real
 * order, against a server that actually fetches the HTTP-01 answer from this
 * build's own gateway over TCP and rejects the order if it is wrong.
 *
 * The CA is acme-fake-ca.inc, a separate CLI process. It is a fake, but not
 * a lenient one -- it verifies the ES256 signature, spends each nonce once,
 * checks the signed `url`, hands out a different token per authorization and
 * refuses a CSR that does not ask for every ordered name. What it cannot do
 * is rate-limit us or need a network, which is exactly why a real CA is not
 * usable in the test suite (see docs/acme-renewal.md). Issuance against a
 * live pebble stays a manual step.
 *
 * The renewer runs where it is meant to run: inside a pool whose type may
 * publish challenges. It is not called from this process, because "the
 * client works when driven from the CLI" is not the claim being made. */

function check(bool $condition, string $message): void
{
    if (!$condition) throw new RuntimeException($message);
}

function waitFor(string $path, float $seconds, string $what): string
{
    $deadline = microtime(true) + $seconds;
    while (microtime(true) < $deadline) {
        $body = @file_get_contents($path);
        if ($body !== false && trim($body) !== '') {
            return trim($body);
        }
        usleep(50000);
    }
    throw new RuntimeException("timed out after {$seconds}s waiting for $what ($path)");
}

$here = __DIR__;
$root = sys_get_temp_dir() . '/fpmng-acme-issue-' . getmypid();
@mkdir("$root/state", 0700, true);

/* A throwaway CA. rsa:2048 rather than an EC key because the point here is
 * the client's own EC account key, not the CA's, and an RSA CA works on every
 * OpenSSL this suite runs on. */
$caKey = openssl_pkey_new(['private_key_bits' => 2048, 'private_key_type' => OPENSSL_KEYTYPE_RSA]);
check($caKey !== false, 'could not generate a CA key');
$caCsr = openssl_csr_new(['commonName' => 'fpmng fake CA'], $caKey, ['digest_alg' => 'sha256']);
$caCert = openssl_csr_sign($caCsr, null, $caKey, 2, ['digest_alg' => 'sha256'], 1);
check($caCert !== false, 'could not self-sign the CA certificate');
$caCertPem = '';
openssl_x509_export($caCert, $caCertPem);
$caKeyPem = '';
openssl_pkey_export($caKey, $caKeyPem);
file_put_contents("$root/ca-cert.pem", $caCertPem);
file_put_contents("$root/ca-key.pem", $caKeyPem);

file_put_contents("$root/env.php", "<?php echo \"worker\\n\";\n");

/* The script the ACME pool runs. It waits for a file rather than for a clock
 * because the CA cannot start until the gateway has a port, and the gateway
 * does not have one until fpm is up -- so the order is: fpm, then CA, then
 * go. Everything it reports goes through files; stdout of a supervisor pool
 * is the error log, and the log is checked at the end for key material. */
$runner = <<<PHP
<?php
\$root = '$root';
/* The CA cannot bind until the gateway has a port, and the gateway does not
 * have one until fpm is up -- so this pool starts first and waits here for
 * the directory URL rather than for a clock. */
\$deadline = microtime(true) + 30;
while (microtime(true) < \$deadline && !file_exists(\$root . '/go')) {
    usleep(20000);
}
\$url = trim((string) @file_get_contents(\$root . '/go'));
if (\$url === '') {
    file_put_contents(\$root . '/tick.out', "EXCEPTION: no directory URL arrived within 30s\\n");
    exit(1);
}
putenv('ACME_STATE_DIR=' . \$root . '/state');
putenv('ACME_DOMAINS=acme.test,www.acme.test');
putenv('ACME_DIRECTORY=' . \$url);
require '$here/../acme/renew.php';

\$lines = [];
\$log = static function (string \$message) use (&\$lines): void { \$lines[] = \$message; };
try {
    /* Criterion 8: the preflight runs here, in the pool, and must pass --
     * if it did not, nothing below would be reachable. */
    \FpmNg\Acme\Client::preflight();
    \$lines[] = 'preflight: ok';

    \$renewer = \FpmNg\Acme\Renewer::fromEnvironment(\$log);
    \$lines[] = 'tick1: ' . \$renewer->tick();
    /* Criterion 3: a second tick against a certificate that is still valid
     * must place no order at all. Proved below by the CA's request log, not
     * by this word. */
    \$lines[] = 'tick2: ' . \$renewer->tick();
} catch (\Throwable \$e) {
    \$lines[] = 'EXCEPTION ' . get_class(\$e) . ': ' . \$e->getMessage();
}
file_put_contents(\$root . '/tick.out', implode("\\n", \$lines) . "\\n");
PHP;
file_put_contents("$root/runner.php", $runner);

$cfg = <<<EOT
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
[web]
listen = {{ADDR}}
chdir = $root
pm = static
pm.max_children = 2
pool.type = http
http.listen = {{ADDR[http]}}
http.front_controller = /env.php
[acme]
pool.type = supervisor
supervisor.script = $root/runner.php
supervisor.processes = 1
supervisor.restart = never
EOT;

$tester = new FPM\Tester($cfg, "<?php echo \"worker\\n\";\n");
$tester->start();
$tester->expectLogStartNotices();
$http = $tester->getAddr('ipv4', '[http]');

/* The CA binds port 0 and reports what it got, so nothing here races it. */
$ca = proc_open(
    [PHP_BINARY, '-n', "$here/acme-fake-ca.inc", '0', $root, $http,
        "$root/ca-cert.pem", "$root/ca-key.pem", '90'],
    [1 => ['file', "$root/ca-stdout.log", 'w'], 2 => ['file', "$root/ca-stderr.log", 'w']],
    $pipes
);
check(is_resource($ca), 'could not start the fake CA');
$caPort = (int) waitFor("$root/ca-ready", 10, 'the fake CA to listen');
check($caPort > 0, "the fake CA reported port $caPort");
echo "fake-ca-listening: ok\n";

file_put_contents("$root/go", "http://127.0.0.1:$caPort/directory");
$ticks = waitFor("$root/tick.out", 60, 'the ACME pool to finish two ticks');

check(!str_contains($ticks, 'EXCEPTION'), "the renewer threw:\n$ticks");
check(str_contains($ticks, 'preflight: ok'), "preflight did not pass in the pool:\n$ticks");
echo "preflight-in-pool: ok\n";

check(str_contains($ticks, 'tick1: renewed'), "first tick:\n$ticks");
echo "first-tick-renewed: ok\n";

/* Criterion 2: the certificate on disk is the one this CA issued, it covers
 * every configured name, and it is readable only by the owner. */
$chain = (string) @file_get_contents("$root/state/acme.test/fullchain.pem");
check(str_contains($chain, '-----BEGIN CERTIFICATE-----'), 'no chain was installed');
$leaf = openssl_x509_parse($chain);
check(is_array($leaf), 'the installed leaf does not parse');
$sans = array_map('trim', explode(',', (string) ($leaf['extensions']['subjectAltName'] ?? '')));
sort($sans);
check($sans === ['DNS:acme.test', 'DNS:www.acme.test'], 'SANs: ' . implode(' ', $sans));
check(str_contains((string) ($leaf['issuer']['CN'] ?? ''), 'fpmng fake CA'),
    'issuer: ' . var_export($leaf['issuer']['CN'] ?? null, true));
echo "certificate-covers-both-names: ok\n";

/* The chain really is a chain: the CA certificate is appended, so a server
 * can present an intermediate. Two PEM blocks, leaf first. */
check(substr_count($chain, '-----BEGIN CERTIFICATE-----') === 2,
    'the installed file is not leaf + issuer: ' . substr_count($chain, '-----BEGIN CERTIFICATE-----') . ' certificates');
check(openssl_x509_verify($chain, $caCertPem) === 1, 'the leaf is not signed by this CA');
echo "chain-is-leaf-plus-issuer: ok\n";

check(sprintf('%o', fileperms("$root/state/acme.test/privkey.pem") & 0777) === '600',
    'private key mode: ' . sprintf('%o', fileperms("$root/state/acme.test/privkey.pem") & 0777));
check(openssl_x509_check_private_key($chain, (string) file_get_contents("$root/state/acme.test/privkey.pem")),
    'the installed key does not match the installed certificate');
echo "key-matches-and-is-private: ok\n";

/* Criterion 4: the answer the CA validated came from the gateway, over TCP,
 * from the shared challenge store -- and it is gone now that the order is
 * over. A token left behind would be an answer to a challenge nobody holds. */
$requests = (string) @file_get_contents("$root/ca-requests.log");
check(substr_count($requests, "POST /challenge/") >= 2,
    "the CA validated fewer than two challenges:\n$requests");
echo "both-challenges-validated: ok\n";

$fp = stream_socket_client("tcp://$http", $errno, $error, 5);
check($fp !== false, "connect to the gateway failed: $error");
fwrite($fp, "GET /.well-known/acme-challenge/fpmngTestToken000000000000000000 HTTP/1.1\r\nHost: acme.test\r\nConnection: close\r\n\r\n");
$raw = stream_get_contents($fp);
fclose($fp);
check(str_contains($raw, ' 404 '), 'the token is still published after the order: ' . substr($raw, 0, 40));
echo "tokens-cleared-after-order: ok\n";

/* Criterion 3, the half that matters: the second tick placed no order. One
 * POST /new-order in the whole run, and the CA never heard from us again
 * after the certificate was fetched. */
check(substr_count($requests, "POST /new-order\n") === 1,
    "orders placed:\n$requests");
check(str_contains($ticks, 'tick2: up-to-date'), "second tick:\n$ticks");
echo "valid-certificate-is-not-reordered: ok\n";

proc_terminate($ca);
proc_close($ca);

/* Criterion 7: nothing secret in the log. Both private keys are checked --
 * the account key and the certificate key -- with the PEM armour stripped,
 * because different OpenSSL versions label it differently and a needle that
 * starts with a header line could never appear in a log whatever the code
 * did. Asserted through the log reader, which sees everything the master
 * wrote including the decorated child output. */
foreach (['state/acme.test/privkey.pem', 'state/account.key'] as $secret) {
    $pem = (string) @file_get_contents("$root/$secret");
    check($pem !== '', "expected a key at $secret");
    $body = trim((string) preg_replace('/-----(?:BEGIN|END)[^-]*-----|\s+/', '', $pem));
    check(strlen($body) > 80, "could not extract the body of $secret: " . strlen($body));
    $tester->expectNoLogPattern('/' . preg_quote(substr($body, 20, 40), '/') . '/', true);
}
echo "no-key-material-in-the-log: ok\n";

$tester->terminate();
$tester->expectLogTerminatingNotices();
$tester->close();

exec('rm -rf ' . escapeshellarg($root));

?>
Done
--EXPECT--
fake-ca-listening: ok
preflight-in-pool: ok
first-tick-renewed: ok
certificate-covers-both-names: ok
chain-is-leaf-plus-issuer: ok
key-matches-and-is-private: ok
both-challenges-validated: ok
tokens-cleared-after-order: ok
valid-certificate-is-not-reordered: ok
no-key-material-in-the-log: ok
Done
--CLEAN--
<?php
require_once "tester.inc";
FPM\Tester::clean();
?>

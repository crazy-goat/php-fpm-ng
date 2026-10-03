--TEST--
ACME renewal policy: staging by default, renew on remaining lifetime, warn before expiry, and say what is missing (issue #49)
--SKIPIF--
<?php
if (!extension_loaded('openssl')) {
    die('skip requires the openssl extension');
}
if (!function_exists('openssl_csr_new')) {
    die('skip requires an OpenSSL build with CSR support');
}
?>
--FILE--
<?php

/* The half of issue #49 that needs no CA: whether to place an order at all,
 * and what an operator is told. Deliberately separate from
 * fpmng-acme-issue.phpt -- a policy that can only be exercised by issuing a
 * real certificate is a policy nobody can test cheaply, and "renew when
 * fewer than N days are left" has more edges than an end-to-end run reaches.
 *
 * This runs under the CLI on purpose. The fpmng_acme_challenge_* builtins do
 * not exist here, which is exactly the environment criterion 8's preflight
 * has to describe correctly. */

require __DIR__ . '/../acme/renew.php';

use FpmNg\Acme\Client;
use FpmNg\Acme\HttpClient;
use FpmNg\Acme\Renewer;
use FpmNg\Acme\State;
use FpmNg\Acme\StateError;
use FpmNg\Acme\TransportError;
use FpmNg\Acme\UnsupportedEnvironment;

function check(bool $condition, string $message): void
{
    if (!$condition) throw new RuntimeException($message);
}

$root = sys_get_temp_dir() . '/fpmng-acme-policy-' . getmypid();
@mkdir("$root/state", 0700, true);

/* A throwaway CA, only so that there are certificates with known expiry. */
$caKey = openssl_pkey_new(['private_key_bits' => 2048, 'private_key_type' => OPENSSL_KEYTYPE_RSA]);
$caCert = openssl_csr_sign(
    openssl_csr_new(['commonName' => 'fpmng policy CA'], $caKey, ['digest_alg' => 'sha256']),
    null, $caKey, 3650, ['digest_alg' => 'sha256'], 1
);

/** Write a certificate for $sans, valid for $days, at $path. */
function issue(array $sans, int $days, string $path, $caCert, $caKey): void
{
    static $serial = 100;
    $conf = tempnam(sys_get_temp_dir(), 'fpmng-policy-');
    file_put_contents($conf, "[req]\ndistinguished_name=dn\n[dn]\n[ext]\nsubjectAltName="
        . implode(',', array_map(static fn(string $s): string => "DNS:$s", $sans)) . "\n");
    $key = openssl_pkey_new(['private_key_bits' => 2048, 'private_key_type' => OPENSSL_KEYTYPE_RSA]);
    $csr = openssl_csr_new(['commonName' => $sans[0]], $key, ['digest_alg' => 'sha256']);
    $cert = openssl_csr_sign($csr, $caCert, $caKey, $days,
        ['digest_alg' => 'sha256', 'config' => $conf, 'x509_extensions' => 'ext'], ++$serial);
    @unlink($conf);
    $pem = '';
    openssl_x509_export($cert, $pem);
    @mkdir(dirname($path), 0700, true);
    file_put_contents($path, $pem);
}

/* 1. Criterion 1: staging is the default and production needs a deliberate
 *    opt-in, checked against the endpoint rather than against a flag
 *    somebody named "production". */
putenv('ACME_ALLOW_PRODUCTION');
check(str_contains(Renewer::STAGING_DIRECTORY, 'acme-staging'),
    'the default directory is not staging: ' . Renewer::STAGING_DIRECTORY);
Renewer::assertDirectoryAllowed(Renewer::STAGING_DIRECTORY);
foreach (['http://127.0.0.1:8080/directory', 'https://localhost/dir',
          'https://pebble.localhost/dir', 'https://ca.internal/dir'] as $allowed) {
    Renewer::assertDirectoryAllowed($allowed);
}
$refused = 'https://acme-v02.api.letsencrypt.org/directory';
try {
    Renewer::assertDirectoryAllowed($refused);
    check(false, 'a production directory was accepted without an opt-in');
} catch (StateError $e) {
    check(str_contains($e->getMessage(), 'ACME_ALLOW_PRODUCTION'),
        'the refusal does not name the opt-in: ' . $e->getMessage());
    check(str_contains($e->getMessage(), $refused),
        'the refusal does not name the URL: ' . $e->getMessage());
}
/* The gate matches whole words, not substrings. "contest" and "latest"
 * contain "test"; a CA whose operator happened to pick such a name would
 * otherwise skip the opt-in entirely and spend a real rate limit. */
foreach (['https://acme.contest-ca.org/directory',
          'https://latest-ca.org/directory',
          'https://testing-grounds.example.org/directory'] as $notATestServer) {
    try {
        Renewer::assertDirectoryAllowed($notATestServer);
        check(false, "a substring match let $notATestServer through the production gate");
    } catch (StateError $e) {
        check(str_contains($e->getMessage(), 'ACME_ALLOW_PRODUCTION'),
            'the refusal does not name the opt-in: ' . $e->getMessage());
    }
}
putenv('ACME_ALLOW_PRODUCTION=1');
Renewer::assertDirectoryAllowed($refused);
putenv('ACME_ALLOW_PRODUCTION');
echo "staging-by-default: ok\n";

/* Plaintext reaches loopback and nothing else. The opt-in above says which
 * CA may be used; this says the account key, the ordered names and the
 * issued chain do not travel in the clear to get there. */
$transport = new HttpClient('php-fpm-ng ACME client test', null);
try {
    $transport->get('http://acme-v02.api.letsencrypt.org/directory');
    check(false, 'a plaintext request to a public CA was allowed');
} catch (TransportError $e) {
    check(str_contains($e->getMessage(), 'plaintext'),
        'the refusal does not say why: ' . $e->getMessage());
}
/* Not loopback, whatever it looks like. */
try {
    $transport->get('http://127.0.0.1.attacker.example/directory');
    check(false, 'a hostname merely starting with 127.0.0.1 was treated as loopback');
} catch (TransportError $e) {
    check(str_contains($e->getMessage(), 'plaintext'), 'wrong refusal: ' . $e->getMessage());
}
echo "plaintext-only-to-loopback: ok\n";

$state = new State("$root/state");
$log = static function (string $message): void {};
$renewer = new Renewer($state, Renewer::STAGING_DIRECTORY,
    ['acme.test', 'www.acme.test'], 30, null, $log);

/* 2. Criterion 3: no certificate at all is the obvious trigger, and the
 *    message says which of the four reasons it was. */
$reason = $renewer->dueReason('acme.test');
check($reason === 'no certificate is installed', 'missing certificate: ' . var_export($reason, true));
echo "no-certificate-is-due: ok\n";

/* 3. A certificate with plenty of life left is not reissued. This is the
 *    case that a naive "renew every N days" scheduler gets wrong on every
 *    restart, which is why the trigger is lifetime and not a calendar. */
$chain = $state->certChainPath('acme.test');
issue(['acme.test', 'www.acme.test'], 60, $chain, $caCert, $caKey);
check($renewer->dueReason('acme.test') === null,
    'a 60-day certificate was due at a 30-day threshold: ' . var_export($renewer->dueReason('acme.test'), true));
check($renewer->expiryWarning('acme.test') === null, 'a 60-day certificate produced an expiry warning');
echo "healthy-certificate-is-not-due: ok\n";

/* 4. Inside the threshold it is due, and the reason carries the number an
 *    operator would check it against. */
issue(['acme.test', 'www.acme.test'], 10, $chain, $caCert, $caKey);
$reason = (string) $renewer->dueReason('acme.test');
check(str_contains($reason, 'days of validity left') && str_contains($reason, 'threshold is 30'),
    'expiring certificate: ' . $reason);
echo "expiring-certificate-is-due: ok\n";

/* 5. Criterion 6: the operator is warned, with the date, before it expires.
 *    The warning is about the expiry, not about the attempt count -- so it
 *    appears for a certificate nobody has even tried to renew yet. */
$warning = (string) $renewer->expiryWarning('acme.test');
check(str_contains($warning, 'acme.test') && str_contains($warning, 'expires in'),
    'warning: ' . $warning);
check(preg_match('/\d{4}-\d{2}-\d{2}T/', $warning) === 1, 'the warning carries no date: ' . $warning);
echo "expiry-warning: ok\n";

/* 6. A certificate that is valid for long enough but does not cover every
 *    configured name is due anyway. Adding a name to a pool must get a new
 *    certificate, not a silently wrong one. */
issue(['acme.test'], 60, $chain, $caCert, $caKey);
check($renewer->dueReason('acme.test') === 'the installed certificate does not cover every configured name',
    'missing SAN: ' . var_export($renewer->dueReason('acme.test'), true));
check(Renewer::covers($chain, ['acme.test']), 'covers() missed a name that is present');
check(!Renewer::covers($chain, ['www.acme.test']), 'covers() accepted a name that is absent');
/* Spelling is not a second name: the comparison is canonical, so a pool
 * configured with 'ACME.test.' does not reissue forever. */
check(Renewer::covers($chain, ['ACME.test.']), 'covers() treated a spelling as a different name');
echo "missing-name-is-due: ok\n";

/* 7. Garbage on disk is a reason to renew, not a crash. A truncated write or
 *    a half-copied file must not take the renewer down with it. */
file_put_contents($chain, "-----BEGIN CERTIFICATE-----\nnot base64 at all\n");
check($renewer->dueReason('acme.test') === 'the installed certificate cannot be parsed',
    'unparseable certificate: ' . var_export($renewer->dueReason('acme.test'), true));
check(Renewer::notAfter($chain) === null, 'notAfter() parsed garbage');
check(Renewer::notAfter("$root/state/nothing-here.pem") === null, 'notAfter() invented a date for a missing file');
echo "unreadable-certificate-is-due: ok\n";

/* 8. Criterion 8: under the CLI the challenge builtins are absent, and the
 *    preflight has to say so in a way that points at the fix -- the pool
 *    type -- rather than reporting "call to undefined function" from three
 *    frames deep in the middle of an order. */
check(!function_exists('fpmng_acme_challenge_set'), 'this CLI unexpectedly has the challenge builtins');
try {
    Client::preflight();
    check(false, 'preflight passed under the CLI');
} catch (UnsupportedEnvironment $e) {
    check(str_contains($e->getMessage(), 'pool.type'),
        'preflight does not name the pool type: ' . $e->getMessage());
    check(str_contains($e->getMessage(), 'fpmng_acme_challenge'),
        'preflight does not name what is missing: ' . $e->getMessage());
}
echo "preflight-explains-the-environment: ok\n";

/* 9. Configuration mistakes are refused where they are made, naming the
 *    directive. An empty domain list is the one that would otherwise fail
 *    much later, as an order for nothing. */
putenv("ACME_STATE_DIR=$root/state");
putenv('ACME_DOMAINS=');
try {
    Renewer::fromEnvironment($log);
    check(false, 'an empty ACME_DOMAINS was accepted');
} catch (StateError $e) {
    check(str_contains($e->getMessage(), 'ACME_DOMAINS'), 'empty domains: ' . $e->getMessage());
}
putenv('ACME_DOMAINS=acme.test');
putenv('ACME_RENEW_DAYS=0');
try {
    Renewer::fromEnvironment($log);
    check(false, 'ACME_RENEW_DAYS = 0 was accepted');
} catch (StateError $e) {
    check(str_contains($e->getMessage(), 'ACME_RENEW_DAYS'), 'zero renew days: ' . $e->getMessage());
}
putenv('ACME_RENEW_DAYS');
$fromEnv = Renewer::fromEnvironment($log);
check($fromEnv->dueReason('acme.test') !== null, 'the environment-built renewer sees no work to do');
echo "configuration-mistakes-are-named: ok\n";

exec('rm -rf ' . escapeshellarg($root));

?>
Done
--EXPECT--
staging-by-default: ok
plaintext-only-to-loopback: ok
no-certificate-is-due: ok
healthy-certificate-is-not-due: ok
expiring-certificate-is-due: ok
expiry-warning: ok
missing-name-is-due: ok
unreadable-certificate-is-due: ok
preflight-explains-the-environment: ok
configuration-mistakes-are-named: ok
Done

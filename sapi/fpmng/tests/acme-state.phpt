--TEST--
ACME state on a writable volume: permissions, missing/read-only directory, account reuse across restarts (task 044)
--SKIPIF--
<?php
if (!extension_loaded('openssl')) {
    die('skip requires the openssl extension');
}
if (function_exists('posix_getuid') && posix_getuid() === 0) {
    die('skip root ignores directory permission bits, which this test relies on');
}
?>
--FILE--
<?php

require __DIR__ . '/../acme/state.php';

use FpmNg\Acme\State;
use FpmNg\Acme\StateError;

function perms(string $path): string
{
    return sprintf('%o', fileperms($path) & 0777);
}

$root = sys_get_temp_dir() . '/' . basename(__FILE__, '.php') . '-' . getmypid();
@mkdir($root, 0700, true);

// 1. Missing state directory: one clear error naming the path, not a crash.
$missing = $root . '/does-not-exist';
try {
    (new State($missing))->assertUsable();
    echo "FAIL: missing directory did not throw\n";
} catch (StateError $e) {
    echo str_contains($e->getMessage(), $missing) ? "missing-dir: named the path\n" : "FAIL: missing-dir message did not name the path: {$e->getMessage()}\n";
}

// 2. Read-only state directory: detected at startup, before any key is touched.
$readonly = $root . '/readonly';
mkdir($readonly, 0700);
chmod($readonly, 0500);
try {
    (new State($readonly))->assertUsable();
    echo "FAIL: read-only directory did not throw\n";
} catch (StateError $e) {
    echo str_contains($e->getMessage(), $readonly) ? "readonly-dir: named the path\n" : "FAIL: readonly-dir message did not name the path: {$e->getMessage()}\n";
}
chmod($readonly, 0700); // so cleanup below can remove it

// 3. A usable directory: account key, cert key and account record are
//    created with restrictive permissions, and never appear in a thrown
//    message.
$base = $root . '/state';
mkdir($base, 0700);

$registerCalls = 0;
$register = function () use (&$registerCalls): array {
    $registerCalls++;
    return ['url' => 'https://acme.example.invalid/acct/1'];
};

$state1 = new State($base);
$state1->assertUsable();

$accountKey1 = $state1->loadOrCreateAccountKey();
openssl_pkey_export($accountKey1, $accountKeyPem1);

$certKey1 = $state1->loadOrCreateCertKey('example.test');
openssl_pkey_export($certKey1, $certKeyPem1);

$account1 = $state1->loadOrRegisterAccount($register);

echo 'account.key perms: ' . perms($state1->accountKeyPath()) . "\n";
echo 'cert privkey perms: ' . perms($state1->certKeyPath('example.test')) . "\n";
echo 'account.json perms: ' . perms($state1->accountMetaPath()) . "\n";
echo 'register calls after first boot: ' . $registerCalls . "\n";
echo 'account url after first boot: ' . $account1['url'] . "\n";

// 4. Certificate chain install: public permissions, readable content.
$state1->installCertificateChain('example.test', "-----BEGIN CERTIFICATE-----\nFAKE\n-----END CERTIFICATE-----\n");
echo 'fullchain perms: ' . perms($state1->certChainPath('example.test')) . "\n";

// 5. "Restart": a brand new State instance over the same directory must
//    reuse the existing account key, cert key and account record -- no new
//    registration, byte-identical material.
$state2 = new State($base);
$state2->assertUsable();

$accountKey2 = $state2->loadOrCreateAccountKey();
openssl_pkey_export($accountKey2, $accountKeyPem2);

$certKey2 = $state2->loadOrCreateCertKey('example.test');
openssl_pkey_export($certKey2, $certKeyPem2);

$register2 = function () use (&$registerCalls): array {
    $registerCalls++;
    return ['url' => 'https://acme.example.invalid/acct/2-should-not-happen'];
};
$account2 = $state2->loadOrRegisterAccount($register2);

echo 'account key reused: ' . ($accountKeyPem1 === $accountKeyPem2 ? 'yes' : 'no') . "\n";
echo 'cert key reused: ' . ($certKeyPem1 === $certKeyPem2 ? 'yes' : 'no') . "\n";
echo 'register calls after restart: ' . $registerCalls . "\n";
echo 'account url after restart: ' . $account2['url'] . "\n";

// 6. Neither key ever appears in a thrown message -- provoke an error from a
//    corrupted key file and check.
$corrupted = new State($root . '/corrupt-holder');
mkdir($corrupted->baseDir, 0700, true);
file_put_contents($corrupted->accountKeyPath(), 'not a key');
try {
    $corrupted->loadOrCreateAccountKey();
    echo "FAIL: corrupted account key did not throw\n";
} catch (StateError $e) {
    $leaksKeyMaterial = str_contains($e->getMessage(), 'BEGIN') || str_contains($e->getMessage(), 'PRIVATE');
    echo $leaksKeyMaterial ? "FAIL: error message contains key material\n" : "corrupt-key error: no key material in message\n";
}

// Cleanup -- own scratch directory only (CLAUDE.md: clean up temporary dirs).
function rrmdir(string $dir): void
{
    foreach (scandir($dir) as $entry) {
        if ($entry === '.' || $entry === '..') {
            continue;
        }
        $path = "$dir/$entry";
        is_dir($path) ? rrmdir($path) : unlink($path);
    }
    rmdir($dir);
}
rrmdir($root);

?>
--EXPECT--
missing-dir: named the path
readonly-dir: named the path
account.key perms: 600
cert privkey perms: 600
account.json perms: 600
register calls after first boot: 1
account url after first boot: https://acme.example.invalid/acct/1
fullchain perms: 644
account key reused: yes
cert key reused: yes
register calls after restart: 1
account url after restart: https://acme.example.invalid/acct/1
corrupt-key error: no key material in message

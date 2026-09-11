--TEST--
ACME JOSE layer: ES256 signatures a CA can verify, a canonical thumbprint, and POST-as-GET (issue #49)
--SKIPIF--
<?php
if (!extension_loaded('openssl')) {
    die('skip requires the openssl extension');
}
if (!in_array('prime256v1', openssl_get_curve_names() ?: [], true)) {
    die('skip requires prime256v1');
}
?>
--FILE--
<?php

/* The JOSE layer is where an ACME client fails in ways no end-to-end test
 * catches reliably. openssl_sign() produces a DER SEQUENCE{r, s}; JWS wants
 * the raw 64-byte r||s, and an EC coordinate that happens to have a leading
 * zero byte must still be padded to 32 bytes. Both bugs are intermittent --
 * roughly one key or one signature in 256 -- so they show up as "the CA
 * rejects us sometimes", weeks later, against a rate limit. This test drives
 * enough keys and signatures to make either one certain rather than likely.
 */

require __DIR__ . '/../acme/jws.php';

use FpmNg\Acme\JoseError;
use FpmNg\Acme\Jws;

function check(bool $condition, string $message): void
{
    if (!$condition) throw new RuntimeException($message);
}

function newKey(): OpenSSLAsymmetricKey
{
    $key = openssl_pkey_new(['curve_name' => 'prime256v1', 'private_key_type' => OPENSSL_KEYTYPE_EC]);
    check($key !== false, 'could not generate a P-256 key');
    return $key;
}

/** r||s -> DER, the inverse of what sign() does, so openssl can verify it. */
function rsToDer(string $signature): string
{
    $integer = static function (string $value): string {
        $value = ltrim($value, "\0");
        if ($value === '' || (ord($value[0]) & 0x80)) {
            $value = "\0" . $value;
        }
        return "\x02" . chr(strlen($value)) . $value;
    };
    $body = $integer(substr($signature, 0, 32)) . $integer(substr($signature, 32, 32));
    return "\x30" . chr(strlen($body)) . $body;
}

/* 1. base64url is the encoding the whole protocol is written in: no padding,
 *    no '+' or '/', and it round-trips bytes that are not text. */
check(Jws::b64('') === '', 'b64 of the empty string');
check(Jws::b64("\x00\xff\xfe") === 'AP_-', 'b64: ' . Jws::b64("\x00\xff\xfe"));
for ($i = 0; $i < 200; $i++) {
    $raw = random_bytes(1 + ($i % 64));
    $encoded = Jws::b64($raw);
    check(preg_match('/\A[A-Za-z0-9_-]*\z/', $encoded) === 1, "not base64url: $encoded");
    check(Jws::b64decode($encoded) === $raw, "round trip failed for " . bin2hex($raw));
}
echo "base64url-round-trips: ok\n";

/* 2. RFC 7638: the JWK carries exactly the four required members, in
 *    lexicographic order, and each coordinate is the fixed-width 32-byte
 *    encoding the curve defines.
 *
 *    This is checked against OpenSSL's own uncompressed point, dug out of
 *    the public key's SubjectPublicKeyInfo, rather than against a length --
 *    because openssl_pkey_get_details() returns coordinates as trimmed big
 *    integers, so roughly one in 256 is 31 bytes or shorter. A client that
 *    forwards them unpadded produces a different thumbprint and a key
 *    authorization no CA accepts, for those keys only. Comparing against the
 *    fixed-width point makes any padding bug fail on every key, instead of
 *    on an unlucky one weeks later against a rate limit.
 *
 *    The loop also keeps going until it has actually seen a trimmed
 *    coordinate, so the padding branch is known to have run. */
$paddedSeen = false;
for ($i = 0; $i < 3000; $i++) {
    $key = newKey();
    $details = openssl_pkey_get_details($key);
    $jwk = (new Jws($key))->jwk();
    check(array_keys($jwk) === ['crv', 'kty', 'x', 'y'], 'JWK members: ' . implode(',', array_keys($jwk)));
    check($jwk['crv'] === 'P-256' && $jwk['kty'] === 'EC', 'JWK: ' . json_encode($jwk));

    /* The last 65 bytes of the SPKI are 0x04 || X || Y, both fixed width. */
    $der = (string) base64_decode((string) preg_replace(
        '/-----[^-]*-----|\s+/', '', (string) $details['key']
    ), true);
    $point = substr($der, -65);
    check(strlen($point) === 65 && $point[0] === "\x04", 'could not find the uncompressed point');

    check(Jws::b64decode($jwk['x']) === substr($point, 1, 32), "x does not match the curve point at key $i");
    check(Jws::b64decode($jwk['y']) === substr($point, 33, 32), "y does not match the curve point at key $i");

    if (strlen($details['ec']['x']) < 32 || strlen($details['ec']['y']) < 32) {
        $paddedSeen = true;
    }
    if ($paddedSeen && $i >= 50) {
        break;
    }
}
check($paddedSeen, 'no trimmed coordinate turned up in 3000 keys, so the padding branch never ran');
echo "jwk-is-canonical-and-padded: ok\n";

/* 3. The thumbprint is a SHA-256, it is stable for one key, it differs
 *    between keys, and the key authorization is exactly token '.' thumbprint
 *    -- the string the gateway serves and the CA recomputes. */
$jws = new Jws($key = newKey());
$thumb = $jws->thumbprint();
check(strlen(Jws::b64decode($thumb)) === 32, 'thumbprint is not 32 bytes');
check($thumb === (new Jws($key))->thumbprint(), 'the thumbprint is not stable for one key');
check($thumb !== (new Jws(newKey()))->thumbprint(), 'two keys share a thumbprint');
$token = 'tokenWithNoDotsInIt0123456789ab';
check($jws->keyAuthorization($token) === "$token.$thumb", 'key authorization: ' . $jws->keyAuthorization($token));
check(substr_count($jws->keyAuthorization($token), '.') === 1,
    'the key authorization has more than one dot, so the CA cannot split it');
echo "thumbprint-and-key-authorization: ok\n";

/* 4. A signature a CA can actually verify. Two hundred of them, because the
 *    DER-to-r||s conversion is where a short integer silently truncates the
 *    signature -- again about one in 256, again only visible as an
 *    intermittent rejection. Each is verified the way the CA does: over
 *    "protected.payload", with the public key rebuilt from nothing but the
 *    signed message. */
$pem = '';
openssl_pkey_export($key, $pem);
$public = openssl_pkey_get_details($key)['key'];
for ($i = 0; $i < 200; $i++) {
    $compact = $jws->sign('https://ca.test/acme/order', 'nonce' . $i, ['i' => $i], 'https://ca.test/acct/1');
    $envelope = json_decode($compact, true);
    check(is_array($envelope) && array_keys($envelope) === ['protected', 'payload', 'signature'],
        'the JWS is not flattened JSON: ' . substr($compact, 0, 120));
    $signature = Jws::b64decode($envelope['signature']);
    check(strlen($signature) === 64, "signature $i is " . strlen($signature) . ' bytes, not 64');
    check(openssl_verify($envelope['protected'] . '.' . $envelope['payload'],
        rsToDer($signature), $public, OPENSSL_ALGO_SHA256) === 1, "signature $i does not verify");
}
echo "es256-signatures-verify: ok\n";

/* 5. The protected header is what RFC 8555 section 6.2 requires, and the two
 *    forms are exclusive: newAccount identifies the key by value, everything
 *    afterwards by account URL. A request carrying both is rejected by a CA. */
$envelope = json_decode($jws->sign('https://ca.test/acme/new-account', 'n1', ['x' => 1], null), true);
$header = json_decode(Jws::b64decode($envelope['protected']), true);
check(($header['alg'] ?? '') === 'ES256', 'alg: ' . json_encode($header));
check(($header['nonce'] ?? '') === 'n1' && ($header['url'] ?? '') === 'https://ca.test/acme/new-account',
    'header: ' . json_encode($header));
check(isset($header['jwk']) && !isset($header['kid']), 'newAccount must sign with jwk, not kid');

$envelope = json_decode($jws->sign('https://ca.test/acme/order', 'n2', ['x' => 1], 'https://ca.test/acct/1'), true);
$header = json_decode(Jws::b64decode($envelope['protected']), true);
check(($header['kid'] ?? '') === 'https://ca.test/acct/1' && !isset($header['jwk']),
    'an account request must sign with kid, not jwk: ' . json_encode($header));
echo "protected-header-is-exclusive: ok\n";

/* 6. POST-as-GET, RFC 8555 section 6.3: the payload is the empty string, not
 *    "{}" and not "null". A CA reads an empty payload as "read this
 *    resource" and anything else as an attempt to modify it. */
$envelope = json_decode($jws->sign('https://ca.test/acme/authz/1', 'n3', null, 'https://ca.test/acct/1'), true);
check($envelope['payload'] === '', 'POST-as-GET payload: ' . var_export($envelope['payload'], true));
$signature = Jws::b64decode($envelope['signature']);
check(openssl_verify($envelope['protected'] . '.', rsToDer($signature), $public, OPENSSL_ALGO_SHA256) === 1,
    'the POST-as-GET signature does not cover "protected."');
/* An empty object is a different thing and must stay one: it is how a client
 * tells the CA to validate a challenge. */
$envelope = json_decode($jws->sign('https://ca.test/acme/challenge/1', 'n4', [], 'https://ca.test/acct/1'), true);
check(Jws::b64decode($envelope['payload']) === '{}',
    'an empty payload array must encode as {}: ' . var_export(Jws::b64decode($envelope['payload']), true));
echo "post-as-get-is-empty: ok\n";

/* 7. The wrong kind of key is refused where it is handed over, not deep
 *    inside an order. ACME accepts RSA accounts, but this client implements
 *    ES256 only, so an RSA key here would produce signatures labelled ES256
 *    that no CA can verify. */
foreach ([
    'RSA' => openssl_pkey_new(['private_key_bits' => 2048, 'private_key_type' => OPENSSL_KEYTYPE_RSA]),
    'the wrong curve' => openssl_pkey_new(['curve_name' => 'secp384r1', 'private_key_type' => OPENSSL_KEYTYPE_EC]),
] as $what => $wrong) {
    if ($wrong === false) {
        continue;
    }
    try {
        new Jws($wrong);
        check(false, "a $what key was accepted");
    } catch (JoseError $e) {
        check(str_contains($e->getMessage(), 'P-256') || str_contains($e->getMessage(), 'prime256v1'),
            "the refusal of $what does not say what is wanted: " . $e->getMessage());
    }
}
echo "only-p256-keys: ok\n";

?>
Done
--EXPECT--
base64url-round-trips: ok
jwk-is-canonical-and-padded: ok
thumbprint-and-key-authorization: ok
es256-signatures-verify: ok
protected-header-is-exclusive: ok
post-as-get-is-empty: ok
only-p256-keys: ok
Done

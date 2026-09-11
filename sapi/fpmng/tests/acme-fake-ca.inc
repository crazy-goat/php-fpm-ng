<?php
/**
 * A minimal RFC 8555 server, so that issue #49's client can be tested end to
 * end without a network, without a rate limit and without Docker.
 *
 * It is not a CA simulator and does not try to be: it implements the exact
 * subset our client uses, and it is strict about that subset, because a fake
 * that accepts anything tests nothing. It verifies the ES256 signature on
 * every JWS, spends each nonce once, checks that the signed `url` matches the
 * request, and -- the part that matters -- fetches
 * /.well-known/acme-challenge/<token> over the network from the gateway under
 * test and compares it against the key authorization it derives itself from
 * the client's own JWK. A client that publishes nothing, or the wrong answer,
 * fails here exactly as it would against Let's Encrypt.
 *
 * Usage:
 *   php -n acme-fake-ca.inc <port|0> <workdir> <challenge-host:port> \
 *       <ca-cert.pem> <ca-key.pem> <validity-days>
 *
 * It writes the port it bound to <workdir>/ca-ready once it is listening
 * (so pass 0 and read it back), appends one line per
 * request to <workdir>/ca-requests.log, and speaks plain HTTP because it
 * lives on loopback -- the client accepts an http:// directory only for hosts
 * that cannot be a production CA (Renewer::isNonProduction()).
 */

error_reporting(E_ALL);

[, $port, $workdir, $challengeHost, $caCertPath, $caKeyPath, $days] = array_pad($argv, 7, null);
$days = (int) $days;

$caCert = (string) file_get_contents((string) $caCertPath);
$caKey = openssl_pkey_get_private((string) file_get_contents((string) $caKeyPath));
if ($caKey === false) {
    fwrite(STDERR, "fake CA: cannot load the CA key\n");
    exit(1);
}

/* Port 0 means "pick one": the caller reads the real port back out of
 * ca-ready, which is race-free, unlike binding a port in the test to find a
 * free one and then handing the number to a process that binds it again. */
$server = @stream_socket_server("tcp://127.0.0.1:$port", $errno, $errstr);
if ($server === false) {
    fwrite(STDERR, "fake CA: cannot listen on $port: $errstr\n");
    exit(1);
}
$bound = (string) stream_socket_get_name($server, false);
$port = (int) substr($bound, (int) strrpos($bound, ':') + 1);
$base = "http://127.0.0.1:$port";

function b64(string $raw): string
{
    return rtrim(strtr(base64_encode($raw), '+/', '-_'), '=');
}

function b64d(string $encoded): string
{
    return (string) base64_decode(strtr($encoded, '-_', '+/'), true);
}

/** r||s -> DER, so openssl_verify() can check an ES256 signature. */
function rs_to_der(string $sig): string
{
    $integer = static function (string $value): string {
        $value = ltrim($value, "\0");
        if ($value === '') {
            $value = "\0";
        }
        if (ord($value[0]) & 0x80) {
            $value = "\0" . $value;
        }
        return "\x02" . chr(strlen($value)) . $value;
    };
    $body = $integer(substr($sig, 0, 32)) . $integer(substr($sig, 32, 32));
    return "\x30" . chr(strlen($body)) . $body;
}

/** RFC 7638 thumbprint of an EC P-256 JWK: lexicographic, no whitespace. */
function thumbprint(array $jwk): string
{
    $canonical = json_encode(
        ['crv' => $jwk['crv'], 'kty' => $jwk['kty'], 'x' => $jwk['x'], 'y' => $jwk['y']],
        JSON_UNESCAPED_SLASHES
    );
    return b64(hash('sha256', (string) $canonical, true));
}

/**
 * A PEM public key built from a JWK. Hand-assembled from the fixed
 * SubjectPublicKeyInfo prefix for id-ecPublicKey + prime256v1, because this
 * build has no JWK parser and that prefix never varies for this one curve.
 * Deliberately re-derives the point from x and y rather than trusting the
 * client: a client that pads a coordinate wrong produces a key that does not
 * verify its own signature, which is the bug this catches.
 */
function jwk_to_pem(array $jwk): string
{
    $point = "\x04" . str_pad(b64d((string) $jwk['x']), 32, "\0", STR_PAD_LEFT)
        . str_pad(b64d((string) $jwk['y']), 32, "\0", STR_PAD_LEFT);
    $prefix = "\x30\x59\x30\x13\x06\x07\x2a\x86\x48\xce\x3d\x02\x01"
        . "\x06\x08\x2a\x86\x48\xce\x3d\x03\x01\x07\x03\x42\x00";
    return "-----BEGIN PUBLIC KEY-----\n"
        . chunk_split(base64_encode($prefix . $point), 64, "\n")
        . "-----END PUBLIC KEY-----\n";
}

function fetch(string $url): string
{
    $context = stream_context_create(['http' => ['timeout' => 5, 'ignore_errors' => true]]);
    return (string) @file_get_contents($url, false, $context);
}

/** Nonces issued and not yet spent; a replay is a badNonce, as at a real CA. */
$nonces = [];
$accountJwk = null;
$authz = [];   /* index => ['domain' => .., 'token' => .., 'status' => .., 'error' => ..] */
$order = null;
$serial = 0x5000;

/* Written last, and atomically, so that a reader that sees the file sees a
 * complete port number and a socket already listening. */
file_put_contents("$workdir/ca-ready.tmp", "$port\n");
rename("$workdir/ca-ready.tmp", "$workdir/ca-ready");

while (true) {
    $client = @stream_socket_accept($server, 60);
    if ($client === false) {
        continue;
    }
    stream_set_timeout($client, 10);

    $head = '';
    while (!str_contains($head, "\r\n\r\n")) {
        $line = fgets($client, 8192);
        if ($line === false || $line === '') {
            break;
        }
        $head .= $line;
    }
    if (!str_contains($head, "\r\n\r\n")) {
        fclose($client);
        continue;
    }
    [$rawHead, $body] = explode("\r\n\r\n", $head, 2);
    $lines = explode("\r\n", $rawHead);
    [$method, $path] = array_pad(explode(' ', (string) array_shift($lines)), 2, '');
    $headers = [];
    foreach ($lines as $line) {
        [$name, $value] = array_pad(explode(':', $line, 2), 2, '');
        $headers[strtolower(trim($name))] = trim($value);
    }
    $length = (int) ($headers['content-length'] ?? 0);
    while (strlen($body) < $length) {
        $chunk = fread($client, $length - strlen($body));
        if ($chunk === false || $chunk === '') {
            break;
        }
        $body .= $chunk;
    }
    file_put_contents("$workdir/ca-requests.log", "$method $path\n", FILE_APPEND | LOCK_EX);

    $status = 200;
    $extra = [];
    $payload = null;
    $request = null;

    /* Every POST is a JWS. Verify it before looking at what it asks for, the
     * way a CA does: an unverifiable request never reaches the logic below. */
    if ($method === 'POST') {
        $jws = json_decode($body, true);
        $protected = is_array($jws) ? json_decode(b64d((string) ($jws['protected'] ?? '')), true) : null;
        if (!is_array($protected)) {
            $status = 400;
            $payload = ['type' => 'urn:ietf:params:acme:error:malformed', 'detail' => 'not a JWS'];
        } elseif (($protected['alg'] ?? '') !== 'ES256') {
            $status = 400;
            $payload = ['type' => 'urn:ietf:params:acme:error:badSignatureAlgorithm',
                'detail' => 'this CA only accepts ES256'];
        } else {
            $jwk = $protected['jwk'] ?? $accountJwk;
            $ok = is_array($jwk) && openssl_verify(
                $jws['protected'] . '.' . $jws['payload'],
                rs_to_der(b64d((string) $jws['signature'])),
                jwk_to_pem($jwk),
                OPENSSL_ALGO_SHA256
            ) === 1;
            if (!$ok) {
                $status = 400;
                $payload = ['type' => 'urn:ietf:params:acme:error:malformed',
                    'detail' => 'JWS verification error'];
            } elseif (!isset($nonces[(string) ($protected['nonce'] ?? '')])) {
                $status = 400;
                $payload = ['type' => 'urn:ietf:params:acme:error:badNonce',
                    'detail' => 'the nonce was already used or never issued'];
            } elseif (($protected['url'] ?? '') !== $base . $path) {
                $status = 403;
                $payload = ['type' => 'urn:ietf:params:acme:error:unauthorized',
                    'detail' => 'the signed url does not match the request'];
            } else {
                unset($nonces[$protected['nonce']]);
                $request = ($jws['payload'] ?? '') === ''
                    ? null
                    : json_decode(b64d((string) $jws['payload']), true);
                if (isset($protected['jwk'])) {
                    $accountJwk = $protected['jwk'];
                }
            }
        }
    }

    if ($payload === null) {
        switch (true) {
            case $path === '/directory':
                $payload = [
                    'newNonce' => "$base/nonce",
                    'newAccount' => "$base/new-account",
                    'newOrder' => "$base/new-order",
                ];
                break;

            case $path === '/nonce':
                $payload = [];
                break;

            case $path === '/new-account':
                $status = 201;
                $extra['Location'] = "$base/account/1";
                $payload = ['status' => 'valid'];
                break;

            case $path === '/new-order':
                $authz = [];
                foreach ((array) ($request['identifiers'] ?? []) as $i => $identifier) {
                    $authz[$i] = [
                        'domain' => (string) ($identifier['value'] ?? '?'),
                        /* A distinct token per authorization, so a client
                         * that publishes one answer and reuses it for the
                         * second name fails here. */
                        'token' => 'fpmngTestToken' . str_pad((string) $i, 18, '0', STR_PAD_LEFT),
                        'status' => 'pending',
                    ];
                }
                $order = ['status' => 'pending', 'cert' => null];
                $status = 201;
                $extra['Location'] = "$base/order/1";
                $payload = [
                    'status' => 'pending',
                    'authorizations' => array_map(
                        static fn(int $i): string => "$base/authz/$i",
                        array_keys($authz)
                    ),
                    'finalize' => "$base/order/1/finalize",
                ];
                break;

            case preg_match('#^/authz/(\d+)$#', $path, $m) === 1 && isset($authz[(int) $m[1]]):
                $a = $authz[(int) $m[1]];
                $payload = [
                    'status' => $a['status'],
                    'identifier' => ['type' => 'dns', 'value' => $a['domain']],
                    'challenges' => [array_filter([
                        'type' => 'http-01',
                        'url' => "$base/challenge/" . (int) $m[1],
                        'token' => $a['token'],
                        'status' => $a['status'],
                        'error' => $a['error'] ?? null,
                    ], static fn($v): bool => $v !== null)],
                ];
                break;

            case preg_match('#^/challenge/(\d+)$#', $path, $m) === 1 && isset($authz[(int) $m[1]]):
                $i = (int) $m[1];
                /* The real check: fetch the answer from the gateway under
                 * test and compare it with what this JWK implies. */
                $expected = $authz[$i]['token'] . '.' . thumbprint((array) $accountJwk);
                $served = fetch("http://$challengeHost/.well-known/acme-challenge/" . $authz[$i]['token']);
                if ($served === $expected) {
                    $authz[$i]['status'] = 'valid';
                } else {
                    $authz[$i]['status'] = 'invalid';
                    $authz[$i]['error'] = [
                        'type' => 'urn:ietf:params:acme:error:unauthorized',
                        'detail' => $authz[$i]['domain'] . ': the gateway served '
                            . var_export($served, true) . ' instead of the key authorization',
                    ];
                    $order['status'] = 'invalid';
                }
                $ready = $order['status'] !== 'invalid';
                foreach ($authz as $a) {
                    $ready = $ready && $a['status'] === 'valid';
                }
                if ($ready) {
                    $order['status'] = 'ready';
                }
                $payload = [
                    'type' => 'http-01',
                    'url' => "$base/challenge/$i",
                    'token' => $authz[$i]['token'],
                    'status' => $authz[$i]['status'],
                ];
                break;

            case $path === '/order/1' && $order !== null:
                $payload = ['status' => $order['status'], 'finalize' => "$base/order/1/finalize"];
                if ($order['status'] === 'valid') {
                    $payload['certificate'] = "$base/cert/1";
                }
                foreach ($authz as $a) {
                    if (isset($a['error'])) {
                        $payload['error'] = $a['error'];
                    }
                }
                break;

            case $path === '/order/1/finalize' && $order !== null:
                $der = b64d((string) ($request['csr'] ?? ''));
                $csrPem = "-----BEGIN CERTIFICATE REQUEST-----\n"
                    . chunk_split(base64_encode($der), 64, "\n")
                    . "-----END CERTIFICATE REQUEST-----\n";
                if (openssl_csr_get_public_key($csrPem) === false) {
                    $status = 400;
                    $payload = ['type' => 'urn:ietf:params:acme:error:badCSR',
                        'detail' => 'the CSR does not parse'];
                    break;
                }
                /* Every ordered name must appear in the DER. A dNSName is
                 * stored as plain ASCII there, and for every name except the
                 * subject the only place it can come from is subjectAltName
                 * -- which is what the order actually depends on. Checking
                 * the DER rather than shelling out to `openssl req -text`
                 * keeps the test running on a build with no openssl CLI. */
                $missing = [];
                foreach ($authz as $a) {
                    if (!str_contains($der, $a['domain'])) {
                        $missing[] = $a['domain'];
                    }
                }
                if ($missing !== []) {
                    $status = 400;
                    $payload = ['type' => 'urn:ietf:params:acme:error:badCSR',
                        'detail' => 'the CSR does not ask for ' . implode(',', $missing)];
                    break;
                }

                $extFile = "$workdir/issue-ext.cnf";
                file_put_contents($extFile, "[req]\ndistinguished_name=dn\n[dn]\n[fpmng]\nbasicConstraints=CA:FALSE\nsubjectAltName="
                    . implode(',', array_map(
                        static fn(array $a): string => 'DNS:' . $a['domain'],
                        $authz
                    )) . "\n");
                $cert = @openssl_csr_sign($csrPem, $caCert, $caKey, $days, [
                    'digest_alg' => 'sha256',
                    'config' => $extFile,
                    'x509_extensions' => 'fpmng',
                ], ++$serial);
                $pem = '';
                if ($cert !== false) {
                    openssl_x509_export($cert, $pem);
                }
                $order['cert'] = $pem;
                $order['status'] = $pem === '' ? 'invalid' : 'valid';
                $payload = ['status' => $order['status'], 'finalize' => "$base/order/1/finalize"];
                if ($pem === '') {
                    $payload['error'] = ['type' => 'urn:ietf:params:acme:error:serverInternal',
                        'detail' => 'the fake CA could not sign the CSR'];
                }
                break;

            case $path === '/cert/1' && $order !== null && $order['status'] === 'valid':
                $nonce = bin2hex(random_bytes(8));
                $nonces[$nonce] = true;
                $chain = $order['cert'] . $caCert;
                fwrite($client, "HTTP/1.1 200 OK\r\n"
                    . "Content-Type: application/pem-certificate-chain\r\n"
                    . "Replay-Nonce: $nonce\r\n"
                    . 'Content-Length: ' . strlen($chain) . "\r\n"
                    . "Connection: close\r\n\r\n" . $chain);
                fclose($client);
                continue 2;

            default:
                $status = 404;
                $payload = ['type' => 'urn:ietf:params:acme:error:malformed',
                    'detail' => "no such resource: $method $path"];
        }
    }

    $nonce = bin2hex(random_bytes(8));
    $nonces[$nonce] = true;
    $encoded = $payload === [] ? '{}' : (string) json_encode($payload, JSON_UNESCAPED_SLASHES);
    $reason = [200 => 'OK', 201 => 'Created', 400 => 'Bad Request',
        403 => 'Forbidden', 404 => 'Not Found'][$status] ?? 'Error';

    $response = "HTTP/1.1 $status $reason\r\n"
        . 'Content-Type: ' . ($status >= 400 ? 'application/problem+json' : 'application/json') . "\r\n"
        . "Replay-Nonce: $nonce\r\n";
    foreach ($extra as $name => $value) {
        $response .= "$name: $value\r\n";
    }
    $response .= 'Content-Length: ' . ($method === 'HEAD' ? 0 : strlen($encoded))
        . "\r\nConnection: close\r\n\r\n";
    fwrite($client, $response . ($method === 'HEAD' ? '' : $encoded));
    fclose($client);
}

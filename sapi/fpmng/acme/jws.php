<?php
/**
 * The JOSE layer of RFC 8555 (issue #49).
 *
 * ACME is a protocol, not a call: every request but the first is a flattened
 * JWS whose protected header carries the nonce the server handed out and the
 * URL being posted to. This file is that envelope and nothing else -- no
 * network, no ACME semantics -- so it can be tested against the published
 * vectors rather than against a CA.
 *
 * The account key is EC P-256 (state.php generates prime256v1), so the only
 * algorithm here is ES256. That is deliberate: a CA accepts several, but a
 * client that supports exactly the key type it generates has no negotiation
 * to get wrong, and no path where a weaker algorithm is selected for it.
 */

namespace FpmNg\Acme;

final class JoseError extends \RuntimeException
{
}

final class Jws
{
    public function __construct(private readonly \OpenSSLAsymmetricKey $key)
    {
        $details = openssl_pkey_get_details($this->key);
        if (!is_array($details) || ($details['type'] ?? null) !== OPENSSL_KEYTYPE_EC) {
            throw new JoseError(
                'the ACME account key must be an EC key on prime256v1 (P-256): '
                . 'this client signs with ES256 only'
            );
        }
        if (($details['ec']['curve_name'] ?? null) !== 'prime256v1') {
            throw new JoseError('the ACME account key must be on prime256v1 (P-256) for ES256');
        }
    }

    /** RFC 4648 section 5 with the padding removed, which is what JOSE means by base64url. */
    public static function b64(string $raw): string
    {
        return rtrim(strtr(base64_encode($raw), '+/', '-_'), '=');
    }

    public static function b64decode(string $encoded): string
    {
        $decoded = base64_decode(strtr($encoded, '-_', '+/'), true);
        if ($decoded === false) {
            throw new JoseError('not valid base64url');
        }
        return $decoded;
    }

    /**
     * The public key as a JWK, with the members in the order RFC 7638 requires
     * for a thumbprint: lexicographic, no whitespace, no extra members.
     *
     * @return array{crv: string, kty: string, x: string, y: string}
     */
    public function jwk(): array
    {
        $details = openssl_pkey_get_details($this->key);
        $ec = $details['ec'] ?? [];
        if (!isset($ec['x'], $ec['y'])) {
            throw new JoseError('the account key has no public point');
        }
        /* Both coordinates are fixed-width for the curve. openssl hands them
         * back as big-endian integers with leading zero bytes already
         * stripped, and a JWK coordinate that is one byte short is a
         * signature the CA rejects roughly once every 256 keys -- so pad. */
        return [
            'crv' => 'P-256',
            'kty' => 'EC',
            'x' => self::b64(str_pad($ec['x'], 32, "\0", STR_PAD_LEFT)),
            'y' => self::b64(str_pad($ec['y'], 32, "\0", STR_PAD_LEFT)),
        ];
    }

    /** RFC 7638 thumbprint; the HTTP-01 key authorization is "$token.$thumbprint". */
    public function thumbprint(): string
    {
        return self::b64(hash('sha256', self::json($this->jwk()), true));
    }

    public function keyAuthorization(string $token): string
    {
        return $token . '.' . $this->thumbprint();
    }

    /**
     * A flattened JWS for an ACME request.
     *
     * $accountUrl is the "kid" once the account exists; before that, pass null
     * and the JWK goes in instead -- RFC 8555 section 6.2 allows exactly one of
     * the two, and newAccount is the only request that uses the JWK form.
     *
     * $payload === null produces a POST-as-GET (an empty payload), which is how
     * RFC 8555 section 6.3 reads a resource that requires authentication.
     */
    public function sign(string $url, string $nonce, array|null $payload, ?string $accountUrl): string
    {
        $protected = ['alg' => 'ES256', 'nonce' => $nonce, 'url' => $url];
        if ($accountUrl === null) {
            $protected['jwk'] = $this->jwk();
        } else {
            $protected['kid'] = $accountUrl;
        }

        $protected64 = self::b64(self::json($protected));
        $payload64 = $payload === null ? '' : self::b64(self::json($payload));
        $signature = $this->es256($protected64 . '.' . $payload64);

        return self::json([
            'protected' => $protected64,
            'payload' => $payload64,
            'signature' => self::b64($signature),
        ]);
    }

    /**
     * ES256 as JOSE defines it: the raw 64-byte r||s pair, not the DER
     * SEQUENCE openssl_sign() produces. Getting this wrong yields a signature
     * every CA rejects with "JWS verification error" and no further detail,
     * so the conversion is explicit and validated rather than assumed.
     */
    private function es256(string $input): string
    {
        $der = '';
        if (!openssl_sign($input, $der, $this->key, OPENSSL_ALGO_SHA256)) {
            throw new JoseError('signing the ACME request failed');
        }
        [$r, $s] = self::derToRs($der);
        return str_pad($r, 32, "\0", STR_PAD_LEFT) . str_pad($s, 32, "\0", STR_PAD_LEFT);
    }

    /**
     * SEQUENCE { INTEGER r, INTEGER s } -> [r, s] as unsigned big-endian bytes.
     *
     * @return array{0: string, 1: string}
     */
    private static function derToRs(string $der): array
    {
        $offset = 0;
        $readLength = static function (string $der, int &$offset): int {
            $first = ord($der[$offset++] ?? "\xff");
            if ($first < 0x80) {
                return $first;
            }
            $count = $first & 0x7f;
            if ($count === 0 || $count > 2) {
                throw new JoseError('unexpected DER length in an ECDSA signature');
            }
            $length = 0;
            for ($i = 0; $i < $count; $i++) {
                $length = ($length << 8) | ord($der[$offset++] ?? "\xff");
            }
            return $length;
        };

        if (($der[$offset++] ?? '') !== "\x30") {
            throw new JoseError('an ECDSA signature did not start with a SEQUENCE');
        }
        $readLength($der, $offset);

        $readInteger = static function (string $der, int &$offset) use ($readLength): string {
            if (($der[$offset++] ?? '') !== "\x02") {
                throw new JoseError('an ECDSA signature component was not an INTEGER');
            }
            $length = $readLength($der, $offset);
            $value = substr($der, $offset, $length);
            if (strlen($value) !== $length) {
                throw new JoseError('an ECDSA signature was truncated');
            }
            $offset += $length;
            /* DER prefixes a zero byte when the high bit would make the
             * integer negative; a JOSE coordinate is unsigned. */
            return ltrim($value, "\0") === '' ? "\0" : ltrim($value, "\0");
        };

        return [$readInteger($der, $offset), $readInteger($der, $offset)];
    }

    /**
     * JSON as ACME needs it: no escaped slashes (a CA compares URLs), no
     * pretty printing, and an object even when empty -- json_encode([]) is
     * "[]", which a server reads as an array and rejects.
     */
    public static function json(array $value): string
    {
        if ($value === []) {
            return '{}';
        }
        $encoded = json_encode($value, JSON_UNESCAPED_SLASHES);
        if ($encoded === false) {
            throw new JoseError('cannot encode an ACME request body: ' . json_last_error_msg());
        }
        return $encoded;
    }
}

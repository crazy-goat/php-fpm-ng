<?php
/**
 * The HTTPS transport for the ACME client (issue #49).
 *
 * The mechanism is PHP's OpenSSL stream wrapper, not curl. NOTES.md section
 * 3l deferred that choice to this task; it is decided here because the
 * canonical build has no curl at all -- .github/workflows/build-matrix.yml
 * configures `--disable-all --enable-fpmng --enable-session --with-openssl`
 * and the CI image installs only libevent-dev, libssl-dev and zlib1g-dev, so
 * a curl-based client could not be exercised by the job that runs the test
 * suite. ext/openssl is already a hard requirement of this client, which
 * makes the stream wrapper the one mechanism that costs nothing extra.
 *
 * What this file owns: one request, its headers, the Replay-Nonce, and the
 * translation of an RFC 7807 problem document into an exception. It owns no
 * ACME semantics -- no orders, no authorizations -- so a protocol change does
 * not reach down here.
 *
 * Nothing in this file logs a request body. A JWS payload is not secret in
 * itself, but the same code path carries key authorizations, and "log the
 * request when something goes wrong" is exactly how those end up in a file
 * (issue #49 criterion 7).
 */

namespace FpmNg\Acme;

/** The transport failed: no answer, a TLS failure, a malformed reply. */
class TransportError extends \RuntimeException
{
}

/**
 * The server answered with an RFC 7807 problem document. $type is the ACME
 * error type ("urn:ietf:params:acme:error:badNonce" and friends), which is
 * the only part a client may branch on -- $detail is prose for an operator.
 */
final class AcmeServerError extends \RuntimeException
{
    public function __construct(
        public readonly int $status,
        public readonly string $type,
        public readonly string $detail,
        public readonly array $subproblems = [],
    ) {
        parent::__construct(sprintf(
            'the CA rejected the request: %s (%s, HTTP %d)',
            $detail !== '' ? $detail : 'no detail given',
            $type !== '' ? $type : 'no type given',
            $status
        ));
    }
}

final class HttpResponse
{
    public function __construct(
        public readonly int $status,
        /** @var array<string,string> lowercased header name => last value */
        public readonly array $headers,
        public readonly string $body,
    ) {
    }

    public function header(string $name): ?string
    {
        return $this->headers[strtolower($name)] ?? null;
    }

    /** @return array<mixed> */
    public function json(): array
    {
        if ($this->body === '') {
            return [];
        }
        $decoded = json_decode($this->body, true);
        if (!is_array($decoded)) {
            throw new TransportError('the CA sent a body that is not a JSON object');
        }
        return $decoded;
    }
}

final class HttpClient
{
    /** The last Replay-Nonce the server handed out, in any response. */
    private ?string $nonce = null;

    /**
     * @param string|null $caBundle A CA file to verify the CA's own TLS
     *        certificate against, instead of the system store. Only a test
     *        harness needs this -- a local pebble signs with its own root.
     *        Peer verification is never turned off, only re-pointed.
     */
    public function __construct(
        private readonly string $userAgent,
        private readonly ?string $caBundle = null,
        private readonly int $timeout = 30,
    ) {
    }

    public function takeNonce(): ?string
    {
        $nonce = $this->nonce;
        $this->nonce = null;
        return $nonce;
    }

    public function get(string $url): HttpResponse
    {
        return $this->request('GET', $url, null, null);
    }

    /** @param string $jws an already-signed flattened JWS */
    public function postJose(string $url, string $jws): HttpResponse
    {
        return $this->request('POST', $url, $jws, 'application/jose+json');
    }

    public function head(string $url): HttpResponse
    {
        return $this->request('HEAD', $url, null, null);
    }

    private function request(string $method, string $url, ?string $body, ?string $contentType): HttpResponse
    {
        if (!preg_match('#\Ahttps?://#i', $url)) {
            throw new TransportError('an ACME URL must be http(s): ' . $url);
        }
        /* Plaintext only to loopback. ACME's own integrity does not depend on
         * TLS -- every request is a signed JWS and every answer is bound to a
         * nonce we issued -- but the account URL, the ordered names and the
         * issued chain travel in the clear, and a cleartext directory is one
         * DNS answer away from an attacker choosing the CA. The exception
         * exists because the test suite runs a fake CA on 127.0.0.1, which
         * cannot present a certificate for a name that resolves nowhere. */
        if (strcasecmp((string) parse_url($url, PHP_URL_SCHEME), 'http') === 0
            && !self::isLoopback((string) parse_url($url, PHP_URL_HOST))
        ) {
            throw new TransportError(
                'refusing to talk to a CA over plaintext HTTP: ' . $url
                . ' -- the account key, the ordered names and the issued certificate '
                . 'would all be readable and modifiable in transit. Use https://'
            );
        }

        $headers = ['User-Agent: ' . $this->userAgent, 'Accept: application/json'];
        if ($contentType !== null) {
            $headers[] = 'Content-Type: ' . $contentType;
        }

        $ssl = [
            /* Never relaxed. A test harness points this at pebble's root; it
             * does not switch verification off, because a client that can be
             * told not to verify is one configuration mistake away from
             * accepting anybody's certificate for a real order. */
            'verify_peer' => true,
            'verify_peer_name' => true,
            'SNI_enabled' => true,
        ];
        if ($this->caBundle !== null) {
            $ssl['cafile'] = $this->caBundle;
        }

        $context = stream_context_create([
            'http' => [
                'method' => $method,
                'header' => implode("\r\n", $headers),
                'content' => $body ?? '',
                'timeout' => $this->timeout,
                /* Read 4xx bodies: an ACME error *is* the response body, and
                 * without this the wrapper returns false and throws the
                 * problem document away. */
                'ignore_errors' => true,
                /* A POST must not be replayed against another URL: the JWS
                 * signs the URL it was made for, so a followed redirect can
                 * only fail, and following it hides where it went. */
                'follow_location' => 0,
                'protocol_version' => 1.1,
            ],
            'ssl' => $ssl,
        ]);

        /* error_get_last() is process-global and nothing clears it. Without
         * this, a transport failure that raises no diagnostic of its own
         * reports whatever warning this worker last produced -- from any
         * part of the application -- as if it were the CA's fault. */
        error_clear_last();
        $raw = @file_get_contents($url, false, $context);
        /* $http_response_header is set by the wrapper even when the body read
         * fails, so read it before deciding the request failed at all. */
        $rawHeaders = $http_response_header ?? [];
        if ($rawHeaders === []) {
            throw new TransportError(sprintf(
                '%s %s: no answer from the CA (%s)',
                $method,
                $url,
                self::lastErrorMessage()
            ));
        }
        if ($raw === false) {
            $raw = '';
        }

        [$status, $headers] = self::parseHeaders($rawHeaders);
        if (isset($headers['replay-nonce'])) {
            $this->nonce = $headers['replay-nonce'];
        }

        return new HttpResponse($status, $headers, $raw);
    }

    /**
     * @param list<string> $rawHeaders
     * @return array{0: int, 1: array<string,string>}
     */
    private static function parseHeaders(array $rawHeaders): array
    {
        $status = 0;
        $headers = [];
        foreach ($rawHeaders as $line) {
            if (preg_match('#\AHTTP/\S+\s+(\d{3})#', $line, $m)) {
                /* A 1xx or a redirect chain leaves several status lines here;
                 * the last one is the response actually being returned. */
                $status = (int) $m[1];
                $headers = [];
                continue;
            }
            $colon = strpos($line, ':');
            if ($colon === false) {
                continue;
            }
            $headers[strtolower(trim(substr($line, 0, $colon)))] = trim(substr($line, $colon + 1));
        }
        return [$status, $headers];
    }

    /**
     * Loopback by address or by name. Matched exactly, never as a substring:
     * "127.0.0.1.attacker.example" is not loopback, and neither is
     * "notlocalhost".
     */
    private static function isLoopback(string $host): bool
    {
        $host = strtolower(trim($host, '[]'));
        if ($host === 'localhost' || str_ends_with($host, '.localhost')) {
            return true;
        }
        $packed = @inet_pton($host);
        if ($packed === false) {
            return false;
        }
        return $packed === inet_pton('::1') || (strlen($packed) === 4 && $packed[0] === "\x7f");
    }

    private static function lastErrorMessage(): string
    {
        $error = error_get_last();
        $message = $error['message'] ?? 'no further detail';
        /* The wrapper prefixes "file_get_contents(<url>): "; the operator
         * wants the reason, not the internals. Matched as a whole call --
         * the name and its parenthesised argument -- because the URL itself
         * contains a colon, so cutting at the first one leaves the message
         * starting in the middle of "http://". */
        return trim(preg_replace('/\A[A-Za-z_][A-Za-z0-9_]*\([^)]*\):\s*/', '', $message) ?? $message);
    }

    /**
     * Turn a non-2xx answer into the right exception, or return it unchanged.
     *
     * 2xx passes through. Anything else with a problem document becomes an
     * AcmeServerError carrying the type a caller may branch on; anything else
     * becomes a TransportError, because a CA answering 500 with HTML is not
     * something a retry policy can reason about.
     */
    public static function ensureOk(HttpResponse $response, string $what): HttpResponse
    {
        if ($response->status >= 200 && $response->status < 300) {
            return $response;
        }
        $type = $response->header('content-type') ?? '';
        if (str_contains($type, 'application/problem+json')) {
            $problem = $response->json();
            throw new AcmeServerError(
                $response->status,
                (string) ($problem['type'] ?? ''),
                (string) ($problem['detail'] ?? ''),
                is_array($problem['subproblems'] ?? null) ? $problem['subproblems'] : []
            );
        }
        throw new TransportError(sprintf(
            '%s: the CA answered HTTP %d with no problem document',
            $what,
            $response->status
        ));
    }
}

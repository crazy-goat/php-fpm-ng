<?php
/**
 * RFC 8555 in the small (issue #49): directory, account, order, HTTP-01
 * authorization, finalize, certificate.
 *
 * Scope is deliberately one shape of certificate: HTTP-01 challenges, one CA,
 * no DNS-01, no OCSP stapling, no alternate chains. Each of those is a
 * separate decision with its own failure modes; a client that supports one
 * path well is easier to reason about than one that guesses.
 *
 * The challenge answers are published through the C store from issue #48
 * (fpmng_acme_challenge_set/clear), which is why the client must run in a
 * pool whose type carries `publishes_acme_challenges` -- a `cron` or
 * `supervisor` pool. Running it anywhere else fails at the first publish
 * rather than silently serving nothing, and preflight() says so earlier.
 */

namespace FpmNg\Acme;

require_once __DIR__ . '/http.php';
require_once __DIR__ . '/jws.php';
require_once __DIR__ . '/state.php';

/** The client cannot work at all here -- a missing extension, a wrong pool. */
final class UnsupportedEnvironment extends \RuntimeException
{
}

final class Client
{
    /** RFC 8555 section 7.1.1: the directory is fetched once and reused. */
    private ?array $directory = null;
    private ?string $accountUrl = null;
    private readonly Jws $jws;

    /** Tokens published by this process, cleared even when an order fails. */
    private array $published = [];

    public function __construct(
        private readonly string $directoryUrl,
        private readonly State $state,
        private readonly HttpClient $http,
        private readonly \Closure $log,
    ) {
        $this->jws = new Jws($this->state->loadOrCreateAccountKey());
    }

    /**
     * Everything this client needs, checked before an order is attempted
     * (issue #49 criterion 8). One message per missing requirement, naming
     * what to do about it.
     */
    public static function preflight(): void
    {
        $missing = [];
        if (!extension_loaded('openssl')) {
            $missing[] = 'the OpenSSL extension (build PHP with --with-openssl): '
                . 'the account key, the CSR and the TLS connection to the CA all need it';
        } elseif (!function_exists('openssl_csr_new')) {
            $missing[] = 'openssl_csr_new() (an OpenSSL build without CSR support): '
                . 'the client cannot ask for a certificate without it';
        }
        if (!in_array('https', stream_get_wrappers(), true)) {
            $missing[] = 'the https:// stream wrapper: this build cannot reach a CA. '
                . 'It is provided by the OpenSSL extension';
        }
        if (!enabled(ini_get('allow_url_fopen'))) {
            $missing[] = 'allow_url_fopen = On: the ACME client reaches the CA through '
                . 'the https:// stream wrapper, which this setting disables';
        }
        if (!function_exists('fpmng_acme_challenge_set')) {
            $missing[] = 'the fpmng_acme_challenge_* builtins: they exist only in a pool '
                . 'whose type may publish challenges (pool.type = cron or supervisor). '
                . 'Check which pool runs this script';
        }
        if ($missing !== []) {
            throw new UnsupportedEnvironment(
                "this build cannot run the ACME client:\n  - " . implode("\n  - ", $missing)
            );
        }
    }

    /**
     * Obtain a certificate for $domains and install it. The first name is the
     * subject; the rest are SANs. Returns the installed chain.
     */
    public function order(array $domains): string
    {
        if ($domains === []) {
            throw new StateError('an order needs at least one domain');
        }
        $domains = array_values(array_map(State::canonicalDomain(...), $domains));
        $primary = $domains[0];

        $this->ensureAccount();

        try {
            $order = $this->post($this->directory()['newOrder'], [
                'identifiers' => array_map(
                    static fn(string $d): array => ['type' => 'dns', 'value' => $d],
                    $domains
                ),
            ]);
            $orderUrl = $order->header('location');
            if ($orderUrl === null) {
                throw new TransportError('the CA created an order without a Location header');
            }
            $body = $order->json();
            $this->say("order created for " . implode(', ', $domains));

            foreach ((array) ($body['authorizations'] ?? []) as $authzUrl) {
                $this->authorize((string) $authzUrl);
            }

            $body = $this->awaitStatus($orderUrl, ['ready', 'valid'], 'order');
            if (($body['status'] ?? '') === 'ready') {
                $csr = $this->csr($primary, $domains);
                $this->post((string) $body['finalize'], ['csr' => Jws::b64($csr)]);
                $body = $this->awaitStatus($orderUrl, ['valid'], 'order');
            }

            $certUrl = (string) ($body['certificate'] ?? '');
            if ($certUrl === '') {
                throw new TransportError('the CA marked the order valid without a certificate URL');
            }
            $chain = $this->postAsGet($certUrl)->body;
            if (!str_contains($chain, '-----BEGIN CERTIFICATE-----')) {
                throw new TransportError('the CA returned something that is not a certificate chain');
            }

            $this->state->installCertificateChain($primary, $chain);
            $this->say("certificate installed for $primary");
            return $chain;
        } finally {
            /* Always, including on failure: a token left published is an
             * answer the next order would serve for a challenge that is no
             * longer live. */
            $this->clearPublished();
        }
    }

    private function authorize(string $authzUrl): void
    {
        $authz = $this->postAsGet($authzUrl)->json();
        $identifier = (string) ($authz['identifier']['value'] ?? '?');
        if (($authz['status'] ?? '') === 'valid') {
            /* A CA may reuse a recent authorization; nothing to prove. */
            $this->say("authorization for $identifier is already valid");
            return;
        }

        $challenge = null;
        foreach ((array) ($authz['challenges'] ?? []) as $candidate) {
            if (($candidate['type'] ?? '') === 'http-01') {
                $challenge = $candidate;
                break;
            }
        }
        if ($challenge === null) {
            throw new TransportError(
                "the CA offered no http-01 challenge for $identifier; "
                . 'this client implements no other type'
            );
        }

        $token = (string) ($challenge['token'] ?? '');
        if ($token === '') {
            throw new TransportError("the CA sent an http-01 challenge for $identifier with no token");
        }
        $this->publish($token);

        /* An empty object, not a POST-as-GET: RFC 8555 section 7.5.1 makes
         * "{}" the signal that the client is ready to be validated. */
        $this->post((string) $challenge['url'], []);
        $this->awaitStatus($authzUrl, ['valid'], "authorization for $identifier");
        $this->say("authorization for $identifier is valid");
    }

    private function publish(string $token): void
    {
        $keyauth = $this->jws->keyAuthorization($token);
        if (!fpmng_acme_challenge_set($token, $keyauth)) {
            throw new StateError(
                'could not publish the challenge answer; the error log says why. '
                . 'A pool that may publish is pool.type = cron or supervisor'
            );
        }
        $this->published[$token] = true;
    }

    private function clearPublished(): void
    {
        foreach (array_keys($this->published) as $token) {
            fpmng_acme_challenge_clear((string) $token);
        }
        $this->published = [];
    }

    /**
     * Poll a resource until it reaches one of $wanted.
     *
     * The wait is bounded and the interval grows: a CA under load answers
     * "pending" for a while, and a client that polls hard is a client that
     * gets rate limited. "invalid" stops immediately -- it will not become
     * valid, and the CA's own error explains why better than a timeout would.
     */
    private function awaitStatus(string $url, array $wanted, string $what, int $seconds = 120): array
    {
        $deadline = microtime(true) + $seconds;
        $interval = 0.5;
        $status = '?';
        while (microtime(true) < $deadline) {
            $body = $this->postAsGet($url)->json();
            $status = (string) ($body['status'] ?? '');
            if (in_array($status, $wanted, true)) {
                return $body;
            }
            if ($status === 'invalid') {
                throw new TransportError(sprintf(
                    '%s is invalid: %s',
                    $what,
                    self::describeFailure($body)
                ));
            }
            usleep((int) ($interval * 1_000_000));
            $interval = min($interval * 1.5, 8.0);
        }
        throw new TransportError("$what did not reach " . implode('/', $wanted)
            . " within {$seconds}s (last status: $status)");
    }

    /** The CA's own reason, dug out of wherever this resource keeps it. */
    private static function describeFailure(array $body): string
    {
        $problem = $body['error'] ?? null;
        foreach ((array) ($body['challenges'] ?? []) as $challenge) {
            $problem = $challenge['error'] ?? $problem;
        }
        if (!is_array($problem)) {
            return 'the CA gave no reason';
        }
        return trim(sprintf(
            '%s (%s)',
            (string) ($problem['detail'] ?? 'no detail'),
            (string) ($problem['type'] ?? 'no type')
        ));
    }

    private function ensureAccount(): void
    {
        $record = $this->state->loadOrRegisterAccount(function (): array {
            $response = $this->post($this->directory()['newAccount'], [
                'termsOfServiceAgreed' => true,
            ], jwk: true);
            $url = $response->header('location');
            if ($url === null) {
                throw new TransportError('the CA registered an account without a Location header');
            }
            $this->say('registered a new ACME account');
            return ['url' => $url];
        });
        $this->accountUrl = (string) $record['url'];
    }

    private function directory(): array
    {
        if ($this->directory === null) {
            $body = HttpClient::ensureOk($this->http->get($this->directoryUrl), 'fetching the directory')->json();
            foreach (['newNonce', 'newAccount', 'newOrder'] as $required) {
                if (!isset($body[$required])) {
                    throw new TransportError(
                        "the ACME directory at {$this->directoryUrl} has no $required; "
                        . 'is that URL a directory?'
                    );
                }
            }
            $this->directory = $body;
        }
        return $this->directory;
    }

    private function nonce(): string
    {
        $nonce = $this->http->takeNonce();
        if ($nonce !== null) {
            return $nonce;
        }
        $this->http->head($this->directory()['newNonce']);
        $nonce = $this->http->takeNonce();
        if ($nonce === null) {
            throw new TransportError('the CA would not hand out a nonce');
        }
        return $nonce;
    }

    private function postAsGet(string $url): HttpResponse
    {
        return $this->post($url, null);
    }

    /**
     * A signed POST, retried once on badNonce.
     *
     * badNonce is not a failure: RFC 8555 section 6.5 says a client that gets
     * one should retry with the fresh nonce the response carries, and it
     * happens routinely when a nonce has been sitting unused. Retrying
     * anything else would risk repeating a side effect -- a second order.
     */
    private function post(string $url, ?array $payload, bool $jwk = false): HttpResponse
    {
        $attempt = 0;
        while (true) {
            $jws = $this->jws->sign($url, $this->nonce(), $payload, $jwk ? null : $this->accountUrl);
            $response = $this->http->postJose($url, $jws);
            try {
                return HttpClient::ensureOk($response, "POST $url");
            } catch (AcmeServerError $e) {
                if ($e->type === 'urn:ietf:params:acme:error:badNonce' && $attempt < 2) {
                    $attempt++;
                    continue;
                }
                throw $e;
            }
        }
    }

    /**
     * A CSR for $domains, signed with the certificate key.
     *
     * Every name goes in subjectAltName, including the one in the subject:
     * RFC 2818 stopped reading the CN years ago, and a CA that honours the CN
     * anyway would produce a certificate no modern client accepts.
     */
    private function csr(string $primary, array $domains): string
    {
        $key = $this->state->loadOrCreateCertKey($primary);
        $sans = implode(',', array_map(static fn(string $d): string => "DNS:$d", $domains));

        /* openssl_csr_new() can only pass extensions through a config file,
         * so there is one, written next to nothing else and removed straight
         * away. It carries no key material. */
        $conf = tempnam(sys_get_temp_dir(), 'fpmng-csr-');
        if ($conf === false) {
            throw new StateError('cannot create a temporary file for the CSR configuration');
        }
        file_put_contents($conf, "[req]\ndistinguished_name=dn\nreq_extensions=san\n"
            . "[dn]\n[san]\nsubjectAltName=$sans\n");

        try {
            $csr = openssl_csr_new(
                ['commonName' => substr($primary, 0, 64)],
                $key,
                ['digest_alg' => 'sha256', 'config' => $conf, 'req_extensions' => 'san']
            );
            if ($csr === false) {
                throw new StateError('could not build a CSR: ' . self::opensslErrors());
            }
            $der = '';
            if (!openssl_csr_export($csr, $der, false)) {
                throw new StateError('could not export the CSR: ' . self::opensslErrors());
            }
            /* ACME wants the DER, base64url-encoded; openssl_csr_export gives
             * PEM even with $notext = false, so unwrap it. */
            if (!preg_match('#-----BEGIN CERTIFICATE REQUEST-----(.+)-----END CERTIFICATE REQUEST-----#s', $der, $m)) {
                throw new StateError('the CSR came back in an unexpected format');
            }
            $decoded = base64_decode(preg_replace('/\s+/', '', $m[1]) ?? '', true);
            if ($decoded === false) {
                throw new StateError('the CSR was not valid base64');
            }
            return $decoded;
        } finally {
            @unlink($conf);
        }
    }

    /** Drain OpenSSL's error queue into one line; it never holds key bytes. */
    private static function opensslErrors(): string
    {
        $errors = [];
        while (($error = openssl_error_string()) !== false) {
            $errors[] = $error;
        }
        return $errors === [] ? 'no detail from OpenSSL' : implode('; ', $errors);
    }

    private function say(string $message): void
    {
        ($this->log)($message);
    }
}

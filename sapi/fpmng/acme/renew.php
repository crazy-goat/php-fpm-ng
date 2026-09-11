<?php
/**
 * The renewal policy, and the script a cron pool actually runs (issue #49).
 *
 * client.php knows RFC 8555 and nothing else; this file decides *whether* to
 * run an order at all, what to do when one fails, and what an operator sees.
 * Keeping the two apart is what makes criterion 3 testable without a CA: the
 * policy can be asked "would you renew this certificate?" with no network.
 *
 * Configuration is plain pool directives, because a cron pool already has
 * env[] and adding a C directive namespace for something only this script
 * reads would be a second place to keep in sync:
 *
 *   env[ACME_STATE_DIR]   = /var/lib/fpmng/acme   (required)
 *   env[ACME_DOMAINS]     = example.com,www.example.com  (required)
 *   env[ACME_DIRECTORY]   = an ACME directory URL (default: staging)
 *   env[ACME_ALLOW_PRODUCTION] = 1, required before a non-staging directory
 *                           URL is accepted (criterion 1)
 *   env[ACME_RENEW_DAYS]  = renew with fewer than this many days left (30)
 *   env[ACME_CA_BUNDLE]   = verify the CA's TLS certificate against this file
 *                           instead of the system store (test harnesses only)
 */

namespace FpmNg\Acme;

require_once __DIR__ . '/client.php';
require_once __DIR__ . '/lock.php';
require_once __DIR__ . '/state.php';

final class Renewer
{
    /**
     * Let's Encrypt's staging directory. The default, deliberately: a
     * misconfigured test against production can rate-limit a real domain for
     * a week, and the failure mode of accidentally using staging is a
     * certificate nobody trusts -- loud, local and reversible.
     */
    public const STAGING_DIRECTORY = 'https://acme-staging-v02.api.letsencrypt.org/directory';

    /** Renew with fewer than this many days of validity left. */
    public const DEFAULT_RENEW_DAYS = 30;

    /**
     * Backoff after a failed order, in seconds, by consecutive failure count.
     * It tops out well below a day so that a transient outage does not turn
     * into a certificate that expires while the renewer sleeps, and starts
     * high enough that a CA rejecting us is not asked again immediately.
     */
    private const BACKOFF = [0, 900, 1800, 3600, 7200, 14400, 21600];

    public function __construct(
        private readonly State $state,
        private readonly string $directoryUrl,
        private readonly array $domains,
        private readonly int $renewDays,
        private readonly ?string $caBundle,
        private readonly \Closure $log,
    ) {
    }

    /**
     * Build a renewer from the pool's environment, refusing anything
     * ambiguous. Every message names the directive to change.
     */
    public static function fromEnvironment(\Closure $log): self
    {
        $state = State::fromEnvironment();

        $domains = array_values(array_filter(array_map(
            'trim',
            explode(',', (string) getenv('ACME_DOMAINS'))
        ), 'strlen'));
        if ($domains === []) {
            throw new StateError('env[ACME_DOMAINS] is empty; there is nothing to get a certificate for');
        }

        $directory = trim((string) getenv('ACME_DIRECTORY'));
        if ($directory === '') {
            $directory = self::STAGING_DIRECTORY;
        }
        self::assertDirectoryAllowed($directory);

        $days = trim((string) getenv('ACME_RENEW_DAYS'));
        $renewDays = $days === '' ? self::DEFAULT_RENEW_DAYS : (int) $days;
        if ($renewDays < 1) {
            throw new StateError('env[ACME_RENEW_DAYS] must be at least 1');
        }

        $bundle = trim((string) getenv('ACME_CA_BUNDLE'));

        return new self(
            $state,
            $directory,
            $domains,
            $renewDays,
            $bundle === '' ? null : $bundle,
            $log
        );
    }

    /**
     * Criterion 1. Staging is the default and anything else is an explicit
     * decision, so that no configuration mistake and no test loop can spend
     * a production rate limit. The opt-in is checked against the URL rather
     * than against a "production" flag, because the hazard is the endpoint,
     * not the name somebody gave it.
     */
    public static function assertDirectoryAllowed(string $directory): void
    {
        if (self::isNonProduction($directory)) {
            return;
        }
        if (enabled(getenv('ACME_ALLOW_PRODUCTION'))) {
            return;
        }
        throw new StateError(
            "env[ACME_DIRECTORY] points at '$directory', which is not a staging or local "
            . 'directory. Issuing against a production CA needs env[ACME_ALLOW_PRODUCTION] = 1. '
            . 'Until then this pool will not place an order: a mistaken test loop against '
            . 'production can rate-limit the domain for a week'
        );
    }

    /**
     * Staging and local test servers, recognised without a list of CAs to
     * maintain: a host that is loopback or carries "staging"/"test" in its
     * name cannot be a production CA that rate limits a real domain.
     */
    private static function isNonProduction(string $directory): bool
    {
        $host = strtolower((string) parse_url($directory, PHP_URL_HOST));
        if ($host === '') {
            throw new StateError("env[ACME_DIRECTORY] is not a URL: '$directory'");
        }
        if ($host === 'localhost' || $host === '127.0.0.1' || $host === '::1' || str_ends_with($host, '.localhost')) {
            return true;
        }
        return str_contains($host, 'staging') || str_contains($host, 'test') || str_ends_with($host, '.internal');
    }

    /**
     * One scheduler tick. Returns a short word for the cron log; every
     * decision is also logged with its reason, because "the cron pool ran and
     * did nothing" must be distinguishable from "the cron pool is broken".
     */
    public function tick(): string
    {
        Client::preflight();
        $this->state->assertUsable();

        $primary = State::canonicalDomain($this->domains[0]);
        $meta = $this->state->readRenewalMeta($primary);

        $due = $this->dueReason($primary, $meta);
        if ($due === null) {
            return 'up-to-date';
        }

        $wait = $this->backoffRemaining($meta);
        if ($wait > 0) {
            $this->say(sprintf(
                'renewal for %s is due (%s) but the last %d attempt(s) failed; '
                . 'waiting %ds more before retrying. Last error: %s',
                $primary,
                $due,
                (int) ($meta['failures'] ?? 0),
                $wait,
                (string) ($meta['last_error'] ?? 'none recorded')
            ));
            return 'backoff';
        }

        try {
            return (new RenewalLock($this->state, $primary))->run(function () use ($primary, $due, $meta): string {
                $this->say("renewing $primary: $due");
                $client = new Client(
                    $this->directoryUrl,
                    $this->state,
                    new HttpClient('php-fpm-ng ACME client', $this->caBundle),
                    $this->log
                );
                $client->order($this->domains);
                $this->state->writeRenewalMeta($primary, [
                    'last_success' => gmdate('c'),
                    'directory' => $this->directoryUrl,
                    'domains' => $this->domains,
                    'failures' => 0,
                ]);
                return 'renewed';
            });
        } catch (RenewalInProgress $e) {
            /* Not an error: another process holds the certificate. */
            $this->say($e->getMessage());
            return 'held-elsewhere';
        } catch (\Throwable $e) {
            $this->recordFailure($primary, $meta, $e);
            return 'failed';
        }
    }

    /**
     * Criterion 3: why this certificate needs renewing, or null if it does
     * not. The trigger is remaining lifetime, never a calendar -- a fixed
     * schedule renews a 90-day certificate on the wrong day the moment a CA
     * changes its validity period, and reissues a perfectly good certificate
     * on every boot.
     */
    public function dueReason(string $domain, array $meta = []): ?string
    {
        $path = $this->state->certChainPath($domain);
        if (!is_file($path)) {
            return 'no certificate is installed';
        }
        $notAfter = self::notAfter($path);
        if ($notAfter === null) {
            return 'the installed certificate cannot be parsed';
        }
        if (!self::covers($path, $this->domains)) {
            return 'the installed certificate does not cover every configured name';
        }
        $left = $notAfter - time();
        if ($left <= $this->renewDays * 86400) {
            return sprintf('%.1f days of validity left, threshold is %d', $left / 86400, $this->renewDays);
        }
        return null;
    }

    /** Unix time the chain's leaf expires, or null if it cannot be read. */
    public static function notAfter(string $chainPath): ?int
    {
        $pem = @file_get_contents($chainPath);
        if ($pem === false) {
            return null;
        }
        $parsed = @openssl_x509_parse($pem);
        if (!is_array($parsed) || !isset($parsed['validTo_time_t'])) {
            return null;
        }
        return (int) $parsed['validTo_time_t'];
    }

    /** Does the leaf certificate carry every configured name as a SAN? */
    public static function covers(string $chainPath, array $domains): bool
    {
        $pem = @file_get_contents($chainPath);
        $parsed = $pem === false ? false : @openssl_x509_parse($pem);
        if (!is_array($parsed)) {
            return false;
        }
        $sans = [];
        foreach (explode(',', (string) ($parsed['extensions']['subjectAltName'] ?? '')) as $entry) {
            $entry = trim($entry);
            if (str_starts_with($entry, 'DNS:')) {
                $sans[] = strtolower(substr($entry, 4));
            }
        }
        foreach ($domains as $domain) {
            if (!in_array(State::canonicalDomain($domain), $sans, true)) {
                return false;
            }
        }
        return true;
    }

    /**
     * Criterion 6: what an operator sees before a certificate expires. The
     * message escalates on days left, not on failure count, because the thing
     * that hurts is the expiry, not the number of attempts.
     */
    public function expiryWarning(string $domain): ?string
    {
        $path = $this->state->certChainPath($domain);
        $notAfter = is_file($path) ? self::notAfter($path) : null;
        if ($notAfter === null) {
            return null;
        }
        $days = ($notAfter - time()) / 86400;
        if ($days > $this->renewDays) {
            return null;
        }
        return sprintf(
            '%s: the certificate expires in %.1f days (%s) and renewal has not succeeded since then',
            $domain,
            $days,
            gmdate('c', $notAfter)
        );
    }

    private function recordFailure(string $domain, array $meta, \Throwable $e): void
    {
        $failures = ((int) ($meta['failures'] ?? 0)) + 1;
        $wait = self::BACKOFF[min($failures, count(self::BACKOFF) - 1)];

        /* Criterion 5: loud, specific, and it changes nothing on disk. The
         * installed certificate is untouched, so the listener keeps serving
         * whatever it was serving. */
        $this->say(sprintf(
            'ERROR renewing %s (attempt %d): %s. The existing certificate keeps serving; '
            . 'the next attempt is in %ds',
            $domain,
            $failures,
            $e->getMessage(),
            $wait
        ));
        $warning = $this->expiryWarning($domain);
        if ($warning !== null) {
            $this->say('WARNING ' . $warning);
        }

        $this->state->writeRenewalMeta($domain, [
            'last_success' => $meta['last_success'] ?? null,
            'directory' => $this->directoryUrl,
            'domains' => $this->domains,
            'failures' => $failures,
            'last_error' => $e->getMessage(),
            'last_failure' => gmdate('c'),
            'next_attempt' => gmdate('c', time() + $wait),
        ]);
    }

    private function backoffRemaining(array $meta): int
    {
        $next = (string) ($meta['next_attempt'] ?? '');
        if ($next === '' || (int) ($meta['failures'] ?? 0) === 0) {
            return 0;
        }
        $at = strtotime($next);
        return $at === false ? 0 : max(0, $at - time());
    }

    private function say(string $message): void
    {
        ($this->log)($message);
    }
}

/*
 * Running as a script: one tick, then exit. A cron pool runs this file per
 * schedule and treats a non-zero exit as a failed run, so the exit code says
 * whether the operator should look, and stdout is the cron log.
 *
 * The guard compares the first entry of get_included_files() -- the entry
 * script, whichever SAPI ran it -- with this file, rather than $_SERVER's
 * SCRIPT_FILENAME, which a script pool does not set. Tests that include this
 * file to reach the classes therefore do not place an order.
 */
$included = get_included_files();
if ($included !== [] && realpath($included[0]) === realpath(__FILE__)) {
    /* error_log(), not echo: a pool's stdout only reaches the FPM error log
     * when catch_workers_output is on, and a renewer whose account of what
     * it did depends on an unrelated directive being set is a renewer that
     * is silent by default -- which is the failure mode criterion 5 exists
     * to prevent. error_log() goes through the SAPI, so the message lands in
     * error_log wherever this pool runs. */
    $log = static function (string $message): void {
        error_log('acme: ' . $message);
    };
    try {
        $outcome = Renewer::fromEnvironment($log)->tick();
        $log("tick finished: $outcome");
        exit($outcome === 'failed' ? 1 : 0);
    } catch (\Throwable $e) {
        $log('ERROR ' . $e->getMessage());
        exit(1);
    }
}

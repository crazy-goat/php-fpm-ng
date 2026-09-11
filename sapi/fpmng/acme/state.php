<?php
/**
 * ACME state on a writable volume (task 044, docs/NOTES.md 3y).
 *
 * The project-owned ACME client (task 043: PHP, run by a dedicated
 * pool.type = cron process) is the only writer of everything this file
 * manages. Gateway processes only ever read the certificate and its key,
 * through the existing http.tls_cert / http.tls_key directives.
 *
 * Layout under the base directory (named by env[ACME_STATE_DIR] on the
 * cron pool -- a plain FPM pool directive, no new C code needed for this):
 *
 *   $base/account.key        ACME account private key, created once, 0600
 *   $base/account.json       {"url": "..."} from registration, 0600
 *   $base/<domain>/privkey.pem    certificate private key, 0600
 *   $base/<domain>/fullchain.pem  leaf + intermediate, 0644 (public)
 *   $base/<domain>/renewal.json   renewal metadata, 0600
 *   $base/<domain>/renewal.lock   the single-renewer lock, 0600 (lock.php)
 *
 * One base directory, one subdirectory per domain -- a pool serving several
 * certificates over SNI (task 041) needs several subdirectories, not several
 * base directories.
 *
 * Ownership: the cron pool running this client and every http pool serving
 * a certificate it manages must share the same 'user'/'group' -- the private
 * key is 0600, not group-readable, so a gateway running as a different uid
 * could not read it after task 010's privilege drop.
 */

namespace FpmNg\Acme;

final class StateError extends \RuntimeException
{
}

final class State
{
    public readonly string $baseDir;

    public function __construct(string $baseDir)
    {
        $baseDir = rtrim($baseDir, '/');
        if ($baseDir === '') {
            throw new StateError('ACME state directory must not be empty');
        }
        $this->baseDir = $baseDir;
    }

    public static function fromEnvironment(string $var = 'ACME_STATE_DIR'): self
    {
        $dir = getenv($var);
        if ($dir === false || $dir === '') {
            throw new StateError(
                "environment variable '$var' is not set -- the ACME cron pool "
                . "needs env[$var] naming the state directory (docs/NOTES.md 3y)"
            );
        }
        return new self($dir);
    }

    /**
     * Fail loudly and specifically before touching any key material --
     * task 044 acceptance criteria 4 and 5. Call once, at process start.
     */
    public function assertUsable(): void
    {
        if (!is_dir($this->baseDir)) {
            throw new StateError("ACME state directory '{$this->baseDir}' does not exist");
        }

        $probe = $this->baseDir . '/.acme-write-test-' . getmypid();
        $written = @file_put_contents($probe, '');
        if ($written === false) {
            throw new StateError(
                "ACME state directory '{$this->baseDir}' is not writable "
                . '(probe file could not be created -- read-only volume or wrong owner?)'
            );
        }
        @unlink($probe);
    }

    public function accountKeyPath(): string
    {
        return $this->baseDir . '/account.key';
    }

    public function accountMetaPath(): string
    {
        return $this->baseDir . '/account.json';
    }

    public function domainDir(string $domain): string
    {
        return $this->baseDir . '/' . self::canonicalDomain($domain);
    }

    /**
     * The directory name a certificate is filed under, and with it the name
     * of its renewal lock (issue #47) -- so two spellings of one certificate
     * must not produce two names. DNS is case-insensitive and the trailing
     * root dot is not part of the name, so 'Example.com', 'example.com.' and
     * 'example.com' are one certificate to the CA and must be one directory
     * here; otherwise two pools configured with different spellings would
     * take two different locks and both run an order for the same name.
     *
     * Everything else is rejected rather than escaped. The domain comes from
     * operator configuration, not from a request, so this is not injection
     * defence -- it is refusing to turn a typo into a path: '../foo' would
     * otherwise have ensureDomainDir() create a directory outside the state
     * root, and an empty name would make the state root its own domain
     * directory.
     */
    public static function canonicalDomain(string $domain): string
    {
        $name = strtolower(rtrim($domain, '.'));
        $labels = $name === '' ? [] : explode('.', $name);
        if ($labels === []) {
            throw new StateError('an empty string is not a domain name');
        }
        foreach ($labels as $i => $label) {
            /* A wildcard certificate is named '*.example.com' by the CA, so
             * that is the one label that may hold a '*', and only first. */
            if ($i === 0 && $label === '*' && count($labels) > 1) {
                continue;
            }
            if (!preg_match('/\\A[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?\\z/', $label)) {
                throw new StateError("'$domain' is not a usable domain name");
            }
        }
        return implode('.', $labels);
    }

    public function certKeyPath(string $domain): string
    {
        return $this->domainDir($domain) . '/privkey.pem';
    }

    public function certChainPath(string $domain): string
    {
        return $this->domainDir($domain) . '/fullchain.pem';
    }

    public function renewalMetaPath(string $domain): string
    {
        return $this->domainDir($domain) . '/renewal.json';
    }

    /**
     * The advisory lock that makes one process the only renewer of this
     * certificate (issue #47; see lock.php for why it lives here rather
     * than in the master's shared memory).
     */
    public function renewalLockPath(string $domain): string
    {
        return $this->domainDir($domain) . '/renewal.lock';
    }

    /** Load the account key, generating it once if absent. */
    public function loadOrCreateAccountKey(): \OpenSSLAsymmetricKey
    {
        return $this->loadOrCreateKey($this->accountKeyPath());
    }

    /** Load a certificate's private key, generating it once if absent. */
    public function loadOrCreateCertKey(string $domain): \OpenSSLAsymmetricKey
    {
        $this->ensureDomainDir($domain);
        return $this->loadOrCreateKey($this->certKeyPath($domain));
    }

    /**
     * Load the persisted account record, or register once and persist it.
     * $register is called AT MOST ONCE per process, and only when
     * account.json does not exist yet -- task 044 acceptance criterion 6:
     * a restart must reuse the existing account, never register a new one.
     *
     * @param callable(): array{url: string} $register
     * @return array{url: string}
     */
    public function loadOrRegisterAccount(callable $register): array
    {
        $path = $this->accountMetaPath();
        if (is_file($path)) {
            $meta = json_decode((string) file_get_contents($path), true);
            if (!is_array($meta) || !isset($meta['url']) || !is_string($meta['url'])) {
                throw new StateError("'$path' exists but does not contain a usable account record");
            }
            return $meta;
        }

        $meta = $register();
        if (!is_array($meta) || !isset($meta['url']) || !is_string($meta['url'])) {
            throw new StateError('account registration did not return a usable record (missing "url")');
        }
        $this->writeRestricted($path, json_encode($meta, JSON_PRETTY_PRINT | JSON_UNESCAPED_SLASHES) . "\n");
        return $meta;
    }

    /** Install a freshly issued certificate chain -- public, 0644 (readers other than the owner never need the key). */
    public function installCertificateChain(string $domain, string $fullchainPem): void
    {
        $this->ensureDomainDir($domain);
        $this->writePublic($this->certChainPath($domain), $fullchainPem);
    }

    /**
     * Public because RenewalLock has to create the directory before there is
     * any certificate in it: on a fresh bootstrap the first thing that
     * happens under $base/<domain>/ is taking the renewal lock.
     */
    public function ensureDomainDir(string $domain): void
    {
        $dir = $this->domainDir($domain);
        if (is_dir($dir)) {
            return;
        }
        if (!@mkdir($dir, 0700, true) && !is_dir($dir)) {
            throw new StateError("cannot create certificate directory '$dir'");
        }
    }

    private function loadOrCreateKey(string $path): \OpenSSLAsymmetricKey
    {
        if (is_file($path)) {
            $pem = file_get_contents($path);
            $key = $pem === false ? false : openssl_pkey_get_private($pem);
            if ($key === false) {
                // Never include $pem in the message -- task 044 acceptance criterion 3.
                throw new StateError("'$path' does not contain a usable private key");
            }
            return $key;
        }

        $key = openssl_pkey_new([
            'private_key_type' => OPENSSL_KEYTYPE_EC,
            'curve_name' => 'prime256v1',
        ]);
        if ($key === false) {
            throw new StateError("cannot generate a private key for '$path'");
        }
        if (!openssl_pkey_export($key, $pem)) {
            throw new StateError("cannot export the generated private key for '$path'");
        }
        $this->writeRestricted($path, $pem);
        return $key;
    }

    /** 0600: owner read/write only -- task 044 acceptance criterion 2. */
    private function writeRestricted(string $path, string $contents): void
    {
        $this->writeAtomically($path, $contents, 0600);
    }

    /** 0644: the certificate chain is public by nature. */
    private function writePublic(string $path, string $contents): void
    {
        $this->writeAtomically($path, $contents, 0644);
    }

    /**
     * Write via a temporary file in the same directory, chmod before the
     * final name exists, then rename -- so a concurrent reader (a gateway
     * process reloading http.tls_cert/http.tls_key) never observes a
     * partially written file, and a private file is never briefly
     * world-readable under the final name.
     */
    private function writeAtomically(string $path, string $contents, int $mode): void
    {
        $tmp = $path . '.tmp-' . getmypid() . '-' . bin2hex(random_bytes(4));
        $fd = @fopen($tmp, 'x');
        if ($fd === false) {
            throw new StateError("cannot create '$tmp'");
        }
        fwrite($fd, $contents);
        fclose($fd);
        chmod($tmp, $mode);
        if (!rename($tmp, $path)) {
            @unlink($tmp);
            throw new StateError("cannot install '$path'");
        }
    }
}

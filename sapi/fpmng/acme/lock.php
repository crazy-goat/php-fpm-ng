<?php
/**
 * "Exactly one process renews" (issue #47).
 *
 * Task 043 put the ACME client in a dedicated pool.type = cron pool, and
 * fpm_pool_cron.c pins such a pool to one process, so a single cron pool
 * cannot overlap with itself. That is not the whole problem:
 *
 *   - a configuration may declare more than one cron pool, or a supervisor
 *     pool, pointing at the same ACME state directory;
 *   - a reload or a master restart may start a new renewer while the old
 *     process is still finishing an order;
 *   - the same state directory may be mounted into more than one pool of the
 *     same master.
 *
 * The property that must hold is therefore not "one process" but "one
 * renewer per certificate", and the certificate is identified by its
 * directory under the ACME state root (see state.php). So the exclusion
 * lives there, as an advisory whole-file lock on
 * $base/<domain>/renewal.lock, taken for the whole order.
 *
 * Why flock() and not the shared-memory generation store used for challenge
 * answers (fpm_acme_challenge.h):
 *
 *   - Scope. The store is one master's shared memory; the lock has to cover
 *     everything writing to one state directory, which is a property of the
 *     volume, not of a process tree.
 *   - Duration. An order takes as long as a CA takes to validate -- seconds
 *     to minutes. A bounded spinlock is exactly the wrong shape for that.
 *   - Recovery. This is the reason that matters. The kernel drops a flock()
 *     when the holding process dies, however it dies, so a renewer killed
 *     mid-order needs no operator action and no stale-lock timeout that
 *     would have to guess how long a legitimate order may take. Recovery
 *     time is one scheduler tick of the ACME cron pool -- issue #47
 *     acceptance criterion 2 -- and nothing has to detect the death.
 *
 * The record written into the lock file (pid, start time, host) is
 * diagnostics only. It is never consulted to decide whether the lock is
 * free: only flock() decides that, precisely so that a record left behind by
 * a killed process cannot block the next renewal.
 *
 * Caveat, stated rather than worked around: flock() is advisory and local.
 * Two containers sharing one state directory over NFS are not covered by
 * this and are not a supported deployment (docs/acme-renewal.md).
 */

namespace FpmNg\Acme;

/**
 * Another process is renewing this certificate right now. Not an error: the
 * caller's tick has nothing to do, and the holder will finish or die.
 */
final class RenewalInProgress extends \RuntimeException
{
}

final class RenewalLock
{
    public function __construct(
        private readonly State $state,
        private readonly string $domain,
    ) {
    }

    public function path(): string
    {
        return $this->state->renewalLockPath($this->domain);
    }

    /**
     * Run $fn as the only renewer of this certificate, and return whatever
     * it returns.
     *
     * Throws RenewalInProgress, without calling $fn, when another process
     * holds the lock. Any other failure -- the state directory is not
     * writable, the lock file cannot be created -- is a StateError, because
     * it means this renewer cannot work at all rather than "not now".
     *
     * @template T
     * @param callable(): T $fn
     * @return T
     */
    public function run(callable $fn): mixed
    {
        $path = $this->path();
        $this->state->ensureDomainDir($this->domain);

        // 'c' creates the file if it is absent and does NOT truncate it: the
        // previous holder's record stays readable until this one replaces
        // it, and a reader that loses the race to flock() below still has
        // something to report.
        $fd = @fopen($path, 'c');
        if ($fd === false) {
            throw new StateError("cannot open the renewal lock '$path'");
        }
        if (!flock($fd, LOCK_EX | LOCK_NB)) {
            $holder = $this->describe((string) @file_get_contents($path));
            fclose($fd);
            throw new RenewalInProgress(
                "another process is renewing '{$this->domain}' ($holder); "
                . 'this tick has nothing to do'
            );
        }

        // The lock file names no secret, but it lives beside the account key
        // and the certificate keys, so it gets the same restrictive mode.
        @chmod($path, 0600);
        ftruncate($fd, 0);
        rewind($fd);
        fwrite($fd, json_encode([
            'pid' => getmypid(),
            'host' => php_uname('n'),
            'started' => gmdate('c'),
        ], JSON_UNESCAPED_SLASHES) . "\n");
        fflush($fd);

        try {
            return $fn();
        } finally {
            // Clear the record before unlocking so that a process which
            // acquires the lock next cannot read a holder that has gone.
            // The lock itself would be released by close(), and by process
            // death if this code never runs at all -- that is the point.
            ftruncate($fd, 0);
            fflush($fd);
            flock($fd, LOCK_UN);
            fclose($fd);
        }
    }

    /**
     * The record of the process currently holding the lock, or null when it
     * is free. Diagnostics for an operator asking "why did this tick do
     * nothing"; never used to decide whether the lock may be taken.
     *
     * Call it from a process that is not itself renewing. A lock is owned by
     * an open file description, so on a platform where flock() is emulated
     * with POSIX record locks -- per process rather than per description --
     * probing from inside run()'s callback would release the order's own
     * lock. That platform is already out of scope (see the caveat above),
     * but the rule costs nothing to follow.
     *
     * @return array{pid?: int, host?: string, started?: string}|null
     */
    public function holder(): ?array
    {
        $path = $this->path();
        $fd = @fopen($path, 'r');
        if ($fd === false) {
            return null;
        }
        // A shared lock, deliberately, not the exclusive one run() takes. It
        // still conflicts with a holder's LOCK_EX, so it answers the question
        // -- but two diagnostic calls do not exclude each other, and more
        // importantly a diagnostic call cannot make a renewer ticking at the
        // same moment see the certificate as busy and skip it. Asking must
        // never change the answer.
        if (flock($fd, LOCK_SH | LOCK_NB)) {
            flock($fd, LOCK_UN);
            fclose($fd);
            return null;
        }
        fclose($fd);
        $record = json_decode((string) @file_get_contents($path), true);
        return is_array($record) ? $record : [];
    }

    private function describe(string $record): string
    {
        $decoded = json_decode($record, true);
        if (!is_array($decoded) || !isset($decoded['pid'])) {
            return 'holder unknown';
        }
        return sprintf(
            'pid %s on %s since %s',
            $decoded['pid'],
            $decoded['host'] ?? '?',
            $decoded['started'] ?? '?'
        );
    }
}

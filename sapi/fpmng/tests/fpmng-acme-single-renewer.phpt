--TEST--
ACME: exactly one process renews a certificate, and a killed renewer recovers without operator action (issue #47)
--SKIPIF--
<?php
if (!function_exists('proc_open')) {
    die('skip requires proc_open to run concurrent renewers');
}
$probe = tempnam(sys_get_temp_dir(), 'flock');
$fd = fopen($probe, 'c');
if ($fd === false || !flock($fd, LOCK_EX | LOCK_NB)) {
    die('skip this filesystem does not support flock()');
}
flock($fd, LOCK_UN);
fclose($fd);
@unlink($probe);
?>
--FILE--
<?php

// Issue #47: the renewer's exclusion must hold per certificate, not per
// pool -- a configuration may point two cron pools at one ACME state
// directory, and a reload may start a new renewer while the old one is
// still finishing an order. The property is therefore tested the only way
// it can be observed: real concurrent processes competing for one state
// directory, counting the orders that actually started.
//
// An "order" here is a stand-in that announces itself and then blocks until
// told to finish. Real orders against a CA are issue #49; what this test
// has to pin down -- that a second process cannot start one -- is a
// property of the lock, and a stand-in makes it deterministic instead of
// dependent on how long a CA takes to answer.

$acme = realpath(__DIR__ . '/../acme');
$root = sys_get_temp_dir() . '/' . basename(__FILE__, '.php') . '-' . getmypid();
@mkdir($root, 0700, true);

$orders = "$root/orders.log";
$worker = "$root/worker.php";

file_put_contents($worker, <<<'PHP'
<?php
// argv: 1 = acme source dir, 2 = state root, 3 = domain, 4 = orders log,
//       5 = ready marker (written once the lock is held), 6 = release file
//       (the "order" runs until this appears), 7 = seconds to wait for it
require $argv[1] . '/state.php';
require $argv[1] . '/lock.php';

use FpmNg\Acme\RenewalInProgress;
use FpmNg\Acme\RenewalLock;
use FpmNg\Acme\State;

$lock = new RenewalLock(new State($argv[2]), $argv[3]);
try {
    $lock->run(function () use ($argv) {
        file_put_contents($argv[4], $argv[3] . ' ' . getmypid() . "\n", FILE_APPEND | LOCK_EX);
        file_put_contents($argv[5], (string) getmypid());
        $deadline = microtime(true) + (float) $argv[7];
        while (!file_exists($argv[6]) && microtime(true) < $deadline) {
            usleep(2000);
        }
    });
    echo "ran\n";
} catch (RenewalInProgress $e) {
    echo "busy\n";
}
PHP);

/* Started through proc_open()'s array form: no shell in the way, so a path
 * with a space in it cannot turn into two arguments (issue #101). */
function start(string $domain, string $ready, string $release, float $wait = 20.0): array
{
    global $acme, $root, $orders, $worker;

    $pipes = [];
    $handle = proc_open(
        [PHP_BINARY, '-n', $worker, $acme, $root, $domain, $orders, $ready, $release, (string) $wait],
        [1 => ['pipe', 'w'], 2 => ['pipe', 'w']],
        $pipes
    );
    if (!is_resource($handle)) {
        echo "FAIL: cannot start a renewer\n";
        exit(1);
    }
    return [$handle, $pipes];
}

function finish(array $started): string
{
    [$handle, $pipes] = $started;
    $out = trim((string) stream_get_contents($pipes[1]));
    $err = trim((string) stream_get_contents($pipes[2]));
    foreach ($pipes as $pipe) {
        fclose($pipe);
    }
    proc_close($handle);
    return $err === '' ? $out : "$out|stderr:$err";
}

function waitFor(string $path, float $seconds = 20.0): bool
{
    $deadline = microtime(true) + $seconds;
    while (microtime(true) < $deadline) {
        clearstatcache(true, $path);
        if (file_exists($path)) {
            return true;
        }
        usleep(2000);
    }
    return false;
}

function orderCount(string $orders): int
{
    clearstatcache(true, $orders);
    $lines = array_filter(explode("\n", (string) @file_get_contents($orders)), 'strlen');
    return count($lines);
}

function check(bool $ok, string $message): void
{
    if (!$ok) {
        echo "FAIL: $message\n";
    }
}

require $acme . '/state.php';
require $acme . '/lock.php';

use FpmNg\Acme\RenewalInProgress;
use FpmNg\Acme\RenewalLock;
use FpmNg\Acme\State;

$state = new State($root);

/* 1. One renewer takes the lock and is still inside its order. Every other
 *    process that ticks meanwhile -- which is what an overlapping schedule
 *    or a restarted scheduler looks like from here -- must decline, not
 *    queue and not start a second order (criteria 1 and 4). */
$holder = start('one.test', "$root/ready-1", "$root/release-1");
check(waitFor("$root/ready-1"), 'the first renewer never took the lock');
check(orderCount($orders) === 1, 'orders after the first renewer started: ' . orderCount($orders));

$others = [];
for ($i = 0; $i < 7; $i++) {
    $others[] = start('one.test', "$root/ready-other-$i", "$root/release-other-$i", 1.0);
}
$results = array_map('finish', $others);
check(array_unique($results) === ['busy'], 'concurrent renewers reported: ' . implode(',', array_unique($results)));
check(orderCount($orders) === 1, 'orders while one renewer held the lock: ' . orderCount($orders));
echo "one-order-while-held: ok\n";

/* The operator can see why a tick did nothing, and the answer names a
 * process rather than being a bare "busy". */
$who = (new RenewalLock($state, 'one.test'))->holder();
check(is_array($who) && isset($who['pid']) && isset($who['started']),
    'holder() did not describe the running renewer: ' . var_export($who, true));
echo "holder-is-visible: ok\n";

/* 2. A different certificate is a different lock: one slow renewal must not
 *    stop an unrelated one. This is why the lock is per state directory and
 *    not one flag for the whole process tree. */
$second = start('two.test', "$root/ready-2", "$root/release-2", 1.0);
check(waitFor("$root/ready-2", 10.0), 'a renewal for another certificate was blocked');
touch("$root/release-2");
check(finish($second) === 'ran', 'the second certificate did not renew');
echo "per-certificate-not-global: ok\n";

/* 3. Releasing is not sticky: the next tick renews normally. */
touch("$root/release-1");
check(finish($holder) === 'ran', 'the first renewer did not finish');
$after = start('one.test', "$root/ready-3", "$root/release-3", 1.0);
check(waitFor("$root/ready-3", 10.0), 'the lock was not released after the order finished');
touch("$root/release-3");
check(finish($after) === 'ran', 'the renewer after the release did not run');
check((new RenewalLock($state, 'one.test'))->holder() === null, 'holder() still reports a renewer after release');
echo "lock-is-released: ok\n";

/* 4. Killed mid-order (criterion 2). No operator action, no stale-lock
 *    timeout: the kernel drops the flock() when the process dies, so the
 *    very next tick renews. The recovery time is therefore one scheduler
 *    tick of the ACME cron pool -- measured here as the time from the kill
 *    to a successful acquisition, which must be immediate rather than
 *    bounded by any timeout this code invented. */
$doomed = start('kill.test', "$root/ready-4", "$root/release-4");
check(waitFor("$root/ready-4"), 'the renewer to be killed never took the lock');
$before = orderCount($orders);
proc_terminate($doomed[0], 9);
finish($doomed);

$start = microtime(true);
$recovered = start('kill.test', "$root/ready-5", "$root/release-5", 1.0);
check(waitFor("$root/ready-5", 10.0), 'no renewer could take the lock after the holder was killed');
$elapsed = microtime(true) - $start;
touch("$root/release-5");
check(finish($recovered) === 'ran', 'the renewer after the kill did not run');
check(orderCount($orders) === $before + 1, 'orders after the recovery: ' . orderCount($orders));
check($elapsed < 5.0, sprintf('recovery took %.2fs, which is a timeout rather than an immediate release', $elapsed));
echo "kill-recovers-immediately: ok\n";

/* 5. The record a killed process left behind is diagnostics, not a lock. */
$lockFile = $state->renewalLockPath('kill.test');
check(is_file($lockFile), "the lock file $lockFile does not exist");
check((new RenewalLock($state, 'kill.test'))->holder() === null,
    'a leftover record is being reported as a live holder');
echo "leftover-record-is-not-a-lock: ok\n";

/* 6. The lock file is beside the account and certificate keys, so it is not
 *    left world-readable. */
check(sprintf('%o', fileperms($lockFile) & 0777) === '600',
    'lock file mode: ' . sprintf('%o', fileperms($lockFile) & 0777));
echo "lock-file-is-private: ok\n";

/* 7. run() is not swallowing failures: an order that throws releases the
 *    lock and lets the exception through, so a failed renewal is loud and
 *    the next tick can retry. */
$lock = new RenewalLock($state, 'throw.test');
try {
    $lock->run(function () {
        throw new \RuntimeException('order failed');
    });
    check(false, 'a failing order did not propagate its exception');
} catch (\RuntimeException $e) {
    check($e->getMessage() === 'order failed', 'unexpected exception: ' . $e->getMessage());
}
check($lock->holder() === null, 'a failing order left the lock held');
echo "failure-releases-the-lock: ok\n";

foreach (glob("$root/*") as $path) {
    if (is_dir($path)) {
        foreach (glob("$path/*") as $inner) {
            @unlink($inner);
        }
        @rmdir($path);
    } else {
        @unlink($path);
    }
}
@rmdir($root);

?>
Done
--EXPECT--
one-order-while-held: ok
holder-is-visible: ok
per-certificate-not-global: ok
lock-is-released: ok
kill-recovers-immediately: ok
leftover-record-is-not-a-lock: ok
lock-file-is-private: ok
failure-releases-the-lock: ok
Done

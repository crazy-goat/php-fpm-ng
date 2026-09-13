--TEST--
fpm-ng: a beta pool announces its tier once at startup, a supported one says nothing (issue #295)
--SKIPIF--
<?php include "skipif.inc"; ?>
--FILE--
<?php
require_once "tester.inc";

/* Issue #295, implementing the decision in #269. A tier is announced in three
 * places or it does not count: as data on the pool type, as one line at
 * startup, and as a table in README.md. This test is about the second.
 *
 * Two pools of the SAME type in one configuration, differing only in the
 * executor, because that is what makes the assertion sharp: the beta line is a
 * property of the resolved type-and-executor pair, not of the type name. And
 * the supported pool is here for the half that is easy to lose -- silence.
 * A daemon that announced every tier would have taught its operator to filter
 * the whole family, and the two lines that matter would have gone with it. */
function check(bool $condition, string $message): void
{
    if (!$condition) throw new RuntimeException($message);
}

$root = sys_get_temp_dir() . '/fpmng-tier-' . getmypid();
@mkdir($root, 0700, true);

file_put_contents("$root/front.php", '<?php echo "classic";');

/* The smallest worker that stays up: one watcher so the loop has something
 * registered to wait on, and a reply for whatever arrives. The requests below
 * exist only to prove the line is not re-emitted per request. */
file_put_contents("$root/worker.php", <<<'PHP'
<?php
$notify = fpmng_worker_notify_stream();
$watcher = fpmng_worker_event_create(FPMNG_WORKER_READ, $notify, function () use ($notify): void {
    fread($notify, 65536);
    while (($id = fpmng_worker_next_request()) !== null) {
        fpmng_worker_respond($id, 200, ['Content-Type' => 'text/plain'], 'worker');
    }
});
fpmng_worker_event_enable($watcher);
while (!fpmng_worker_may_exit()) {
    fpmng_worker_loop(true);
}
PHP);

$base = (int) (getenv('FPMNG_DIRECT_TEST_PORT') ?: 28054);
$supportedPort = $base + 36;
$betaPort = $base + 37;
$cfg = <<<CFG
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
[supported]
listen = 127.0.0.1:$supportedPort
pool.type = http-direct
pm = static
pm.max_children = 1
chdir = $root
http.front_controller = /front.php
http.read_timeout = 10000
[beta]
listen = 127.0.0.1:$betaPort
pool.type = http-direct
pool.executor = worker
pm = static
pm.max_children = 1
chdir = $root
http.front_controller = /worker.php
http.read_timeout = 10000
php_admin_value[max_execution_time] = 0
CFG;

$tester = new FPM\Tester($cfg, '<?php');
try {
    $tester->start();
    $tester->expectLogStartNotices();

    /* The tier line is emitted before the first child is forked, so it is
     * already behind the log reader by the time expectLogStartNotices() has
     * walked past "ready to handle connections" -- hence checkAllLogs. */
    $tester->expectLogNotice(
        'pool\.type = http-direct with pool\.executor = worker is BETA: .*README\.md',
        'beta',
        checkAllLogs: true
    );
    echo "beta announced as a NOTICE: ok\n";

    /* Not "no line mentioning the pool" -- it has ordinary startup lines of
     * its own. No line about its TIER, at either level. */
    $tester->expectNoLogPattern('/\[pool supported\].*is (BETA|EXPERIMENTAL)/', true);
    echo "supported pool is silent: ok\n";

    /* Two requests to each pool. The assertion that follows them is the one
     * #269 asked for in as many words: a line an operator sees per request is
     * one they learn to filter. checkAllLogs is false here on purpose -- the
     * reader is past the startup line already, so anything it finds now was
     * written after it. */
    for ($i = 0; $i < 2; $i++) {
        check(@file_get_contents("http://127.0.0.1:$supportedPort/") === 'classic',
            "supported pool did not answer request $i");
        check(@file_get_contents("http://127.0.0.1:$betaPort/") === 'worker',
            "beta pool did not answer request $i");
    }
    $tester->expectNoLogPattern('/is (BETA|EXPERIMENTAL)/', false);
    echo "not repeated per request: ok\n";

    echo "Done\n";
} finally {
    /* terminate()/close() are idempotent; this is the path where an assertion
     * above threw before reaching them. */
    $tester->terminate();
    $tester->close();
    @unlink("$root/front.php");
    @unlink("$root/worker.php");
    @rmdir($root);
}
?>
--EXPECT--
beta announced as a NOTICE: ok
supported pool is silent: ok
not repeated per request: ok
Done
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>

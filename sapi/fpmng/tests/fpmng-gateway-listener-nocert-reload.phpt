--TEST--
fpm-ng: a gateway listener in NO_CERT is not handed over on a reload, so no socket is left behind (issue #661)
--SKIPIF--
<?php
include "fpmng-skipif.inc";
fpmng_skip_if_pool_type_unsupported('gateway');
if (!is_dir('/proc/self/fd')) {
    die('skip needs /proc to count the socket descriptors of the master');
}
$probe = new FPM\Tester(<<<'EOT'
[global]
error_log = {{FILE:LOG}}
[gw]
pool.type = gateway
listen = {{ADDR[probe]}}
http.tls_cert = /nonexistent-cert.pem
http.tls_key = /nonexistent-key.pem
http.tls_wait_for_cert = yes
http.route[unconfined] = /
[unconfined]
pool.type = fastcgi
listen = {{ADDR}}
pm = static
pm.max_children = 1
EOT, '<?php');
$messages = $probe->testConfig(true, null, false, false);
FPM\Tester::clean();
foreach ((array) $messages as $message) {
    if (str_contains($message, 'built with TLS support')) {
        die('skip php-fpm-ng built without TLS support (configure without --enable-fpmng-tls, issue #280)');
    }
}
?>
--FILE--
<?php

require_once "tester.inc";

/* Issue #661, review of the first commit. A gateway with http.tls_wait_for_cert
 * and no certificate yet is in NO_CERT: its public socket is bound, but it is not
 * listening. Such a socket must not be handed over. The next master rejects a
 * record that does not name a listening socket, and leaves the descriptor open
 * with no owner. Each reload would then keep one more bound socket of the
 * gateway address, and the socket would survive every later exec.
 *
 * The test counts the socket descriptors of the master before a reload and after
 * each of two reloads. A generation logs "ready to handle connections" once every
 * pool has bound its socket, and the count is taken after that line. A leaked
 * socket makes the count grow by one for each reload. */

$root = sys_get_temp_dir() . '/fpmng-gw-listener-nocert-' . getmypid();
@mkdir($root, 0700, true);
$log = "$root/error.log";

function fail(string $what): void
{
    throw new RuntimeException($what);
}

function wait_ready(string $log, int $count): void
{
    $deadline = microtime(true) + 20;
    while (substr_count((string) @file_get_contents($log), 'ready to handle connections') < $count) {
        if (microtime(true) > $deadline) {
            fail("the master did not reach 'ready to handle connections' $count time(s)\n" . (string) @file_get_contents($log));
        }
        usleep(50000);
    }
}

function socket_count(int $pid): int
{
    $count = 0;
    foreach (glob("/proc/$pid/fd/*") ?: [] as $fd) {
        if (str_starts_with((string) @readlink($fd), 'socket:')) {
            $count++;
        }
    }
    return $count;
}

$cfg = <<<EOT
[global]
error_log = $log
pid = {{FILE:PID}}
log_level = notice
process_control_timeout = 10
[gw]
pool.type = gateway
listen = {{ADDR[http]}}
chdir = $root
http.gateways = 1
http.route[app] = /
http.tls_cert = $root/missing-cert.pem
http.tls_key = $root/missing-key.pem
http.tls_wait_for_cert = yes
[app]
pool.type = fastcgi
listen = {{ADDR}}
chdir = $root
pm = static
pm.max_children = 1
EOT;

$tester = new FPM\Tester($cfg, '<?php');
try {
    $tester->start([], false);
    wait_ready($log, 1);
    $master = $tester->getPid();

    /* The fastcgi listener and the gateway socket are both held by the master. */
    $before = socket_count($master);
    if ($before < 2) {
        fail("the master holds $before socket descriptor(s) before the reload, want at least 2\n" . (string) @file_get_contents($log));
    }

    $tester->signal('USR2');
    wait_ready($log, 2);
    $afterFirst = socket_count($master);

    $tester->signal('USR2');
    wait_ready($log, 3);
    $afterSecond = socket_count($master);

    if ($afterFirst !== $before || $afterSecond !== $before) {
        fail("socket descriptors of the master: $before before the reload, $afterFirst after one, $afterSecond after two\n" . (string) @file_get_contents($log));
    }
    echo "no socket is left behind by the reloads: ok\n";

    echo "Done\n";
} finally {
    $tester->terminate();
    $tester->close();
    foreach (glob("$root/*") ?: [] as $f) {
        @unlink($f);
    }
    @rmdir($root);
}
?>
--EXPECT--
no socket is left behind by the reloads: ok
Done
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>

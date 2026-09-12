--TEST--
fpm-ng: with no operator path set nothing is bound, not even the configured listen (issue #274)
--SKIPIF--
<?php include "skipif.inc"; ?>
--FILE--
<?php

require_once "tester.inc";

$root = sys_get_temp_dir() . '/fpmng-operator-off-' . getmypid();
@mkdir($root, 0700, true);
file_put_contents($root . '/front.php', '<?php echo "php:" . $_SERVER["REQUEST_URI"];');

/* pm.metrics_listen is set and no path is: issue #273, point 4 -- there is no
 * separate on/off directive, the endpoint exists iff a path is set. So the
 * address below must stay unbound, and this is the configuration where getting
 * that wrong would be invisible: an address named in the config looks like an
 * address someone meant to open.
 *
 * The metrics pair rather than the status pair because http-direct has not
 * moved its status page onto the operator listener yet and refuses
 * pm.status_listen outright (#275). */
$cfg = <<<EOT
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}

[web]
listen = {{ADDR}}
pool.type = http-direct
pm = static
pm.max_children = 1
chdir = $root
http.front_controller = /front.php
pm.metrics_listen = {{ADDR[operator]}}
EOT;

$tester = new FPM\Tester($cfg, '<?php');
try {
    $tester->start();
    $tester->expectLogStartNotices();

    /* The master is up and serving -- so a refused connection below is the
     * absence of a listener, not the absence of a running fpm. */
    $public = $tester->getListen('{{ADDR}}');
    $fp = stream_socket_client("tcp://$public", $errno, $error, 5);
    if (!$fp) {
        throw new RuntimeException("connect $public: $error");
    }
    stream_set_timeout($fp, 5);
    fwrite($fp, "GET /anything HTTP/1.1\r\nHost: test\r\nConnection: close\r\n\r\n");
    $raw = (string) stream_get_contents($fp);
    fclose($fp);
    if (!str_contains($raw, 'php:/anything')) {
        throw new RuntimeException("public listener not serving:\n$raw");
    }
    echo "pool serving: ok\n";

    $operator = $tester->getListen('{{ADDR[operator]}}');
    $fp = @stream_socket_client("tcp://$operator", $errno, $error, 5);
    if ($fp) {
        fclose($fp);
        throw new RuntimeException("something is listening on $operator");
    }
    echo "operator listener absent: ok\n";

    echo "Done\n";
} finally {
    $tester->terminate();
    $tester->close();
    @unlink($root . '/front.php');
    @rmdir($root);
}
?>
--EXPECT--
pool serving: ok
operator listener absent: ok
Done
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>

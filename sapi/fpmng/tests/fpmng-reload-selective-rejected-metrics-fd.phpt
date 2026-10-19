--TEST--
fpm-ng: a rejected inherited metrics descriptor is closed, no worker inherits it (issue #842)
--SKIPIF--
<?php
include "fpmng-skipif.inc";
fpmng_skip_if_pool_type_unsupported('http-direct');
if (!is_dir('/proc/self/fd')) {
    die("skip needs /proc to read the worker's descriptors");
}
?>
--FILE--
<?php

require_once "tester.inc";

/* Issue #842: the master takes an inherited M: record only if its descriptor
 * passes fpm_reload_shm_fd_size_ok(). A record that fails it used to leave
 * the descriptor open: it is not in own_mx_fd, so fpm_reload_shm_child_init()
 * did not close it, and every worker inherited it.
 *
 * The record names descriptor 0. fpm_stdio_init_main() has put /dev/null on
 * it before the metrics init, and /dev/null is not a memfd: its size (0) is
 * below the claimed size, so the master must reject it. The fixed master
 * closes it, so the worker does not hold /dev/null on descriptor 0. */
$root = sys_get_temp_dir() . '/fpmng-reload-sel-rejected-fd-' . getmypid();
@mkdir($root, 0700, true);

file_put_contents("$root/a.php", <<<'PHP'
<?php
echo 'pid ' . getmypid() . ' fd0 ' . (readlink('/proc/self/fd/0') ?: 'closed');
PHP);

$cfg = <<<EOT
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
log_level = notice

[a]
listen = {{ADDR[a]}}
chdir = $root
pm = static
pm.max_children = 1
pool.type = http-direct
http.front_controller = /a.php
EOT;

function hit(string $addr): string
{
    $fp = stream_socket_client("tcp://$addr", $errno, $error, 5);
    if (!$fp) {
        throw new RuntimeException("connect $addr: $error");
    }
    stream_set_timeout($fp, 5);
    fwrite($fp, "GET / HTTP/1.1\r\nHost: test\r\nConnection: close\r\n\r\n");
    $raw = (string) stream_get_contents($fp);
    fclose($fp);
    return explode("\r\n\r\n", $raw, 2)[1] ?? '';
}

function check(bool $condition, string $message): void
{
    if (!$condition) {
        throw new RuntimeException($message);
    }
}

$tester = new FPM\Tester($cfg, '<?php');
try {
    @unlink($tester->getPrefixedFile(FPM\Tester::FILE_EXT_LOG_ERR));
    $tester->start([], false, false, [], [], array_merge(getenv(), [
        'FPMNG_SELECTIVE_RELOAD_SHM' => 'M:0:1048576:1:1',
    ]));
    $tester->switchLogSource('{{FILE:LOG}}');
    $tester->expectLogStartNotices();

    $a = $tester->getListen('{{ADDR[a]}}');
    $body = trim(hit($a));
    check(preg_match('/^pid \d+ fd0 (.*)$/', $body, $m) === 1, 'unexpected body: ' . var_export($body, true));
    check($m[1] !== '/dev/null', 'the worker still holds the descriptor that was rejected: ' . var_export($m[1], true));
    echo "rejected descriptor closed: ok\n";
    echo "Done\n";
} finally {
    $tester->terminate();
    $tester->close();
    @unlink("$root/a.php");
    @rmdir($root);
}
?>
--EXPECT--
rejected descriptor closed: ok
Done
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>

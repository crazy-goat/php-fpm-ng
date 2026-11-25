--TEST--
fpm-ng: an http-direct access line that is truncated never ends inside an escape (issue #1003)
--SKIPIF--
<?php include "skipif.inc"; ?>
--FILE--
<?php
require_once "tester.inc";

/* Issue #1003, review round 1: the line buffer is 8 KiB. A field that fills it
 * up to the last byte used to be followed by the forced newline, which was
 * written over the last byte of the content. Here the header value is 8189
 * bytes of 'A' and one quote. The quote is escaped as two bytes, so the content
 * is exactly 8191 bytes and the newline must come after the escape, not over
 * it. The old code wrote the newline over the second byte of the escape and
 * the line ended with a lone backslash. The second request checks that the
 * next line still starts on its own line. */
$root = sys_get_temp_dir() . '/fpmng-direct-trunc-' . getmypid();
@mkdir($root, 0700, true);
file_put_contents($root . '/index.php', '<?php echo "ok";');

$cfg = <<<CFG
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
[direct]
listen = {{ADDR[http]}}
pool.type = http-direct
pm = static
pm.max_children = 1
chdir = $root
http.front_controller = /index.php
access.log = {{FILE:LOG:ACC}}
access.format = "%{HTTP_X_A}e"
CFG;

function fetch(string $addr, string $target, string $extraHeader): void
{
    $fp = stream_socket_client("tcp://$addr", $errno, $error, 5);
    if (!$fp) {
        throw new RuntimeException("connect $addr: $error");
    }
    stream_set_timeout($fp, 10);
    fwrite($fp, "GET $target HTTP/1.0\r\nHost: h\r\n$extraHeader\r\n");
    stream_get_contents($fp);
    fclose($fp);
}

/* The children write the log, so the last line may not be there yet. */
function accessLog(string $path, int $lines): string
{
    $deadline = microtime(true) + 10;
    $content = '';
    do {
        $content = (string) @file_get_contents($path);
        if (substr_count($content, "\n") >= $lines) {
            break;
        }
        usleep(100000);
    } while (microtime(true) < $deadline);
    return $content;
}

$tester = new FPM\Tester($cfg, '<?php echo "unused";');
try {
    $tester->start();
    $tester->expectLogStartNotices();
    $http = $tester->getAddr('ipv4', '[http]');

    fetch($http, '/index.php?r=long', 'X-A: ' . str_repeat('A', 8189) . "\"\r\n");
    fetch($http, '/index.php?r=short', "X-A: short\r\n");

    $log = $tester->getPrefixedFile(FPM\Tester::FILE_EXT_LOG_ACC);
    $content = accessLog($log, 2);

    /* explode() gives the two lines and one empty string after the last LF. */
    $parts = explode("\n", $content);
    if (count($parts) !== 3 || $parts[2] !== '') {
        echo "FAIL: expected two lines, got " . (count($parts) - 1) . ":\n$content\n";
        exit(1);
    }
    echo "two-lines: yes\n";

    if ($parts[0] !== str_repeat('A', 8189) . '\"') {
        $tail = substr($parts[0], -4);
        echo "FAIL: the long line does not end with the whole escape \\\" (tail "
            . json_encode($tail) . ", length " . strlen($parts[0]) . ")\n";
        exit(1);
    }
    echo "long-line: ends with the whole escape\n";

    if ($parts[1] !== 'short') {
        echo "FAIL: the next line is not 'short':\n{$parts[1]}\n";
        exit(1);
    }
    echo "next-line: short\n";
} finally {
    $tester->terminate();
    $tester->expectLogTerminatingNotices();
    $tester->close();
    @unlink("$root/index.php");
    @rmdir($root);
}
echo "Done\n";
?>
--EXPECT--
two-lines: yes
long-line: ends with the whole escape
next-line: short
Done
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>

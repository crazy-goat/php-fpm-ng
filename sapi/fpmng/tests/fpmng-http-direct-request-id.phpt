--TEST--
fpm-ng: an http-direct access.format prints the X-Request-Id it receives with %{HTTP_X_REQUEST_ID}e, "-" when there is none (issue #642)
--SKIPIF--
<?php include "skipif.inc"; ?>
--FILE--
<?php
require_once "tester.inc";

/* Issue #642: http.request_id is a gateway directive and http-direct refuses
 * it. The http-direct pool still needs the id in its access log when a gateway
 * sends one: the gateway passes it to a route target as the X-Request-Id
 * header (fpm_http_client.c), and the header becomes HTTP_X_REQUEST_ID in the
 * CGI environment. access.format reads that with %{HTTP_X_REQUEST_ID}e. This
 * pool has no gateway in front of it, so the header below is the client's own
 * value: the test shows the token, not a trust decision. */

$root = sys_get_temp_dir() . '/fpmng-direct-reqid-' . getmypid();
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
access.format = "%m %r%Q%q %s id=%{HTTP_X_REQUEST_ID}e"
CFG;

function fetch(string $addr, string $target, string $extraHeader): string
{
    $fp = stream_socket_client("tcp://$addr", $errno, $error, 5);
    if (!$fp) {
        throw new RuntimeException("connect $addr: $error");
    }
    stream_set_timeout($fp, 10);
    fwrite($fp, "GET $target HTTP/1.0\r\nHost: h\r\n$extraHeader\r\n");
    $raw = (string) stream_get_contents($fp);
    fclose($fp);
    return $raw;
}

/* The children write the log, so the last line may not be there yet. */
function accessLog(string $path, string $needle): string
{
    $deadline = microtime(true) + 10;
    $content = '';
    do {
        $content = (string) @file_get_contents($path);
        if (str_contains($content, $needle)) {
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

    fetch($http, '/index.php?r=with', "X-Request-Id: edge.trace_42-a\r\n");
    fetch($http, '/index.php?r=without', '');

    $log = $tester->getPrefixedFile(FPM\Tester::FILE_EXT_LOG_ACC);
    $content = accessLog($log, 'r=without');

    $with = null;
    $without = null;
    foreach (explode("\n", $content) as $line) {
        if (str_contains($line, 'r=with ')) {
            $with = $line;
        }
        if (str_contains($line, 'r=without ')) {
            $without = $line;
        }
    }
    if ($with === null || $without === null) {
        echo "FAIL: missing access log line(s):\n$content\n";
        exit(1);
    }

    if (!str_ends_with($with, ' 200 id=edge.trace_42-a')) {
        echo "FAIL: line with X-Request-Id does not print the id:\n$with\n";
        exit(1);
    }
    echo "with-header: id printed\n";

    if (!str_ends_with($without, ' 200 id=-')) {
        echo "FAIL: line without X-Request-Id does not print -:\n$without\n";
        exit(1);
    }
    echo "without-header: -\n";
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
with-header: id printed
without-header: -
Done
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>

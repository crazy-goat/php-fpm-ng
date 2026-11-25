--TEST--
fpm-ng: an http-direct access.format escapes the client fields it writes, so one request is one line (issue #1003)
--SKIPIF--
<?php include "skipif.inc"; ?>
--FILE--
<?php
require_once "tester.inc";

/* Issue #1003: the http-direct access log wrote %u, %r, %q, %e and %o raw.
 * %u is the Basic auth user, which php_handle_auth_data() takes from the
 * Authorization header (fpm_http_direct.c:2427), so a client can put a LF in it
 * and forge a second line. The fields are escaped with the rule the gateway
 * access log uses (fpm_http_access_log.c): a quote and a backslash get a
 * backslash in front, and a control byte or DEL becomes \?. A space is kept.
 * The requests below send a LF in %u, a quote and a backslash in %q, %e and
 * %o, and a bare CR in %r. The expected lines are exact, so a missing escape
 * fails here, and a forged line adds a line that the count check reports. */

$root = sys_get_temp_dir() . '/fpmng-direct-escape-' . getmypid();
@mkdir($root, 0700, true);
file_put_contents($root . '/index.php', <<<'PHP'
<?php
header('X-Out: q"x\\y');
echo "ok";
PHP);

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
ping.path = /ping
access.log = {{FILE:LOG:ACC}}
access.format = "%u|%r|%q|%{HTTP_X_EVIL}e|%{X-Out}o|%m|%R"
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

    /* The user name is in the Basic credentials, so it is encoded here and PHP
     * decodes it back to a LF. The quote and the backslash are in the query
     * and in the X-Evil header. */
    $auth = base64_encode("evil\nfake:pw");
    fetch($http, '/index.php?q=a"b', "Authorization: Basic $auth\r\n"
        . "X-Evil: say \"hi\" \\ back\r\n");
    /* A second request through the front controller, with no auth and no
     * X-Evil header, so its empty fields print "-". */
    fetch($http, '/x"y\\z', '');
    /* A third request answered without PHP, by the ping ending
     * (fpm_direct_log_local), which takes %r and %q from libevent's copy of
     * the target. */
    fetch($http, '/ping?r=a"b', '');
    /* A bare CR in the path, which is %r. PHP did not run for it: the line has
     * no %{X-Out}o value, so %o prints "-". The CR reaches the log as \? and
     * the line is not split. */
    fetch($http, "/a\rb?c=d", '');

    $log = $tester->getPrefixedFile(FPM\Tester::FILE_EXT_LOG_ACC);
    $content = accessLog($log, 4);

    $lines = array_values(array_filter(explode("\n", $content), 'strlen'));
    if (count($lines) !== 4) {
        echo "FAIL: expected one line per request (4), got " . count($lines) . ":\n$content\n";
        exit(1);
    }
    echo "one-line-per-request: 4\n";

    $expected = [
        'evil\?fake|/index.php|q=a\"b|say \"hi\" \\\\ back|q\"x\\\\y|GET|127.0.0.1',
        '-|/x\"y\\\\z||-|q\"x\\\\y|GET|127.0.0.1',
        '-|/ping|r=a\"b|-|-|GET|127.0.0.1',
        '-|/a\?b|c=d|-|-|GET|127.0.0.1',
    ];
    foreach ($lines as $i => $line) {
        if ($line !== $expected[$i]) {
            echo "FAIL: line " . ($i + 1) . " is not escaped as expected:\n$line\n"
                . "expected:\n{$expected[$i]}\n";
            exit(1);
        }
    }
    echo "fields: escaped as expected\n";
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
one-line-per-request: 4
fields: escaped as expected
Done
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>

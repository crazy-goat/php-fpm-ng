--TEST--
fpm-ng: HTTP gateway logs an upstream that took the whole request and never answered apart from one that vanished mid-write (issue #118)
--SKIPIF--
<?php
include "fpmng-skipif.inc";
fpmng_skip_if_pool_type_unsupported('gateway');
?>
--FILE--
<?php

require_once "tester.inc";

/* A fixed name, not one derived from getmypid(): --CLEAN-- runs in another
 * process and could not find a pid-suffixed directory to remove. */
$docRoot = sys_get_temp_dir() . '/fpmng-http-upstream-loss';
@mkdir($docRoot, 0700, true);

/* The worker kills itself instead of answering: SIGKILL sends no output and no
 * FastCGI END_REQUEST, which is exactly the shape both cases of issue #118
 * have on the gateway's socket. `kill` through exec() rather than posix_kill()
 * because CI configures with --disable-all, so ext/posix is not built
 * (.github/workflows/build-matrix.yml). */
file_put_contents("$docRoot/die.php", '<?php exec("kill -9 " . getmypid()); usleep(500000);');

$cfg = <<<EOT
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
[gw]
pool.type = gateway
listen = {{ADDR[http]}}
chdir = $docRoot
http.route[web] = /
[web]
pool.type = fastcgi
listen = {{ADDR}}
chdir = $docRoot
pm = static
pm.max_children = 4
EOT;

/* No script for the tester to generate: every request in this test goes to
 * $docRoot/die.php through the gateway's document root. */
$tester = new FPM\Tester($cfg);
/* The log has to be a file, not the -O stderr pipe: this test reads it whole,
 * twice, and asserts what is NOT in the second half. */
@unlink($tester->getPrefixedFile(FPM\Tester::FILE_EXT_LOG_ERR));
$tester->start([], false);
$tester->switchLogSource('{{FILE:LOG}}');
$tester->expectLogStartNotices();

$errorLog = $tester->getPrefixedFile(FPM\Tester::FILE_EXT_LOG_ERR);
$http = $tester->getAddr('ipv4', '[http]');

/* The line that must appear only for a request the gateway managed to write in
 * full, and the line that must appear only when it did not. */
$mutePattern  = '/upstream .* closed [0-9.]+ ms after the complete request was written to it/';
$gonePattern  = '/http: no answer from /';

function logWait(string $errorLog, string $pattern, int $seconds = 10): string
{
    $deadline = microtime(true) + $seconds;
    do {
        $log = (string) @file_get_contents($errorLog);
        if (preg_match($pattern, $log)) {
            return $log;
        }
        usleep(100000);
    } while (microtime(true) < $deadline);

    return $log;
}

/* ---- case 1: the whole request goes out, then the worker disappears ------ */

$ctx = stream_context_create(['http' => ['timeout' => 10, 'ignore_errors' => true]]);
@file_get_contents("http://$http/die.php", false, $ctx);

$log = logWait($errorLog, $mutePattern);
if (!preg_match($mutePattern, $log)) {
    echo "FAIL: no line for a fully written request without a reply\n";
    echo $log;
    exit(1);
}
echo "fully-written-no-reply: ok\n";

/* The gateway cannot tell a dead child from a refused request head on the
 * socket alone, so the new line tells the operator to look for the master's
 * child-exit line. That line has to actually be there in this case.
 *
 * Waited for, not asserted on the snapshot above: the two lines come from two
 * processes (the gateway's event loop and fpm_children_bury() after the master
 * reaps SIGCHLD) and nothing orders them, so requiring the master's line to be
 * present the instant the gateway's is would fail intermittently on correct
 * code. */
$log = logWait($errorLog, '/child \d+ exited on signal 9/');
if (!preg_match('/child \d+ exited on signal 9/', $log)) {
    echo "FAIL: master did not report the killed child, the new line points at nothing\n";
    echo $log;
    exit(1);
}
echo "child-exit-line-present: ok\n";

if (preg_match($gonePattern, $log)) {
    echo "FAIL: a fully written request also produced the mid-write line\n";
    echo $log;
    exit(1);
}
echo "not-confused-with-mid-write: ok\n";

$sizeAfterCase1 = strlen($log);

/* ---- case 2: the worker dies while the request is still being written ---- */

/* 24 MiB with a Content-Type PHP does not read at request startup: the worker
 * never drains the body, so the gateway still has megabytes pending on the
 * socket when the script kills the process. Under http.max_body (32m default,
 * fpm_conf.c), so the gateway buffers it and dispatches. */
$body = str_repeat('x', 24 * 1024 * 1024);
$parts = explode(':', $http);
$sock = @fsockopen($parts[0], (int) $parts[1], $errno, $errstr, 10);
if (!$sock) {
    echo "FAIL: cannot connect to the gateway: $errstr\n";
    exit(1);
}
stream_set_timeout($sock, 10);
@fwrite($sock, "POST /die.php HTTP/1.1\r\nHost: $http\r\nContent-Type: application/octet-stream\r\n"
    . 'Content-Length: ' . strlen($body) . "\r\nConnection: close\r\n\r\n");
@fwrite($sock, $body);
@stream_get_contents($sock);
@fclose($sock);

$log = logWait($errorLog, $gonePattern);
$tail = substr($log, $sizeAfterCase1);
if (!preg_match($gonePattern, $tail)) {
    echo "FAIL: no 'no answer' line for a request lost mid-write\n";
    echo $tail;
    exit(1);
}
echo "lost-mid-write: ok\n";

if (preg_match($mutePattern, $tail)) {
    echo "FAIL: a request lost mid-write claimed to have been written in full\n";
    echo $tail;
    exit(1);
}
echo "two-lines-distinguishable: ok\n";

$tester->terminate();
$tester->close();

?>
--EXPECT--
fully-written-no-reply: ok
child-exit-line-present: ok
not-confused-with-mid-write: ok
lost-mid-write: ok
two-lines-distinguishable: ok
--CLEAN--
<?php
require_once "tester.inc";
FPM\Tester::clean();

$docRoot = sys_get_temp_dir() . '/fpmng-http-upstream-loss';
@unlink("$docRoot/die.php");
@rmdir($docRoot);
?>

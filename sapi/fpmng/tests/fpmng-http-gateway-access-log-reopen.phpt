--TEST--
fpm-ng: a gateway reopens http.access_log on a SIGUSR1 after a logrotate (issue #137)
--SKIPIF--
<?php
include "fpmng-skipif.inc";
fpmng_skip_if_pool_type_unsupported('http');
?>
--FILE--
<?php

require_once "tester.inc";

/* Pid-suffixed so two runs of the suite on the same box (the poligon is
 * shared) cannot share a docroot; --CLEAN-- runs in another process and finds
 * it back with a glob, the way fpmng-http-gateway-log-reopen.phpt does. */
$docRoot = sys_get_temp_dir() . '/fpmng-http-acclog-reopen-' . getmypid();
@mkdir($docRoot, 0700, true);
file_put_contents("$docRoot/hit.php", '<?php echo "ok";');

/* http.gateways = 1 so exactly one process owns the access log: with the
 * default 2 the assertions below would hold for whichever gateway the kernel
 * handed the connection to, and a failure would read as a flake instead of a
 * bug. */
$cfg = <<<EOT
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
[web]
listen = {{ADDR}}
chdir = $docRoot
pm = static
pm.max_children = 2
pool.type = http
http.listen = {{ADDR[http]}}
http.gateways = 1
http.access_log = {{FILE:LOG:ACC}}
EOT;

function fileWait(string $file, string $pattern, int $seconds = 10): string
{
    $deadline = microtime(true) + $seconds;
    do {
        $content = (string) @file_get_contents($file);
        if (preg_match($pattern, $content)) {
            return $content;
        }
        usleep(100000);
    } while (microtime(true) < $deadline);

    return $content;
}

function httpGet(string $url): void
{
    $ctx = stream_context_create(['http' => ['timeout' => 10, 'ignore_errors' => true]]);
    @file_get_contents($url, false, $ctx);
}

$tester = new FPM\Tester($cfg);
/* A file, not the -O stderr pipe: the master's reopen has to be a reopen of a
 * real file for the notification under test to mean anything — the same
 * arrangement fpmng-http-gateway-log-reopen.phpt uses. */
@unlink($tester->getPrefixedFile(FPM\Tester::FILE_EXT_LOG_ERR));
$tester->start([], false);
$tester->switchLogSource('{{FILE:LOG}}');
$tester->expectLogStartNotices();

$accessLog = $tester->getPrefixedFile(FPM\Tester::FILE_EXT_LOG_ACC);
$rotated   = $accessLog . '.rotated';
$http      = $tester->getAddr('ipv4', '[http]');

/* Before the rotation, so that a broken assertion later cannot be blamed on
 * the access log never having worked at all. */
httpGet("http://$http/hit.php?r=before");
if (!preg_match('/r=before/', fileWait($accessLog, '/r=before/'))) {
    echo "FAIL: nothing was logged before the rotation\n";
    echo (string) @file_get_contents($accessLog);
    exit(1);
}
echo "line-before-rotation: ok\n";

/* logrotate, in the two steps it performs: move the file out of the way, then
 * tell the daemon. */
if (!rename($accessLog, $rotated)) {
    echo "FAIL: cannot rename $accessLog\n";
    exit(1);
}
$tester->reloadLogs();

/* The master's own confirmation that it has handled the signal — the point
 * from which the gateway's notification is on its channel. */
$errorLog = $tester->getPrefixedFile(FPM\Tester::FILE_EXT_LOG_ERR);
if (!preg_match('/error log file re-opened/', fileWait($errorLog, '/error log file re-opened/'))) {
    echo "FAIL: the master did not report reopening the error log\n";
    exit(1);
}

/* One event-loop turn: the master notifies, the gateway reopens from its own
 * loop, so there is a window in which a line still belongs in the rotated file
 * (fpm_error_log_follow.h). Provoking the request after it is deliberate — the
 * test asserts where a line written after a completed rotation goes, not how
 * wide that window is. */
usleep(300000);

httpGet("http://$http/hit.php?r=after");

if (!preg_match('/r=after/', fileWait($accessLog, '/r=after/'))) {
    echo "FAIL: the post-rotation line is not in the reopened http.access_log\n";
    echo "--- reopened ---\n" . (string) @file_get_contents($accessLog);
    echo "--- rotated ---\n" . (string) @file_get_contents($rotated);
    exit(1);
}
echo "line-in-reopened-log: ok\n";

/* The regression this test exists for: before issue #137 the line above was
 * absent from the file the operator is reading and present in this one. */
if (preg_match('/r=after/', (string) @file_get_contents($rotated))) {
    echo "FAIL: a post-rotation line still went into the rotated http.access_log\n";
    echo (string) @file_get_contents($rotated);
    exit(1);
}
echo "rotated-log-has-no-post-rotation-line: ok\n";

$tester->terminate();
$tester->close();

?>
--EXPECT--
line-before-rotation: ok
line-in-reopened-log: ok
rotated-log-has-no-post-rotation-line: ok
--CLEAN--
<?php
require_once "tester.inc";
FPM\Tester::clean();

foreach (glob(sys_get_temp_dir() . '/fpmng-http-acclog-reopen-*') as $docRoot) {
    @unlink("$docRoot/hit.php");
    @rmdir($docRoot);
}

/* FPM\Tester::clean() only knows the extensions it created — the rotated copy
 * is ours (the test's files live next to the .phpt, see getPrefixedFile()). */
@unlink(__DIR__ . '/fpmng-http-gateway-access-log-reopen.acc.log.rotated');
?>

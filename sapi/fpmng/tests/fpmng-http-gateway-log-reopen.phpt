--TEST--
fpm-ng: a gateway child follows error_log across a SIGUSR1 reopen (issue #134)
--SKIPIF--
<?php include "skipif.inc"; ?>
--FILE--
<?php

require_once "tester.inc";

/* Pid-suffixed so two runs of the suite on the same box (the poligon is
 * shared) cannot share a docroot; --CLEAN-- runs in another process and finds
 * it back with a glob, the way fpmng-fiber-stream-select.phpt does. */
$docRoot = sys_get_temp_dir() . '/fpmng-http-log-reopen-' . getmypid();
@mkdir($docRoot, 0700, true);

/* SIGKILL on the worker mid-request is what makes the GATEWAY process write a
 * line of its own — the same provocation as
 * fpmng-http-gateway-log-decoration.phpt and the one the issue asks for.
 * `kill` through exec() because CI configures with --disable-all, so ext/posix
 * is not built. */
file_put_contents("$docRoot/die.php", '<?php exec("kill -9 " . getmypid()); usleep(500000);');

$cfg = <<<EOT
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
[web]
listen = {{ADDR}}
chdir = $docRoot
pm = static
pm.max_children = 4
pool.type = http
http.listen = {{ADDR[http]}}
EOT;

function logWait(string $file, string $pattern, int $seconds = 10): string
{
    $deadline = microtime(true) + $seconds;
    do {
        $log = (string) @file_get_contents($file);
        if (preg_match($pattern, $log)) {
            return $log;
        }
        usleep(100000);
    } while (microtime(true) < $deadline);

    return $log;
}

$tester = new FPM\Tester($cfg);
/* A file, not the -O stderr pipe: the whole test is about which file on disk
 * a line lands in. */
@unlink($tester->getPrefixedFile(FPM\Tester::FILE_EXT_LOG_ERR));
$tester->start([], false);
$tester->switchLogSource('{{FILE:LOG}}');
$tester->expectLogStartNotices();

$errorLog = $tester->getPrefixedFile(FPM\Tester::FILE_EXT_LOG_ERR);
$rotated  = $errorLog . '.rotated';
$http     = $tester->getAddr('ipv4', '[http]');

/* logrotate, in the two steps it performs: move the file out of the way, then
 * tell the daemon. Nothing below reads the log through $tester's log reader
 * any more — it still holds the pre-rotation descriptor, which is the very
 * thing under test here. */
if (!rename($errorLog, $rotated)) {
    echo "FAIL: cannot rename $errorLog\n";
    exit(1);
}
$tester->reloadLogs();

/* The master's own confirmation, from fpm_events.c, and the point from which a
 * new error_log file exists at all. */
$log = logWait($errorLog, '/error log file re-opened/');
if (!preg_match('/error log file re-opened/', $log)) {
    echo "FAIL: the master did not report reopening the error log\n";
    echo $log;
    exit(1);
}
echo "master-reopened: ok\n";

/* One event-loop turn: the master hands the new file to every gateway process
 * over its follow channel and the gateway adopts it from its own loop, so
 * there is a window in which a gateway line still belongs in the rotated file
 * (fpm_error_log_follow.h). Provoking the line after it is deliberate — the
 * test asserts where a line written after a completed rotation goes, not how
 * wide that window is. */
usleep(300000);

$ctx = stream_context_create(['http' => ['timeout' => 10, 'ignore_errors' => true]]);
@file_get_contents("http://$http/die.php", false, $ctx);

$gatewayLine = '/http: (no answer from|upstream )/';

$log = logWait($errorLog, $gatewayLine);
if (!preg_match($gatewayLine, $log)) {
    echo "FAIL: the gateway line is not in the reopened error_log\n";
    echo "--- reopened ---\n$log";
    echo "--- rotated ---\n" . (string) @file_get_contents($rotated);
    exit(1);
}
echo "gateway-line-in-reopened-log: ok\n";

/* The regression this test exists for: before issue #134 the line above was
 * absent from the file the operator is reading and present in this one. */
if (preg_match($gatewayLine, (string) @file_get_contents($rotated))) {
    echo "FAIL: a gateway line still went into the rotated error_log\n";
    echo (string) @file_get_contents($rotated);
    exit(1);
}
echo "rotated-log-has-no-gateway-line: ok\n";

$tester->terminate();
$tester->close();

?>
--EXPECT--
master-reopened: ok
gateway-line-in-reopened-log: ok
rotated-log-has-no-gateway-line: ok
--CLEAN--
<?php
require_once "tester.inc";
FPM\Tester::clean();

foreach (glob(sys_get_temp_dir() . '/fpmng-http-log-reopen-*') as $docRoot) {
    @unlink("$docRoot/die.php");
    @rmdir($docRoot);
}

/* FPM\Tester::clean() only knows the extensions it created — the rotated copy
 * is ours (the test's files live next to the .phpt, see getPrefixedFile()). */
@unlink(__DIR__ . '/fpmng-http-gateway-log-reopen.err.log.rotated');
?>

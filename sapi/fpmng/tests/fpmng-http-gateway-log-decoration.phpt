--TEST--
fpm-ng: a gateway child's own line is decorated like a master line in the same error_log (issue #130)
--SKIPIF--
<?php
include "fpmng-skipif.inc";
fpmng_skip_if_pool_type_unsupported('http');
?>
--FILE--
<?php

require_once "tester.inc";

/* A fixed name, not one derived from getmypid(): --CLEAN-- runs in another
 * process and could not find a pid-suffixed directory to remove. */
$docRoot = sys_get_temp_dir() . '/fpmng-http-log-decoration';
@mkdir($docRoot, 0700, true);

/* The same provocation as fpmng-http-gateway-upstream-loss.phpt, for the same
 * reason: SIGKILL on the worker makes the gateway write a line of its own AND
 * the master write the child-exit line the gateway line tells the operator to
 * look for (issue #118). Both land in one error_log, which is exactly the
 * comparison this test is about. `kill` through exec() because CI configures
 * with --disable-all, so ext/posix is not built. */
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

$tester = new FPM\Tester($cfg);
/* A file, not the -O stderr pipe: the point of the test is what the shared
 * error_log looks like. */
@unlink($tester->getPrefixedFile(FPM\Tester::FILE_EXT_LOG_ERR));
$tester->start([], false);
$tester->switchLogSource('{{FILE:LOG}}');
$tester->expectLogStartNotices();

$errorLog = $tester->getPrefixedFile(FPM\Tester::FILE_EXT_LOG_ERR);
$http = $tester->getAddr('ipv4', '[http]');

/* "[dd-Mon-yyyy hh:mm:ss] LEVEL: " — zlog_print_time() plus the level name,
 * the decoration zlog() gives a line written by the master. */
$decorated = '/^\[\d\d-[A-Za-z]{3}-\d{4} \d\d:\d\d:\d\d\] WARNING: ';

$gatewayLine = $decorated . '\[pool web\] http: /m';
$masterLine  = $decorated . '\[pool web\] child \d+ exited on signal 9/m';

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

$ctx = stream_context_create(['http' => ['timeout' => 10, 'ignore_errors' => true]]);
@file_get_contents("http://$http/die.php", false, $ctx);

/* Waited for by the undecorated text first, so that a regression reports "the
 * line is there but has no timestamp" instead of timing out for ten seconds
 * and printing nothing useful. */
$log = logWait($errorLog, '/http: (no answer from|upstream )/');
if (!preg_match('/^.*http: (no answer from|upstream ).*$/m', $log, $m)) {
    echo "FAIL: the gateway logged nothing about the killed worker\n";
    echo $log;
    exit(1);
}
echo "gateway-line-present: ok\n";

if (!preg_match($gatewayLine, $log)) {
    echo "FAIL: the gateway line is not decorated: {$m[0]}\n";
    exit(1);
}
echo "gateway-line-decorated: ok\n";

/* The two processes are not ordered against each other (the master's line
 * comes from fpm_children_bury() after it reaps SIGCHLD), so this is waited
 * for rather than asserted on the snapshot above. */
$log = logWait($errorLog, $masterLine);
if (!preg_match($masterLine, $log)) {
    echo "FAIL: no decorated master child-exit line to compare against\n";
    echo $log;
    exit(1);
}
echo "master-line-decorated: ok\n";

$tester->terminate();
$tester->close();

?>
--EXPECT--
gateway-line-present: ok
gateway-line-decorated: ok
master-line-decorated: ok
--CLEAN--
<?php
require_once "tester.inc";
FPM\Tester::clean();

$docRoot = sys_get_temp_dir() . '/fpmng-http-log-decoration';
@unlink("$docRoot/die.php");
@rmdir($docRoot);
?>

--TEST--
fpm-ng: http.access_log carries the target pool as a trailing field (issue #341)
--SKIPIF--
<?php
include "fpmng-skipif.inc";
fpmng_skip_if_pool_type_unsupported('http');
?>
--FILE--
<?php

require_once "tester.inc";

/* Issue #341, requirement 1: once http.route[] lets one gateway serve several
 * pools, the access log line has to say WHICH pool answered -- a 503 on
 * /sse/* and a 503 on / were indistinguishable before this. The field is a
 * trailing "target=<pool>" (fpm_http_access_log.h documents the exact shape),
 * so this test also doubles as the format-stability check for everything
 * BEFORE that field: it still parses as plain Combined Log Format.
 *
 * One gateway, two prefixes: "/" falls through to the gateway's own pool
 * (targets[0], the implicit target http.route[] does not override) and
 * "/sse/..." is routed to the "events" pool. Both are targets fpm_http_route()
 * can return, so both get a real "target=<pool>" trailer -- "target=-" is
 * reserved for a gateway that never configured http.route[] at all (see
 * fpmng-http-gateway-no-route-unchanged.phpt) and for requests this gateway
 * answers without going through fpm_http_route() at all (ping, static, ACME,
 * an ACL rejection before routing runs). */
$docroot = sys_get_temp_dir() . '/fpmng-http-route-acclog-' . getmypid();
@mkdir($docroot, 0700, true);
file_put_contents($docroot . '/index.php', '<?php echo "web-ok";');

$config = <<<EOT
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
[web]
listen = {{ADDR}}
chdir = $docroot
pm = static
pm.max_children = 2
pool.type = http
http.gateways = 1
http.listen = {{ADDR[http]}}
http.front_controller = /index.php
http.access_log = {{FILE:LOG:ACC}}
http.route[events] = /sse

[events]
listen = {{ADDR[events]}}
pm = static
pm.max_children = 2
EOT;

$tester = new FPM\Tester($config, '<?php echo "events-ok";');
$tester->start();
$tester->expectLogStartNotices();

$httpAddr = $tester->getAddr('ipv4', '[http]');

function httpGet(string $url): void
{
    $ctx = stream_context_create(['http' => ['timeout' => 10, 'ignore_errors' => true]]);
    @file_get_contents($url, false, $ctx);
}

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

httpGet("http://$httpAddr/index.php?r=self");
httpGet("http://$httpAddr/sse/one?r=routed");

$accessLog = $tester->getPrefixedFile(FPM\Tester::FILE_EXT_LOG_ACC);
$content = fileWait($accessLog, '/r=routed/');

$selfLine = null;
$routedLine = null;
foreach (explode("\n", $content) as $line) {
    if (str_contains($line, 'r=self')) {
        $selfLine = $line;
    }
    if (str_contains($line, 'r=routed')) {
        $routedLine = $line;
    }
}

if ($selfLine === null || $routedLine === null) {
    echo "FAIL: missing access log line(s):\n$content\n";
    exit(1);
}

/* "/" fell through to the gateway's own pool ("web"), targets[0] -- a real
 * target, not the "no target at all" NULL case, so it gets its own name, not
 * "-". */
if (!preg_match('/ target=web$/', $selfLine)) {
    echo "FAIL: self-target line has no target=web trailer:\n$selfLine\n";
    exit(1);
}
echo "self-line: target=web\n";

if (!preg_match('/ target=events$/', $routedLine)) {
    echo "FAIL: routed line has no target=events trailer:\n$routedLine\n";
    exit(1);
}
echo "routed-line: target=events\n";

/* Everything before the trailing field is unchanged CLF -- the field is
 * appended, not inserted. */
if (!preg_match('/^\S+ - \S+ \[[^\]]+\] "GET \S+ HTTP\/\d\.\d" \d+ \d+ "[^"]*" "[^"]*" target=\S+$/', $routedLine)) {
    echo "FAIL: routed line does not match the expected CLF+target shape:\n$routedLine\n";
    exit(1);
}
echo "clf-shape: ok\n";

$tester->terminate();
$tester->close();

@unlink($docroot . '/index.php');
@rmdir($docroot);

?>
Done
--EXPECT--
self-line: target=web
routed-line: target=events
clf-shape: ok
Done
--CLEAN--
<?php
require_once "tester.inc";
FPM\Tester::clean();

foreach (glob(sys_get_temp_dir() . '/fpmng-http-route-acclog-*') as $docRoot) {
    @unlink("$docRoot/index.php");
    @rmdir($docRoot);
}
?>

--TEST--
fpm-ng: http.access_log carries duration_ms and upstream_ms as trailing fields, "-" when no target was used (issue #642)
--SKIPIF--
<?php
include "fpmng-skipif.inc";
fpmng_skip_if_pool_type_unsupported('gateway');
?>
--FILE--
<?php

require_once "tester.inc";

/* Issue #642: two trailing timing fields after target=. A request dispatched
 * to a pool prints a number for upstream_ms. A request the gateway answers
 * itself (ping.path here) never reaches a target, so it prints target=- and
 * upstream_ms=-. Both lines keep the classic Combined Log Format prefix
 * unchanged, and neither prints request_id= or queue_ms=: http.request_id is
 * off by default, and the pool is not http.pool_full_policy = wait. */
$docroot = sys_get_temp_dir() . '/fpmng-acclog-timing-' . getmypid();
@mkdir($docroot, 0700, true);
file_put_contents($docroot . '/index.php', '<?php echo "web-ok";');

$config = <<<EOT
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
[gw]
pool.type = gateway
listen = {{ADDR[http]}}
chdir = $docroot
http.gateways = 1
http.front_controller = /index.php
http.access_log = {{FILE:LOG:ACC}}
http.route[web] = /
ping.path = /ping
[web]
pool.type = fastcgi
listen = {{ADDR}}
chdir = $docroot
pm = static
pm.max_children = 2
EOT;

$tester = new FPM\Tester($config, '<?php echo "unused";');
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

httpGet("http://$httpAddr/index.php?r=routed");
httpGet("http://$httpAddr/ping?r=local");

$accessLog = $tester->getPrefixedFile(FPM\Tester::FILE_EXT_LOG_ACC);
$content = fileWait($accessLog, '/r=local/');

$routedLine = null;
$localLine = null;
foreach (explode("\n", $content) as $line) {
    if (str_contains($line, 'r=routed')) {
        $routedLine = $line;
    }
    if (str_contains($line, 'r=local')) {
        $localLine = $line;
    }
}

if ($routedLine === null || $localLine === null) {
    echo "FAIL: missing access log line(s):\n$content\n";
    exit(1);
}

if (!preg_match('/ target=web duration_ms=\d+ upstream_ms=\d+$/', $routedLine)) {
    echo "FAIL: routed line has no numeric timing trailer:\n$routedLine\n";
    exit(1);
}
echo "routed-line: duration_ms and upstream_ms are numbers\n";

if (!preg_match('/ target=- duration_ms=\d+ upstream_ms=-$/', $localLine)) {
    echo "FAIL: locally answered line does not print upstream_ms=-:\n$localLine\n";
    exit(1);
}
echo "local-line: upstream_ms=-\n";

if (str_contains($content, 'request_id=') || str_contains($content, 'queue_ms=')) {
    echo "FAIL: request_id= or queue_ms= printed with the default configuration:\n$content\n";
    exit(1);
}
echo "defaults: no request_id, no queue_ms\n";

/* The prefix before target= is unchanged CLF (#341 rule). */
foreach ([$routedLine, $localLine] as $line) {
    if (!preg_match('/^\S+ - \S+ \[[^\]]+\] "GET \S+ HTTP\/\d\.\d" \d+ \d+ "[^"]*" "[^"]*" target=/', $line)) {
        echo "FAIL: line does not keep the CLF prefix:\n$line\n";
        exit(1);
    }
}
echo "clf-prefix: ok\n";

$tester->terminate();
$tester->expectLogTerminatingNotices();
$tester->close();

@unlink("$docroot/index.php");
@rmdir($docroot);
echo "Done\n";
?>
--EXPECT--
routed-line: duration_ms and upstream_ms are numbers
local-line: upstream_ms=-
defaults: no request_id, no queue_ms
clf-prefix: ok
Done
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>

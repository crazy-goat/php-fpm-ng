--TEST--
fpm-ng: pool.type = gateway answers operator.status_path on the operator listener and not on the public one (issues #274, #388)
--SKIPIF--
<?php
include "fpmng-skipif.inc";
fpmng_skip_if_pool_type_unsupported('gateway');
?>
--FILE--
<?php

require_once "tester.inc";

/* The rule from #273: one directive, one page, one socket. On the gateway
 * (issue #388; pool.type = http before it) the status path names the gateway's
 * OWN page on the operator listener and nothing on the public port.
 *
 * The public half is shown with the front controller turned OFF and the path
 * routed to an ordinary fastcgi target: the same URL on the public port is an
 * ordinary request the application answers, not a second copy of the operator
 * page. With the front controller on, an unmatched path would fall back to it
 * and the application would answer there too; the point is the same.
 *
 * ping.path is checked in the same run for the opposite reason: #273, point 9,
 * leaves it on the public listener, answered by the gateway process itself
 * (#382). If moving the status page silently took ping with it, this is where
 * that shows up. */

$root = sys_get_temp_dir() . '/fpmng-operator-http-' . getmypid();
@mkdir($root, 0700, true);
file_put_contents($root . '/index.php', '<?php echo "app:", $_SERVER["REQUEST_URI"], "\n";');
/* A real script at the status path, so that "the public listener hands it to
 * the application" is shown by the application running, not by a 404 that a
 * missing file would produce either way. */
file_put_contents($root . '/gw-status.php', '<?php echo "app:", $_SERVER["REQUEST_URI"], "\n";');

$cfg = <<<EOT
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}

[gw]
pool.type = gateway
listen = {{ADDR[public]}}
chdir = $root
http.route[app] = /
http.front_controller =
operator.status_path = /gw-status.php
operator.status_listen = {{ADDR[operator]}}
ping.path = /gw-ping
ping.response = pong

[app]
pool.type = fastcgi
listen = {{ADDR}}
pm = static
pm.max_children = 2
chdir = $root
EOT;

$tester = new FPM\Tester($cfg, '<?php');
$tester->start();
$tester->expectLogStartNotices();

function httpGet(string $addr, string $path): string
{
    $parts = explode(':', $addr);
    $port = (int) array_pop($parts);
    $host = implode(':', $parts);
    $sock = @stream_socket_client("tcp://$host:$port", $errno, $errstr, 5);
    if (!$sock) {
        return "CONNECT FAILED: $errstr";
    }
    fwrite($sock, "GET $path HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
    $raw = stream_get_contents($sock);
    fclose($sock);
    return $raw;
}

$public = $tester->getListen('{{ADDR[public]}}');
$operator = $tester->getListen('{{ADDR[operator]}}');

/* The status path on the public listener is now an ordinary request: the front
 * controller answers it, and the reply is the application's. */
$body = httpGet($public, '/gw-status.php');
echo 'public status path -> ', str_contains($body, 'app:/gw-status.php') ? "the application\n" : "UNEXPECTED: $body\n";

/* ...and on the operator listener it is the per-pool JSON, reporting this pool
 * and no other. */
$body = httpGet($operator, '/gw-status.php');
$json = json_decode(substr($body, strpos($body, "\r\n\r\n") + 4), true);
$pools = $json['pools'] ?? [];
echo 'operator status path -> ', count($pools) === 1 && $pools[0]['name'] === 'gw' && $pools[0]['type'] === 'gateway'
    ? "json for pool gw, type gateway\n" : "UNEXPECTED: $body\n";

/* ping did not move. */
$body = httpGet($public, '/gw-ping');
echo 'public ping path -> ', str_contains($body, 'pong') ? "pong\n" : "UNEXPECTED: $body\n";

$tester->terminate();
$tester->close();

@unlink($root . '/gw-status.php');
@unlink($root . '/index.php');
@rmdir($root);

echo "Done\n";
?>
--EXPECT--
public status path -> the application
operator status path -> json for pool gw, type gateway
public ping path -> pong
Done
--CLEAN--
<?php
require_once "tester.inc";
FPM\Tester::clean();
?>

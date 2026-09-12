--TEST--
fpm-ng: pool.type = http answers pm.status_path on the operator listener and no longer on the public one (issue #274)
--SKIPIF--
<?php
include "fpmng-skipif.inc";
fpmng_skip_if_pool_type_unsupported('http');
?>
--FILE--
<?php

require_once "tester.inc";

/* The rule from #273: one directive, one page, one socket. On pool.type = http
 * that had two halves to enforce and only one of them is in the operator
 * endpoint's own code.
 *
 * The half this test exists for is the other one. Upstream's in-child handler
 * matches pm.status_path against SG(request_info).request_uri, which the
 * gateway fills from SCRIPT_NAME. With http.front_controller set -- the default
 * -- every unmatched path falls back to the front controller and SCRIPT_NAME is
 * never the request path, so the handler is invisible. With the fallback turned
 * OFF, as below, SCRIPT_NAME *is* the request path and the handler answers on
 * the pool's public listener. That is the configuration in which the same
 * directive used to name two different pages on two different sockets, so it is
 * the configuration worth pinning: the application sees the path now, and the
 * page is on the operator listener instead.
 *
 * ping.path is checked in the same run for the opposite reason: #273, point 9,
 * leaves it on the public listener, and it travels through the same in-child
 * handler the status page just left. If suppressing one silently took the other
 * with it, this is where that shows up. */

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
listen = {{ADDR}}
pool.type = http
pm = static
pm.max_children = 2
chdir = $root
http.listen = {{ADDR[public]}}
http.front_controller =
pm.status_path = /gw-status.php
pm.status_listen = {{ADDR[operator]}}
ping.path = /gw-ping
ping.response = pong
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
echo 'operator status path -> ', count($pools) === 1 && $pools[0]['name'] === 'gw' && $pools[0]['type'] === 'http'
    ? "json for pool gw, type http\n" : "UNEXPECTED: $body\n";

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
operator status path -> json for pool gw, type http
public ping path -> pong
Done
--CLEAN--
<?php
require_once "tester.inc";
FPM\Tester::clean();
?>

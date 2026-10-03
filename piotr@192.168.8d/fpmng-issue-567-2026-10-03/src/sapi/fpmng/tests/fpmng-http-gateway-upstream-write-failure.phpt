--TEST--
fpm-ng: HTTP gateway survives a synchronous write failure while handing a request to an upstream (issue #129)
--SKIPIF--
<?php
include "fpmng-skipif.inc";
fpmng_skip_if_pool_type_unsupported('gateway');
?>
--FILE--
<?php

require_once "tester.inc";

/* Issue #129: fpm_http_pump() handed the assembled request to an upstream and
 * then freed the buffer it had just handed over. A *synchronous* hard write
 * error takes the failure path all the way to fpm_http_conn_free(), so the
 * free on the way back released an already-released zend_string and read the
 * freed connection to find it.
 *
 * Two conditions make that window reachable from a test:
 *   - a UNIX-socket upstream: connect() completes immediately, so the first
 *     write happens inside fpm_http_pump() rather than in the write callback
 *     one loop iteration later;
 *   - http.fault_upstream_write = 1: the gateway's first write() towards the
 *     pool fails with ECONNRESET without touching the socket. Nothing outside
 *     the process can time that error onto the write instead of onto the
 *     following read.
 *
 * This test asserts the behaviour: the request that cannot be written is
 * answered 502, the gateway keeps serving, and no gateway process dies. It is
 * NOT the memory-safety proof -- the corruption is silent in a stock build
 * (the freed zend_string's refcount word holds a heap pointer by then, so the
 * release never reaches free() and glibc reports nothing). That proof is
 * build/test-http-gateway-write-failure.sh, which runs the same two rounds
 * under valgrind. */

/* A fixed name, not one derived from getmypid(): --CLEAN-- runs in another
 * process and could not find a pid-suffixed directory to remove. */
$docroot = sys_get_temp_dir() . '/fpmng-http-write-failure';
@mkdir($docroot, 0700, true);
file_put_contents("$docroot/served.php", '<?php echo "served";');

$config = <<<EOT
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
[gw]
pool.type = gateway
listen = {{ADDR[http]}}
chdir = $docroot
http.gateways = 1
http.fault_upstream_write = 1
http.route[gw_app] = /
[gw_app]
pool.type = fastcgi
listen = {{ADDR:UDS}}
pm = static
pm.max_children = 2
chdir = $docroot
EOT;

$tester = new FPM\Tester($config);
$tester->start();
$tester->expectLogStartNotices();

$script = '/served.php';
[$host, $port] = explode(':', $tester->getAddr('ipv4', '[http]'));

function httpGet(string $host, int $port, string $script): string
{
    $fp = @fsockopen($host, $port, $errno, $errstr, 5);
    if (!$fp) {
        return "CONNECT FAILED: $errstr ($errno)";
    }
    stream_set_timeout($fp, 5);
    fwrite($fp, "GET $script HTTP/1.1\r\nHost: $host\r\nConnection: close\r\n\r\n");
    $response = stream_get_contents($fp);
    fclose($fp);

    return $response;
}

/* Request 1 is the one whose write fails. It cannot be served, and 502 is the
 * documented answer for an upstream that gives no answer (fpm_http_finish());
 * what matters here is that the gateway is still alive afterwards. */
$first = httpGet($host, (int) $port, $script);
echo preg_match('#^HTTP/1\.[01] 502 #', $first) ? "first-request-502: ok\n"
    : "FAIL: expected 502 for the failed write, got:\n$first\n";

/* Requests 2 and 3 write successfully (the injected failure is the first
 * write only). They are the regression assertion: a gateway that corrupted
 * its heap on request 1 does not serve them. */
foreach ([2, 3] as $n) {
    $response = httpGet($host, (int) $port, $script);
    echo preg_match('#^HTTP/1\.[01] 200 #', $response) && str_contains($response, 'served')
        ? "request-$n-served: ok\n"
        : "FAIL: request $n was not served, got:\n$response\n";
}

$tester->expectLogPattern('/http: upstream .*Connection reset by peer/');

/* A dead gateway is respawned, and the requests above would then have been
 * served by its replacement -- so "still serving" is only an assertion once
 * the master reports no gateway death. */
$tester->expectNoLogPattern('/http gateway \d+ \(pid \d+\) (killed by signal|exited with code)/');

$tester->terminate();
$tester->close();

?>
--EXPECT--
first-request-502: ok
request-2-served: ok
request-3-served: ok
--CLEAN--
<?php
require_once "tester.inc";
FPM\Tester::clean();

$docroot = sys_get_temp_dir() . '/fpmng-http-write-failure';
@unlink("$docroot/served.php");
@rmdir($docroot);
?>

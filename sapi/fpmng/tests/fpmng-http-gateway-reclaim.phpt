--TEST--
FPM http gateway: an idle upstream held by a sibling is reclaimed instead of refused (issue #156, THROWAWAY)
--SKIPIF--
<?php
include "fpmng-skipif.inc";
fpmng_skip_if_pool_type_unsupported('http');
?>
--FILE--
<?php

require_once "tester.inc";

/* THROWAWAY test for the throwaway "reclaim before rejecting" policy of issue
 * #156. It belongs to the branch, not to main: the policy it exercises is a
 * spike deliverable that #54 puts out of scope for merging.
 *
 * The failure mode under test: with http.gateways = 2 the shared budget can
 * be held by an upstream that is merely IDLE in one gateway process while
 * another answers 503. Two concurrent slow requests below take the whole
 * budget (pm.max_children = 2) and then finish, leaving two idle upstreams
 * spread over the two processes. A request arriving at the process that holds
 * none of them can only be served if the other lets one go.
 *
 * http.reuseport = 1 on purpose: without it both gateways accept from one
 * shared listening socket and the kernel is free to wake the same one every
 * time, which would leave the cross-process case untested while the test
 * still passed. With it, the 4-tuple hash spreads the connections.
 */

$docroot = __DIR__;

putenv('FPMNG_GW_RECLAIM=1');
putenv('FPMNG_GW_RECLAIM_MS=50');

$config = <<<EOT
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
process_control_timeout = 5
[reclaim]
listen = {{ADDR[fastcgi]}}
pool.type = http
pm = static
pm.max_children = 2
chdir = $docroot
http.gateways = 2
http.reuseport = 1
http.listen = {{ADDR[http]}}
EOT;

$tester = new FPM\Tester($config, '<?php if (isset($_GET["slow"])) { sleep(4); echo "slow"; } else { echo "fast"; }');
$tester->start();
$tester->expectLogStartNotices();

$script = '/' . basename($tester->makeSourceFile());

$httpAddr = $tester->getAddr('ipv4', '[http]');
[$host, $port] = explode(':', $httpAddr);
$port = (int) $port;

function request(string $host, int $port, string $path): array
{
    $fp = fsockopen($host, $port, $errno, $errstr, 5);
    if (!$fp) {
        return ['', 0.0];
    }
    $start = microtime(true);
    fwrite($fp, "GET $path HTTP/1.1\r\nHost: $host\r\nConnection: close\r\n\r\n");
    $body = stream_get_contents($fp);
    fclose($fp);

    return [$body, microtime(true) - $start];
}

function open_slow(string $host, int $port, string $path)
{
    $fp = fsockopen($host, $port, $errno, $errstr, 5);
    if (!$fp) {
        echo "FAIL: connect: $errstr ($errno)\n";
        exit(1);
    }
    fwrite($fp, "GET $path HTTP/1.1\r\nHost: $host\r\nConnection: close\r\n\r\n");
    stream_set_blocking($fp, false);

    return $fp;
}

function finish(string $host, $fp): string
{
    stream_set_blocking($fp, true);
    $body = stream_get_contents($fp);
    fclose($fp);

    return $body;
}

/* Step 1: take the whole budget with two concurrent slow requests, then let
 * them finish. Both upstreams stay open and idle afterwards -- an idle
 * upstream still holds its worker and its budget slot, which is the whole
 * premise of the issue. */
$warm = [open_slow($host, $port, $script . '?slow=1'), open_slow($host, $port, $script . '?slow=1')];
foreach ($warm as $fp) {
    if (!str_contains(finish($host, $fp), 'slow')) {
        echo "FAIL: a warm-up request did not complete\n";
        exit(1);
    }
}

/* Step 2: one slow request in flight -- so at most one of the two budget
 * slots can be freed by its own gateway -- and fast requests alongside it.
 * Every one of them must be answered 200 well inside http.idle_timeout
 * (500 ms by default), which is the only other thing that would ever hand
 * that budget back. */
$slow = open_slow($host, $port, $script . '?slow=1');
usleep(500000);

for ($i = 0; $i < 8; $i++) {
    [$response, $elapsed] = request($host, $port, $script);
    if (!str_contains($response, ' 200 ') || !str_contains($response, 'fast')) {
        echo "FAIL: round $i was not served:\n$response\n";
        exit(1);
    }
    if ($elapsed > 0.2) {
        echo sprintf("FAIL: round %d took %.3f s; nothing was reclaimed in time\n", $i, $elapsed);
        exit(1);
    }
}

/* Criterion 4: the request in flight on a BUSY upstream is never disturbed by
 * a reclaim. Only idle upstreams are candidates, so this is a property of the
 * policy and not a coincidence of timing. */
if (!str_contains(finish($host, $slow), 'slow')) {
    echo "FAIL: the in-flight request was disturbed\n";
    exit(1);
}

/* Criterion 3: the reclaims are observable from outside the process -- here,
 * in the error log the operator already reads. Asserted over the whole run
 * because with two gateways some of the eight rounds above land on the
 * process that already holds an idle upstream and need no reclaim at all. */
$tester->expectLogPattern('/released an idle upstream on a sibling/', checkAllLogs: true);

$tester->terminate();
$tester->expectLogTerminatingNotices();
$tester->close();

?>
Done
--EXPECT--
Done
--CLEAN--
<?php
require_once "tester.inc";
FPM\Tester::clean();
?>

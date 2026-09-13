--TEST--
THROWAWAY (spike #54, issue #155): the gateway 'wait' pool-full policy, its queue cap and its wait bound
--SKIPIF--
<?php
include "fpmng-skipif.inc";
fpmng_skip_if_pool_type_unsupported('http');
if (!getenv('FPMNG_SPIKE155')) {
    die('skip throwaway spike test: set FPMNG_SPIKE155=1 to run it against a spike/155-wait-policy binary');
}
?>
--FILE--
<?php

require_once "tester.inc";

/* NOT FOR MERGE. This file exists to demonstrate the six acceptance criteria of
 * issue #155 against the throwaway binary built from spike/155-wait-policy, and
 * is deleted with the branch.
 *
 * The policy's knobs are environment variables (FPMNG_GW_WAIT_QUEUE_MAX,
 * FPMNG_GW_WAIT_MS, FPMNG_GW_WAIT_STATUS), so each case starts its own master
 * with its own environment -- which is also the demonstration of the last
 * criterion: both bounds are set per run, on one binary, with no rebuild
 * between the cases below.
 *
 * The script sleeps for as long as the query string asks. The request that
 * pins the only worker asks for 5 seconds; the ones that queue behind it ask
 * for none, so the elapsed time they report is the wait and nothing else. */
const SCRIPT = '<?php sleep((int) ($_GET["s"] ?? 0)); echo "slow";';

function makeTester(array $env): array
{
    $docroot = __DIR__;
    $config = <<<EOT
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
process_control_timeout = 5
[full]
listen = {{ADDR[fastcgi]}}
pool.type = http
pm = static
pm.max_children = 1
chdir = $docroot
http.gateways = 1
http.listen = {{ADDR[http]}}
EOT;
    $tester = new FPM\Tester($config, SCRIPT);
    $tester->start([], true, false, [], [], $env + getenv());
    $tester->expectLogStartNotices();
    $script = '/' . basename($tester->makeSourceFile());
    [$host, $port] = explode(':', $tester->getAddr('ipv4', '[http]'));

    return [$tester, $host, (int) $port, $script];
}

/* Send a request and do not wait for it: the caller needs it in flight. */
function fire(string $host, int $port, string $path): array
{
    $fp = fsockopen($host, $port, $errno, $errstr, 5);
    if (!$fp) {
        echo "FAIL: connect: $errstr ($errno)\n";
        exit(1);
    }
    fwrite($fp, "GET $path HTTP/1.1\r\nHost: $host\r\nConnection: close\r\n\r\n");
    stream_set_blocking($fp, false);

    return [$fp, microtime(true)];
}

function collect(array $pending): array
{
    [$fp, $start] = $pending;
    stream_set_blocking($fp, true);
    stream_set_timeout($fp, 30);
    $raw = '';
    while (!feof($fp)) {
        $chunk = fgets($fp);
        if ($chunk === false) {
            break;
        }
        $raw .= $chunk;
    }
    fclose($fp);
    preg_match('#^HTTP/1\.[01] (\d+)#', $raw, $m);
    preg_match('/^X-Fpmng-Queue-Wait:\s*(\d+)/mi', $raw, $w);

    return [
        'status'   => (int) ($m[1] ?? 0),
        'elapsed'  => microtime(true) - $start,
        'wait_ms'  => isset($w[1]) ? (int) $w[1] : null,
        'retry'    => (bool) preg_match('/^Retry-After:\s*\d+/mi', $raw),
        'body'     => $raw,
    ];
}

function within(float $v, float $lo, float $hi): string
{
    return $v >= $lo && $v <= $hi ? 'yes' : sprintf('no (%.2f)', $v);
}

/* ---- criteria 1 and 5: the queued request is served, the one in flight is
 *      not disturbed. ------------------------------------------------------ */
[$t, $host, $port, $script] = makeTester([
    'FPMNG_GW_WAIT_QUEUE_MAX' => '4',
    'FPMNG_GW_WAIT_MS'        => '20000',
]);
$slow = fire($host, $port, "$script?s=5");
usleep(500000);
$queued = collect(fire($host, $port, "$script?s=0"));
$first  = collect($slow);
echo "queued request: status=", $queued['status'],
    " elapsed_in_[4.0,8.0]=", within($queued['elapsed'], 4.0, 8.0),
    " body_ok=", var_export(str_contains($queued['body'], 'slow'), true), "\n";
echo "in-flight request still completed: ",
    var_export($first['status'] === 200 && str_contains($first['body'], 'slow'), true), "\n";

/* ---- criterion 4: the reported queue wait is the client's wall time. ----- */
$reported = $queued['wait_ms'];
$measured = $queued['elapsed'] * 1000;
echo "queue wait reported: ", $reported === null ? 'MISSING' : 'present',
    " within_10pct=", $reported === null ? 'n/a'
        : (abs($reported - $measured) <= 0.1 * $measured ? 'yes'
            : sprintf('no (reported %d ms, measured %.0f ms)', $reported, $measured)), "\n";
$t->terminate();
$t->close();

/* ---- criterion 2: with the cap at N, request N+2 is refused at once while
 *      2..N+1 are still queued. --------------------------------------------- */
[$t, $host, $port, $script] = makeTester([
    'FPMNG_GW_WAIT_QUEUE_MAX' => '2',
    'FPMNG_GW_WAIT_MS'        => '20000',
]);
$slow = fire($host, $port, "$script?s=5");
usleep(500000);
$q1 = fire($host, $port, "$script?s=0");
$q2 = fire($host, $port, "$script?s=0");
usleep(200000);
$over = collect(fire($host, $port, "$script?s=0"));
echo "over-cap request: status=", $over['status'],
    " retry_after=", var_export($over['retry'], true),
    " under_200ms=", within($over['elapsed'], 0.0, 0.2), "\n";
$r1 = collect($q1);
$r2 = collect($q2);
$f  = collect($slow);
echo "the two within the cap: ", $r1['status'], " ", $r2['status'], "\n";
echo "in-flight request still completed: ",
    var_export($f['status'] === 200 && str_contains($f['body'], 'slow'), true), "\n";
$t->terminate();
$t->close();

/* ---- criterion 3: a wait longer than T is answered within T + 500 ms, at two
 *      values of T. The status is FPMNG_GW_WAIT_STATUS; the spike's default is
 *      504, which is what this run records. ---------------------------------- */
foreach ([1000, 2500] as $T) {
    [$t, $host, $port, $script] = makeTester([
        'FPMNG_GW_WAIT_QUEUE_MAX' => '4',
        'FPMNG_GW_WAIT_MS'        => (string) $T,
    ]);
    $slow = fire($host, $port, "$script?s=5");
    usleep(500000);
    $expired = collect(fire($host, $port, "$script?s=0"));
    echo "wait bound T=", $T, "ms: status=", $expired['status'],
        " answered_within_T+500ms=", within($expired['elapsed'], $T / 1000, $T / 1000 + 0.5), "\n";
    $f = collect($slow);
    echo "in-flight request still completed: ",
        var_export($f['status'] === 200 && str_contains($f['body'], 'slow'), true), "\n";
    $t->terminate();
    $t->close();
}

?>
Done
--EXPECT--
queued request: status=200 elapsed_in_[4.0,8.0]=yes body_ok=true
in-flight request still completed: true
queue wait reported: present within_10pct=yes
over-cap request: status=503 retry_after=true under_200ms=yes
the two within the cap: 200 200
in-flight request still completed: true
wait bound T=1000ms: status=504 answered_within_T+500ms=yes
in-flight request still completed: true
wait bound T=2500ms: status=504 answered_within_T+500ms=yes
in-flight request still completed: true
Done
--CLEAN--
<?php
require_once "tester.inc";
FPM\Tester::clean();
?>

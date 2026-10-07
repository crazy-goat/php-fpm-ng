--TEST--
fpm-ng: HTTP gateway cuts an upstream that went silent (http.upstream_read_timeout): 504 before the head, the reply cut after it, a progressing upstream untouched (issue #716)
--SKIPIF--
<?php
include "fpmng-skipif.inc";
fpmng_skip_if_pool_type_unsupported('gateway');
?>
--FILE--
<?php

require_once "tester.inc";

/* Issue #716: the gateway bounded only its client side. A target that accepted the
 * request and then stopped producing held the client connection and one slot of
 * the target's admission budget for as long as it liked -- nothing in the gateway
 * ended it, and a FastCGI target only ends it when the operator set
 * request_terminate_timeout (0, unlimited, by default).
 *
 * The cases below are the whole contract of http.upstream_read_timeout:
 *
 *   [normal]    an ordinary answer, with the directives set  -> untouched;
 *   [silent]    a worker that produces nothing at all          -> 504, and the
 *                client connection stays usable;
 *   [progress]  a worker slower than the timeout but producing  -> untouched;
 *   [head]      a worker that sent the head and went silent    -> the reply is cut
 *                and not completed (issue #533's rule, which a timeout must not
 *                break by sending a clean end and lying about the body).
 *
 * pm.max_children is 3 with one worker stuck per case, so a gateway that kept the
 * connection (and its budget slot) would answer the next case 503 instead of
 * running it. Every case after [silent] is therefore also the assertion that the
 * slot came back while the stuck worker is still asleep. */

const READ_TIMEOUT_MS = 1500;
/* Comfortably inside READ_TIMEOUT_MS and comfortably outside it for [progress]
 * (four gaps of this size) and [silent] (one gap of 10 s). */
const GAP_US = 400 * 1000;
const SILENT_US = 10 * 1000 * 1000;

$docroot = sys_get_temp_dir() . '/fpmng-gw-upstream-read-timeout';
@mkdir($docroot, 0700, true);
/* Nothing at all, not even a header: the gateway sees a request it wrote in full
 * and never a byte back. */
file_put_contents("$docroot/silent.php", sprintf('<?php usleep(%d);', SILENT_US));
/* Slower than the timeout, never silent for longer than it. Each echo+flush is one
 * STDOUT record, so each is a read that re-arms the deadline. */
file_put_contents("$docroot/trickle.php", sprintf(
    '<?php header("Content-Type: text/plain"); for ($i = 1; $i <= 4; $i++) { echo "chunk$i "; flush(); usleep(%d); }',
    GAP_US));
/* The head first, then silence. Nothing can be said to the client once the head is
 * out, so the body can only be cut -- and the client must be able to tell. */
file_put_contents("$docroot/head.php", sprintf(
    '<?php header("Content-Type: text/plain"); echo "head-then-silence\n"; flush(); usleep(%d); echo "never-reached\n";',
    SILENT_US));
file_put_contents("$docroot/ok.php", '<?php echo "served";');

/* The heredoc interpolates variables, not constants, so the one directive that is
 * built from a constant above gets its value handed over as a variable. */
$readTimeout = READ_TIMEOUT_MS;

$config = <<<EOT
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
[gw]
pool.type = gateway
listen = {{ADDR[http]}}
chdir = $docroot
http.gateways = 1
http.upstream_connect_timeout = 3000
http.upstream_read_timeout = {$readTimeout}
http.route[web] = /
[web]
pool.type = fastcgi
listen = {{ADDR:UDS}}
pm = static
pm.max_children = 3
chdir = $docroot
EOT;

$tester = new FPM\Tester($config);
$tester->start();
$tester->expectLogStartNotices();
[$host, $port] = explode(':', $tester->getAddr('ipv4', '[http]'));

/* One request per connection, keep-alive (so a chunked reply is chunked and a cut
 * one is visibly unfinished). Returns the status line, the headers and the body,
 * plus the wall time the answer took. */
function request(string $host, int $port, string $path, float $timeout = 20.0): array
{
    $fp = @fsockopen($host, $port, $errno, $errstr, 5);
    if (!$fp) {
        echo "FAIL: connect: $errstr ($errno)\n";
        exit(1);
    }
    stream_set_timeout($fp, (int) $timeout);
    $started = microtime(true);
    fwrite($fp, "GET $path HTTP/1.1\r\nHost: $host\r\nConnection: keep-alive\r\n\r\n");

    $raw = '';
    $deadline = $started + $timeout;
    while (microtime(true) < $deadline) {
        $chunk = fread($fp, 65536);
        $meta = stream_get_meta_data($fp);
        if ($chunk === false || ($chunk === '' && $meta['timed_out'])) {
            break;      /* EOF or the client's own timeout: whatever arrived is all there is */
        }
        $raw .= $chunk;
        if (response_is_complete($raw)) {
            break;
        }
    }
    fclose($fp);
    $ms = (microtime(true) - $started) * 1000;

    $split = strpos($raw, "\r\n\r\n");
    $head = $split === false ? $raw : substr($raw, 0, $split);
    $body = $split === false ? '' : substr($raw, $split + 4);

    return ['head' => $head, 'body' => $body, 'ms' => $ms, 'raw' => $raw];
}

/* Enough of HTTP/1.1 to tell "the gateway finished this reply" from "the gateway
 * was cut off": Content-Length, or a chunked terminator. A reply that is neither
 * yet complete is one the timeout cut. */
function response_is_complete(string $raw): bool
{
    $split = strpos($raw, "\r\n\r\n");
    if ($split === false) {
        return false;
    }
    $head = substr($raw, 0, $split);
    if (stripos($head, "Transfer-Encoding: chunked") !== false) {
        return preg_match("/\r\n0\r\n\r\n$/", substr($raw, $split + 4)) === 1;
    }
    if (preg_match('/^Content-Length:\s*(\d+)/mi', $head, $m)) {
        return strlen(substr($raw, $split + 4)) >= (int) $m[1];
    }
    return false;   /* close-delimited: only EOF ends it, which is the client's read timeout */
}

function status_of(array $response): int
{
    return preg_match('#^HTTP/1\.[01] (\d{3})#', $response['head'], $m) ? (int) $m[1] : 0;
}

function check(bool $ok, string $what, array $response = []): void
{
    if ($ok) {
        echo "$what: ok\n";
        return;
    }
    echo "FAIL: $what\n";
    echo "status line: " . strtok($response['head'], "\r\n") . "\n";
    echo "took: " . round($response['ms']) . " ms\n";
    echo "head: " . str_replace("\r\n", ' | ', $response['head']) . "\n";
    echo "body: " . substr($response['body'], 0, 400) . "\n";
    exit(1);
}

/* ---- [normal]: an ordinary upstream is untouched -------------------------------- */

$normal = request($host, (int) $port, '/ok.php');
check(status_of($normal) === 200 && str_contains($normal['body'], 'served'), 'normal', $normal);

/* ---- [silent]: nothing at all comes back ---------------------------------------- */

$silent = request($host, (int) $port, '/silent.php');
check(status_of($silent) === 504, 'silent-504', $silent);
/* Not answered by the worker (which is asleep for another ~8 s) and not answered
 * instantly either: the lower bound is well under READ_TIMEOUT_MS so a gateway
 * that cut on some other signal still passes, the upper one leaves room for a
 * loaded machine but is far below the worker's silence. */
check($silent['ms'] >= READ_TIMEOUT_MS * 0.6 && $silent['ms'] < READ_TIMEOUT_MS + 5000,
    'silent-cut-at-the-timeout', $silent);
/* The reply is complete: a 504 that itself got cut would be indistinguishable from
 * the case below on the wire, and would not be the 504 this issue promises. */
check(response_is_complete($silent['raw']), 'silent-504-is-complete', $silent);

/* The budget slot came back while the stuck worker is still asleep (3 children,
 * one of them is the silent one): this request is served, not answered 503. */
$after = request($host, (int) $port, '/ok.php');
check(status_of($after) === 200 && str_contains($after['body'], 'served'), 'budget-returned', $after);

/* ---- [progress]: slower than the timeout, never silent for longer ---------------- */

$progress = request($host, (int) $port, '/trickle.php');
check(status_of($progress) === 200 && substr_count($progress['body'], 'chunk') === 4,
    'progress-untouched', $progress);

/* ---- [head]: the head is out, then silence -------------------------------------- */

$head = request($host, (int) $port, '/head.php');
check(status_of($head) === 200, 'head-status-200', $head);
check(str_contains($head['body'], 'head-then-silence'), 'head-body-arrived', $head);
check(!str_contains($head['body'], 'never-reached'), 'head-body-cut', $head);
/* The chunked terminator is what would tell the client the body ended; its absence
 * is how the client learns the body is short. */
check(!preg_match("/\r\n0\r\n\r\n$/", $head['raw']), 'head-not-terminated', $head);

$tester->expectLogPattern('/http: upstream .* made no progress within http\.upstream_read_timeout/');
$tester->expectLogPattern('/http: upstream .* timed out after the response head was sent/');
/* A gateway that died handling its own timer would be respawned and would serve
 * the requests above anyway, so "still serving" is only an assertion once the
 * master reports no gateway death. */
$tester->expectNoLogPattern('/http gateway \d+ \(pid \d+\) (killed by signal|exited with code)/');

$tester->terminate();
$tester->close();

?>
--EXPECT--
normal: ok
silent-504: ok
silent-cut-at-the-timeout: ok
silent-504-is-complete: ok
budget-returned: ok
progress-untouched: ok
head-status-200: ok
head-body-arrived: ok
head-body-cut: ok
head-not-terminated: ok
--CLEAN--
<?php
require_once "tester.inc";
FPM\Tester::clean();

$docroot = sys_get_temp_dir() . '/fpmng-gw-upstream-read-timeout';
foreach (['silent.php', 'trickle.php', 'head.php', 'ok.php'] as $f) {
    @unlink("$docroot/$f");
}
@rmdir($docroot);
?>
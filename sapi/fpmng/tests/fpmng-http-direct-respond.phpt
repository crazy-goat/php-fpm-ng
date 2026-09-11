--TEST--
fpm-ng: fpmng_respond() finishes the response and lets the script keep running (issue #57)
--SKIPIF--
<?php
include "skipif.inc";
?>
--FILE--
<?php
require_once "tester.inc";
function verify(bool $ok, string $message): void { if (!$ok) throw new RuntimeException($message); }

$root = __DIR__;
$tag = getmypid();
$script = "/fpmng-http-direct-respond-front-$tag.php";
$marker = "$root/fpmng-http-direct-respond-marker-$tag";
file_put_contents($root . $script, '<?php $marker = ' . var_export($marker, true) . ';' . <<<'PHP'
$case = $_GET['case'] ?? 'plain';
if ($case === 'plain') {
    echo 'plain-body';
    return;
}
if ($case === 'after') {
    echo 'after-body';
    fpmng_respond();
    // Observable only if the script really keeps running once the response is
    // finished.
    file_put_contents($marker, 'ran-after-respond');
    return;
}
if ($case === 'sealed') {
    echo 'sealed-body';
    fpmng_respond();
    echo 'THIS-MUST-NOT-REACH-THE-CLIENT';
    return;
}
if ($case === 'twice') {
    echo 'twice-body';
    $first = fpmng_respond();
    $second = fpmng_respond();
    file_put_contents($marker, var_export($first, true) . '|' . var_export($second, true));
    return;
}
if ($case === 'fatal') {
    echo 'fatal-body';
    fpmng_respond();
    file_put_contents($marker, 'before-fatal');
    strlen(); // ArgumentCountError, uncaught: a fatal after the response is sent
    return;
}
if ($case === 'early') {
    echo 'early-body';
    fpmng_respond();
    file_put_contents($marker, 'sleeping');
    usleep(1500000);
    return;
}
PHP);

function readCrlfLine($fp): string
{
    $line = '';
    while (!str_ends_with($line, "\r\n")) {
        $c = fread($fp, 1);
        verify($c !== '' && $c !== false, 'connection closed mid-message');
        $line .= $c;
    }
    return $line;
}

// One request per connection, so each case is framed on its own. Reads exactly
// one message and stops at its end rather than at EOF: the point of
// fpmng_respond() is that the response is complete while the script still runs,
// and the connection does not close until the script ends, so a read to EOF
// would measure the script and not the response. Returns
// [status, body, elapsed ms].
function fetch(string $addr, string $case): array
{
    [$host] = explode(':', $addr);
    $started = microtime(true);
    $fp = stream_socket_client("tcp://$addr", $errno, $error, 5);
    verify((bool) $fp, "connect: $error ($errno)");
    stream_set_timeout($fp, 10);
    fwrite($fp, "GET /?case=$case HTTP/1.1\r\nHost: $host\r\nConnection: close\r\n\r\n");

    $head = '';
    while (!str_contains($head, "\r\n\r\n")) {
        $c = fread($fp, 1);
        verify($c !== '' && $c !== false, 'connection closed inside the headers');
        $head .= $c;
    }
    preg_match('~^HTTP/1\.\d (\d+)~', $head, $m);
    $status = (int) ($m[1] ?? 0);

    $body = '';
    if (preg_match('~^Transfer-Encoding:\s*chunked~mi', $head)) {
        while (true) {
            $size = hexdec(trim(readCrlfLine($fp)));
            if ($size === 0) {
                readCrlfLine($fp); // the trailer-terminating CRLF
                break;
            }
            while (strlen($body) < $size) {
                $chunk = fread($fp, $size - strlen($body));
                verify($chunk !== '' && $chunk !== false, 'connection closed inside a chunk');
                $body .= $chunk;
            }
            readCrlfLine($fp);
        }
    } elseif (preg_match('~^Content-Length:\s*(\d+)~mi', $head, $m)) {
        while (strlen($body) < (int) $m[1]) {
            $chunk = fread($fp, (int) $m[1] - strlen($body));
            verify($chunk !== '' && $chunk !== false, 'connection closed inside the body');
            $body .= $chunk;
        }
    } else {
        verify(false, 'the response is framed neither by Content-Length nor as chunked');
    }
    $ms = (microtime(true) - $started) * 1000;
    fclose($fp);
    return [$status, $body, $ms];
}

$port = (int) (getenv('FPMNG_DIRECT_TEST_PORT') ?: 28054) + 13;
$streamPort = $port + 1;
$cfg = <<<CFG
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
process_control_timeout = 3
[direct]
listen = 127.0.0.1:$port
pool.type = http-direct
pm = static
pm.max_children = 1
chdir = $root
http.front_controller = $script
[stream]
listen = 127.0.0.1:$streamPort
pool.type = http-direct
pm = static
pm.max_children = 1
chdir = $root
http.front_controller = $script
http.stream = yes
http.stream_write_timeout = 5000
CFG;

$addr = "127.0.0.1:$port";
$streamAddr = "127.0.0.1:$streamPort";
$tester = new FPM\Tester($cfg, '<?php');
try {
    $tester->start();
    $tester->expectLogStartNotices();

    [$status, $body] = fetch($addr, 'after');
    verify($status === 200 && $body === 'after-body', "after: got $status '$body'");
    verify(@file_get_contents($marker) === 'ran-after-respond', 'the script did not run past fpmng_respond()');
    @unlink($marker);
    echo "keeps-running: ok\n";

    // Everything written after the call is discarded rather than appended to a
    // response whose framing is already decided.
    [$status, $body] = fetch($addr, 'sealed');
    verify($status === 200 && $body === 'sealed-body', "sealed: got $status '$body'");
    echo "sealed: ok\n";

    [$status, $body] = fetch($addr, 'twice');
    verify($status === 200 && $body === 'twice-body', "twice: got $status '$body'");
    verify(@file_get_contents($marker) === 'true|false', 'second call did not report that it did nothing');
    @unlink($marker);
    echo "second-call: ok\n";

    // A fatal error after the response must not damage the response, and must
    // not take the worker with it for the next request.
    [$status, $body] = fetch($addr, 'fatal');
    verify($status === 200 && $body === 'fatal-body', "fatal: got $status '$body'");
    verify(@file_get_contents($marker) === 'before-fatal', 'the script did not reach the code before the fatal');
    @unlink($marker);
    [$status, $body] = fetch($addr, 'plain');
    verify($status === 200 && $body === 'plain-body', "after fatal: got $status '$body'");
    echo "fatal-after-respond: ok\n";

    // Keep-alive framing: a finished-early response followed by an ordinary one
    // on the same connection, both byte-exact.
    $fp = stream_socket_client("tcp://$addr", $errno, $error, 5);
    verify((bool) $fp, "keep-alive connect: $error ($errno)");
    stream_set_timeout($fp, 10);
    $bodies = [];
    foreach (['sealed', 'plain'] as $i => $case) {
        $close = $i === 1 ? 'close' : 'keep-alive';
        fwrite($fp, "GET /?case=$case HTTP/1.1\r\nHost: test\r\nConnection: $close\r\n\r\n");
        $head = '';
        while (!str_contains($head, "\r\n\r\n")) {
            $chunk = fread($fp, 1);
            verify($chunk !== '' && $chunk !== false, 'keep-alive: connection closed inside headers');
            $head .= $chunk;
        }
        preg_match('~Content-Length: (\d+)~i', $head, $m);
        verify(isset($m[1]), 'keep-alive: no Content-Length, framing is not byte-exact');
        $bodies[] = (int) $m[1] === 0 ? '' : fread($fp, (int) $m[1]);
    }
    fclose($fp);
    verify($bodies === ['sealed-body', 'plain-body'], 'keep-alive bodies: ' . implode('|', $bodies));
    echo "keep-alive-framing: ok\n";

    // The buffered pool, which is the default one: the response is complete and
    // on the wire while the script still has 1500 ms of work left. Without the
    // push in fpm_direct_send_buffered() the reply would only be queued, and
    // the event loop that writes it cannot run until the script returns.
    @unlink($marker);
    [$status, $body, $ms] = fetch($addr, 'early');
    verify($status === 200 && $body === 'early-body', "buffered early: got $status '$body'");
    verify($ms < 700, sprintf('the buffered response waited %.0f ms for the script', $ms));
    echo "buffered-early: ok\n";

    // The case the feature exists for: on a streaming pool the client has the
    // whole response while the script is still running. The script sleeps 1500
    // ms after the call, so anything near that means the response waited for it.
    @unlink($marker);
    [$status, $body, $ms] = fetch($streamAddr, 'early');
    verify($status === 200 && $body === 'early-body', "early: got $status '$body'");
    verify($ms < 700, sprintf('the streamed response waited %.0f ms for the script', $ms));
    verify(@file_get_contents($marker) === 'sleeping', 'the streaming script did not run past fpmng_respond()');
    echo "streamed-early: ok\n";

    $tester->terminate();
    $tester->expectLogTerminatingNotices();
    $tester->close();
} finally {
    foreach ([$root . $script, $marker] as $path) @unlink($path);
}
echo "Done\n";
?>
--EXPECT--
keeps-running: ok
sealed: ok
second-call: ok
fatal-after-respond: ok
keep-alive-framing: ok
buffered-early: ok
streamed-early: ok
Done
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>

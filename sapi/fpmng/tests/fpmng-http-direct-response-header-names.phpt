--TEST--
fpm-ng: direct HTTP refuses a malformed response header name instead of writing it to the wire
--SKIPIF--
<?php include "skipif.inc"; ?>
--FILE--
<?php
require_once "tester.inc";
/* Issue #102. header() rejects only CR, LF and NUL in the whole line
 * (main/SAPI.c:758-773), so a name with a space, a tab or no name at all
 * reaches the SAPI, and evhttp_add_header() stores it verbatim. Measured on
 * php-8.5.9 before the fix: " Lead: ws" and ": novalue" were emitted as
 * response header lines. The worker executor already answered 500 for such a
 * name (fpmng-http-direct-worker.phpt); this is the same contract on the
 * classic transport, from the same check. */
$root = __DIR__;
$script = '/fpmng-direct-hdrname-front-' . getmypid() . '.php';
file_put_contents($root . $script, <<<'PHP'
<?php
$bad = [
    'space' => 'X Y: sp',
    'tab'   => "X\tTab: tab",
    'empty' => ': novalue',
    'paren' => 'X(a): paren',
    'at'    => 'X@Y: at',
    'escape' => "X\x1b[1;31mY: esc",
];
$which = $_GET['h'] ?? '';
if (isset($bad[$which])) {
    header($bad[$which]);
} elseif ($which === 'ok') {
    header('X-Fine: ok');
} elseif ($which === 'framing') {
    /* A dropped framing header must stay silently dropped, not become a 500:
     * the name is a perfectly good token, so the two checks must not be
     * confused with one another. */
    header('Content-Length: 999');
    header('Transfer-Encoding: chunked');
}
echo 'controller';
PHP);
$port = (int) (getenv('FPMNG_DIRECT_TEST_PORT') ?: 28054) + 11;
/* catch_workers_output is required to see the warning: it is emitted by the
 * CHILD, whose zlog fd is closed at fpm_stdio_init_child() and falls back to
 * stderr, and without this the child's stderr is discarded and the log stays
 * empty (issue #73). */
$cfg = <<<CFG
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
[hdrname]
listen = 127.0.0.1:$port
pool.type = http-direct
pm = static
pm.max_children = 1
catch_workers_output = yes
chdir = $root
http.front_controller = $script
CFG;
/* Returns [status, [header lines], body] read off the wire, so a malformed
 * header line can be looked for as a line and not through a client that has
 * already normalised it away. */
function fetchRaw($fp, string $path): array
{
    fwrite($fp, "GET $path HTTP/1.1\r\nHost: test\r\n\r\n");
    $line = fgets($fp);
    if (!$line || !preg_match('{^HTTP/1\.1 (\d{3}) }', $line, $m)) {
        throw new RuntimeException('bad response framing: ' . var_export($line, true));
    }
    $status = (int) $m[1];
    $headers = [];
    $length = 0;
    while (($line = fgets($fp)) !== false && $line !== "\r\n") {
        $headers[] = rtrim($line, "\r\n");
        if (preg_match('/^Content-Length: (\d+)/i', $line, $m)) {
            $length = (int) $m[1];
        }
    }
    $body = '';
    while (strlen($body) < $length) {
        $chunk = fread($fp, $length - strlen($body));
        if ($chunk === false || $chunk === '') {
            throw new RuntimeException('short body');
        }
        $body .= $chunk;
    }
    return [$status, $headers, $body];
}
function check(bool $ok, string $message): void
{
    if (!$ok) {
        throw new RuntimeException($message);
    }
}
$tester = new FPM\Tester($cfg, '<?php');
try {
    $tester->start();
    $tester->expectLogStartNotices();
    $fp = stream_socket_client("tcp://127.0.0.1:$port", $errno, $error, 5);
    stream_set_timeout($fp, 5);

    [$status, $headers, $body] = fetchRaw($fp, '/?h=ok');
    check($status === 200, "a valid header name did not get 200, got $status");
    check(in_array('X-Fine: ok', $headers, true), 'valid header missing: ' . json_encode($headers));
    check($body === 'controller', "unexpected body: $body");
    echo "valid-name-passes: ok\n";

    foreach ([
        'space'  => 'X Y',
        'tab'    => "X\tTab",
        'empty'  => '',
        'paren'  => 'X(a)',
        'at'     => 'X@Y',
        'escape' => "X\x1b[1;31mY",
    ] as $key => $name) {
        [$status, $headers, $body] = fetchRaw($fp, "/?h=$key");
        check($status === 500, "malformed name '$name' did not get 500, got $status");
        check($body === "http-direct: malformed response header name\n",
            "unexpected 500 body for '$name': " . var_export($body, true));
        foreach ($headers as $line) {
            check(!str_starts_with($line, "$name:"),
                "malformed header reached the wire for '$key': " . var_export($line, true));
        }
        /* The whole point of answering 500 rather than emitting the line: the
         * connection has to stay usable, which is what a corrupted header
         * block would break. */
        [$status, , $body] = fetchRaw($fp, '/?h=ok');
        check($status === 200 && $body === 'controller',
            "the connection did not survive a refused header ('$key')");
    }
    echo "malformed-names-refused: ok\n";

    [$status, $headers, $body] = fetchRaw($fp, '/?h=framing');
    check($status === 200, "a dropped framing header became $status");
    check($body === 'controller', "unexpected body: $body");
    check(count(preg_grep('/^Content-Length: 999$/', $headers)) === 0,
        'application Content-Length reached the wire: ' . json_encode($headers));
    check(count(preg_grep('/^Transfer-Encoding:/i', $headers)) === 0,
        'application Transfer-Encoding reached the wire: ' . json_encode($headers));
    echo "framing-headers-still-dropped: ok\n";
    fclose($fp);

    /* An operator who sees the 500 has to be able to find out which header
     * caused it; the response body deliberately does not name it. */
    $tester->expectLogPattern(
        '/WARNING: .*\[pool hdrname\] http-direct: response header name is not an HTTP token, '
            . "answering 500: 'X\\\\x20Y'/",
        true
    );
    echo "bad-name-is-logged: ok\n";

    /* A name that is not a token can carry anything header() does not filter,
     * ESC included, and the log is read in a terminal. It is escaped, not
     * dropped, so the operator can still recognise the header. */
    $tester->expectLogPattern(
        '/answering 500: \'X\\\\x1b\[1;31mY\'/',
        true
    );
    echo "bad-name-is-escaped: ok\n";
} finally {
    $tester->terminate();
    $tester->close();
    @unlink($root . $script);
}
?>
--EXPECT--
valid-name-passes: ok
malformed-names-refused: ok
framing-headers-still-dropped: ok
bad-name-is-logged: ok
bad-name-is-escaped: ok
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>

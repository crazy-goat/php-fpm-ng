--TEST--
fpm-ng: http.access_format = json writes one JSON object per line with a fixed key set and valid escaping (issue #642)
--SKIPIF--
<?php
include "fpmng-skipif.inc";
fpmng_skip_if_pool_type_unsupported('gateway');
?>
--FILE--
<?php

require_once "tester.inc";

/* Issue #642: http.access_format = json. Every line is one JSON object with
 * the same keys in the same order. An unknown value is null, not "-". A quote,
 * a backslash and a control byte are escaped. A valid UTF-8 sequence is copied
 * as it is, and a byte that is not valid UTF-8 is written as \u00XX. The routed
 * request has a target and numeric timings. The ping request is answered by the
 * gateway itself, so its target, upstream_ms and queue_ms are null. */
$docroot = sys_get_temp_dir() . '/fpmng-acclog-json-' . getmypid();
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
http.access_format = json
http.request_id = generate
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

/* A raw request: a stream wrapper would refuse or rewrite the control byte
 * and the invalid UTF-8 byte that the header values carry on purpose. */
function rawGet(string $addr, string $target, array $headers): void
{
    $fp = stream_socket_client("tcp://$addr", $errno, $errstr, 10);
    if ($fp === false) {
        echo "FAIL: cannot connect: $errstr\n";
        exit(1);
    }
    $req = "GET $target HTTP/1.1\r\nHost: test\r\nConnection: close\r\n";
    foreach ($headers as $name => $value) {
        $req .= "$name: $value\r\n";
    }
    fwrite($fp, $req . "\r\n");
    stream_get_contents($fp);
    fclose($fp);
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

rawGet($httpAddr, '/index.php?r=routed', [
    'Referer' => 'http://ex.test/"q"\\back',
    'User-Agent' => "caf\xc3\xa9\tT\x01\xff",
]);
rawGet($httpAddr, '/ping?r=local', ['User-Agent' => 'ping-client']);

$accessLog = $tester->getPrefixedFile(FPM\Tester::FILE_EXT_LOG_ACC);
$content = fileWait($accessLog, '/r=local/');

$lines = array_values(array_filter(explode("\n", $content), fn($l) => $l !== ''));
if (count($lines) !== 2) {
    echo "FAIL: expected one line per request (2), got " . count($lines) . ":\n$content\n";
    exit(1);
}

$keys = ['time', 'remote_addr', 'remote_user', 'method', 'uri', 'protocol', 'status', 'bytes',
    'referer', 'user_agent', 'target', 'duration_ms', 'upstream_ms', 'queue_ms', 'request_id'];

$routed = null;
$local = null;
foreach ($lines as $line) {
    /* The /u modifier fails on invalid UTF-8, so '//u' checks the encoding. */
    if (preg_match('/[\x00-\x08\x0b\x0c\x0e-\x1f]/', $line) || !preg_match('//u', $line)) {
        echo "FAIL: line is not clean UTF-8 text:\n$line\n";
        exit(1);
    }
    $obj = json_decode($line, true);
    if (!is_array($obj)) {
        echo "FAIL: line is not a JSON object:\n$line\n";
        exit(1);
    }
    if (array_keys($obj) !== $keys) {
        echo "FAIL: keys are not the fixed set in order:\n$line\n";
        exit(1);
    }
    if ($obj['uri'] === '/index.php?r=routed') {
        $routed = $obj;
    } elseif ($obj['uri'] === '/ping?r=local') {
        $local = $obj;
    }
}
echo "json-keys: fixed order on both lines\n";

if ($routed === null || $local === null) {
    echo "FAIL: missing the routed or the ping line:\n$content\n";
    exit(1);
}

$timeFormat = '/^\d{2}\/[A-Z][a-z]{2}\/\d{4}:\d{2}:\d{2}:\d{2} [+-]\d{4}$/';
$hexId = '/^[0-9a-f]{32}$/';

if ($routed['target'] !== 'web' || $routed['protocol'] !== 'HTTP/1.1' || $routed['status'] !== 200
    || $routed['remote_addr'] !== '127.0.0.1' || $routed['method'] !== 'GET' || $routed['remote_user'] !== null
    || !is_int($routed['bytes']) || $routed['bytes'] <= 0 || !preg_match($timeFormat, $routed['time'])
    || !is_int($routed['duration_ms']) || $routed['duration_ms'] < 0 || !is_int($routed['upstream_ms'])
    || $routed['queue_ms'] !== null) {
    echo "FAIL: routed line has an unexpected value:\n" . json_encode($routed) . "\n";
    exit(1);
}
echo "routed-line: target=web, numeric timings, queue_ms null\n";

if ($routed['referer'] !== 'http://ex.test/"q"\\back') {
    echo "FAIL: referer does not decode to the sent bytes: " . json_encode($routed['referer']) . "\n";
    exit(1);
}
/* \xff is not UTF-8, so it comes back as U+00FF, two bytes C3 BF. */
if ($routed['user_agent'] !== "caf\xc3\xa9\tT\x01\xc3\xbf") {
    echo "FAIL: user_agent does not decode as expected: " . json_encode($routed['user_agent']) . "\n";
    exit(1);
}
echo "routed-line: quote, backslash, control byte and UTF-8 round-trip; invalid byte is U+00FF\n";

if ($local['target'] !== null || $local['upstream_ms'] !== null || $local['queue_ms'] !== null
    || $local['status'] !== 200 || !is_int($local['duration_ms'])) {
    echo "FAIL: ping line must have null target, upstream_ms and queue_ms:\n" . json_encode($local) . "\n";
    exit(1);
}
echo "local-line: target, upstream_ms and queue_ms are null\n";

if (!preg_match($hexId, (string) $routed['request_id']) || !preg_match($hexId, (string) $local['request_id'])) {
    echo "FAIL: request_id is not 32 lowercase hex characters\n";
    exit(1);
}
echo "request-id: 32 lowercase hex characters on both lines\n";

$tester->terminate();
$tester->expectLogTerminatingNotices();
$tester->close();

@unlink("$docroot/index.php");
@rmdir($docroot);
echo "Done\n";
?>
--EXPECT--
json-keys: fixed order on both lines
routed-line: target=web, numeric timings, queue_ms null
routed-line: quote, backslash, control byte and UTF-8 round-trip; invalid byte is U+00FF
local-line: target, upstream_ms and queue_ms are null
request-id: 32 lowercase hex characters on both lines
Done
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>

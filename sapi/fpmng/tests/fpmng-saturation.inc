<?php
/**
 * Driving a pool into saturation from a test (issue #644).
 *
 * The master samples the listen queue and checks pm.max_children on its
 * heartbeat, about once a second, so every check here polls. A pool with
 * pm.max_children = 1 is made busy by one connection that its child accepts and
 * does not finish. A second connection then waits in the kernel's listen
 * backlog, where TCP_INFO sees it.
 */

/* Opens a connection to $address. When $request is given it is written at
 * once. Without a request the child still accepts the connection and then waits
 * for the request headers, which keeps the child busy. The caller closes the
 * stream. */
function fpmng_saturation_connect(string $address, ?string $request = null)
{
    $fp = stream_socket_client("tcp://$address", $errno, $error, 5);
    if (!$fp) {
        throw new RuntimeException("connect $address: $error");
    }
    if ($request !== null) {
        fwrite($fp, $request);
    }
    return $fp;
}

/* A FastCGI request for $script, sent in full, the answer not read. The child
 * runs the script and is busy until it returns. A bare connection is not enough
 * for fastcgi: the child gives up on a connection that never sends a request,
 * and the pool goes idle again. Records follow the FastCGI 1.0 layout: BEGIN_REQUEST,
 * PARAMS, an empty PARAMS and an empty STDIN, all on request id 1. The caller
 * closes the stream. */
function fpmng_saturation_fcgi_start(string $address, string $script)
{
    $length = static fn(string $s): string => strlen($s) < 128
        ? pack('C', strlen($s))
        : pack('N', strlen($s) | 0x80000000);
    $params = [
        'GATEWAY_INTERFACE' => 'FastCGI/1.0',
        'REQUEST_METHOD' => 'GET',
        'SCRIPT_FILENAME' => $script,
        'SCRIPT_NAME' => '/' . basename($script),
        'REQUEST_URI' => '/' . basename($script),
        'SERVER_PROTOCOL' => 'HTTP/1.1',
        'SERVER_NAME' => 'test',
        'SERVER_PORT' => '80',
        'REMOTE_ADDR' => '127.0.0.1',
        'REMOTE_PORT' => '1',
    ];
    $pairs = '';
    foreach ($params as $name => $value) {
        $pairs .= $length($name) . $length($value) . $name . $value;
    }
    $record = static fn(int $type, string $body): string
        => pack('CCnnCC', 1, $type, 1, strlen($body), 0, 0) . $body;

    $fp = fpmng_saturation_connect($address);
    fwrite($fp, $record(1, pack('nCx5', 1, 0))   /* BEGIN_REQUEST, responder, keep-conn off */
        . $record(4, $pairs)                     /* PARAMS */
        . $record(4, '')                         /* end of PARAMS */
        . $record(5, ''));                       /* end of STDIN */
    return $fp;
}

/* One request on its own connection, read to the end. Blocks until the
 * script has answered. Returns the raw response. */
function fpmng_saturation_get(string $address, string $path): string
{
    $fp = fpmng_saturation_connect($address, "GET $path HTTP/1.1\r\nHost: test\r\nConnection: close\r\n\r\n");
    stream_set_timeout($fp, 30);
    $raw = (string) stream_get_contents($fp);
    fclose($fp);
    return $raw;
}

/* Polls $probe every 200 ms until it returns true, for at most 15 seconds. On a
 * timeout, $describe (when given) says what the page showed at the end. */
function fpmng_saturation_wait(callable $probe, string $what, ?callable $describe = null): void
{
    $deadline = microtime(true) + 15.0;
    do {
        if ($probe()) {
            return;
        }
        usleep(200000);
    } while (microtime(true) < $deadline);
    throw new RuntimeException("timed out waiting for $what" . ($describe ? "\n" . $describe() : ''));
}

/* The value of one Prometheus series, for example
 * 'fpmng_pool_listen_queue{pool="app"}', or null when the page has no such line. */
function fpmng_saturation_value(string $body, string $series): ?float
{
    return preg_match('/^' . preg_quote($series, '/') . ' (\S+)$/m', $body, $m) ? (float) $m[1] : null;
}

function fpmng_saturation_check(bool $condition, string $message): void
{
    if (!$condition) {
        throw new RuntimeException($message);
    }
}

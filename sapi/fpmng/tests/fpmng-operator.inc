<?php
/**
 * Reading an operator endpoint from a test.
 *
 * Since issue #275 the status and metrics pages of a pool are answered by the
 * operator endpoint (fpm_operator_endpoint.h) on a listener of its own, by a
 * process that is not one of the pool's children. That server is deliberately
 * one request per connection -- no keep-alive -- so a test cannot hold a
 * connection open across reads the way it could when the page came from the
 * pool's own listener.
 *
 * Every test that reads such a page therefore does the same three things:
 * connect, send one request line, read one framed response. It lives here so
 * that the four tests which do it do not each carry their own copy, and so
 * that "the operator endpoint speaks HTTP/1.1 and closes" is stated once.
 */

/* One request, one connection, one framed response. Returns
 * [status, headers (lower-cased names), body].
 *
 * Retries the CONNECT, because the internal listener pool is forked like any
 * other pool and a test that starts reading the instant the master says
 * "ready" can get there first. Does not retry the request: a connection that
 * was accepted and then answered badly is a failure, not a race. */
function fpmng_operator_fetch(string $address, string $path, float $timeout = 10.0): array
{
    $deadline = microtime(true) + $timeout;
    do {
        $fp = @stream_socket_client("tcp://$address", $errno, $error, 5);
        if ($fp) {
            break;
        }
        usleep(50000);
    } while (microtime(true) < $deadline);
    if (!$fp) {
        throw new RuntimeException("connect $address: $error");
    }

    try {
        stream_set_timeout($fp, 10);
        fwrite($fp, "GET $path HTTP/1.1\r\nHost: operator\r\nConnection: close\r\n\r\n");
        $line = fgets($fp);
        if (!$line || !preg_match('#^HTTP/1\.1 (\d+) #', $line, $m)) {
            throw new RuntimeException("bad status line from $address$path: " . var_export($line, true));
        }
        $status = (int) $m[1];
        $headers = [];
        $length = null;
        while (($line = fgets($fp)) !== false && $line !== "\r\n") {
            [$name, $value] = explode(':', $line, 2);
            $headers[strtolower($name)] = trim($value);
        }
        if (isset($headers['content-length'])) {
            $length = (int) $headers['content-length'];
        }
        $body = '';
        while ($length === null ? !feof($fp) : strlen($body) < $length) {
            $chunk = fread($fp, $length === null ? 8192 : $length - strlen($body));
            if ($chunk === false || $chunk === '') {
                break;
            }
            $body .= $chunk;
        }
        if ($length !== null && strlen($body) !== $length) {
            throw new RuntimeException("short body from $address$path: " . strlen($body) . " of $length");
        }
        return [$status, $headers, $body];
    } finally {
        fclose($fp);
    }
}

/* Just the body, for the common case of a page that must answer 200. */
function fpmng_operator_body(string $address, string $path): string
{
    [$status, , $body] = fpmng_operator_fetch($address, $path);
    if ($status !== 200) {
        throw new RuntimeException("$path on $address answered $status\n$body");
    }
    /* Kept for the diagnostics of whatever polls this page: a gauge that did
     * not move is only diagnosable against the numbers that were there. */
    $GLOBALS['fpmng_operator_last_page'] = $body;
    return $body;
}

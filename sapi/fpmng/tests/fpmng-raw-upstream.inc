<?php
/**
 * A scripted raw-socket upstream for the gateway's HTTP/1.1 client transport
 * (issue #462 and its siblings).
 *
 * The gateway dials a route target's listen address itself, on every connect,
 * so once fpm-ng is up the test unlinks the http-direct target's unix socket
 * and binds a socket of its own at the same path: from then on the "target" is
 * a script that can send any byte sequence, byte-timed, which a real
 * http-direct pool never would.
 *
 * The Tester is started with start([], false): the default -O sends zlog output
 * to the master's stderr pipe and the error log FILE stays empty, and these
 * tests are about log lines.
 */

require_once "tester.inc";

final class FpmngRaw
{
    public FPM\Tester $tester;
    public string $http;
    public string $errorLog;
    public int $accepts = 0;
    public string $docRoot;
    public string $sockPath;
    /** @var resource */
    private $server;
    /** @var resource|null */
    private $client = null;
    private string $clientBuf = '';
    /** @var resource|null */
    private $lastConn = null;

    /** The fixed names --CLEAN-- needs: it runs in another process. */
    public static function docRoot(string $name): string
    {
        return sys_get_temp_dir() . "/fpmng-raw-$name";
    }

    public static function clean(string $name): void
    {
        FPM\Tester::clean();
        $root = self::docRoot($name);
        @unlink("$root/index.php");
        @unlink("$root.sock");
        @rmdir($root);
    }

    /**
     * @param string $extraPools   more pool sections (e.g. a real target)
     * @param string $extraRoutes  more http.route[] lines for the gateway
     */
    public function __construct(string $name, string $extraPools = '', string $extraRoutes = '')
    {
        $this->docRoot = self::docRoot($name);
        $this->sockPath = $this->docRoot . '.sock';
        @mkdir($this->docRoot, 0700, true);
        file_put_contents($this->docRoot . '/index.php', '<?php echo getenv("FPMNG_RAW_INDEX") ?: ($_SERVER["REQUEST_URI"] ?? "");');

        $cfg = <<<EOT
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
[gw]
pool.type = gateway
listen = {{ADDR[http]}}
chdir = {$this->docRoot}
http.gateways = 1
http.route[evil] = /evil
$extraRoutes
[evil]
pool.type = http-direct
listen = {$this->sockPath}
chdir = {$this->docRoot}
pm = static
pm.max_children = 1
http.front_controller = /index.php
$extraPools
EOT;
        $this->tester = new FPM\Tester($cfg);
        @unlink($this->tester->getPrefixedFile(FPM\Tester::FILE_EXT_LOG_ERR));
        $this->tester->start([], false);
        $this->tester->switchLogSource('{{FILE:LOG}}');
        $this->tester->expectLogStartNotices();
        $this->errorLog = $this->tester->getPrefixedFile(FPM\Tester::FILE_EXT_LOG_ERR);
        $this->http = $this->tester->getAddr('ipv4', '[http]');

        /* Take over the target's address. */
        @unlink($this->sockPath);
        $server = @stream_socket_server('unix://' . $this->sockPath, $errno, $errstr);
        if (!$server) {
            throw new RuntimeException("raw upstream bind failed: $errstr");
        }
        $this->server = $server;
    }

    public function log(): string
    {
        return (string) @file_get_contents($this->errorLog);
    }

    /** Waits for a log line; returns the log text from byte $from on, or null. */
    public function waitLog(string $pattern, int $from = 0, float $seconds = 8.0): ?string
    {
        $deadline = microtime(true) + $seconds;
        do {
            $tail = substr($this->log(), $from);
            if (preg_match($pattern, $tail)) {
                return $tail;
            }
            usleep(50000);
        } while (microtime(true) < $deadline);

        return null;
    }

    /** The gateway dialled the raw upstream: its connection, or null on timeout. */
    public function accept(float $seconds = 5.0)
    {
        $r = [$this->server];
        $w = $e = null;
        if (stream_select($r, $w, $e, (int) $seconds, (int) (($seconds - (int) $seconds) * 1e6)) < 1) {
            return null;
        }
        $conn = @stream_socket_accept($this->server, 0);
        if (!$conn) {
            return null;
        }
        $this->accepts++;
        stream_set_timeout($conn, 5);
        $this->lastConn = $conn;

        return $conn;
    }

    /**
     * The upstream connection the next request travels on: a fresh one if the
     * gateway dials, else the idle keep-alive one it reuses.
     */
    public function conn(float $seconds = 1.0)
    {
        return $this->accept($seconds) ?? $this->lastConn;
    }

    /** One request head (the tests send no request bodies) from an upstream connection. */
    public function readRequest($conn, float $seconds = 5.0): string
    {
        $buf = '';
        $deadline = microtime(true) + $seconds;
        stream_set_blocking($conn, false);
        while (microtime(true) < $deadline && !str_contains($buf, "\r\n\r\n")) {
            $chunk = fread($conn, 65536);
            if ($chunk === '' || $chunk === false) {
                if (feof($conn)) {
                    break;
                }
                usleep(20000);
                continue;
            }
            $buf .= $chunk;
        }
        stream_set_blocking($conn, true);

        return $buf;
    }

    /** True when the gateway closed this upstream connection (within $seconds). */
    public function closedByGateway($conn, float $seconds = 3.0): bool
    {
        $deadline = microtime(true) + $seconds;
        stream_set_blocking($conn, false);
        while (microtime(true) < $deadline) {
            $chunk = fread($conn, 65536);
            if (($chunk === '' || $chunk === false) && feof($conn)) {
                return true;
            }
            usleep(20000);
        }

        return false;
    }

    public function send(string $request, bool $fresh = false): void
    {
        if ($fresh && $this->client) {
            fclose($this->client);
            $this->client = null;
            $this->clientBuf = '';
        }
        if (!$this->client) {
            $parts = explode(':', $this->http);
            $this->client = stream_socket_client("tcp://{$parts[0]}:{$parts[1]}", $errno, $errstr, 5);
            if (!$this->client) {
                throw new RuntimeException("cannot connect to the gateway: $errstr");
            }
            stream_set_blocking($this->client, false);
        }
        fwrite($this->client, $request);
    }

    /** Length of the first complete response in $b, or null when more bytes are needed. */
    private static function responseLength(string $b): ?int
    {
        $p = strpos($b, "\r\n\r\n");
        if ($p === false) {
            return null;
        }
        $head = substr($b, 0, $p);
        $off = $p + 4;
        if (preg_match('/^Transfer-Encoding:[^\r\n]*chunked/mi', $head)) {
            while (true) {
                $e = strpos($b, "\r\n", $off);
                if ($e === false) {
                    return null;
                }
                $n = hexdec(substr($b, $off, $e - $off));
                if ($n === 0) {
                    return strlen($b) >= $e + 4 ? $e + 4 : null;
                }
                $off = $e + 2 + $n + 2;
                if ($off > strlen($b)) {
                    return null;
                }
            }
        }
        if (preg_match('/^Content-Length:\s*(\d+)/mi', $head, $m)) {
            return strlen($b) >= $off + (int) $m[1] ? $off + (int) $m[1] : null;
        }

        return null;    /* close-delimited: only EOF or the timeout ends it */
    }

    /** The next complete response on the client connection (or what arrived by the deadline). */
    public function readResponse(float $seconds = 6.0): string
    {
        $deadline = microtime(true) + $seconds;
        while (microtime(true) < $deadline) {
            $len = self::responseLength($this->clientBuf);
            if ($len !== null) {
                $out = substr($this->clientBuf, 0, $len);
                $this->clientBuf = substr($this->clientBuf, $len);

                return $out;
            }
            $chunk = fread($this->client, 65536);
            if ($chunk !== '' && $chunk !== false) {
                $this->clientBuf .= $chunk;
                continue;
            }
            if (feof($this->client)) {
                break;
            }
            usleep(20000);
        }
        $out = $this->clientBuf;
        $this->clientBuf = '';

        return $out;
    }

    public static function status(string $response): string
    {
        return strtok($response, "\r\n") ?: '(nothing)';
    }

    public static function head(string $response): string
    {
        $p = strpos($response, "\r\n\r\n");

        return $p === false ? $response : substr($response, 0, $p);
    }

    /** The response body, de-chunked when the response was chunked. */
    public static function body(string $response): string
    {
        $p = strpos($response, "\r\n\r\n");
        if ($p === false) {
            return '';
        }
        $rest = substr($response, $p + 4);
        if (!preg_match('/^Transfer-Encoding:[^\r\n]*chunked/mi', substr($response, 0, $p))) {
            return $rest;
        }
        $out = '';
        while (($e = strpos($rest, "\r\n")) !== false && ($n = hexdec(substr($rest, 0, $e))) > 0) {
            $out .= substr($rest, $e + 2, $n);
            $rest = substr($rest, $e + 2 + $n + 2);
        }

        return $out;
    }

    public function finish(): void
    {
        if ($this->client) {
            fclose($this->client);
        }
        fclose($this->server);
        $this->tester->terminate();
        $this->tester->close();
    }
}

function fpmng_raw_get(string $path, string $extra = ''): string
{
    return "GET $path HTTP/1.1\r\nHost: t\r\n$extra\r\n";
}

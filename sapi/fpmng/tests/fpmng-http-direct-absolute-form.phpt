--TEST--
fpm-ng: direct HTTP answers an absolute-form and a network-path request-target like the origin-form one (issue #681, RFC 9112 3.2.2)
--SKIPIF--
<?php include "skipif.inc"; ?>
--FILE--
<?php
require_once "tester.inc";

/* The gateway reduces an absolute-form request-target to its origin-form path
 * at ingress (#534, PR #680). http-direct has its own listener and reads the
 * raw target, so every matcher and every CGI key it derives is checked here
 * against the same spelling an origin-form request produces: ping.path,
 * access.suppress_path[], the static lookup, REQUEST_URI, PATH_INFO and
 * HTTP_HOST.
 *
 * The operator endpoint is deliberately not here: since #275 it is answered on
 * a listener of its own, by a server of its own (fpm_operator_http.c), for
 * every pool type -- and it reads the request line itself, so it 404s an
 * absolute-form target on the gateway exactly as it does here. */

$root = sys_get_temp_dir() . '/fpmng-direct-absform-' . getmypid();
@mkdir($root . '/assets', 0700, true);
file_put_contents($root . '/index.php', <<<'PHP'
<?php
echo 'app:' . $_SERVER['REQUEST_URI'] . ':' . ($_SERVER['HTTP_HOST'] ?? '-') . ':' . $_SERVER['PATH_INFO'];
PHP);
file_put_contents($root . '/assets/app.css', 'CSS');

$cfg = <<<CFG
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
[direct]
listen = {{ADDR[http]}}
pool.type = http-direct
pm = static
pm.max_children = 1
chdir = $root
http.front_controller = /index.php
http.static = yes
ping.path = /ping
ping.response = pong
access.log = {{FILE:LOG:ACC}}
access.format = "%m %r %s"
access.suppress_path[] = /quiet
CFG;

/* One connection per request, read to EOF: the target is written by hand so
 * that a spelling a URL parser would rewrite still reaches the pool exactly as
 * typed. `host` is off only for the case that is about what happens with no Host
 * header -- libevent then derives the host from the authority. */
function fetch(string $addr, string $target, bool $host = true): array
{
    $fp = stream_socket_client("tcp://$addr", $errno, $error, 5);
    if (!$fp) {
        throw new RuntimeException("connect $addr: $error");
    }
    stream_set_timeout($fp, 10);
    fwrite($fp, "GET $target HTTP/1.0\r\n" . ($host ? "Host: h\r\n" : '') . "\r\n");
    $raw = (string) stream_get_contents($fp);
    fclose($fp);
    $status = preg_match('#^HTTP/\S+ (\d+)#', $raw, $m) ? $m[1] : 'no status';
    $p = strpos($raw, "\r\n\r\n");
    $body = $p === false ? $raw : substr($raw, $p + 4);
    return [$status, trim(explode("\n", $body)[0] ?? '')];
}

/* The children write the log, so the last line may not be there yet. */
function accessLog(string $path, string $needle): string
{
    $deadline = microtime(true) + 10;
    $content = '';
    do {
        $content = (string) @file_get_contents($path);
        if (str_contains($content, $needle)) {
            break;
        }
        usleep(100000);
    } while (microtime(true) < $deadline);
    return $content;
}

$tester = new FPM\Tester($cfg, '<?php echo "unused";');
try {
    $tester->start();
    $tester->expectLogStartNotices();
    $http = $tester->getAddr('ipv4', '[http]');

    foreach ([
        'origin ping' => '/ping',
        'absolute ping' => 'http://h/ping',
        'origin app' => '/x?a=1',
        'absolute app' => 'http://h/x?a=1',
        'absolute no path' => 'http://h',
        'absolute other authority' => 'http://other.example:8080/x?a=1',
        'absolute no host header' => 'http://other.example/x',
        'network-path app' => '//h/x?a=1',
        'origin static' => '/assets/app.css',
        'absolute static' => 'http://h/assets/app.css',
        'network-path static' => '//h/assets/app.css',
        'scheme-only path' => 'http:/x?a=1',
        'long authority' => 'http://' . str_repeat('a', 300) . '/x',
        'asterisk-form' => '*',
    ] as $what => $target) {
        [$status, $body] = fetch($http, $target, $what !== 'absolute no host header');
        printf("%-25s %s %s\n", $what . ':', $status, $body);
    }

    /* access.suppress_path[] must match the same path ping.path does. The
     * network-path reference is origin-form (RFC 9112 3.2.1), so its path is
     * "//h/quiet" and no entry matches it -- exactly as on the gateway. */
    $log = $tester->getPrefixedFile(FPM\Tester::FILE_EXT_LOG_ACC);
    foreach (['/quiet', 'http://h/quiet', '//h/quiet', '/loud'] as $target) {
        fetch($http, $target);
    }
    $content = accessLog($log, '/loud');
    $quiet = array_values(array_filter(explode("\n", $content),
        fn (string $line): bool => str_contains($line, 'quiet')));
    printf("%-25s %s\n", 'quiet lines logged:', json_encode($quiet));
    printf("%-25s %s\n", 'loud logged:', str_contains($content, '/loud') ? 'yes' : 'no');
} finally {
    $tester->terminate();
    $tester->close();
    @unlink($root . '/index.php');
    @unlink($root . '/assets/app.css');
    @rmdir($root . '/assets');
    @rmdir($root);
}
?>
--EXPECT--
origin ping:              200 pong
absolute ping:            200 pong
origin app:               200 app:/x?a=1:h:/x
absolute app:             200 app:/x?a=1:h:/x
absolute no path:         200 app:/:h:/
absolute other authority: 200 app:/x?a=1:other.example:8080:/x
absolute no host header:  200 app:/x:other.example:/x
network-path app:         200 app://h/x?a=1:h://h/x
origin static:            200 CSS
absolute static:          200 CSS
network-path static:      200 app://h/assets/app.css:h://h/assets/app.css
scheme-only path:         200 app:/x?a=1:h:/x
long authority:           400 <HTML><HEAD>
asterisk-form:            400 <HTML><HEAD>
quiet lines logged:       ["GET \/\/h\/quiet 200"]
loud logged:              yes
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>
--TEST--
fpm-ng: the plain :80 companion answers the HTTP-01 challenge instead of redirecting it (issue #48)
--SKIPIF--
<?php
include "fpmng-skipif.inc";
fpmng_skip_if_pool_type_unsupported('http');
if (!function_exists('openssl_x509_parse')) {
    die('skip requires the openssl extension');
}
if (trim((string) shell_exec('command -v openssl 2>/dev/null')) === '') {
    die('skip requires the openssl CLI to generate a test certificate');
}
$probe = new FPM\Tester(<<<'EOT'
[global]
error_log = {{FILE:LOG}}
[unconfined]
listen = {{ADDR}}
pm = static
pm.max_children = 1
pool.type = http
chdir = /tmp
http.listen = {{ADDR[http]}}
http.tls_cert = /nonexistent-cert.pem
http.tls_key = /nonexistent-key.pem
EOT, '<?php');
$messages = $probe->testConfig(true, null, false, false);
FPM\Tester::clean();
foreach ((array) $messages as $message) {
    if (str_contains($message, 'built with TLS support')) {
        die('skip php-fpm-ng built without TLS support (configure without --enable-fpmng-tls, issue #280)');
    }
}
?>
--FILE--
<?php
require_once "tester.inc";

/* http.plain_listen is the redirect-only companion of a TLS pool (task 042),
 * and it is the socket a CA actually connects to: HTTP-01 is plain HTTP by
 * definition. Until this issue it answered the challenge namespace with a
 * hardcoded 404. Redirecting it to :443 instead would be worse than the 404
 * -- during the NO_CERT bootstrap of docs/NOTES.md section 3l there is no
 * certificate on :443 to redirect to, which is exactly when the challenge
 * has to work. */

const TOKEN   = 'zQ8wE7rT6yU5iO4pA3sD2fG1hJ0kL9mN';
const KEYAUTH = 'zQ8wE7rT6yU5iO4pA3sD2fG1hJ0kL9mN.VGVzdEtleUF1dGhvcml6YXRpb25TdWZmaXg';

function check(bool $condition, string $message): void
{
    if (!$condition) throw new RuntimeException($message);
}

function run(string $cmd): void
{
    exec($cmd . ' 2>&1', $output, $code);
    if ($code !== 0) {
        throw new RuntimeException("COMMAND FAILED: $cmd\n" . implode("\n", $output));
    }
}

/* Returns [status, headers, body] over plain HTTP on its own connection. */
function request(string $addr, string $path, string $method = 'GET'): array
{
    $fp = stream_socket_client("tcp://$addr", $errno, $error, 5);
    check($fp !== false, "connect to $addr failed: $error");
    stream_set_timeout($fp, 5);
    fwrite($fp, "$method $path HTTP/1.1\r\nHost: acme.test\r\nConnection: close\r\n\r\n");
    $raw = '';
    while (!feof($fp)) {
        $chunk = fread($fp, 8192);
        if ($chunk === false || $chunk === '') break;
        $raw .= $chunk;
    }
    fclose($fp);

    [$head, $body] = array_pad(explode("\r\n\r\n", $raw, 2), 2, '');
    $lines = explode("\r\n", $head);
    $status = (int) (explode(' ', array_shift($lines))[1] ?? 0);
    $headers = [];
    foreach ($lines as $line) {
        [$name, $value] = array_pad(explode(':', $line, 2), 2, '');
        $headers[strtolower(trim($name))] = trim($value);
    }
    return [$status, $headers, $body];
}

$root = sys_get_temp_dir() . '/fpmng-acme-plain-' . getmypid();
@mkdir($root, 0700, true);
run("openssl req -x509 -newkey rsa:2048 -nodes -days 2 -sha256 "
    . "-subj /CN=acme.test -keyout $root/tls.key -out $root/tls.crt");

$frontController = "<?php echo \"worker\\n\";\n";
file_put_contents("$root/index.php", $frontController);

$publisher = <<<PHP
<?php
fpmng_acme_challenge_set('%TOKEN%', '%KEYAUTH%');
file_put_contents('{$root}/publisher.state', implode(',', fpmng_acme_challenge_list()) . "\\n");
while (!file_exists('{$root}/stop')) {
    usleep(50000);
}
PHP;
file_put_contents("$root/publisher.php", strtr($publisher, ['%TOKEN%' => TOKEN, '%KEYAUTH%' => KEYAUTH]));

$cfg = <<<EOT
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
[web]
listen = {{ADDR}}
chdir = $root
pm = static
pm.max_children = 1
pool.type = http
http.listen = {{ADDR[https]}}
http.plain_listen = {{ADDR[plain]}}
http.gateways = 2
http.front_controller = /index.php
http.tls_cert = $root/tls.crt
http.tls_key = $root/tls.key
[publisher]
pool.type = supervisor
supervisor.script = $root/publisher.php
supervisor.restart = never
EOT;

$tester = new FPM\Tester($cfg, $frontController);
$tester->start();
$tester->expectLogStartNotices();
$plain = $tester->getAddr('ipv4', '[plain]');

$deadline = microtime(true) + 10;
while (microtime(true) < $deadline && !file_exists("$root/publisher.state")) {
    usleep(50000);
}
check(trim((string) @file_get_contents("$root/publisher.state")) === TOKEN,
    'publisher state: ' . var_export(@file_get_contents("$root/publisher.state"), true));

/* Everything else on this socket is still a redirect -- the companion did not
 * turn into a general-purpose server. */
[$status, $headers] = request($plain, '/index.php');
check($status === 308, "ordinary path on the plain companion: $status");
check(str_starts_with($headers['location'] ?? '', 'https://'), 'redirect location: ' . ($headers['location'] ?? '(none)'));
echo "still-redirects: ok\n";

/* The challenge is answered here, in full, over plain HTTP. */
[$status, $headers, $body] = request($plain, '/.well-known/acme-challenge/' . TOKEN);
check($status === 200, "challenge on the plain companion: $status");
check(($headers['content-type'] ?? '') === 'text/plain', 'content-type: ' . ($headers['content-type'] ?? '(none)'));
check($body === KEYAUTH, 'body: ' . var_export($body, true));
check(!isset($headers['location']), 'the challenge was redirected: ' . ($headers['location'] ?? ''));
echo "challenge-answered-not-redirected: ok\n";

/* An unknown token is a 404 here too, never a redirect to a port that may
 * have no certificate yet. */
[$status, $headers] = request($plain, '/.well-known/acme-challenge/unknown-token');
check($status === 404, "unknown token on the plain companion: $status");
check(!isset($headers['location']), 'an unknown token was redirected: ' . ($headers['location'] ?? ''));
echo "unknown-token-404: ok\n";

/* A percent-encoded prefix must not slip past the namespace check and come
 * back out as a redirect carrying the token in a Location header. */
[$status, $headers] = request($plain, '/.well-known/acme-challenge/' . TOKEN . '?x=1');
check($status === 200, "challenge with a query string: $status");
[$status, , $body] = request($plain, '/%2ewell-known/acme-challenge/' . TOKEN);
check($status === 200 && $body === KEYAUTH, "percent-encoded prefix: $status " . var_export($body, true));
echo "prefix-decoding: ok\n";

touch("$root/stop");
$tester->terminate();
$tester->expectLogTerminatingNotices();
$tester->close();

$log = (string) @file_get_contents($tester->getPrefixedFile('log'));
check(!str_contains($log, KEYAUTH), 'the key authorization appeared in error_log');

foreach (['tls.crt', 'tls.key', 'index.php', 'publisher.php', 'publisher.state', 'stop'] as $file) {
    @unlink("$root/$file");
}
@rmdir($root);

?>
Done
--EXPECT--
still-redirects: ok
challenge-answered-not-redirected: ok
unknown-token-404: ok
prefix-decoding: ok
Done
--CLEAN--
<?php
require_once "tester.inc";
FPM\Tester::clean();
?>

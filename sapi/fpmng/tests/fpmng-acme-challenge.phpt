--TEST--
fpm-ng: the gateway answers the HTTP-01 challenge from shared state, before static files and without a worker (issue #48)
--SKIPIF--
<?php
include "fpmng-skipif.inc";
fpmng_skip_if_pool_type_unsupported('http');
?>
--FILE--
<?php
require_once "tester.inc";

/* docs/NOTES.md section 3l reserved one hook for responses the gateway
 * produces itself and required the fixed challenge path to come before files
 * from disk. This test pins all of it: the answer comes from the shared
 * challenge state a *different* process published into, every gateway process
 * has it, http.static does not switch it on or off, and a file physically
 * present at the challenge path cannot shadow or leak into the answer. */

const TOKEN   = 'wF3Nq7Rk2pLzXyB1aC4dE5fG6hJ8kM0n';
const KEYAUTH = 'wF3Nq7Rk2pLzXyB1aC4dE5fG6hJ8kM0n.Qm9ndXNUaHVtYnByaW50Rm9yQVRlc3RPbmx5';
/* Not published, but it *does* exist as a file under the document root. */
const SHADOW  = 'aB1cD2eF3gH4iJ5kL6mN7oP8qR9sT0uV';

function check(bool $condition, string $message): void
{
    if (!$condition) throw new RuntimeException($message);
}

/* Returns [status, headers, body] of one request on its own connection.
 * Connection: close on purpose -- with http.gateways > 1 a fresh connection
 * is the only way SO_REUSEPORT gets to pick a different process. */
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

$root = sys_get_temp_dir() . '/fpmng-acme-challenge-' . getmypid();
@mkdir($root . '/.well-known/acme-challenge', 0700, true);

/* The front controller records every request that reaches a worker. An empty
 * (or absent) file is how "did not reach a worker" is verified, rather than
 * by eyeballing a response body -- issue #48, criterion 2. */
$frontController = <<<PHP
<?php
file_put_contents('{$root}/worker-hits.log', (\$_SERVER['REQUEST_URI'] ?? '?') . "\\n", FILE_APPEND | LOCK_EX);
echo "worker\\n";
PHP;
file_put_contents("$root/env.php", $frontController);

/* Two real files inside the challenge namespace: one at a token that IS
 * published, one at a token that is not. Neither may ever be served. */
file_put_contents($root . '/.well-known/acme-challenge/' . TOKEN, "FILE-AT-A-PUBLISHED-TOKEN\n");
file_put_contents($root . '/.well-known/acme-challenge/' . SHADOW, "FILE-AT-AN-UNPUBLISHED-TOKEN\n");

/* The publisher: a pool type that serves no request (criterion 7). It
 * publishes, says so, waits to be told, clears, and says so again -- the test
 * drives the two transitions rather than racing a timer. */
$publisher = <<<PHP
<?php
if (!fpmng_acme_challenge_set('%TOKEN%', '%KEYAUTH%')) {
    file_put_contents('{$root}/publisher.state', "set-failed\\n");
    exit(1);
}
file_put_contents('{$root}/publisher.state', implode(',', fpmng_acme_challenge_list()) . "\\n");
while (!file_exists('{$root}/please-clear')) {
    usleep(50000);
}
fpmng_acme_challenge_clear('%TOKEN%');
file_put_contents('{$root}/publisher.state', 'cleared:' . implode(',', fpmng_acme_challenge_list()) . "\\n");
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
pm.max_children = 2
pool.type = http
http.listen = {{ADDR[http]}}
http.gateways = 4
http.static = 1
http.front_controller = /env.php
[nostatic]
listen = {{ADDR[fcgi2]}}
chdir = $root
pm = static
pm.max_children = 1
pool.type = http
http.listen = {{ADDR[nostatic]}}
http.static = 0
http.front_controller = /env.php
[publisher]
pool.type = supervisor
supervisor.script = $root/publisher.php
supervisor.restart = never
EOT;

$tester = new FPM\Tester($cfg, $frontController);
$tester->start();
$tester->expectLogStartNotices();
$http = $tester->getAddr('ipv4', '[http]');
$nostatic = $tester->getAddr('ipv4', '[nostatic]');

/* The publisher is a separate process in a separate pool; wait for it. */
$deadline = microtime(true) + 10;
while (microtime(true) < $deadline && !file_exists("$root/publisher.state")) {
    usleep(50000);
}
check(trim((string) @file_get_contents("$root/publisher.state")) === TOKEN,
    'publisher state: ' . var_export(@file_get_contents("$root/publisher.state"), true));
echo "published-by-another-process: ok\n";

/* 1. The exact key authorization, text/plain, no trailing newline. */
[$status, $headers, $body] = request($http, '/.well-known/acme-challenge/' . TOKEN);
check($status === 200, "known token status: $status");
check(($headers['content-type'] ?? '') === 'text/plain', 'content-type: ' . ($headers['content-type'] ?? '(none)'));
check($body === KEYAUTH, 'body: ' . var_export($body, true));
echo "known-token: ok\n";

/* 3. The file sitting at exactly that path under the document root did not
 *    shadow the answer and did not leak into it -- http.static = 1 here. */
check(!str_contains($body, 'FILE-AT-A-PUBLISHED-TOKEN'), 'the file on disk leaked into the answer');
echo "file-does-not-shadow: ok\n";

/* HEAD is answered the same way, with a length and no body. */
[$status, $headers, $body] = request($http, '/.well-known/acme-challenge/' . TOKEN, 'HEAD');
check($status === 200 && $body === '', "HEAD: $status " . var_export($body, true));
check(($headers['content-length'] ?? '') === (string) strlen(KEYAUTH), 'HEAD content-length: ' . ($headers['content-length'] ?? '(none)'));
echo "head: ok\n";

/* 2. An unknown token is 404 from the gateway, even though a file exists at
 *    that path: the whole namespace belongs to ACME. */
[$status, , $body] = request($http, '/.well-known/acme-challenge/' . SHADOW);
check($status === 404, "unpublished token with a file present: $status");
check(!str_contains($body, 'FILE-AT-AN-UNPUBLISHED-TOKEN'), 'the file on disk was served for an unpublished token');
echo "unknown-token-404: ok\n";

/* 5. A token is a flat opaque string. A slash inside it is not a lookup, and
 *    a traversal attempt is refused before anything touches the filesystem. */
[$status] = request($http, '/.well-known/acme-challenge/sub/' . TOKEN);
check($status === 404, "token containing a slash: $status");
[$status, , $body] = request($http, '/.well-known/acme-challenge/../../env.php');
check($status === 400 || $status === 404, "traversal: $status");
check(!str_contains($body, 'worker'), 'traversal reached the front controller');
echo "flat-token-only: ok\n";

/* 2, continued: none of the above occupied a worker. */
$hits = trim((string) @file_get_contents("$root/worker-hits.log"));
check($hits === '', 'the challenge namespace reached a worker: ' . var_export($hits, true));
echo "no-worker-touched: ok\n";

/* 4. http.static = 0 is a different pool with no static files at all; the
 *    challenge is not a static-file feature. */
[$status, $headers, $body] = request($nostatic, '/.well-known/acme-challenge/' . TOKEN);
check($status === 200 && $body === KEYAUTH, "http.static = 0: $status " . var_export($body, true));
check(($headers['content-type'] ?? '') === 'text/plain', 'content-type without http.static: ' . ($headers['content-type'] ?? '(none)'));
echo "works-without-http-static: ok\n";

/* 6. Every gateway process answers. http.gateways = 4 and SO_REUSEPORT, so
 *    40 fresh connections land on all four with overwhelming likelihood; a
 *    token visible in only the publishing process, or in only the gateway
 *    that happened to be forked first, would show up here as a run of 404s
 *    rather than as a flake. */
$statuses = [];
for ($i = 0; $i < 40; $i++) {
    [$status, , $body] = request($http, '/.well-known/acme-challenge/' . TOKEN);
    $statuses[] = $status;
    check($body === KEYAUTH, "connection $i body: " . var_export($body, true));
}
check(array_unique($statuses) === [200], 'statuses across 40 connections: ' . implode(',', array_unique($statuses)));
echo "every-gateway-answers: ok\n";

/* 7. Clearing propagates the same way. */
touch("$root/please-clear");
$deadline = microtime(true) + 10;
while (microtime(true) < $deadline && !str_starts_with((string) @file_get_contents("$root/publisher.state"), 'cleared:')) {
    usleep(50000);
}
check(trim((string) @file_get_contents("$root/publisher.state")) === 'cleared:',
    'publisher state after clear: ' . var_export(@file_get_contents("$root/publisher.state"), true));
for ($i = 0; $i < 8; $i++) {
    [$status] = request($http, '/.well-known/acme-challenge/' . TOKEN);
    check($status === 404, "after clearing, connection $i: $status");
}
echo "clearing-propagates: ok\n";

/* The pool still serves ordinary traffic. */
[$status, , $body] = request($http, '/env.php');
check($status === 200 && str_contains($body, 'worker'), "ordinary request: $status " . var_export($body, true));
echo "pool-still-serves: ok\n";

$tester->terminate();
$tester->expectLogTerminatingNotices();
$tester->close();

/* No key authorization anywhere in the log, at any level. */
$log = (string) @file_get_contents($tester->getPrefixedFile('log'));
check(!str_contains($log, KEYAUTH), 'the key authorization appeared in error_log');

foreach (glob("$root/.well-known/acme-challenge/*") as $file) {
    @unlink($file);
}
@rmdir("$root/.well-known/acme-challenge");
@rmdir("$root/.well-known");
foreach (['env.php', 'publisher.php', 'publisher.state', 'please-clear', 'worker-hits.log'] as $file) {
    @unlink("$root/$file");
}
@rmdir($root);

?>
Done
--EXPECT--
published-by-another-process: ok
known-token: ok
file-does-not-shadow: ok
head: ok
unknown-token-404: ok
flat-token-only: ok
no-worker-touched: ok
works-without-http-static: ok
every-gateway-answers: ok
clearing-propagates: ok
pool-still-serves: ok
Done
--CLEAN--
<?php
require_once "tester.inc";
FPM\Tester::clean();
?>

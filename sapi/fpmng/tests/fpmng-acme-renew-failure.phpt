--TEST--
fpm-ng: a failed renewal is loud, leaves the installed certificate serving, and backs off instead of hammering the CA (issue #49)
--SKIPIF--
<?php
include "fpmng-skipif.inc";
fpmng_skip_if_pool_type_unsupported('gateway');
fpmng_skip_if_no_acme();
if (!extension_loaded('openssl')) {
    die('skip requires the openssl extension');
}
if (!function_exists('openssl_csr_new')) {
    die('skip requires an OpenSSL build with CSR support');
}
?>
--FILE--
<?php
require_once "tester.inc";

/* Criterion 5, and the part of criterion 6 that only shows up on failure.
 *
 * The interesting property is a negative one: when the CA cannot be reached,
 * nothing on disk changes. A renewer that half-installs, or that clears the
 * old certificate before it has a new one, takes a working listener down at
 * the worst possible moment -- while the certificate is already close to
 * expiry. So the test hashes the installed files before and after.
 *
 * The CA here is a port nothing listens on. That is a real failure mode (a
 * CA outage, a broken egress rule) and it is the cheapest one to produce
 * deterministically -- no process to start, no timing to get right. */

function check(bool $condition, string $message): void
{
    if (!$condition) throw new RuntimeException($message);
}

function request(string $addr, string $path): array
{
    $fp = stream_socket_client("tcp://$addr", $errno, $error, 5);
    check($fp !== false, "connect to $addr failed: $error");
    stream_set_timeout($fp, 5);
    fwrite($fp, "GET $path HTTP/1.1\r\nHost: acme.test\r\nConnection: close\r\n\r\n");
    $raw = '';
    while (!feof($fp)) {
        $chunk = fread($fp, 8192);
        if ($chunk === false || $chunk === '') break;
        $raw .= $chunk;
    }
    fclose($fp);
    [$head, $body] = array_pad(explode("\r\n\r\n", $raw, 2), 2, '');
    return [(int) (explode(' ', $head)[1] ?? 0), $body];
}

$here = __DIR__;
$root = sys_get_temp_dir() . '/fpmng-acme-failure-' . getmypid();
@mkdir("$root/state/acme.test", 0700, true);

/* An installed certificate that is inside the renewal threshold, so the
 * renewer certainly wants to replace it -- and certainly must not manage to. */
$caKey = openssl_pkey_new(['private_key_bits' => 2048, 'private_key_type' => OPENSSL_KEYTYPE_RSA]);
$caCert = openssl_csr_sign(
    openssl_csr_new(['commonName' => 'fpmng failure CA'], $caKey, ['digest_alg' => 'sha256']),
    null, $caKey, 3650, ['digest_alg' => 'sha256'], 1
);
$conf = "$root/ext.cnf";
file_put_contents($conf, "[req]\ndistinguished_name=dn\n[dn]\n[ext]\nsubjectAltName=DNS:acme.test\n");
$leafKey = openssl_pkey_new(['private_key_bits' => 2048, 'private_key_type' => OPENSSL_KEYTYPE_RSA]);
$leaf = openssl_csr_sign(
    openssl_csr_new(['commonName' => 'acme.test'], $leafKey, ['digest_alg' => 'sha256']),
    $caCert, $caKey, 5, ['digest_alg' => 'sha256', 'config' => $conf, 'x509_extensions' => 'ext'], 42
);
$leafPem = '';
openssl_x509_export($leaf, $leafPem);
$caPem = '';
openssl_x509_export($caCert, $caPem);
$leafKeyPem = '';
openssl_pkey_export($leafKey, $leafKeyPem);
file_put_contents("$root/state/acme.test/fullchain.pem", $leafPem . $caPem);
file_put_contents("$root/state/acme.test/privkey.pem", $leafKeyPem);
chmod("$root/state/acme.test/privkey.pem", 0600);

$before = [];
foreach (['fullchain.pem', 'privkey.pem'] as $file) {
    $before[$file] = hash_file('sha256', "$root/state/acme.test/$file");
}

file_put_contents("$root/env.php", "<?php echo \"worker\\n\";\n");

/* Port 1 is privileged and nothing in this suite listens there, so the
 * connection is refused immediately and the test does not wait on a timeout. */
$runner = <<<PHP
<?php
\$root = '$root';
putenv('ACME_STATE_DIR=' . \$root . '/state');
putenv('ACME_DOMAINS=acme.test');
putenv('ACME_DIRECTORY=http://127.0.0.1:1/directory');
require '$here/../acme/renew.php';

/* error_log(), the same sink renew.php's own entry point uses -- the
 * operator-visible path criterion 5 is about, not a file this test
 * invented. Deliberately not echo: stdout only reaches the log when
 * catch_workers_output is on, and this configuration does not set it. */
\$log = static function (string \$message): void { error_log('acme: ' . \$message); };
\$results = [];
try {
    \$renewer = \FpmNg\Acme\Renewer::fromEnvironment(\$log);
    \$results[] = 'tick1: ' . \$renewer->tick();
    \$results[] = 'tick2: ' . \$renewer->tick();
} catch (\Throwable \$e) {
    \$results[] = 'EXCEPTION ' . get_class(\$e) . ': ' . \$e->getMessage();
}
file_put_contents(\$root . '/tick.out', implode("\\n", \$results) . "\\n");
PHP;
file_put_contents("$root/runner.php", $runner);

$cfg = <<<EOT
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
[gw]
pool.type = gateway
listen = {{ADDR[http]}}
chdir = $root
http.front_controller = /env.php
http.route[web] = /
[web]
pool.type = fastcgi
listen = {{ADDR}}
chdir = $root
pm = static
pm.max_children = 2
[acme]
pool.type = supervisor
supervisor.script = $root/runner.php
supervisor.processes = 1
supervisor.restart = never
EOT;

$tester = new FPM\Tester($cfg, "<?php echo \"worker\\n\";\n");
$tester->start();
$tester->expectLogStartNotices();
$http = $tester->getAddr('ipv4', '[http]');

$deadline = microtime(true) + 60;
while (microtime(true) < $deadline && !file_exists("$root/tick.out")) {
    usleep(50000);
}
$ticks = trim((string) @file_get_contents("$root/tick.out"));
check($ticks !== '', 'the ACME pool never finished');
check(!str_contains($ticks, 'EXCEPTION'), "the renewer threw instead of reporting a failure:\n$ticks");

/* A failed order is a reported outcome, not an exception escaping the pool. */
check(str_contains($ticks, 'tick1: failed'), "first tick:\n$ticks");
echo "unreachable-ca-is-a-reported-failure: ok\n";

/* The whole point: nothing on disk moved. */
foreach ($before as $file => $digest) {
    check(hash_file('sha256', "$root/state/acme.test/$file") === $digest,
        "$file changed during a failed renewal");
}
echo "installed-certificate-untouched: ok\n";

/* And the listener is still a listener. */
[$status, $body] = request($http, '/env.php');
check($status === 200 && str_contains($body, 'worker'), "the pool stopped serving: $status");
echo "listener-keeps-serving: ok\n";

/* The second tick did not try again: the failure is recorded with a time to
 * retry, so a cron pool ticking every minute does not turn a CA outage into
 * a request flood that earns a rate limit on top of the outage. */
check(str_contains($ticks, 'tick2: backoff'), "second tick:\n$ticks");
$meta = json_decode((string) @file_get_contents("$root/state/acme.test/renewal.json"), true);
check(is_array($meta), 'no renewal record was written');
check(($meta['failures'] ?? 0) === 1, 'failures: ' . var_export($meta['failures'] ?? null, true));
check(isset($meta['next_attempt']) && strtotime((string) $meta['next_attempt']) > time(),
    'next_attempt: ' . var_export($meta['next_attempt'] ?? null, true));
check(str_contains((string) ($meta['last_error'] ?? ''), '127.0.0.1:1'),
    'the recorded error does not name what failed: ' . var_export($meta['last_error'] ?? null, true));
check(sprintf('%o', fileperms("$root/state/acme.test/renewal.json") & 0777) === '600',
    'the renewal record is world-readable');
echo "failure-is-recorded-and-backed-off: ok\n";

/* Criterion 5 and 6 as an operator sees them: one line saying the renewal
 * failed and why, and -- because this certificate expires in days -- one
 * saying that too. Silence here is the failure mode that ends in an expired
 * certificate nobody noticed.
 *
 * Asserted through the log reader rather than by reading the file, because
 * the renewer writes to stdout and the master decorates the line before it
 * lands ("child N said into stdout: ..."), which is the path the operator
 * actually reads. */
$tester->expectLogPattern('/ERROR renewing acme\.test \(attempt 1\)/', true);
$tester->expectLogPattern('/The existing certificate keeps serving/', true);
$tester->expectLogPattern('/WARNING acme\.test: the certificate expires in/', true);
echo "operator-is-told: ok\n";

$tester->terminate();
$tester->expectLogTerminatingNotices();
$tester->close();

exec('rm -rf ' . escapeshellarg($root));

?>
Done
--EXPECT--
unreachable-ca-is-a-reported-failure: ok
installed-certificate-untouched: ok
listener-keeps-serving: ok
failure-is-recorded-and-backed-off: ok
operator-is-told: ok
Done
--CLEAN--
<?php
require_once "tester.inc";
FPM\Tester::clean();
?>

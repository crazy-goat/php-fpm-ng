--TEST--
fpm-ng: a TLS/ACME build announces both build flags as BETA at startup (issues #294, #295)
--SKIPIF--
<?php
include "fpmng-skipif.inc";
fpmng_skip_if_no_acme();
/* The TLS half, probed the way fpmng-http-direct-tls.phpt probes it: a build
 * without TLS refuses http.tls_cert with this wording. */
$probe = new FPM\Tester(<<<'EOT'
[global]
error_log = {{FILE:LOG}}
[unconfined]
listen = {{ADDR}}
pm = static
pm.max_children = 1
pool.type = http-direct
chdir = /tmp
http.front_controller = /nonexistent-front-controller.php
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

/* Issue #294 found this by starting the package it had just built: the ACME
 * line was in the log and the TLS one was not, because fpm.c guarded it with
 * HAVE_FPMNG_TLS and the macro config.m4 defines is HAVE_FPM_HTTP_TLS. Nothing
 * noticed, because both lines were prose in a #ifdef and no test ever read
 * them -- fpmng-tier-announce.phpt covers the per-pool tier lines, which are
 * a different call site.
 *
 * So this is the test for the two announcements that are a property of the
 * BINARY rather than of any pool. The pool below is a plain fastcgi one on
 * purpose: these lines do not depend on what is configured, and a pool type
 * that a distribution libphp cannot run would make this skip in the package
 * gate, which is exactly the build that ships them. */
$cfg = <<<CFG
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
[plain]
listen = {{ADDR}}
pm = static
pm.max_children = 1
CFG;

$tester = new FPM\Tester($cfg);
try {
    $tester->start();
    $tester->expectLogStartNotices();

    /* checkAllLogs, and through the tester's log API rather than the file:
     * see fpmng-tier-announce.phpt for both reasons. Each line is asserted with
     * its flag name in it, which is the half a build could get wrong and still
     * log something plausible. The pattern is anchored at both ends by the
     * tester, so it has to reach the end of the message -- hence the trailing
     * README.md, the same shape fpmng-tier-announce.phpt uses. */
    $tester->expectLogNotice(
        'TLS termination, unaudited and network-facing '
        . '\(this binary was built with --enable-fpmng-tls\) is BETA: .*README\.md',
        null,
        checkAllLogs: true
    );
    echo "TLS announced once, as a NOTICE: ok\n";

    $tester->expectLogNotice(
        'ACME certificate issuance, unaudited '
        . '\(this binary was built with --enable-fpmng-acme\) is BETA: .*README\.md',
        null,
        checkAllLogs: true
    );
    echo "ACME announced once, as a NOTICE: ok\n";

    echo "Done\n";
} finally {
    $tester->terminate();
    $tester->close();
}
?>
--EXPECT--
TLS announced once, as a NOTICE: ok
ACME announced once, as a NOTICE: ok
Done
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>

<?php
/**
 * The fpm-ng SKIPIF preamble: everything upstream's skipif.inc gates on, plus
 * the one gate that is ours.
 *
 * A binary built by build/libphp-build.sh links against a distribution's
 * libphp and therefore refuses `pool.type = fastcgi-ng` and `pool.type = http`
 * at configuration time -- see fpm_pool_type_check_build_support(). A test
 * whose pool needs one of those cannot pass there, and must not pretend to
 * fail either: the binary is telling it the truth. It skips.
 *
 * The decision is asked OF THE BINARY UNDER TEST, by running the same `-t`
 * configuration test a real start runs and reading what comes back. Nothing
 * here keeps a list of which types are supported; such a list would be a
 * second copy of a fact that already lives in fpm_pool_type.c and it would
 * drift the first time a type changes side.
 *
 * The map below is not that list. It holds the MINIMAL VALID configuration for
 * each type -- what that type's own validate() insists on before it will
 * consider a configuration well-formed at all. Without it the probe would be
 * refused for a missing directive and a supported type would look unsupported.
 */

require_once "skipif.inc";

/* The refusal this file skips on, and only this one. A pool refused for any
 * other reason is a real failure of a real test and is left to fail. */
const FPMNG_MISSING_PATCH_0006 = 'does not carry patches/0006';

function fpmng_pool_type_probe_config(string $type, string $root): string
{
    switch ($type) {
        case 'http':
            /* -t validates, it does not bind, so the port need not be free. */
            return "http.listen = 127.0.0.1:9\n";
        case 'http-direct':
            return "chdir = $root\nhttp.front_controller = /index.php\n";
        default:
            return '';
    }
}

/**
 * Skip the test unless the binary under test can honour this pool type.
 */
function fpmng_skip_if_pool_type_unsupported(string $type): void
{
    $binary = FPM\Tester::findExecutable();

    $root = sys_get_temp_dir() . '/fpmng-type-probe-' . getmypid();
    @mkdir($root);
    file_put_contents($root . '/index.php', '<?php');
    $conf = $root . '/probe.conf';

    file_put_contents($conf, "[global]\nerror_log = /dev/null\n"
        . "[probe]\nlisten = 127.0.0.1:9\npm = static\npm.max_children = 1\n"
        . "pool.type = $type\n" . fpmng_pool_type_probe_config($type, $root));

    $output = [];
    $status = 0;
    exec(escapeshellarg($binary) . ' -n -t -y ' . escapeshellarg($conf) . ' 2>&1', $output, $status);

    @unlink($conf);
    @unlink($root . '/index.php');
    @rmdir($root);

    if ($status !== 0 && str_contains(implode("\n", $output), FPMNG_MISSING_PATCH_0006)) {
        die("skip pool.type = $type is not supported by this binary "
            . '(linked against a distribution libphp, which does not carry patches/0006)');
    }
}

/* The refusal a build without --enable-fpmng-acme answers with (issue #281),
 * from fpm_payload_dist_validate(). */
const FPMNG_MISSING_ACME = 'carries no ACME client';

/**
 * Skip the test unless the binary under test was built with ACME issuance.
 *
 * Asked of the binary, like everything else here: a `-t` over a cron pool
 * whose script is the embedded ACME client. A build with --enable-fpmng-acme
 * resolves that name out of its own payload and validates; a build without it
 * refuses by name, which is what this skips on. Probing the PHP side instead
 * (does fpmng_acme_challenge_set() exist?) would need a running pool, which is
 * most of what the tests themselves do.
 */
function fpmng_skip_if_no_acme(): void
{
    $binary = FPM\Tester::findExecutable();

    $root = sys_get_temp_dir() . '/fpmng-acme-probe-' . getmypid();
    @mkdir($root);
    $conf = $root . '/probe.conf';

    /* No listen/pm/pm.max_children: pool.type = cron refuses all three, and
     * a probe refused for the wrong reason would skip nothing. */
    file_put_contents($conf, "[global]\nerror_log = /dev/null\n"
        . "[probe]\npool.type = cron\ncron.schedule = @daily\n"
        . "cron.script = fpmng-dist://acme/renew.php\n");

    $output = [];
    $status = 0;
    exec(escapeshellarg($binary) . ' -n -t -y ' . escapeshellarg($conf) . ' 2>&1', $output, $status);

    @unlink($conf);
    @rmdir($root);

    if ($status !== 0 && str_contains(implode("\n", $output), FPMNG_MISSING_ACME)) {
        die('skip php-fpm-ng built without ACME support '
            . '(configure without --enable-fpmng-acme, issue #281)');
    }
}

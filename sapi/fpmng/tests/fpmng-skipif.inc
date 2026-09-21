<?php
/**
 * The fpm-ng SKIPIF preamble: everything upstream's skipif.inc gates on, plus
 * the gates that are ours.
 *
 * fpmng_skip_if_pool_type_unsupported() is a no-op since issue #420. It used to
 * run a `-t` probe of the pool type and skip when the binary was linked against
 * a distribution libphp, which refuses a type whose children need patches/0006
 * (inside Zend/) -- see fpm_pool_type_check_build_support(), removed by #420.
 * Issue #376 retired `fastcgi-ng`, issue #388 retired `pool.type = http` and
 * split it into `pool.type = gateway` (which runs no PHP child and needs
 * nothing from the engine), so no type in this tree needs a build-time
 * capability any more. The symbol stays so the ~52 callers do not have to be
 * edited, and so a future type with a capability has one designated place to
 * gate on it: ask the binary under test, do not keep a list of supported types
 * here.
 */

require_once "skipif.inc";

/**
 * Skip the test unless the binary under test can honour this pool type.
 *
 * A no-op today: every configurable type is supported by every build. See the
 * file comment above for why the probe was removed in issue #420.
 */
function fpmng_skip_if_pool_type_unsupported(string $type): void
{
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

<?php

/* fpm-ng test helper: say WHY the master refused to start (issue #99).
 *
 * FPM\Tester::expectLogStartNotices() reports only that the expected NOTICE
 * did not arrive. When a pool is rejected the master has already written the
 * reason to the error log the harness itself created -- every validate() in
 * fpm_pool_type_s rejects a pool that way -- and the test hides it: two of the
 * fiber tests in issue #87 cost a full build matrix to diagnose from "missing
 * NOTICE" when the log said "max_execution_time = 30 is not enforced ...".
 *
 * This lives here rather than in FPM\Tester because tester.inc is copied from
 * php-src by build/prepare.sh and is not ours to edit.
 *
 * Nothing is printed when the master started: only lines logged BEFORE
 * "ready to handle connections" are shown, and a healthy startup has no
 * ALERT/ERROR among them. Tests that provoke an ALERT at runtime (the
 * supervisor ones) are therefore unaffected -- theirs is logged after that
 * line, by a child.
 */
function fpmng_expect_log_start_notices(FPM\Tester $tester): void
{
    $tester->expectLogStartNotices();

    /* Same convention as FPM\Tester::clean(): the log sits next to the
     * generated test file, under the test's own name. debug_backtrace() gives
     * that file rather than this one. */
    $backtrace = debug_backtrace(DEBUG_BACKTRACE_IGNORE_ARGS, 1);
    $caller = $backtrace[0]['file'] ?? __FILE__;
    $log = substr($caller, 0, -strlen(pathinfo($caller, PATHINFO_EXTENSION))) . 'log';
    if (!is_file($log)) {
        return;
    }

    $lines = file($log, FILE_IGNORE_NEW_LINES) ?: [];
    $startup = [];
    foreach ($lines as $line) {
        if (str_contains($line, 'ready to handle connections')) {
            return;     /* The master is up; whatever follows is not this. */
        }
        if (str_contains($line, 'ALERT:') || str_contains($line, 'ERROR:')) {
            $startup[] = $line;
        }
    }
    if ($startup === []) {
        return;
    }

    echo "\nFPM did not start; the master logged:\n";
    foreach ($startup as $line) {
        echo $line . "\n";
    }
    echo "\n";
}

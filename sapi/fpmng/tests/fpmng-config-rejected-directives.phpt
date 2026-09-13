--TEST--
fpm-ng: rejected directives and illegal executors fail at configuration validation (fpm_pool_type.c rejects[], docs/NOTES.md §3i/3o)
--SKIPIF--
<?php
include "fpmng-skipif.inc";
?>
--FILE--
<?php

require_once "tester.inc";

/* Whether the async executor was compiled in, asked OF THE BINARY, not of the
 * way it was built.
 *
 * This used to read the `Configure Command` line out of `php-fpm-ng -i`. That
 * line exists only because we ran ./configure ourselves; a binary linked
 * against a distribution libphp (build/libphp-build.sh) never printed it, and
 * the test failed there for a reason that had nothing to do with rejected
 * directives (issue #215).
 *
 * The two builds refuse `pool.executor = async` through two different code
 * paths and say so differently: without the flag the executor entry has no
 * .type and fpm_pool_type_resolve() names the flag that is missing
 * (fpm_pool_type.c), with it the resolve succeeds and fpm_pool_async_validate()
 * rejects the pool as policy (fpm_pool_async.c). Which sentence comes back is
 * the binary stating its own build, and it is the only such statement there is:
 * async is rejected in every build, so no accepted configuration differs.
 *
 * A build that lost the async sources would answer the other sentence and this
 * probe would believe it -- so the answer is not taken on trust. The caller
 * asserts the full message of the branch it picked AND that the other build's
 * sentence is absent, which is what makes a half-changed message a failure
 * rather than a silent change of branch. */
const FPMNG_ASYNC_DISABLED_BY_POLICY = 'pool.executor = async is disabled';
const FPMNG_ASYNC_NOT_BUILT = '--enable-fpmng-async';

/* A binary linked against a distribution libphp refuses `pool.type = http` and
 * `pool.type = fastcgi-ng` outright, before any directive of that pool is
 * looked at (fpm_pool_type_check_build_support()). The cases below that use
 * `http` are then rejected for that reason instead of the one they name, which
 * is the binary being right, not the test failing -- so that refusal counts as
 * a rejection here. The other ten cases run on both builds, which is the point:
 * this file used to SKIP in its entirety on the libphp path and covered nothing
 * there at all (issue #215). */
const FPMNG_TYPE_UNSUPPORTED = 'does not carry patches/0006';

function expectConfigFailure(string $label, string $cfg, array $needles): void
{
    $tester = new FPM\Tester($cfg, '<?php echo "ok";');
    $messages = $tester->testConfig(true);
    if ($messages === null) {
        echo "FAIL: $label unexpectedly passed validation\n";
        exit(1);
    }
    $text = implode("\n", $messages);
    if (str_contains($text, FPMNG_TYPE_UNSUPPORTED)) {
        echo "$label: rejected\n";
        return;
    }
    foreach ($needles as $needle) {
        if (!str_contains($text, $needle)) {
            echo "FAIL: $label missing needle: $needle\n";
            echo "got:\n$text\n";
            exit(1);
        }
    }
    echo "$label: rejected\n";
}

/* See the comment at the top: the branch is chosen by what the binary says,
 * and then both halves of that branch are asserted while the other build's
 * sentence must be absent. */
function expectAsyncRejected(string $cfg): void
{
    $tester = new FPM\Tester($cfg, '<?php echo "ok";');
    $messages = $tester->testConfig(true);
    if ($messages === null) {
        echo "FAIL: async-disabled unexpectedly passed validation\n";
        exit(1);
    }
    $text = implode("\n", $messages);

    if (str_contains($text, FPMNG_TYPE_UNSUPPORTED)) {
        echo "async-disabled: rejected\n";
        return;
    }

    $builtIn = str_contains($text, FPMNG_ASYNC_DISABLED_BY_POLICY);
    $needles = $builtIn
        ? [FPMNG_ASYNC_DISABLED_BY_POLICY, 'pool.executor = classic or fiber']
        : [FPMNG_ASYNC_NOT_BUILT];
    $forbidden = $builtIn ? FPMNG_ASYNC_NOT_BUILT : FPMNG_ASYNC_DISABLED_BY_POLICY;

    foreach ($needles as $needle) {
        if (!str_contains($text, $needle)) {
            echo "FAIL: async-disabled missing needle: $needle\n";
            echo "got:\n$text\n";
            exit(1);
        }
    }
    if (str_contains($text, $forbidden)) {
        echo "FAIL: async-disabled answered for both builds at once\n";
        echo "got:\n$text\n";
        exit(1);
    }
    echo "async-disabled: rejected\n";
}

$base = <<<EOT
[global]
error_log = {{FILE:LOG}}
[pool]
listen = {{ADDR}}
pm = static
pm.max_children = 1
EOT;

expectConfigFailure(
    'fastcgi-http-directive',
    $base . "\nhttp.listen = 127.0.0.1:8080",
    ["'http.listen' is not supported by pool.type = fastcgi"]
);

expectConfigFailure(
    'http-fiber-directive-on-classic',
    $base . "\npool.type = http\nfiber.revalidate_freq = 0",
    ["'fiber.revalidate_freq' is not supported by pool.type = http"]
);

expectConfigFailure(
    'supervisor-listen',
    str_replace('[pool]', '[sup]', $base) . "\npool.type = supervisor\nsupervisor.script = {{FILE:*src.php}}\nsupervisor.processes = 1",
    ["'listen' is not supported by pool.type = supervisor"]
);

expectConfigFailure(
    'cron-pm',
    str_replace('[pool]', '[job]', $base) . "\npool.type = cron\ncron.schedule = * * * * *\ncron.script = {{FILE:*src.php}}\npm.max_children = 2",
    ["'pm.max_children' is not supported by pool.type = cron"]
);

expectConfigFailure(
    'supervisor-executor',
    str_replace('[pool]', '[sup2]', $base) . "\npool.type = supervisor\npool.executor = fiber\nsupervisor.script = {{FILE:*src.php}}\nsupervisor.processes = 1",
    ['pool.executor is not supported by pool.type = supervisor']
);

expectConfigFailure(
    'default-fastcgi-executor',
    $base . "\npool.executor = classic",
    ['pool.executor is not supported by pool.type = fastcgi']
);

/* pool.type = http-direct + pool.executor = worker (task 073). The worker
 * script runs for the whole life of the child, so directives whose semantics
 * assume "one request ends" must fail loudly rather than sit unenforced. */
$workerRoot = sys_get_temp_dir() . '/fpmng-worker-reject-' . getmypid();
@mkdir($workerRoot, 0700, true);
file_put_contents("$workerRoot/worker.php", '<?php');
$workerBase = str_replace('[pool]', '[wrk]', $base)
    . "\npool.type = http-direct\npool.executor = worker"
    . "\nchdir = $workerRoot\nhttp.front_controller = /worker.php"
    . "\nhttp.read_timeout = 10000\nhttp.max_body = 1M";

expectConfigFailure(
    'direct-worker-request-terminate-timeout',
    $workerBase . "\nphp_admin_value[max_execution_time] = 0\nrequest_terminate_timeout = 10",
    ["'request_terminate_timeout' is not supported by pool.type = http-direct with pool.executor = worker"]
);

/* issue #56. http.stream hooks the SAPI write of a per-request script; the
 * worker executor answers from a PHP callable it drives itself, so accepting
 * the directive here would read as if streaming were merely unimplemented. */
expectConfigFailure(
    'direct-worker-stream',
    $workerBase . "\nphp_admin_value[max_execution_time] = 0\nhttp.stream = yes",
    ["'http.stream' is not supported by pool.type = http-direct with pool.executor = worker"]
);

expectConfigFailure(
    'direct-worker-max-execution-time',
    $workerBase . "\nphp_admin_value[max_execution_time] = 30",
    ['pool.executor = worker: max_execution_time = 30 would apply to']
);

expectConfigFailure(
    'direct-worker-missing-script',
    str_replace('/worker.php', '/absent.php', $workerBase) . "\nphp_admin_value[max_execution_time] = 0",
    ['[pool wrk]', 'the worker script must be a regular file inside chdir']
);

/* The new executor is accepted only by the type that declares it
 * (its entry in fpm_pool_type_s.executors); http-direct still refuses fiber. */
expectConfigFailure(
    'direct-worker-foreign-executor',
    str_replace('pool.executor = worker', 'pool.executor = fiber', $workerBase)
        . "\nphp_admin_value[max_execution_time] = 0",
    ['pool.type = http-direct supports only pool.executor = classic or worker']
);

/* issue #60. A direct pool reads .user.ini from the front controller's own
 * directory up to the document root, so user_ini.filename is walked once per
 * directory on that path; a separator in it would point each probe somewhere
 * else entirely and the scan could leave the root. The master refuses it so
 * `-t` says so, instead of every child exiting one after another. */
expectConfigFailure(
    'direct-user-ini-filename-separator',
    $workerBase . "\nphp_admin_value[max_execution_time] = 0"
        . "\nphp_admin_value[user_ini.filename] = ../../etc/evil.ini",
    ['user_ini.filename must be a bare file name']
);

unlink("$workerRoot/worker.php");
rmdir($workerRoot);

/* pool.executor = async is rejected in both builds, but by two different code
 * paths, so the case has to say which build it is looking at instead of
 * inheriting one (issue #87). Without --enable-fpmng-async the executor entry
 * has no .type and fpm_pool_type_resolve() names the flag that is missing
 * (sapi/fpmng/fpm/fpm_pool_type.c:472); with the flag the resolve succeeds and
 * fpm_pool_async_validate() rejects the pool as a matter of policy
 * (sapi/fpmng/fpm/fpm_pool_async.c:73). Asserting only the first needle made
 * this case fail in any --enable-fpmng-async build. */
expectAsyncRejected($base . "\npool.type = http\npool.executor = async\nhttp.listen = {{ADDR[http]}}");

?>
Done
--EXPECT--
fastcgi-http-directive: rejected
http-fiber-directive-on-classic: rejected
supervisor-listen: rejected
cron-pm: rejected
supervisor-executor: rejected
default-fastcgi-executor: rejected
direct-worker-request-terminate-timeout: rejected
direct-worker-stream: rejected
direct-worker-max-execution-time: rejected
direct-worker-missing-script: rejected
direct-worker-foreign-executor: rejected
direct-user-ini-filename-separator: rejected
async-disabled: rejected
Done
--CLEAN--
<?php
require_once "tester.inc";
FPM\Tester::clean();
?>

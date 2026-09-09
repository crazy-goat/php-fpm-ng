--TEST--
fpm-ng: rejected directives and illegal executors fail at configuration validation (fpm_pool_type.c rejects[], docs/NOTES.md §3i/3o)
--SKIPIF--
<?php include "skipif.inc"; ?>
--FILE--
<?php

require_once "tester.inc";

/* Which optional executors this binary was built with. The configure line in
 * `php-fpm-ng -i` is the only thing that reports it: the flags decide whether
 * the sources are compiled at all (build/prepare.sh, sapi/fpmng/config.m4), and
 * the same query is what the fiber tests' SKIPIF sections use. */
function fpmngBuildHasFlag(string $flag): bool
{
    static $info = null;

    if ($info === null) {
        $binary = getenv('TEST_PHP_FPM_EXECUTABLE') ?: FPM\Tester::findExecutable();
        exec(escapeshellarg($binary) . ' -i 2>&1', $output, $status);
        if ($status !== 0) {
            echo "FAIL: cannot query the build flags of $binary\n";
            exit(1);
        }
        $info = implode("\n", $output);
    }

    return str_contains($info, $flag);
}

function expectConfigFailure(string $label, string $cfg, array $needles): void
{
    $tester = new FPM\Tester($cfg, '<?php echo "ok";');
    $messages = $tester->testConfig(true);
    if ($messages === null) {
        echo "FAIL: $label unexpectedly passed validation\n";
        exit(1);
    }
    $text = implode("\n", $messages);
    foreach ($needles as $needle) {
        if (!str_contains($text, $needle)) {
            echo "FAIL: $label missing needle: $needle\n";
            echo "got:\n$text\n";
            exit(1);
        }
    }
    echo "$label: rejected\n";
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
$asyncBuiltIn = fpmngBuildHasFlag('--enable-fpmng-async');
expectConfigFailure(
    'async-disabled',
    $base . "\npool.type = http\npool.executor = async\nhttp.listen = {{ADDR[http]}}",
    $asyncBuiltIn
        ? ['pool.executor = async is disabled', 'pool.executor = classic or fiber']
        : ['--enable-fpmng-async']
);

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
direct-worker-max-execution-time: rejected
direct-worker-missing-script: rejected
direct-worker-foreign-executor: rejected
async-disabled: rejected
Done
--CLEAN--
<?php
require_once "tester.inc";
FPM\Tester::clean();
?>

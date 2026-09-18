--TEST--
fpm-ng: rejected directives and illegal executors fail at configuration validation (fpm_pool_type.c rejects[], docs/NOTES.md §3i/3o)
--SKIPIF--
<?php
include "fpmng-skipif.inc";
?>
--FILE--
<?php

require_once "tester.inc";

/* Issue #373: the async executor (and fiber, its sibling) does not exist on
 * this branch at all any more -- both configure flags are reserved names that
 * refuse to build (sapi/fpmng/config.m4), so there is exactly one way for
 * `pool.executor = async` to fail validation here: fpm_pool_type_resolve()
 * finds the entry's .type still NULL and says where the executor went
 * (fpm_pool_type.c). Before the cut this file also had to allow for a build
 * with --enable-fpmng-async, where fpm_pool_async_validate() rejected the
 * pool as policy instead -- that branch of the check moved to branch async
 * with the sources it was testing. */
const FPMNG_ASYNC_NOT_ON_BRANCH = 'is not on this branch';

/* A binary linked against a distribution libphp refuses `pool.type = http`
 * outright, before any directive of that pool is looked at
 * (fpm_pool_type_check_build_support()). The cases below that use
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

/* See the comment at the top: on this branch there is only one sentence left
 * to assert -- the pointer at branch async. */
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

    if (!str_contains($text, FPMNG_ASYNC_NOT_ON_BRANCH)) {
        echo "FAIL: async-disabled missing needle: " . FPMNG_ASYNC_NOT_ON_BRANCH . "\n";
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

/* issue #376: pool.type = fastcgi-ng was removed. It is a retired name, not an
 * unknown one -- a config file outlives the release that broke it, so the
 * message has to say what happened and where the transport went. The libphp
 * guard below (FPMNG_TYPE_UNSUPPORTED) counts as a rejection on its own, so
 * this case reads the same on both builds. */
expectConfigFailure(
    'retired-fastcgi-ng',
    $base . "\npool.type = fastcgi-ng",
    [
        "pool.type 'fastcgi-ng' no longer exists",
        'removed in 0.9.0 (issue #376)',
        'use pool.type = fastcgi, or pool.type = http-direct for a pool with no web server in front',
    ]
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

/* issue #331. worker.max_pending must be a positive value: validated in
 * fpm_http_direct_worker_validate(), the same place max_execution_time is
 * checked just above. */
expectConfigFailure(
    'direct-worker-max-pending-zero',
    $workerBase . "\nphp_admin_value[max_execution_time] = 0\nworker.max_pending = 0",
    ['worker.max_pending(0) must be a positive value']
);

/* issue #331. worker.max_pending and worker.request_timeout mean something
 * only under pool.executor = worker -- the classic executor of the SAME
 * pool.type rejects them, the same way it rejects request_terminate_timeout
 * above but for the opposite reason (a worker-only directive, not a
 * classic-only one). */
expectConfigFailure(
    'direct-classic-worker-max-pending',
    str_replace('pool.executor = worker', 'pool.executor = classic', $workerBase)
        . "\nworker.max_pending = 10",
    ["'worker.max_pending' is not supported by pool.type = http-direct with pool.executor = classic"]
);

/* issue #331. Every other pool.type rejects the whole worker.* namespace too,
 * the same way it already rejects fiber.*. */
expectConfigFailure(
    'http-worker-directive-on-classic',
    $base . "\npool.type = http\nworker.request_timeout = 100",
    ["'worker.request_timeout' is not supported by pool.type = http"]
);

/* issue #332. worker.send_buffer_limit bounds fpmng_worker_respond_chunk()'s
 * backpressure and, like the two directives above, means nothing outside
 * pool.executor = worker: the classic executor of the same pool.type rejects
 * it too. */
expectConfigFailure(
    'direct-classic-worker-send-buffer-limit',
    str_replace('pool.executor = worker', 'pool.executor = classic', $workerBase)
        . "\nworker.send_buffer_limit = 64K",
    ["'worker.send_buffer_limit' is not supported by pool.type = http-direct with pool.executor = classic"]
);

/* issue #334. worker.max_memory/worker.max_lifetime mean something only under
 * pool.executor = worker, the same reasoning as worker.send_buffer_limit
 * just above. */
expectConfigFailure(
    'direct-classic-worker-max-memory',
    str_replace('pool.executor = worker', 'pool.executor = classic', $workerBase)
        . "\nworker.max_memory = 64M",
    ["'worker.max_memory' is not supported by pool.type = http-direct with pool.executor = classic"]
);

expectConfigFailure(
    'http-worker-max-lifetime-directive-on-classic',
    $base . "\npool.type = http\nworker.max_lifetime = 60",
    ["'worker.max_lifetime' is not supported by pool.type = http"]
);

/* issue #338. worker.accept_threshold bounds how much of the kernel's accept
 * queue one worker takes at a time; the classic executor of the
 * same pool.type has its own, non-configurable gate (issue #53) and rejects
 * this one, the same reasoning as worker.max_memory just above. */
expectConfigFailure(
    'direct-classic-worker-accept-threshold',
    str_replace('pool.executor = worker', 'pool.executor = classic', $workerBase)
        . "\nworker.accept_threshold = 1",
    ["'worker.accept_threshold' is not supported by pool.type = http-direct with pool.executor = classic"]
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

/* issue #340. http.route[] is the gateway's routing table and http-direct runs
 * no gateway, so the directive would be read by nobody. This type accepts the
 * rest of the "http." namespace, so it has to name http.route explicitly
 * instead of inheriting the prefix rule that covers fastcgi pools. */
expectConfigFailure(
    'direct-http-route',
    $workerBase . "\nphp_admin_value[max_execution_time] = 0\nhttp.route[api] = /api",
    ["'http.route' is not supported by pool.type = http-direct"]
);

unlink("$workerRoot/worker.php");
rmdir($workerRoot);

/* pool.executor = async (issue #87). On this branch --enable-fpmng-async is a
 * reserved, always-refused flag (issue #373), so the executor entry always
 * has .type == NULL and fpm_pool_type_resolve() names branch async as where
 * it went (sapi/fpmng/fpm/fpm_pool_type.c). */
expectAsyncRejected($base . "\npool.type = http\npool.executor = async\nhttp.listen = {{ADDR[http]}}");

?>
Done
--EXPECT--
fastcgi-http-directive: rejected
supervisor-listen: rejected
cron-pm: rejected
supervisor-executor: rejected
default-fastcgi-executor: rejected
direct-worker-request-terminate-timeout: rejected
direct-worker-stream: rejected
direct-worker-max-execution-time: rejected
direct-worker-max-pending-zero: rejected
direct-classic-worker-max-pending: rejected
http-worker-directive-on-classic: rejected
direct-classic-worker-send-buffer-limit: rejected
direct-classic-worker-max-memory: rejected
http-worker-max-lifetime-directive-on-classic: rejected
direct-classic-worker-accept-threshold: rejected
direct-worker-missing-script: rejected
direct-worker-foreign-executor: rejected
direct-user-ini-filename-separator: rejected
direct-http-route: rejected
async-disabled: rejected
Done
--CLEAN--
<?php
require_once "tester.inc";
FPM\Tester::clean();
?>

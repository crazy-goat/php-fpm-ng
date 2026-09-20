--TEST--
fpm-ng: rejected directives and illegal executors fail at configuration validation (fpm_pool_type.c rejects[], docs/NOTES.md §3i/3o)
--SKIPIF--
<?php
include "fpmng-skipif.inc";
?>
--FILE--
<?php

require_once "tester.inc";

/* Issue #388 retired pool.type = http and pool.type = gateway is a proxy-only
 * type that needs no patch of ours (it runs no PHP child, so it never sets
 * reuses_request_runtime). No type in this suite needs patches/0006 any more,
 * so on the libphp path the FPMNG_TYPE_UNSUPPORTED short-circuit below no
 * longer fires for these cases -- but it is kept so a future type that does
 * need the patch is counted as rejected here rather than failing the file. */
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

/* Issue #388: the gateway is the type that used to be pool.type = http. It
 * accepts the http.* namespace (it IS the proxy those directives tune) and
 * rejects the worker., pm. and php_ families because it runs no PHP. It needs at
 * least one http.route[] and a target to route to. */
/* The extra directives have to land in the [gw] section, not after [app]:
 * appending to the whole config would put them on the target, where most of
 * them are legal and the refusal being tested would not fire. */
function gatewayConfig(string $extra = ''): string
{
    return "[global]\nerror_log = {{FILE:LOG}}\n"
        . "[gw]\npool.type = gateway\nlisten = {{ADDR[http]}}\nhttp.route[app] = /\n"
        . $extra
        . "[app]\nlisten = {{ADDR}}\npm = static\npm.max_children = 1\n";
}

/* issue #331. Every other pool.type rejects the whole worker.* namespace too,
 * the same way it already rejects fiber.*. */
expectConfigFailure(
    'gateway-worker-directive-on-classic',
    gatewayConfig("worker.request_timeout = 100\n"),
    ["'worker.request_timeout' is not supported by pool.type = gateway"]
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
    'gateway-worker-max-lifetime-directive-on-classic',
    gatewayConfig("worker.max_lifetime = 60\n"),
    ["'worker.max_lifetime' is not supported by pool.type = gateway"]
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

/* Issue #388: pool.type = http is retired, refused by name with the shape that
 * replaces it. A retired name is a config that used to work, so it earns its
 * own message rather than "unknown pool.type". */
expectConfigFailure(
    'retired-http-type',
    $base . "\npool.type = http",
    ["pool.type 'http' no longer exists", 'pool.type = gateway']
);

/* Issue #388 acceptance criteria: the gateway runs no PHP, so what configures
 * PHP is refused, each naming the directive. */
expectConfigFailure(
    'gateway-pm',
    gatewayConfig("pm.max_children = 2\n"),
    ["'pm.max_children' is not supported by pool.type = gateway"]
);
expectConfigFailure(
    'gateway-php-admin-value',
    gatewayConfig("php_admin_value[memory_limit] = 256M\n"),
    ["'php_admin_value' is not supported by pool.type = gateway"]
);
expectConfigFailure(
    'gateway-php-value',
    gatewayConfig("php_value[memory_limit] = 256M\n"),
    ["'php_value' is not supported by pool.type = gateway"]
);
expectConfigFailure(
    'gateway-environment',
    gatewayConfig("env[APP_ENV] = production\n"),
    ["'env' is not supported by pool.type = gateway"]
);
/* Worker-output, worker-identity and resource directives are read only by
 * fpm_unix_init_child()/fpm_php_init_child()/fpm_stdio_init_child() for a PHP
 * worker, which a gateway never runs; accepted, they would silently do
 * nothing, so they are refused like the php_* families. */
expectConfigFailure(
    'gateway-catch-workers-output',
    gatewayConfig("catch_workers_output = yes\n"),
    ["'catch_workers_output' is not supported by pool.type = gateway"]
);
expectConfigFailure(
    'gateway-clear-env',
    gatewayConfig("clear_env = no\n"),
    ["'clear_env' is not supported by pool.type = gateway"]
);
expectConfigFailure(
    'gateway-chroot',
    gatewayConfig("chroot = /\n"),
    ["'chroot' is not supported by pool.type = gateway"]
);

/* `listen` is the public HTTP(S) port on this type, so http.listen has nothing
 * left to override. Refused as redundant rather than accepted as a second way
 * to spell the same socket (fpm_http_validate_pool()). */
expectConfigFailure(
    'gateway-http-listen-redundant',
    gatewayConfig("http.listen = {{ADDR[pub]}}\n"),
    ['http.listen is redundant on pool.type = gateway']
);

/* With no implicit own-pool target, a gateway with no http.route[] would
 * answer 404 to everything; that is a configuration error, not a proxy. */
expectConfigFailure(
    'gateway-no-routes',
    "[global]\nerror_log = {{FILE:LOG}}\n[gw]\npool.type = gateway\nlisten = {{ADDR[http]}}\n",
    ['pool.type = gateway with no http.route[] serves nothing']
);

?>
Done
--EXPECT--
fastcgi-http-directive: rejected
supervisor-listen: rejected
cron-pm: rejected
supervisor-executor: rejected
default-fastcgi-executor: rejected
retired-fastcgi-ng: rejected
direct-worker-request-terminate-timeout: rejected
direct-worker-stream: rejected
direct-worker-max-execution-time: rejected
direct-worker-max-pending-zero: rejected
direct-classic-worker-max-pending: rejected
gateway-worker-directive-on-classic: rejected
direct-classic-worker-send-buffer-limit: rejected
direct-classic-worker-max-memory: rejected
gateway-worker-max-lifetime-directive-on-classic: rejected
direct-classic-worker-accept-threshold: rejected
direct-worker-missing-script: rejected
direct-worker-foreign-executor: rejected
direct-user-ini-filename-separator: rejected
direct-http-route: rejected
retired-http-type: rejected
gateway-pm: rejected
gateway-php-admin-value: rejected
gateway-php-value: rejected
gateway-environment: rejected
gateway-catch-workers-output: rejected
gateway-clear-env: rejected
gateway-chroot: rejected
gateway-http-listen-redundant: rejected
gateway-no-routes: rejected
Done
--CLEAN--
<?php
require_once "tester.inc";
FPM\Tester::clean();
?>

# `fastcgi-ng` optimization plan

**Won't do: `pool.type = fastcgi-ng` was removed in 0.9.0 (issue #376).** The
measured gain was 114.53 us upstream against 105.01 us per request ("Worker
CPU without saturating the machine" below) -- 9.5 us, 8.3% of a hello-world
request and 0.02-0.2% of a 5-50 ms framework request; throughput at
saturation moved +0.41%, within noise ("Throughput at saturation" below).
The optimizations themselves (patches 0004/0005/0006) survive
under `pool.type = http`, which sets the same capability bit; the numbers
below still apply there. This document is the measurement that justified the
removal and stays as written.

## Goal

`pool.type = fastcgi-ng` is meant to be an optimized FastCGI frontend, while:

```ini
pool.type = fastcgi
```

should retain as much compatibility as possible with classic upstream PHP-FPM.

The executor is a separate dimension:

```ini
pool.executor = classic | fiber | async
```

The optimizations described here primarily concern:

```ini
pool.type = fastcgi-ng
pool.executor = classic
```

The `fiber` and `async` executors remain experimental and are not intended for production.

## Initial state

The transport optimizations live in `main/fastcgi.c`, but are protected by a process-wide switch set by the worker after identifying the pool. The switch is disabled by default, so `pool.type = fastcgi` uses the upstream read path and `accept() + fcntl()`. `pool.type = fastcgi-ng` and the internal transport of the `http` frontend enable buffered reads and `accept4()` after the child has forked.

The split does not duplicate all of `main/fastcgi.c`: the small `fcgi_set_optimized_transport()` interface keeps one protocol implementation and selects only the optimized transport operations. Because each worker is a separate process assigned to one pool, the setting cannot leak between pools.

Current changes relative to an unmodified upstream:

1. 16 KB FastCGI input buffer: a typical request header is fetched with one `read()` instead of about six;
2. `accept4(..., SOCK_CLOEXEC)` instead of `accept()` and two `fcntl()` calls when the platform provides `accept4`;
3. skip the technical write to fd 2 when `catch_workers_output = no`;
4. optional `request_cpu_tracking = no`, removing two `times()` calls per request;
5. fix setting `TCP_NODELAY` for TCP FastCGI keep-alive connections;
6. GH-18956 fix for idle/active counters on keep-alive connections.

The `TCP_NODELAY` and GH-18956 changes are bug fixes. They may remain shared by `fastcgi` and `fastcgi-ng`. Optimizations that change the transport implementation should be enabled only by `fastcgi-ng`.

## Measured baseline

Test host: `192.168.8.103`, user `piotr`, i7-6700T, 4 physical cores / 8 logical threads, Linux 7.0.

PHP baseline: upstream master, commit `5be4de10`. Compared binaries must be release builds from the same checkout and execute identical PHP code.

### Throughput at saturation

`hello.php`, nginx, 4 workers, `wrk -t2 -c32`, seven 10-second runs:

| variant | median |
|---|---:|
| upstream `php-fpm` | 11 583,73 req/s |
| `php-fpm-ng`, `fastcgi-ng + classic` | 11 631,00 req/s |

The `+0,41%` difference is within noise. This test saturates the machine and also measures nginx, `wrk`, and workers competing for cores and hyperthreads.

### Worker CPU without saturating the machine

`hello.php`, nginx, 4 workers, `wrk -t1 -c2`, five alternating 10-second runs:

| variant | median worker CPU/request |
|---|---:|
| upstream `php-fpm` | 114,53 us |
| `php-fpm-ng`, `fastcgi-ng + classic` | 105,01 us |

The current gain is about `9,5 us/request`, or `8,3%` of the worker's CPU. Median throughput was about 8812 req/s for upstream and 8910 req/s for fpm-ng (`+1,1%`).

Raw results, on the test box only -- these paths are not in this repository and
the figures above are what survives of them:

```text
/home/piotr/opencode-fiber-poligon/bench-fastcgi/results.txt
/home/piotr/opencode-fiber-poligon/bench-fastcgi/cpu-results.txt
```

## Work plan

### 1. Actually separate `fastcgi` and `fastcgi-ng` — done, pending full PHP-version regression

Transport implementation selection based on the effective `pool.type` has been introduced.

For `fastcgi`:

- keep the upstream FastCGI path;
- do not handle `pool.executor`;
- do not enable `fastcgi-ng`-specific optimizations;
- preserve the default behavior of existing configurations without `pool.type`.

For `fastcgi-ng`:

- enable buffered FastCGI reads;
- enable `accept4(SOCK_CLOEXEC)`;
- enable future transport optimizations;
- default to `pool.executor = classic`.

The preferred solution must not duplicate all of `main/fastcgi.c`. Find the smallest interface that can set the behavior variant per worker before the accept loop starts. The flag must not change the behavior of other pools in the same master process.

After the split, run the benchmark for three variants again:

1. unmodified upstream `php-fpm`;
2. `php-fpm-ng` with `pool.type = fastcgi`;
3. `php-fpm-ng` with `pool.type = fastcgi-ng` and `pool.executor = classic`.

Variant 2 should be performance- and behavior-equivalent to upstream. Variant 3 should retain the current reduction in worker CPU.

### 2. Establish the recommended `fastcgi-ng` configuration

> Closed as won't-do (issue #376): the type was removed in 0.9.0. Text left
> as written.

For lightweight endpoints, measure and document this configuration:

```ini
pool.type = fastcgi-ng
pool.executor = classic
listen = /run/php/pool.sock
catch_workers_output = no
request_cpu_tracking = no
php_admin_value[max_execution_time] = 0
```

Measure each option separately as well:

- UDS instead of TCP loopback: so far about `7-11 us/request` less;
- `request_cpu_tracking = no`: removes two `times()` calls;
- `max_execution_time = 0`: removes two `setitimer()` calls and part of signal-mask handling;
- `catch_workers_output = no`: allows the technical `write()` to be skipped.

Do not change the defaults of classic `fastcgi` to improve the benchmark result.

### 3. Profile the hot path again

> Closed as won't-do (issue #376): the type was removed in 0.9.0. Text left
> as written.

After splitting the frontends, collect the following for both variants:

- `strace -c` per request for TCP keep-alive;
- `strace -c` for new TCP connections;
- the same two measurements for UDS;
- FPM child CPU from `/proc`, not the whole host;
- `perf record` and `perf report` for a lightweight `hello.php`;
- throughput and latency through real nginx.

The previous profile after the optimizations contained about 20 syscalls per keep-alive request. The largest remaining groups were:

- 8 x `rt_sigaction` and 1 x `rt_sigprocmask`;
- 2 x `setitimer`;
- 2 x `chdir` and `getcwd`;
- 2 x `fcntl` originating in OPcache;
- 2 x `times`;
- individual FastCGI `read` and `write` calls.

Choose further work only on the basis of the new profile.

### 4. Consider registering signals once per process

The largest potential remaining cost is registering `rt_sigaction` again for every request.

Check:

- which handlers are actually immutable between requests;
- whether they can be installed once during worker initialization;
- which elements must be reset per request;
- behavior after a fatal error, timeout, request interruption, and reload;
- compatibility with extensions that install their own handlers.

This is a Zend change, not a local FPM optimization. Do not implement it without a separate reproducer, regression tests, and a measured gain. The preferred path is a change suitable for upstream, not a permanent Zend fork.

### 5. Investigate OPcache locks per request

The two `fcntl()` calls remaining on the hot path come from OPcache activation and deactivation.

Determine:

- exactly what these locks protect;
- whether their frequency can be reduced in the classic model, where one request runs at a time per worker;
- whether the change remains correct across restarts, invalidation, and shared memory between workers;
- whether the solution can be sent to upstream OPcache.

Do not bypass locks based only on a Hello World benchmark.

### 6. Optional mode without changing CWD

`getcwd()` and two `chdir()` calls could potentially be removed for applications that use only absolute paths.

If the profile confirms a significant cost, consider an explicit option for `fastcgi-ng` only, disabled by default. The option must clearly document the semantic change for relative paths, `include`, `require`, and file operations.

Do not use an invisible CWD cache, because it could change application behavior.

### 7. Check the input-buffer size — measured for 8/16/32 KB

The CPU/request measurement showed no significant advantage for any variant:

| buffer | CPU/request |
|---|---:|
| 8 KB | 86,591 us |
| 16 KB | 87,321 us |
| 32 KB | 86,564 us |

The differences stayed below 1%, so the input buffer remains unchanged at 16 KB.

## Accepted large-response optimization

For a large FastCGI record, the optimized Unix transport sends the header and body with one `writev()`. Classic `fastcgi` and Windows retain the existing `write()` path.

For a 262 144 B response, the number of transport operations fell from 11 to 6. An `strace` test on PHP 8.5 confirmed 11 writes and no `writev()` for `fastcgi`, versus 5 `writev()` calls and a final record write for `fastcgi-ng`.

Five alternating runs on PHP 8.5, `wrk -t1 -c2 -d10s`:

| frontend | metric | baseline | `writev` | change |
|---|---|---:|---:|---:|
| `fastcgi-ng` | worker CPU/request | 195,433 us | 178,824 us | **-8,5%** |
| `fastcgi-ng` | req/s | 2018,63 | 2037,15 | **+0,9%** |
| `http`, `Connection: close` | combined gateway and worker CPU/request | 525,209 us | 490,612 us | **-6,6%** |
| `http`, `Connection: close` | req/s | 2682,67 | 2705,14 | **+0,8%** |

CPU fell in all five pairs of both benchmarks. The first HTTP keep-alive measurement, about 50 req/s, was not used to assess `writev()`, because the missing `TCP_NODELAY` on the HTTP listener triggered Nagle/delayed ACK. Setting this option once on the listener (inherited by accepted sockets) raised the median large keep-alive response from 49,74 to 2686,13 req/s and reduced combined gateway and worker CPU/request from 643,939 to 494,200 us.

The PHP 8.5 regression passed for small and large responses, a binary 65 792 B POST with SHA-256 verification, keep-alive/close, and a disconnected client. Responses from 1 B to 1 MiB, including FastCGI record boundaries, had already been compared byte for byte on master.

Small-response batching was rejected: a complete small FastCGI response already reaches one `write()`, and the observed second write belongs to another descriptor. It provides no safe transport saving.

## Benchmark methodology

Every comparison must satisfy all of these conditions:

1. the same upstream PHP commit;
2. a release build, without `--enable-debug`;
3. the same compiler and compiler flags;
4. identical PHP code and OPcache configuration;
5. the same number of workers;
6. the same nginx frontend and FastCGI settings;
7. alternating run order;
8. a warm-up before measurement;
9. at least five runs, reporting the median and spread;
10. separate measurement of worker CPU and total throughput;
11. `wrk -t1 -c2` as the primary measurement of CPU cost without saturating the test host;
12. high-concurrency tests reported separately as whole-system throughput tests.

For every result, record:

- the PHP commit and php-fpm-ng commit;
- the complete configure/build commands;
- the FPM and nginx configurations;
- nginx and `wrk` versions;
- raw results;
- the number of cores and the state of other host workloads.

## Correctness criteria

Every optimization must pass:

- the complete `sapi/fpm/tests` suite;
- GET and POST through nginx;
- small and large request bodies;
- small and multi-megabyte responses;
- keep-alive and new connections;
- TCP and UDS;
- `fastcgi_request_buffering` and `fastcgi_buffering` enabled and disabled;
- a connection dropped during a request and during a response;
- master restart/reload;
- `pm.max_requests`;
- status, slowlog, access log, and timeouts;
- checks for descriptor and memory leaks.

Classic `pool.type = fastcgi` must additionally pass a comparison test against unmodified upstream.

## Optimization acceptance criteria

Accept a change only when it:

- does not change the FastCGI protocol or application behavior without an explicit option;
- has a regression test;
- provides a repeatable worker-CPU gain or fixes a demonstrated problem;
- does not gain its result from a different configuration or host saturation;
- has a maintenance cost and upstream deviation proportional to the effect;
- has a plan for sending changes outside `sapi/fpmng` upstream.

Do not accept micro-optimizations based only on higher req/s in a saturated test.

## Deliberately rejected directions

At this stage, do not return to:

- `SO_RCVTIMEO` instead of `poll` after `accept`: it breaks idle TCP and is not inherited correctly for UDS;
- `SO_REUSEPORT` as a cure for thundering herd: blocking accept did not demonstrate that problem;
- `io_uring`: too much complexity, a separate backend, and problems with the default Docker seccomp profile;
- an invisible `chdir` cache: risk of changing application semantics;
- optimizing only the Hello World result at the expense of BC;
- further development of a Zend fork without a path to upstream.

## Expected result

The next concrete result should be:

1. `fastcgi` really uses an upstream-compatible path;
2. `fastcgi-ng + classic` explicitly enables the optimized transport;
3. the current gain of about `9-10 us` worker CPU per lightweight request is retained;
4. the recommended UDS configuration without unnecessary telemetry is measured separately;
5. further changes are selected based on `perf` and `strace`, not assumptions.

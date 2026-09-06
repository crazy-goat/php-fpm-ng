# sapi/fpmng

Our files. The rest of the SAPI is copied from upstream `sapi/fpm/` by
`build/prepare.sh` and overlaid by this directory.

| file | why it's ours |
|---|---|
| `config.m4` | separate SAPI `--enable-fpmng`, binary `php-fpm-ng`, libevent mandatory |
| `fpm/fpm.c` | hook `fpm_http_init_main()` in `fpm_run()` — the attachment point for pool types |
| `fpm/fpm_http.c` | HTTP gateway |
| `fpm/fpm_http.h` | |
| `fpm/fpm_pool_async.c` | EXPERIMENT: `pool.executor = async` for `fastcgi-ng` and `http` — a True Async coroutine per request; even with cache-hit support, `opcache.enable=0` is still recommended (NOTES 3t) |
| `fpm/fpm_pool_async.h` | |
| `fpm/fpm_pool_coop.c` | shared request state for the experimental multi-request executors |
| `fpm/fpm_pool_coop.h` | |
| `fpm/fpm_pool_fiber.c` | EXPERIMENT: `pool.executor = fiber` for `fastcgi-ng` and `http` on upstream PHP, libevent scheduler (NOTES 3u) |
| `fpm/fpm_pool_fiber.h` | |
| `fpm/fpm_pool_fiber_xport.c` | suspending Fibers on the `tcp` and `unix` transports |
| `fpm/fpm_metrics.c` | application-metrics glue (NOTES 3k/3w): the master allocates shm slots, the child in `run_child:` gets a slot + pool label |
| `fpm/fpm_process_ctl.c` | reload: on `SIGUSR2` request workers get `SIGQUIT`, while supervisor/cron/status get `SIGTERM`, so the consumer can finish its current iteration (NOTES 3x) |

Eventually `fpm/fpm_conf.c`, `fpm/fpm_status.c` and `fpm/fpm_main.c` will be
added — those are the only FPM files with real upstream churn (see
`docs/NOTES.md`).

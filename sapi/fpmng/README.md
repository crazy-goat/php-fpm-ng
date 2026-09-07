# sapi/fpmng

Our files. The rest of the SAPI is copied from upstream `sapi/fpm/` by
`build/prepare.sh` and overlaid by this directory.

| file | why it's ours |
|---|---|
| `config.m4` | separate SAPI `--enable-fpmng`, binary `php-fpm-ng`, libevent mandatory |
| `fpm/fpm.c` | hook `fpm_http_init_main()` in `fpm_run()` — the attachment point for pool types |
| `fpm/fpm_http.c` | HTTP gateway |
| `fpm/fpm_http.h` | |
| `fpm/fpm_pool_async.c` | POC `pool.executor = async` for `fastcgi-ng` and `http` — the True Async implementation stays in the tree, but validation rejects the executor until it has the required hardening (NOTES 3t, `docs/async_errors.md`) |
| `fpm/fpm_pool_async.h` | |
| `fpm/fpm_pool_coop.c` | shared request state for the experimental multi-request executors |
| `fpm/fpm_pool_coop.h` | |
| `fpm/fpm_pool_fiber.c` | EXPERIMENT: `pool.executor = fiber` for `fastcgi-ng` and `http` on upstream PHP, libevent scheduler (NOTES 3u) |
| `fpm/fpm_pool_fiber.h` | |
| `fpm/fpm_pool_fiber_xport.c` | suspending Fibers on the `tcp` and `unix` transports |
| `fpm/fpm_metrics.c` | application-metrics glue (NOTES 3k/3w): the master allocates shm slots, the child in `run_child:` gets a slot + pool label |
| `fpm/fpm_process_ctl.c` | reload: on `SIGUSR2` request workers get `SIGQUIT`, while supervisor/cron/status get `SIGTERM`, so the consumer can finish its current iteration (NOTES 3x) |
| `acme/state.php` | ACME state layout on a writable volume (NOTES 3y): account key, account record, per-domain certificate key and chain — used by the project-owned ACME client that task 043 puts in a `pool.type = cron` process |

Eventually `fpm/fpm_conf.c`, `fpm/fpm_status.c` and `fpm/fpm_main.c` will be
added — those are the only FPM files with real upstream churn (see
`docs/NOTES.md`).

## Comments

Comments here are the primary record of **why**. What earns one — a decision,
a measurement, a rejected alternative, a trap the code cannot express — and
what does not (restating the next line) is in
[`CLAUDE.md`](../../CLAUDE.md#comments-what-earns-one). There is no scheduled
comment-cleanup pass; remove redundant comments only when you are already
changing that code.

## Style and lint

Style rules and the clang-tidy subset live in [`docs/c-style.md`](../../docs/c-style.md).
EditorConfig matches php-src; there is no `.clang-format` (deliberate).

Run the static-analysis pass locally (needs `clang-tidy` on `PATH`):

```sh
# Smoke check of the config against our .c files only (missing php.h is expected):
./build/lint-c.sh

# After build/prepare.sh + configure in a php-src tree, pass that path so
# includes resolve:
./build/lint-c.sh /path/to/prepared-php-src
```

The script never walks a prepared `sapi/fpmng/` inside php-src — only paths
that exist in this repository — so upstream copies are not part of the report.
CI runs the same script as a non-blocking report (see the `lint` job).

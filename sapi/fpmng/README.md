# sapi/fpmng

Nasze pliki. Reszta SAPI jest kopiowana z `sapi/fpm/` upstreamu przez
`build/prepare.sh` i nadpisywana tym katalogiem.

| plik | dlaczego nasz |
|---|---|
| `config.m4` | osobne SAPI `--enable-fpmng`, binarka `php-fpm-ng`, libevent obowiazkowy |
| `fpm/fpm.c` | haczyk `fpm_http_init_main()` w `fpm_run()` — punkt wpiecia dla typow poola |
| `fpm/fpm_http.c` | bramka HTTP |
| `fpm/fpm_http.h` | |
| `fpm/fpm_pool_async.c` | EKSPERYMENT: `pool.executor = async` dla `fastcgi-ng` i `http` — korutyna True Async per request; mimo obslugi cache hit nadal zalecane `opcache.enable=0` (NOTES 3t) |
| `fpm/fpm_pool_async.h` | |
| `fpm/fpm_pool_coop.c` | wspolny stan requestu dla eksperymentalnych executorow wielorequestowych |
| `fpm/fpm_pool_coop.h` | |
| `fpm/fpm_pool_fiber.c` | EKSPERYMENT: `pool.executor = fiber` dla `fastcgi-ng` i `http` na upstreamowym PHP, scheduler libevent (NOTES 3u) |
| `fpm/fpm_pool_fiber.h` | |
| `fpm/fpm_pool_fiber_xport.c` | zawieszanie Fiberow na transportach `tcp` i `unix` |
| `fpm/fpm_metrics.c` | glue metryk aplikacyjnych (NOTES 3k/3w): master alokuje shm slotow, dziecko w `run_child:` dostaje slot + etykiete pool |
| `fpm/fpm_process_ctl.c` | reload: przy `SIGUSR2` request workery dostaja `SIGQUIT`, a supervisor/cron/status `SIGTERM`, zeby consumer mogl dokończyć aktualna iteracje (NOTES 3x) |

Docelowo dojda `fpm/fpm_conf.c`, `fpm/fpm_status.c` i `fpm/fpm_main.c` —
to jedyne pliki FPM z realnym churnem upstreamu (patrz `docs/NOTES.md`).

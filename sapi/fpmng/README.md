# sapi/fpmng

Nasze pliki. Reszta SAPI jest kopiowana z `sapi/fpm/` upstreamu przez
`build/prepare.sh` i nadpisywana tym katalogiem.

| plik | dlaczego nasz |
|---|---|
| `config.m4` | osobne SAPI `--enable-fpmng`, binarka `php-fpm-ng`, libevent obowiazkowy |
| `fpm/fpm.c` | haczyk `fpm_http_init_main()` w `fpm_run()` — punkt wpiecia dla typow poola |
| `fpm/fpm_http.c` | bramka HTTP |
| `fpm/fpm_http.h` | |

Docelowo dojda `fpm/fpm_conf.c`, `fpm/fpm_status.c` i `fpm/fpm_main.c` —
to jedyne pliki FPM z realnym churnem upstreamu (patrz `docs/NOTES.md`).

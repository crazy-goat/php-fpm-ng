# php-fpm-ng

POC. PHP-FPM z dodatkowymi trybami pracy: HTTP, supervisor, cron i metryki —
tak, żeby obraz kontenera zawierał jedną binarkę i kod aplikacji, bez nginxa,
bez supervisord i bez crona z systemu.

**Grupa docelowa: małe projekty.** Jeden VPS, jedna instancja, typowo aplikacja
plus jeden lub dwa consumery plus kilka cronów. Nie k8s.

Argumentem nie jest wydajność — zmierzone kilka procent CPU przy realnym
obciążeniu (szczegóły w notatkach). Argumentem jest jeden plik konfiguracyjny
opisujący całość i jedna binarka do skanowania.

## Stan na dziś

Działa POC bramki HTTP na libevent, jako gałąź w php-src:
https://github.com/s2x/php-src/tree/fpm-http-poc

Zweryfikowane 2026-09-05:

- pełne statyczne budowanie na musl (`-static-pie`) z opcache, mbstring, curl
  + OpenSSL, zlib, pdo_mysql, sockets, pcntl, posix
- uruchomienie w gołym `FROM scratch`, FPM jako PID 1, HTTP 200, cały obraz
  20 MB w wersji minimalnej
- frontend wybiera `pool.type = fastcgi | fastcgi-ng | http`; brak dyrektywy
  oznacza klasyczny `fastcgi` i zachowuje zgodnosc z upstreamowym FPM
- `fastcgi-ng` i `http` przyjmuja opcjonalne
  `pool.executor = classic | fiber | async` (domyslnie `classic`)
- metryki: `pool.type = status` wystawia wbudowane `/metrics` (Prometheus)
  i `/status` (JSON); metryki aplikacyjne z PHP (`fpm_metric_register/inc/
  set/observe`, NOTES 3k/3w) przez rozszerzenie `ext/fpmng_metrics/`,
  rowniez z CLI przez `fpm_metric_render()`
- executory `fiber` i `async` sa eksperymentalne i nie sa przeznaczone do
  produkcji; Fiber wymaga wylaczonego OPcache, a dla True Async nadal jest to
  zalecane; ograniczenia opisuje `docs/NOTES.md`, sekcje 3t–3u

## Plan

Docelowo **osobne SAPI** w `sapi/fpmng/`, nie fork php-src — `configure.ac`
wykrywa katalogi w `sapi/` globem, więc nie trzeba tknąć żadnego istniejącego
pliku. Szczegóły, decyzje, zmierzone liczby i lista znanych problemów:
[`docs/NOTES.md`](docs/NOTES.md).

## Zalecana konfiguracja poola dla lekkich endpointów

Zysk bez linijki kodu, zmierzony na poligonie (szczegóły: `docs/NOTES.md`, 3m i 3t):

```ini
listen = /run/php/pool.sock          ; UDS zamiast loopbacku TCP: -7..-11 us/req
php_admin_value[max_execution_time] = 0   ; bez setitimer per request: -3 us/req
catch_workers_output = no            ; logi przez error_log()/stderr do wlasnego stosu
request_cpu_tracking = no            ; jesli nikt nie czyta "last request cpu" ani %C
```

`request_terminate_timeout` nadal pilnuje czasu ściennego, więc
`max_execution_time = 0` nie zostawia requestu bez strażnika.

## Budowanie

Skrypty w `build/` uruchamiane w kontenerze Alpine, budowanie out-of-tree:

```sh
docker run --rm \
  -v /sciezka/do/php-src:/src \
  -v $PWD/build-dir:/build \
  -v $PWD:/out \
  alpine:latest sh /out/build/static-full.sh
```

Dwie flagi, bez których to wygląda na zepsute bez powodu:

- `LDFLAGS=-static-pie` — samo `-static` nie działa, bo toolchain Alpine
  domyślnie robi PIE i linker po cichu produkuje binarkę dynamiczną,
  a build kończy się sukcesem
- `PKG_CONFIG="pkg-config --static"` — inaczej statyczny curl nie przechodzi
  testu konfiguracji, bo brakuje jego zależności na linii linkowania

Alpine nie ma `oniguruma-static`, więc mbstring buduje się z `--disable-mbregex`.

## Licencja

PHP License 3.01 — kod pochodzi z PHP-FPM.

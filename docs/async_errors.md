# Znane problemy executora Async

Stan na 2026-09-06. `pool.executor = async` jest eksperymentem i nie jest
przeznaczony do uzycia produkcyjnego. Dziala wylacznie na forku
`true-async/php-src` z zaladowanym `ext/async`; na upstreamowym silniku
`fpm_pool_async_validate()` odrzuca pool z czytelnym komunikatem
(`fpm_pool_async.c:68`).

## Dlaczego ten plik istnieje

Async ma **dokladnie ten sam ksztalt co Fiber** — jeden proces, jeden
`php_request_startup()` na cale zycie procesu ("request-kontener"), wiele
requestow w locie, kazdy w osobnej korutynie (`fpm_pool_async.c:1-9`). Z tego
ksztaltu wynikaja te same trzy problemy, co opisane w
[fiber_errors.md](fiber_errors.md).

Roznica jest taka, ze **Fiber dostal zabezpieczenia, a Async nie**. Fiber
przechodzi przez rdzen coop (`fpm_pool_coop.c`), ktory:

- odrzuca `max_execution_time != 0` w `fpm_coop_validate()`;
- odrzuca wlaczony opcache;
- usuwa procesowe funkcje `pcntl` przez `zend_disable_functions()`
  w `fpm_coop_container_start()`;
- przywraca wpis ini po `set_time_limit()` w `fpm_coop_req_run()`.

Async ma **wlasny** `validate()` i **wlasny** `child_main()`
(`fpm_pool_type.c:124,146`) i nie wola `fpm_coop_container_start()` —
sprawdzone: jedynym wywolaniem w calym drzewie jest `fpm_pool_fiber.c:358`.
Zaden z powyzszych mechanizmow go nie obejmuje.

## Co konkretnie zostaje otwarte

### `max_execution_time` przyjmowane po cichu

`fpm_pool_async_validate()` sprawdza tylko: obecnosc True Async API, ZTS,
zarejestrowany scheduler/reaktor i `pm = static`. Nie sprawdza
`max_execution_time`. Nagłówek pliku sam wymienia `max_execution_time` wsrod
stanu WSPOLNEGO dla requestow w locie (`fpm_pool_async.c:6-8`), wiec
niezerowa wartosc jest przyjmowana i nieegzekwowana — timeout Zend to jeden
`setitimer()`/`SIGPROF` na proces, a proces obsluguje N requestow.

Naprawa symetryczna do Fibera: odrzucic w `fpm_pool_async_validate()`.

### Opcache nie jest sprawdzany

Fiber odrzuca wlaczony opcache, bo maska auto-globali i znaczniki czasu plikow
sa resetowane raz na request-kontener. Async ma ten sam model i **nie ma tego
sprawdzenia**. To jest luka, nie swiadome zezwolenie.

### Procesowe API `pcntl` nie jest blokowane

`pcntl_signal()` ustawia jedna tablice na proces
(`PCNTL_G(php_signal_table)` i `SIGG(handlers)`), a `pcntl_fork()`/`pcntl_exec()`
duplikuja albo podmieniaja caly wielorequestowy proces wraz ze schedulerem,
deskryptorami i requestami w locie. Async nie usuwa tych funkcji.

### `set_time_limit()` nie jest sprzatane po requescie

`set_time_limit()`/`ini_set()` idzie przez `OnUpdateTimeout` do
`zend_set_timeout()` i uzbraja timer procesu. Fiber przywraca wpis ini po
skrypcie; Async nie, wiec wartosc i timer przezywaja request, ktory je zmienil.

## Asymetria wprowadzona swiadomie

Zabezpieczenia Fibera zostaly celowo umieszczone w `fpm_pool_coop.c`, w kodzie,
ktory typ fiber juz posiada — zeby nie dokladac `if (type == ...)` do rdzenia
(kontrakt z `fpm_pool_type.h`). Skutkiem ubocznym jest to, ze Async, majacy
wlasny rdzen, niczego nie odziedziczyl.

Nie jest to przeoczenie, tylko odlozona decyzja: Async wymaga forka silnika,
wiec nikt nie uruchomi go przypadkiem, a wyrownanie zabezpieczen ma sens
dopiero, gdy przestanie byc POC. Warianty:

1. **Skopiowac** sprawdzenia do `fpm_pool_async_validate()` i blokade do
   `fpm_pool_async_child_main()` — najprostsze, duplikuje kod.
2. **Wydzielic** wspolna czesc (walidacja ini + lista blokowanych funkcji) do
   funkcji dzielonej przez oba rdzenie — czysciejsze, bo problem wynika ze
   wspolnego KSZTALTU, a nie ze wspolnej implementacji.

Rekomendacja: wariant 2, ale dopiero przy nastepnej powaznej pracy nad Async.
Dopoki `validate()` odrzuca pool na kazdym upstreamowym silniku, ryzyko
praktyczne jest zerowe.

## Poza pcntl — wspolne z Fiberem

`posix_kill(getmypid(), SIGTERM)` ze skryptu ubija proces z N requestami
w locie. Dotyczy obu executorow i nie jest zablokowane w zadnym.

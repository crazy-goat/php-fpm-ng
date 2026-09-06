# Znane problemy executora Fiber

Stan na 2026-09-06, po hardeningu z brancha `fiber-hardening`. `pool.executor = fiber` jest eksperymentalny i nie jest przeznaczony do użycia produkcyjnego.

## Zakres

Problemy dotyczą konfiguracji:

```ini
pool.type = fastcgi-ng | http
pool.executor = fiber
```

Nie dotyczą produkcyjnych ścieżek `fastcgi`, `fastcgi-ng/classic` ani `http/classic`. Cały kod opisany niżej żyje w `sapi/fpmng/fpm/fpm_pool_coop.c` (wspólny rdzeń „wiele requestów w jednym procesie”), w callbacku `validate` typu i w starcie kontenera — `fpm_conf.c`, `fpm_children.c` i `fpm_pool_type.h` nie zostały tknięte, zgodnie z kontraktem rozszerzalności.

Fiber wymaga wyłączonego OPcache i zerowego `max_execution_time`:

```ini
php_admin_value[opcache.enable] = 0
php_admin_value[max_execution_time] = 0
```

## Podsumowanie: co jest domknięte i czym

Żaden z trzech problemów nie został **naprawiony** — fiber nadal nie ma per-requestowego timeoutu ani izolacji sygnałów. Zamiast po cichu nie działać, konfiguracja **odmawia**, a API **jest zablokowane**:

| Problem | Mechanizm | Co to jest | Co zostaje otwarte |
|---|---|---|---|
| `max_execution_time` ≠ 0 nie przerywa requestu | `fpm_coop_validate()`: efektywna wartość (pool → php.ini) ≠ 0 ⇒ `ALERT` + odmowa startu | odmowa | timeout per fiber nie istnieje |
| `set_time_limit(N)` uzbraja procesowy timer | `fpm_coop_req_run()`: `zend_restore_ini_entry("max_execution_time", DEACTIVATE)` po skrypcie; `php_admin_value` dodatkowo blokuje `ini_set` | mitygacja | w trakcie requestu SIGPROF może trafić w cudzy fiber |
| `pcntl_signal()` przecieka między requestami | `fpm_coop_container_start()`: `zend_disable_functions()` na procesowym API pcntl | blokada | handlery sygnałów są procesowe; brak izolacji |
| `pcntl_fork()` duplikuje wielorequestowy proces | ta sama blokada (`pcntl_fork`, `pcntl_rfork`, `pcntl_forkx`, `pcntl_exec`) | blokada | — |

## `max_execution_time` nie przerywa requestu

Request wykonujący nieskończoną pętlę nie jest przerywany po przekroczeniu skonfigurowanego limitu:

```php
<?php while (true) {}
```

### Przyczyna

Executor Fiber nie wykonuje pełnego `php_request_startup()` i `php_request_shutdown()` osobno dla każdego requestu — robi jeden `php_request_startup()` na życie procesu („request-kontener”, `fpm_coop_container_start`). Timeout Zend jest procesowy: jeden `setitimer()` (`ITIMER_PROF`/`SIGPROF`; na aarch64 macOS `ITIMER_REAL`/`SIGALRM`) w `zend_set_timeout_ex()`. Jeden timer nie reprezentuje niezależnych deadline'ów wielu requestów. Kontener od zawsze robił `zend_unset_timeout()`, więc niezerowa wartość była przyjmowana i po cichu nieegzekwowana.

### Stan po zmianie: odmowa w walidacji

`fpm_coop_validate()` liczy efektywną wartość poola — `php_admin_value`/`php_value[max_execution_time]` z poola (admin przed value, jak `fpm_php_apply_defines` w `fpm_php.c`), a gdy pool jej nie ustawia, wartość z php.ini przez `zend_ini_long()` (domyślnie `30`, `main/main.c`). Wartość ≠ 0 kończy się odmową startu, tak samo jak włączony OPcache:

```text
ALERT: [pool fiber] pool.executor = fiber: max_execution_time = 30 is not enforced (the Zend timeout is one setitimer()/SIGPROF timer per process, and this process runs many requests at once; see docs/fiber_errors.md); set php_admin_value[max_execution_time] = 0 in this pool or max_execution_time = 0 in php.ini
ERROR: failed to post process the configuration
ERROR: FPM initialization failed
```

Konsekwencja: **każdy** pool fiber bez jawnego `php_admin_value[max_execution_time] = 0` przestaje startować, bo domyślne ini to 30. To zamierzone — tak samo działa wymóg wyłączonego OPcache. Zmierzone: `php_admin_value = 1` → odmowa; brak dyrektywy (default 30) → odmowa; `php_value = 5` + `php_admin_value = 0` → start (admin wygrywa); `= 0` → start i obsługa requestów.

### `set_time_limit()` — dziura, która zostawała, i jej mitygacja

Walidacja odrzuca tylko konfigurację. `set_time_limit(N)` (`main/main.c`, `PHP_FUNCTION(set_time_limit)`) idzie przez `zend_alter_ini_entry_ex(..., PHP_INI_STAGE_RUNTIME)` do `OnUpdateTimeout`, a ten woła `zend_set_timeout()` — timer procesu zostaje uzbrojony. Gdy odpali, `zend_timeout_handler` ustawia `EG(timed_out)` i request, który następny wykona opcode — niekoniecznie ten, który wołał `set_time_limit` — dostaje „Maximum execution time exceeded”. Nie jest to crash: bailout łapie `zend_try` w `fpm_coop_execute`.

Mitygacja w `fpm_coop_req_run()`, po skrypcie i po wysłaniu nagłówków: `zend_restore_ini_entry("max_execution_time", ZEND_INI_STAGE_DEACTIVATE)` — dokładnie to, co `zend_ini_deactivate()` robi dla wszystkich wpisów w klasycznym `php_request_shutdown()`. `OnUpdateTimeout` w stage DEACTIVATE rozbraja timer i **nie** uzbraja go na nowo, a wartość ini wraca do wyjściowej. Timer ani `ini_get('max_execution_time')` nie przeżywają requestu, który je zmienił.

Dwie obserwacje z pomiarów:

- Z `php_admin_value[max_execution_time] = 0` (zalecana konfiguracja) `set_time_limit(1)` zwraca `false` — `php_admin_value` blokuje `ini_set` (`fpm_php_zend_ini_alter_master` z `ZEND_INI_SYSTEM`). Timer nigdy się nie uzbraja; mitygacja nie ma czego robić.
- Z `php_value[max_execution_time] = 0` `set_time_limit(1)` zwraca `true`; request wołający je kończy się, następny request z 2 s pętli CPU przeżywa, a `ini_get` w kolejnym pokazuje znów `0`.

**Otwarte:** w trakcie samego requestu, który wołał `set_time_limit(N)`, timer jest uzbrojony i może trafić w fiber innego requestu. Zamknięcie tego to per-fiber timeout (poniżej), nie tania łatka. Nie blokujemy `set_time_limit` — frameworki wołają `set_time_limit(0)` rutynowo, a zero jest nieszkodliwe.

### Pełna naprawa (osobny projekt)

1. deadline przechowywany osobno dla każdego fibera;
2. kolejka timerów zintegrowana z schedulerem;
3. przełączanie aktywnego timera przy suspend/resume;
4. przypisanie `SIGPROF` do aktualnie wykonywanego fibera;
5. bezpieczne przerwanie tylko jednego requestu;
6. obsługa kodu CPU-bound, który nie wraca do event loop;
7. potwierdzenie, że bailout nie uszkadza współdzielonego stanu procesu.

## Handlery `pcntl_signal()` przeciekają między requestami

Handler ustawiony w jednym requeście pozostawał widoczny w następnym requeście obsługiwanym przez ten sam proces:

```php
// Request 1
pcntl_signal(SIGUSR1, static function (): void {});

// Request 2
var_dump(pcntl_signal_get_handler(SIGUSR1) === SIG_DFL); // false
```

### Przyczyna

Przeciekają dwa stany, nie jeden:

- logiczna tablica pcntl `PCNTL_G(php_signal_table)` (`ext/pcntl/php_pcntl.h`) — to ją czyta `pcntl_signal_get_handler()`;
- dyspozycja jądra i tablica Zend: `php_signal4()` (`ext/pcntl/php_signal.c`) → `zend_sigaction()`, która zapisuje `SIGG(handlers)[signo-1]` **i** instaluje `zend_signal_handler_defer` w jądrze (`Zend/zend_signal.c`).

Classic resetuje oba w `PHP_RSHUTDOWN(pcntl)` (`ext/pcntl/pcntl.c`: `php_signal(signo, SIG_DFL)` dla każdego wpisu, `zend_hash_destroy` tablicy), potem `zend_signal_deactivate()` w `php_request_shutdown()`, a przy następnym requeście `zend_signal_activate()` robi `memcpy(&SIGG(handlers), &global_orig_handlers, ...)`. Fiber wykonuje RINIT raz na proces, więc tablica żyje do końca procesu. Sam `zend_signal_activate()` między requestami nic nie da: nie dotyka `PCNTL_G(php_signal_table)`.

### Stan po zmianie: blokada procesowego API pcntl

Na początku `fpm_coop_container_start()`, jeśli `zend_get_module_started("pcntl") == SUCCESS`, wołane jest `zend_disable_functions()` na liście `fpm_coop_disabled_functions` (obok `fpm_coop_rejects`):

```text
pcntl_signal, pcntl_signal_get_handler, pcntl_signal_dispatch, pcntl_async_signals,
pcntl_sigprocmask, pcntl_sigwaitinfo, pcntl_sigtimedwait, pcntl_alarm,
pcntl_fork, pcntl_rfork, pcntl_forkx, pcntl_exec
```

To ten sam mechanizm, którym `fpm_php.c` (`fpm_php_apply_defines_ex`) stosuje `php_admin_value[disable_functions]` w dziecku — usunięcie wpisu z `CG(function_table)` przed pierwszym requestem, po `fpm_php_init_child` (MINIT i `php_admin_value[extension]` już za nami). Funkcja znika: wywołanie daje `Error: Call to undefined function pcntl_signal()`, a `function_exists()` zwraca `false`, więc biblioteki z fallbackiem działają dalej. `extension_loaded('pcntl')` nadal zwraca `true`. `pcntl_alarm` jest na liście, bo `SIGALRM` jest procesowy (a na aarch64 macOS to sygnał timeoutu Zend). `pcntl_wait*`, `pcntl_wifexited` itd. zostają — bez forka są nieszkodliwe, a przydają się przy `proc_open`. `proc_open`/`popen`/`exec` zostają: robią fork+exec, dziecko nie wraca do schedulera.

Kontener loguje jeden `NOTICE` z listą wyłączonych funkcji. Uwaga: to log **workera**; przy `error_log = /dev/stderr` bez `catch_workers_output = yes` nie widać go, tak jak każdego innego logu dziecka.

Zmierzone przez `http/fiber`:

```text
pcntl_loaded=true
function_exists(pcntl_signal)=false
function_exists(pcntl_fork)=false
function_exists(pcntl_waitpid)=true
pcntl_signal=Error: Call to undefined function pcntl_signal()
pcntl_fork=Error: Call to undefined function pcntl_fork()
```

Ten sam skrypt przez `http/classic` i `fastcgi` (bez zmian w konfiguracji, `max_execution_time` domyślne 30): `function_exists(pcntl_signal)=true`, `pcntl_signal=OK handler=SET`, `pcntl_fork=OK`, worker obsługuje kolejne requesty, brak alertów w logu.

### Odrzucone: reset handlerów przez RSHUTDOWN/RINIT pcntl między requestami

Rozważany „tani reset”: po każdym requeście w `fpm_coop_req_run()` wołać `request_shutdown_func` + `request_startup_func` modułu `pcntl` z `module_registry` (`zend_hash_str_find_ptr(&module_registry, "pcntl", 5)`, pola `zend_module_entry` w `Zend/zend_modules.h`). Technicznie ~15 linii. **Odrzucony** z trzech powodów, każdy samodzielnie dyskwalifikujący:

1. **Tick functions rosną per request.** `PHP_RINIT(pcntl)` woła `php_add_tick_function(pcntl_signal_dispatch_tick_function, NULL)` przy każdym wywołaniu (`ext/pcntl/pcntl.c`). `PG(tick_functions)` rośnie o jeden wpis na request, a `php_run_ticks()` iteruje całą listę (`main/php_ticks.c`) — koszt ticka O(liczba requestów), bez końca. Selektywne usunięcie nie jest możliwe: funkcja ticka jest `static` w `pcntl.c`, a `php_deactivate_ticks()` czyści całą listę, łącznie z tickiem `run_user_tick_functions` z `ext/standard/basic_functions.c`. Ominięcie RINIT i wyzerowanie samej tablicy wymagałoby `PCNTL_G()`, czyli symbolu `pcntl_globals` — linkowalnego przy pcntl statycznym, nieosiągalnego przenośnie, gdy pcntl jest ładowany jako `.so`.
2. **Brak izolacji w trakcie requestu.** Handlery są procesowe *podczas* requestu: request B wołający `pcntl_signal(SIGUSR1, ...)` nadpisuje handler A, a sygnał jest dostarczany do tego fibera, który akurat wykonuje tick albo obsługę `vm_interrupt`. Reset po zakończeniu requestu naprawia wyłącznie test „SET → LEAK”, nie izolację.
3. **`SIG_DFL` psuje równoległy fiber.** `PHP_RSHUTDOWN(pcntl)` po requeście A robi `php_signal(signo, SIG_DFL)` także dla sygnałów, na których jeszcze polega równolegle biegnący request B. Kolejny taki sygnał zabija cały wielorequestowy proces (domyślna dyspozycja `SIGUSR1`/`SIGTERM`).

Wniosek: reset byłby kosmetyką z pułapką — usuwa objaw z testu, zostawia i pogarsza realne zachowanie. Blokada jest uczciwa: mówi wprost, że w tym executorze tego API nie ma.

### Pełna naprawa (osobny branch)

Stan handlerów trzeba dołączyć do kontekstu requestu Fiber:

1. zapisywać go przy zawieszeniu fibera;
2. przywracać przed wznowieniem;
3. resetować po zakończeniu requestu;
4. zachować wewnętrzne handlery wymagane przez Zend i FPM;
5. przetestować kilka współbieżnych requestów ustawiających różne handlery.

Dopóki to nie istnieje, blokada zostaje.

## `pcntl_fork()` wewnątrz requestu

`pcntl_fork()` duplikuje cały wielorequestowy proces wraz ze schedulerem libevent, deskryptorami wszystkich połączeń i requestami w locie. Samo `exit()` w potomku nie kończy procesu FPM — kończy skrypt, po czym potomek przechodzi dalszą część `fpm_coop_req_run()` (wysyła **duplikat odpowiedzi** na współdzielony deskryptor) i wraca do pętli workera jako klon. Rodzic czekający przez `pcntl_waitpid()` może blokować się bez końca. `pcntl_exec()` ma ten sam problem od drugiej strony: podmienia obraz całego procesu.

### Stan po zmianie: blokada

`pcntl_fork`, `pcntl_rfork`, `pcntl_forkx` i `pcntl_exec` są na liście `fpm_coop_disabled_functions` (patrz wyżej). Zmierzone: `pcntl_fork=Error: Call to undefined function pcntl_fork()`, `function_exists('pcntl_fork') === false`.

### Odrzucone alternatywy

- `pthread_atfork()` z `_exit()` w potomku — psuje `proc_open`/`popen`, które forkują wewnętrznie. Odpada.
- Strażnik PID w schedulerze (`getpid()` porównywany w `fpm_fiber_after_switch()` z PID-em zapamiętanym przy starcie, `_exit()` przy różnicy) — domyka zawieszenie rodzica, ale nie poprawność: potomek dociera do strażnika dopiero po wysłaniu duplikatu odpowiedzi. Zbędny, gdy `pcntl_fork` jest zablokowany.

## Potwierdzone działające elementy

Dla `fastcgi-ng/fiber` i `http/fiber` na czystym release buildzie PHP 8.5 potwierdzono wcześniej:

- małą i dużą odpowiedź;
- binarny POST;
- FastCGI/HTTP keep-alive i `Connection: close`;
- przeżycie zerwania połączenia przez klienta;
- kolejny request po błędzie klienta;
- reload przez `SIGUSR2`;
- zatrzymanie przez `SIGTERM`;
- wymóg wyłączonego OPcache.

Hardening z tego dokumentu zmierzono 2026-09-06 na debug buildzie `PHP 8.6.0-dev` (master `4e55e35ead7` + 6 łatek z `patches/`, `--disable-all --enable-fpmng --enable-pcntl`), macOS aarch64:

- `http/fiber`: odmowa startu dla `max_execution_time` = 1 i dla domyślnego 30; start dla `= 0`; `php_admin_value` wygrywa nad `php_value`; requesty obsługiwane; pcntl zablokowane; `set_time_limit(1)` (przy `php_value`) nie przeżywa requestu.
- `http/classic` i `fastcgi` w tym samym binarium: `max_execution_time = 30` nie blokuje startu, `pcntl_signal()` i `pcntl_fork()` działają, brak alertów — ścieżki produkcyjne nietknięte.

Ta sama seria powtórzona na poligonie (Ubuntu 26.04, x86_64, gcc 15, ten sam commit i te same łatki) dała identyczne wyniki. To istotne dla `set_time_limit`: na Linuksie timer to `ITIMER_PROF` (czas CPU), więc `set_time_limit(1)` w jednym requeście, a potem 2 s pętli CPU w następnym, bez rozbrojenia skończyłoby się „Maximum execution time of 1 second exceeded” — w logu 0 takich wpisów, request przeżył.

## Rekomendacja

Nie rozszerzać bieżącej poprawki o przebudowę lifecycle Fiber. Kolejność dalszych prac:

1. ~~jawnie odrzucić niezerowe `max_execution_time` dla Fiber~~ — zrobione (odmowa w walidacji);
2. ~~udokumentować lub zablokować procesowe API `pcntl`~~ — zrobione (blokada w kontenerze);
3. na osobnym branchu zaimplementować izolację handlerów sygnałów — dopiero wtedy zdejmować blokadę;
4. per-fiber timeout potraktować jako osobny projekt wymagający testów współbieżności, bailoutów i kodu CPU-bound — dopiero wtedy zdejmować odmowę i mitygację `set_time_limit`;
5. utrzymać oznaczenie Fiber jako eksperymentalnego do czasu rozwiązania tych problemów.

## EKSPERYMENT: `included_files` wspolne dla procesu

`FPMNG_SHARED_INCLUDES=1` (przez `env[]` w poolu — `clear_env = 1` jest
domyslne, wiec bez tego zmienna nie dociera do dziecka). Domyslnie wylaczone.

**Po co.** Bez tego zadna aplikacja z Composerem nie przezywa drugiego
requestu. Lista wczytanych plikow jest per request, tablice funkcji i klas
per proces — wiec drugi request uznaje, ze `vendor/autoload.php` nie byl
wczytany, wczytuje go ponownie i redeklaruje klase, ktora nadal zyje
w procesie. Wspolna lista czyni bootstrap jednorazowym BEZ worker-mode
i bez zmian w aplikacji.

**Zmierzone** (wzorzec bootstrapu: `require vendor/autoload.php`,
`require_once` helpers z funkcja i klasa, `$app = require_once bootstrap/app.php`):

    PRZED:  req1 ok, req2 i req3 Fatal: Cannot redeclare class ComposerAutoloaderInit...
    PO:     req1 ok, req2 ok, req3 ok — 30/30 requestow, zero bledow w logu

Wspolbieznosc I/O nietknieta: 4 rownolegle `fsockopen` po 500 ms nadal
0,525 s lacznie.

### Dwa skutki uboczne — jeden do naprawy, jeden nie

**Glosny, do naprawy:** `require_once` przy drugim wywolaniu zwraca `true`,
a nie wartosc zwrocona przez plik. Wzorzec
`$app = require_once 'bootstrap/app.php'` daje `app=true` od requestu 2
(zmierzone). Do zrobienia: cache'owac wartosc zwracana przez plik
i oddawac ja przy kolejnych wywolaniach zamiast `true`.

**Cichy, NIE do naprawy:** kod wykonywany na gorze pliku wciaganego przez
`require_once` przestaje sie wykonywac od drugiego requestu. Bez bledu, bez
ostrzezenia:

    PRZED:  req1 init_runs=1   req2 init_runs=1     req3 init_runs=1
    PO:     req1 init_runs=1   req2 init_runs=BRAK  req3 init_runs=BRAK

To nie jest blad do usuniecia — to dokladnie ta semantyka, o ktora chodzi
("wykonaj raz"). To jest WYMAGANIE wobec aplikacji: nic istotnego nie moze
dziac sie jako efekt uboczny wczytania pliku. Frameworki sa tu w dobrej
formie (bootstrap buduje obiekty i je zwraca), ale to trzeba potwierdzic na
prawdziwym kodzie, nie na wzorcu.

### Co sie NIE zmienia

- **Opcache dalej odrzucany** przez walidacje — sprawdzone. Przy okazji
  wspolne `included_files` w duzej mierze usuwaja powod, dla ktorego chcialoby
  sie opcache: bootstrap kompiluje sie raz na proces, per request rekompiluje
  sie tylko `index.php`.
- **Statyki klas dalej przeciekaja** miedzy requestami (`Foo::$hits` roslo
  2 -> 3 -> 4). To samo ryzyko co w Octane i Swoole.
- `EG(symbol_table)` zostaje per request — jego rozdzielenie jest wymuszone
  przez `zend_attach_symbol_table` (SIGABRT zmierzony w NOTES 3t).

### Czego ten eksperyment NIE sprawdzil

Prawdziwego Symfony ani Laravela — tylko wzorzec bootstrapu. Nie wiadomo, ile
w realnych frameworkach jest miejsc polegajacych na wartosci zwracanej przez
`require_once` ani na efektach ubocznych wczytania pliku. To jest nastepny
krok i jest wykonalny od reki: binarka istnieje.

### Podmiana plikow na dysku: deploy WYMAGA reloadu

Zmierzone przy `FPMNG_SHARED_INCLUDES=1`, podmiana obu plikow miedzy requestami:

    req1                     entry=ENTRY-1  lib=WERSJA-1
    req2 (po podmianie)      entry=ENTRY-2  lib=WERSJA-1   <- stan MIESZANY
    req3                     entry=ENTRY-2  lib=WERSJA-1
    req4 (po SIGUSR2)        entry=ENTRY-2  lib=WERSJA-2

Skrypt wejsciowy jest czytany z dysku przy KAZDYM requescie, bo wykonujemy go
bezposrednio, a nie przez `require_once`. Wszystko, co on wciaga, zostaje
zamrozone w procesie. Po `git pull` bez reloadu dziala wiec nowy `index.php`
na starym bootstrapie — bledy bez sensu i bez tropu.

To jest ZMIANA ZACHOWANIA, nie tylko nowe ograniczenie: wczesniej podmiana
pliku z funkcja dawala glosny fatal "Cannot redeclare" przy drugim requescie,
teraz dostaje sie cichy stary kod. Dla klas ladowanych autoloaderem
nieswiezosc istniala juz wczesniej (klasa siedzi w tablicy procesu, autoloader
nie jest wolany) — ta zmiana rozciaga ja na caly bootstrap.

Zasada do dokumentacji uzytkownika: **deploy konczy sie `SIGUSR2`**, tak samo
jak w Octane, Swoole i RoadRunnerze. Reload jest graceful i dziala (req4).
Alternatywa dla dev: `fiber.revalidate_freq` ponizej.

### `fiber.revalidate_freq` — worker wymienia sie sam po zmianie pliku

Dyrektywa poola, sekundy, **domyslnie `0` = wylaczone**. Przy `N > 0` worker
zapamietuje mtime/rozmiar/inode kazdego pliku w chwili, gdy silnik go
kompiluje (hook `zend_compile_file` w `fpm_pool_coop_reval.c` — ten sam
moment, w ktorym `opened_path` trafia do `EG(included_files)`; skrypt
wejsciowy jest pomijany, bo i tak jest czytany z dysku per request), a co N
sekund z petli zdarzen robi jeden przebieg `stat()` po tej tablicy. Zadnego
`stat()` per request — koszt to (liczba plikow / N) na sekunde, niezaleznie
od ruchu. Zmiana ktoregokolwiek pliku (mtime, rozmiar, inode po `rename`,
albo `stat()` bledny — plik usuniety w trakcie deployu) daje NOTICE z nazwa
pliku i **drain**: worker przestaje przyjmowac polaczenia, zamyka bezczynne
keep-alive, konczy requesty w locie i wychodzi z kodem 0; master go wymienia
ta sama sciezka, ktora recykluje klasyczny worker po `pm.max_requests`
(`fpm_children_bury`, `restart_child = 1`). To NIE jest sciezka SIGQUIT
(`fcgi_in_shutdown`), ktora porzuca requesty w locie.

Zmierzone (2026-09-06, `pm.max_children = 1`, z `FPMNG_SHARED_INCLUDES=1`
i bez):

    freq = 1:  req1 lib=WERSJA-1 pid=2137 | podmiana | req2 (od razu) lib=WERSJA-1 pid=2137
               req3 (po 1.5 s) lib=WERSJA-2 pid=2166 — NOTICE "lib.php changed on disk (mtime ...)"
    w locie:   slow.php (3 s na gniezdzie) w trakcie podmiany: "worker will exit after
               1 request(s) in flight finish", "draining, 1 request(s) still in flight",
               request skonczyl sie po 3.09 s z poprawna odpowiedzia; "abandoned" w logu: 0
    freq = 0:  podmiana -> lib=WERSJA-1 ten sam pid po 2.2 s (zachowanie jak dotad)
    freq = 2:  sweepy co 2.000 s; podmiana tuz po sweepie: +0.3 s stare, +1.3 s stare, +2.5 s nowe;
               200 requestow w oknie -> licznik stat() rosl o 1 na sweep, nie o 200
    fastcgi / http classic: bez zmian; dyrektywa odrzucana:
               ALERT: [pool fc] 'fiber.revalidate_freq' is not supported by pool.type = fastcgi
    4 x fsockopen 500 ms rownolegle: 504 ms lacznie (klasyczny worker: 2017 ms)

Dlaczego domyslnie `0`: (1) proces sam sie zabijajacy to nowe zachowanie i ma
byc opt-in — istniejace configi dzialaja jak dotad; (2) w produkcji sa pliki
wczytywane przez `require`, ktore aplikacja LEGALNIE nadpisuje w trakcie pracy
(skompilowane szablony Twig/Blade, `bootstrap/cache/*.php`, kontener DI) —
z automatem kazdy taki zapis wymienialby workera; (3) deploy przez `rsync`
nie jest atomowy — worker moglby wystartowac na polowie skopiowanego drzewa
i wymienic sie jeszcze raz w nastepnym oknie. W dev `fiber.revalidate_freq = 1`
daje "skopiowales pliki, dziala nowy kod" bez pamietania o `SIGUSR2`.

Ograniczenia:

- **Symlink-deploy (`current -> release-N`) nie jest wykrywany.**
  `EG(included_files)` trzyma realpathy, wiec pliki starego wydania sie nie
  zmieniaja — przelaczenie dowiazania nie rusza ich mtime. Tam nadal
  `SIGUSR2`.
- W trakcie drain nowe polaczenia czekaja w backlogu gniazda az wystartuje
  nastepca (przy `pm.max_children = 1` tyle, ile trwa najdluzszy request
  w locie; przy wiekszej liczbie dzieci przejmuja je pozostale). Bezczynne
  keep-alive bramki sa zamykane od razu — bramka traktuje to jak EOF po
  `pm.max_requests` i laczy sie na nowo; request, ktory bramka wyslala
  dokladnie w tym oknie, przepada tak samo jak dzis przy `pm.max_requests`.
- Request zawieszony poza schedulerem (`Fiber::suspend()` w glownym fiberze,
  juz dzis logowany jako "dropping it") nigdy nie zejdzie z licznika w locie,
  wiec drain sie nie skonczy; tick loguje wtedy co sekunde "draining, N
  request(s) still in flight".
- Pierwszy stan pliku wygrywa: `stat()` po sciezce robimy tuz PO pierwszej
  kompilacji; podmiana w tym mikrosekundowym oknie zapisze nowy stan przy
  starym kodzie.
- Sledzone sa tylko pliki przechodzace przez `zend_compile_file`: `eval()`,
  dane czytane `file_get_contents` (konfiguracja YAML/JSON) i szablony
  nie-PHP nie sa widziane.

## Polaczenia trwale: ZABLOKOWANE (zmierzone na poligonie)

`EG(persistent_list)` jest PROCESOWA, a rdzen coop jej nie podmienia przy
przelaczaniu requestow — wiec dwa requesty w locie moga dostac ten sam socket.
Protokoly bazodanowe sa naprzemienne (zapytanie-odpowiedz), wiec to nie jest
spowolnienie, tylko rozjechany protokol.

**Zmierzone** (Ubuntu 26.04, epoll, jeden worker, 4 rownolegle requesty, ten sam
DSN, MySQL 8):

    PDO z ATTR_PERSISTENT, przebieg 1:  3 requesty WISZA do timeoutu klienta (60 s),
                                        1 konczy sie poprawnie
    PDO z ATTR_PERSISTENT, przebieg 2:  3 x HTTP 502, czwarty
                                        "PDOException: Trying to access array offset on false",
                                        worker padl i zostal wymieniony
    PDO bez persistent, ten sam test:   1,019 s, czysto

    mysqli z prefiksem "p:":            NIE psul sie — cztery ROZNE identyfikatory
                                        sesji (215-218), tag_ok=true, 1,017 s

Czyli bezpieczenstwo zalezy od tego, czy dany klient pilnuje zajetosci
polaczenia: PDO nie pilnuje, mysqli pilnuje. Z warstwy transportu tego nie
widac i nie kontrolujemy tego, wiec **blokujemy oba**. Lepiej odmowic glosno
przy nawiazywaniu polaczenia niz rozjechac protokol w losowym requescie.

### Gdzie i dlaczego tam

W fabryce transportu (`fpm_pool_fiber_xport.c`), nie w `validate()` — persistent
to atrybut polaczenia podawany w kodzie aplikacji, a nie dyrektywa konfiguracji,
wiec w momencie walidacji poola nie ma czego sprawdzac. Odmowa dotyczy tylko
executora fiber (`fpm_pool_fiber_can_wait()`); `fastcgi` i `http/classic` sa
nietkniete — zmierzone, `classic` z persistent dalej dziala jak dotad.

### Komunikat: dwa odbiorcy

Programista dostaje `E_WARNING` z wyjasnieniem. **Ale PDO lapie blad polaczenia
i rzuca wlasny `PDOException: SQLSTATE[HY000] [2002] Unknown error while
connecting`, wiec do autora kodu nasze ostrzezenie NIE dociera** — zmierzone.
Dlatego operator dostaje osobny `ZLOG_NOTICE` w logu workera, raz na proces.

**`persistent_id` NIE jest logowany**: PDO sklada ten klucz z DSN wraz
z uzytkownikiem i haslem, wiec trafiloby to do error logu.

### Regresje sprawdzone

    pdo nietrwaly N=4       1,018 s
    mysqli nietrwaly N=4    1,018 s
    phpredis N=4 (BLPOP)    1,116 s
    classic z persistent    2,019 s (bez zmian)

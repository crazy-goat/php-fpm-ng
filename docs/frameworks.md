# Symfony i Laravel na `pool.executor = fiber`

Pomiar z 2026-09-06 na poligonie (Ubuntu 26.04, epoll, MySQL 8.4, Redis,
phpredis 6.3.0RC1), binarka z commita `a5800e4`. **Symfony 8.1.6** (skeleton +
orm-pack + security-bundle, Doctrine ORM 3.6, sesje i cache na Redisie),
**Laravel 13.30.1** (`SESSION_DRIVER=redis`, `CACHE_STORE=redis`,
`REDIS_CLIENT=phpredis`, Eloquent na MySQL). Kazdy w trzech poolach: `classic`,
`fiber` bez flagi, `fiber` z `env[FPMNG_SHARED_INCLUDES] = 1`. `pm = static`,
`pm.max_children = 1`.

## Werdykt

**Symfony: TAK, pod warunkiem** — `FPMNG_SHARED_INCLUDES=1`, wlasny
`public/index.php` bez `symfony/runtime`, brak sesji PHP i stanowego firewalla
przy wspolbieznosci (czyli: API bezstanowe albo jeden request w locie).

**Laravel: NIE.** Sekwencyjnie dziala po trzech zmianach w `index.php`, ale przy
wiecej niz jednym requescie w locie miesza sesje uzytkownikow i polaczenia do
baz. Bez wspolbieznosci fiber nie ma sensu, wiec to jest "nie".

## Gdzie dziala, dziala bardzo dobrze

Symfony, endpoint czytajacy MySQL (DBAL + ORM) i Redis (store + cache pool):

    N=8 /mix?sleep=0.3    fiber 0,351 s    classic 2,950 s    8/8 poprawnych danych
    N=4 SELECT SLEEP(1)   fiber 1,028 s    classic 4,221 s
    N=8 SELECT SLEEP(1)   fiber 1,046 s    classic 8,436 s
    600 requestow         fiber 172 req/s  classic 15,5 req/s
                          RSS 40,2 -> 42,4 MB, ten sam worker, 0 bledow

"8/8 poprawnych danych" znaczy: kazdy z osmiu rownoleglych requestow dostal
SWOJE id, item, tag, wartosc z Redisa i z cache. To jest teza, ktora
sprawdzalismy, i ona sie broni.

## Co dokladnie trzeba zmienic w aplikacji

**Symfony: jeden plik.** `public/index.php` przepisany na styl sprzed
`symfony/runtime` (siedem linii: `require vendor/autoload.php`, `bootEnv()`,
`new Kernel`, `handle()`, `send()`, `terminate()`). Mniejszym kosztem sie nie da
— patrz nizej.

**Laravel: jeden plik, trzy zmiany** w `public/index.php`:
1. `require_once` -> `require` dla `bootstrap/app.php` (naprawia
   `Call to a member function handleRequest() on true`); `bootstrap/app.php` nic
   nie deklaruje, wiec `require` jest tam poprawniejsze;
2. `define('LARAVEL_START', ...)` -> `defined(...) || define(...)`, bo stale sa
   procesowe i zachowuja wartosc z pierwszego requestu;
3. `$_SERVER; $_ENV; $_REQUEST;` — **obejscie NASZEGO bledu**, patrz "Autoglobale".

Dodatkowy wymog wobec aplikacji: **zadnych deklaracji klas i funkcji w plikach
wciaganych `require` per request**. Laravel robi `require routes/web.php` przy
kazdym boocie, wiec klasa zadeklarowana w tym pliku daje "Cannot redeclare".

## Punkt odniesienia: bez `FPMNG_SHARED_INCLUDES`

Oba frameworki, req1 OK, req2 fatal:

    Symfony: Cannot redeclare class ComposerAutoloaderInite8d3a95... (autoload_real.php:5)
             stack: vendor/autoload.php(20) -> vendor/autoload_runtime.php(5) -> public/index.php(5)
    Laravel: Cannot redeclare class ComposerAutoloaderInitcda2add... (autoload_real.php:5)
             + Warning: Constant LARAVEL_START already defined

## Dlaczego cache wartosci `require_once` NIE pomoze

Byl na liscie TODO jako naprawa wzorca `$app = require_once bootstrap/app.php`.
Dla Laravela faktycznie naprawia ten jeden blad. **Dla Symfony nie daje nic**:
`public/index.php` robi `require_once vendor/autoload_runtime.php`, a caly
runtime (`$runtime->getRunner($app)->run()`) jest **efektem ubocznym tego
include**. Gdy include staje sie no-opem, dostajemy HTTP 200 z pustym cialem
i zero wpisow w logu — aplikacja sie nie wykonuje. Problem nie jest w wartosci
zwracanej, tylko w tym, ze kod ma sie wykonac.

Zmierzone tez obejscie A (`require` zamiast `require_once` w index.php): nie
pomaga, bo `autoload_runtime.php:3` ma
`if (true === (require_once __DIR__.'/autoload.php') || ...) return;`.

Wniosek: cache wartosci `require_once` to naprawa jednego wzorca w jednym
frameworku, nie rozwiazanie modelu include.

## Autoglobale — NASZ blad, tani i krytyczny

`$_SERVER`, `$_ENV` i `$_REQUEST` powstaja przez `auto_globals_jit` **w chwili
KOMPILACJI** pliku, ktory ich uzywa. Przy wspolnych `included_files` vendor nie
jest rekompilowany, a `index.php` Laravela sam ich nie dotyka — wiec:

    Warning: Undefined global variable $_SERVER in vendor/vlucas/phpdotenv/.../ServerConstAdapter.php:42
    Warning: Undefined global variable $_ENV   in .../EnvConstAdapter.php:42
    Fatal: Uncaught TypeError: Request::createRequestFromFactory(): Argument #6 ($server)
           must be of type array, null given (symfony/http-foundation/Request.php:2094)

**`php_admin_flag[auto_globals_jit] = off` NIE pomaga** — zmierzone. Executor nie
tworzy autoglobali per request na zadnej sciezce poza JIT-em kompilacji.

Symfony przezylo tylko przypadkiem: reczny `index.php` odwoluje sie do
`$_SERVER['APP_ENV']`, wiec JIT je tworzy.

**To pada w kazdej aplikacji, ktorej skrypt wejsciowy sam nie dotyka
autoglobali.** Naprawa jest tania: wolac callbacki autoglobali w
`fpm_coop_req_enter`.

## Wspolbieznosc: gdzie sie lamie

### Symfony — sesje PHP

    20/20 rund: HTTP 500, "Failed to start the session."
                "PHP Request Startup: Cannot call session save handler in a recursive manner"

Sekwencyjnie (uzytkownik A, potem B) dziala: rozne sid, count rosnie. Pada
dopiero przy wspolbieznosci, bo `ext/session` trzyma stan w PROCESIE
(`PS(session_status)`, `in_save_handler`, `$_SESSION`): fiber A wisi w I/O
Redisa wewnatrz `read()` handlera, fiber B wola `session_start()`.

Stanowy firewall (`http_basic` + sesja) z tego samego powodu: 2/6 rownoleglych
requestow OK, 4/6 blad.

Kontener DI, `Request` i `RequestStack` (depth 1) sa czyste per request —
`/who` bez auth zwraca `user: null`. Przecieka tylko statyk klasy
(`leaked_static_user: "alice"`), czyli procesowy stan, ktorego nie rozdzielamy.

### Laravel — statyki frameworka, i to jest wyciek DANYCH

    N=8 /mix?sleep=0.3:  1 x 200, 4 x 500, 3 requesty wisialy do timeoutu 60 s
    N=4 SELECT SLEEP(1): 4/4 timeout
    bramka: "upstream: Connection reset by peer" / "no answer from upstream"

    laravel.log:
      QueryException 2014 Cannot execute queries while other unbuffered queries are active
      RedisException: read error on connection to 127.0.0.1:6379
      SQLSTATE[08S01] [1159] Got timeout reading communication packets
      PDOException: Error at offset 0 of 3 bytes   (unserialize CUDZEJ odpowiedzi)

    sesje: 19/20 rund zle, w tym  user=B  sess_user=A   <- dane sesji A w requescie B
    auth:  po Auth::login alice i bob, 5/6 rownoleglych /me zwrocilo user: null

Przyczyna, spojna ze wszystkimi objawami: `Container::$instance` i
`Facade::$app` to statyki procesu. Request B, bootstrapujac nowa `Application`,
podmienia je; gdy fiber A wraca z I/O, `app()`, `DB::`, `Redis::` i `session()`
rozwiazuja sie w kontenerze B. Dwa requesty dziela jedno polaczenie PDO i Redis
oraz jeden Store sesji.

To jest ta sama pulapka, przed ktora chroni Octane — z ta roznica, ze Octane
NIGDY nie ma dwoch requestow w locie w jednym workerze.

Dodatkowo: `HandleExceptions` w req1 ustawia `display_errors = Off`
i `error_reporting = -1`, co zostaje dla procesu (ini nie jest przywracane per
request), przez co fatale na fiberze daja 500 bez tresci.

## Lista napraw po naszej stronie

W kolejnosci: tanie i blokujace wszystko najpierw.

1. **Autoglobale per request** niezaleznie od `auto_globals_jit` — wolac
   callbacki w `fpm_coop_req_enter`. Bez tego pada kazda aplikacja, ktorej
   skrypt wejsciowy sam ich nie dotyka. Jedyna tania pozycja z tej listy.
2. **Stan `ext/session` per request** — swap `ps_globals` przy enter/leave, tak
   jak robimy z SG i OG. Bez tego sesje i security Symfony nie dzialaja
   wspolbieznie.
3. **Przywracanie WSZYSTKICH wpisow ini per request** (odpowiednik
   `zend_ini_deactivate`), nie tylko `max_execution_time`. `define()` zostaje
   procesowe i to sie nie zmieni.
4. **Model include** — cache wartosci `require_once` nie ratuje Symfony. Albo
   per-requestowe `included_files` z pomijaniem redeklaracji juz istniejacych
   klas i funkcji przy rekompilacji, albo udokumentowany wymog wlasnego
   `index.php`.
5. **Laravel: statyki klas per fiber** (`Container::$instance`, `Facade::$app`).
   To kierunek, ktory w forku True Async zrobiono i **cofnieto**. Bez tego
   Laravel na fiberze to co najwyzej jeden request w locie.
6. **Bramka HTTP: fallback na front controller** — `/mix` daje dzis
   "File not found", trzeba `/index.php/mix`. Osobny brak, opisany tez
   w NOTES ("index.php hardcoded / brak try_files").

## Czego ten pomiar NIE objal

- Laravel `classic` jako linia odniesienia dla testow rownoleglych i sesji
  (pojedyncze requesty OK).
- Laravel: stabilnosc 300 requestow (przerwane przez wiszace requesty). RSS po
  ~55 requestach 73 MB wobec 40 MB u Symfony.
- Symfony z `APP_ENV=prod`, `pm.max_children > 1`, `fiber.revalidate_freq`.

## Uwaga metodyczna

Bramka HTTP nie ma fallbacku na front controller, wiec wszystkie zadania szly
przez `/index.php/mix?...` (PATH_INFO). Stary build na poligonie byl
`--disable-all` i trzeba go bylo odbudowac z mbstring, session, ctype,
tokenizer, dom, iconv, fileinfo, phar i curl — bez nich composer i oba
frameworki sie nie instaluja.

---

# AKTUALIZACJA 2026-09-06: po naprawach 1, 2, 3 i izolacji wartosci ini

Werdykty wyzej sa juz NIEAKTUALNE dla Symfony. Zmierzone ponownie na tym samym
poligonie, `pool.executor = fiber`, `env[FPMNG_SHARED_INCLUDES] = 1`,
`pm.max_children = 1`, sesje przez handler uzytkownika, N=8 rownoleglych.

## Co doszlo

1. **Autoglobale per request** — `$_SERVER`/`$_ENV`/`$_REQUEST` wymuszane w
   `fpm_coop_req_run`, niezaleznie od `auto_globals_jit`. Obejscie
   `$_SERVER; $_ENV; $_REQUEST;` w `index.php` Laravela nie jest juz potrzebne.
2. **Przywracanie WSZYSTKICH wpisow ini** na koncu requestu
   (`zend_ini_deactivate()` zamiast jednego `max_execution_time`).
3. **Izolacja stanu `ext/session` per request** — swap globali modulu przy
   enter/leave plus RINIT/RSHUTDOWN per request (`fpm_pool_coop_session.c`).
   Adres `ps_globals` brany z `mh_arg2` wpisu ini `session.save_path`, bez
   zaleznosci linkera: build z `--enable-session=shared` i z sesja wylaczona
   linkuja sie i dzialaja.
4. **Izolacja WARTOSCI wpisow ini miedzy requestami w locie**
   (`fpm_pool_coop_ini.c`) — bez tego `ini_get()` widzial cudze `ini_set()`.

## Symfony: TAK, takze z sesjami

    N=8, /session?user=X&sleep=0.3
    8/8 http=200, rozlaczne sid, ok=true, handler=user we wszystkich osmiu
    druga runda tymi samymi ciasteczkami: 8/8 count=2, te same sid

Poprzednio: **20/20 rund HTTP 500**, `Failed to start the session`,
`Cannot call session save handler in a recursive manner`. Logi czyste — 421
linii "WARNING" w logu poola to wylacznie debug Symfony w trybie dev
przepuszczony przez `catch_workers_output`, zero prawdziwych bledow.

Skrypt testowy uzywa handlera z PRAWDZIWYM blokujacym I/O wewnatrz `read()`
(`blPop` na Redisie), czyli dokladnie tym, co wywolywalo pierwotna awarie.

## Laravel: nadal NIE — i tryb awarii zmienil sie z GLOSNEGO na CICHY

    user    sess_user  ok      count  app_oid  container_request_oid
    alice   alice      True    1      1261     1329
    bob     bob        True    1      225      1842
    carol   carol      True    1      1540     1604
    dave    bob        False   3      225      1842
    eve     bob        False   2      225      1842
    frank   frank      True    1      1011     1051
    grace   bob        False   4      225      1842
    heidi   heidi      True    1      225      1842

5/8 poprawnych, 3/8 dostalo CUDZA sesje. Piec requestow dzieli jeden obiekt
`Application` (`app_oid = 225`) i jeden obiekt `Request` (1842) — to jest
`Container::$instance` i `Facade::$app`, czyli pozycja 5 z listy napraw,
ktorej nie ruszalismy.

Poprzednio: 1 x 200, 4 x 500, trzy requesty wiszace do timeoutu 60 s, plus
`Cannot execute queries while other unbuffered queries are active` i
`unserialize` cudzej odpowiedzi Redisa. Teraz: **8 x 200, zero wyjatkow, zero
fatali, pusty `laravel.log`**.

To NIE jest poprawa z punktu widzenia wdrozenia. Wczesniej Laravel na fiberze
sie wywracal i bylo to widac. Teraz zwraca 200 z cudzymi danymi i nic tego nie
zglasza. Werdykt "Laravel: NIE" jest po tej zmianie MOCNIEJSZY, nie slabszy.

Roznica miedzy frameworkami nie lezy w ich jakosci: Symfony trzyma stan w
kontenerze przekazywanym jawnie, Laravel w statyku klasy — a statyki klas sa
procesowe i my ich nie rozdzielamy.

## Znane ograniczenie naprawy 4

Izolowane sa WARTOSCI wpisow ini (to, co widzi `ini_get`/`ini_set`), ale nie
procesowe globale, ktore niektore wpisy ustawiaja przez `on_modify` — np.
`precision` trafia do `core_globals` i jest realnie uzywane przez `var_dump`
i `serialize`, wiec tam nadal jest wspolne miedzy requestami w locie. Pelna
naprawa wymagalaby swapowania globali kazdego takiego modulu, tak jak
`fpm_pool_coop_session.c` robi to dla sesji.

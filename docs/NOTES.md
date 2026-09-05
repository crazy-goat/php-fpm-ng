# php-fpm-ng — notatki projektowe

Stan na 2026-09-05. Plik jest pamięcią projektu: decyzje, zmierzone liczby,
otwarte pytania i lista znanych problemów. Aktualizować przy każdej zmianie
kierunku, żeby nie przerabiać po raz drugi tych samych ustaleń.

## 1. Po co to jest

Jedna binarka plus kod aplikacji w obrazie kontenera i nic więcej. Bez nginxa,
bez supervisord, bez crona z systemu, bez shella. Jeden plik konfiguracyjny
opisuje aplikację razem z workerami i cronami.

Grupa docelowa: małe projekty. Jeden VPS, jedna instancja, typowo aplikacja
plus jeden lub dwa consumery plus kilka cronów. **Nie** k8s i nie duże firmy.

Argument sprzedażowy to NIE wydajność (patrz sekcja 4), tylko:
- jeden plik konfiguracyjny na całą aplikację razem z workerami
- obraz do przeskanowania to jedna binarka
- brak sidecarów i brak drugiego systemu do nauczenia

## 2. Architektura: osobne SAPI, nie fork FPM

`configure.ac:289` woła `esyscmd(./build/config-stubs sapi)`, a `build/config-stubs`
robi `for stubfile in $dir/*/config.m4`. Czyli **każdy katalog wrzucony do `sapi/`
jest wykrywany automatycznie**. Zero łatek na istniejące pliki php-src.

Stąd struktura:
- osobne repo zawiera tylko `sapi/fpmng/`
- build klonuje php-src na **przypiętym tagu**, kopiuje/symlinkuje katalog,
  `./buildconf --force && ./configure --enable-fpmng`
- aktualizacja PHP = podbicie taga

### Co bierzemy przez referencję, a co na własność

Churn w `sapi/fpm/fpm` od 2024-01 (commity):

```
20  fpm_main.c        <- na własność (własne wejście SAPI i tak musi być inne)
16  fpm_conf.c        <- na własność (dyrektywy, typy poola)
11  fpm_status.c      <- na własność (wyjście świadome typów)
 1  fpm.c             <- na własność (haczyk przy run_child:)
 1  fpm_process_ctl.c <- referencja
 0  fpm_children.c    <- referencja
 0  fpm_worker_pool.c <- referencja
```

Zarządzanie procesami jest praktycznie zamrożone — to jest fundament pod
supervisor i cron i nie trzeba go przejmować. Reszta stabilnych plików
(`fpm_unix.c`, `fpm_stdio.c`, `fpm_scoreboard.c`, `fpm_events.c`,
`fpm_signals.c`, `fpm_sockets.c`, `fpm_shm.c`, `zlog.c`, ...) też przez
referencję — `PHP_ADD_SOURCES` przyjmuje katalog, ale **to trzeba zweryfikować
na żywo** zanim się na tym oprzemy.

Koszt utrzymania: ~14 commitów upstreamu rocznie do przejrzenia w czterech
przejętych plikach. Kilka godzin w roku.

Sztuczka: zmiany w `fpm_conf.c` pisać jako jedna linia dopisana do tablicy
dyrektyw plus nowe funkcje na końcu pliku — wtedy konflikt przy merge'u jest
trywialny.

### Szew: typ poola

`fpm.c:88` — `fpm_run()` w rodzicu nigdy nie wraca, w dziecku dochodzi do
etykiety `run_child:` i zwraca deskryptor, który `fpm_main.c:1793` odbiera
i wchodzi w pętlę `fcgi_accept_request`. **To jest cały punkt wpięcia.**

Typ poola musi być prawdziwą abstrakcją (struktura z operacjami: `init`,
`spawn_child`, `child_main`, `status`, `validate_config`), po jednym pliku na
typ. Nie `if`-ami — przy czterech trybach `if`-y rozjadą kod i to jest
dokładnie to, czego chcieliśmy uniknąć.

`ini_fpm_pool_options` to jedna płaska tablica — trzeba dołożyć informację,
które dyrektywy są sensowne dla którego typu, i **odrzucać** bezsensowne
kombinacje (np. `pm.max_children` w poolu supervisora), a nie ignorować.

## 3. Typy poola

Brak `pool.type` → `fcgi`. Zero BC.

| typ | co robi | stan |
|---|---|---|
| `fcgi` | jak dziś | gotowe, tylko dispatch |
| `http` | bramka HTTP na libevent przed poolem | POC działa, patrz sekcja 6 |
| `supervisor` | N długo żyjących procesów, wskrzeszanych | do zrobienia, małe |
| `cron` | skrypt odpalany z harmonogramu | do zrobienia, małe |

Metryki to rzecz przekrojowa, nie typ poola.

### http

Pod modelem typów `type = http` powinno znaczyć: pool gada FastCGI po gniazdku
uniksowym w katalogu runtime, na przód idzie bramka. Socket FastCGI staje się
wewnętrzny. UDS jest o 11-17% tańsze od loopbacku TCP (zmierzone). Znika
`FPM_HTTP_LISTEN` i gimnastyka z portem +1.

### supervisor

Mniejsze niż się wydaje. `pm = static` z `pm.max_children = 1` to już
"jeden proces, wskrzeszany po śmierci". Logi lecą do logu FPM przez istniejące
przechwytywanie pipe'em (`fpm_stdio_child_said`). User/group/chdir/rlimity
z `fpm_unix.c`. SIGTERM z `fpm_signals.c`.

Dziecko przy `run_child:` nie wraca do pętli accept, tylko wykonuje skrypt.
Jedno wywołanie w tym samym miejscu co dziś `fpm_http_init_main()`.

Bonus nie do podrobienia: dziecko jest forkiem mastera z **już zainicjowanym
PHP**, więc consumer startuje bez odpalania interpretera i z gotowym opcache.
supervisord tego nie potrafi, bo tylko uruchamia `php consumer.php` od nowa.

Do dopisania: `listen` musi stać się opcjonalne (pool supervisora nie nasłuchuje),
dyrektywa ze ścieżką skryptu, backoff przy szybkiej śmierci, semantyka
`pm.max_requests` (skrypt kończy się sam po N zadaniach, FPM go podnosi —
to samo co `--max-jobs`, rozwiązuje wycieki pamięci), oznaczenie typu w statusie.

### cron

Ta sama ścieżka dziecka co supervisor, inna polityka odpalania. Master ma już
timery (`fpm_events.c:86` używa `fpm_event_set_timer`). Czyli timer + fork,
kilkadziesiąt linijek na wierzchu supervisora.

Parser crontaba: 5 pól, ~100 linijek, bez zależności. Alternatywa
`cron.interval = 300s` jest o klasę mniejsza, ale ludzie oczekują `*/5 * * * *`.

**Cztery rzeczy do rozstrzygnięcia, bo inaczej gryzą:**
1. Nakładanie się przebiegów — domyślnie pomijać i logować. To najczęstszy błąd
   w implementacjach crona.
2. Czas — liczyć w UTC. Przy czasie lokalnym DST daje przebieg podwójny albo
   żaden.
3. Zgubione ticki — **nie nadrabiamy**. Zapisać to, bo ktoś kiedyś doda
   nadrabianie i dostaniemy dwanaście przebiegów naraz.
4. Timeout na przebieg + logowanie kodu wyjścia.

Uwaga: to singleton na master, nie na klaster. Przy jednej instancji (nasza
grupa docelowa) nieistotne, ale musi być w dokumentacji.

### metryki

Struktura w `fpm_shm_alloc`, funkcje PHP, wystawienie w formacie tekstowym
Prometheusa. Precedens: `pm.status_path` jest dopasowywany wewnątrz requestu
w `fpm_main.c:1832`.

**Pułapka — kardynalność.** Pamięć dzielona ma stały rozmiar, a etykiety
potrafią pochodzić z danych użytkownika. Stała liczba slotów i jasna decyzja,
co po ich wyczerpaniu: odrzucamy i logujemy, nie rośniemy.

**Rozwidlenie do rozstrzygnięcia wcześnie.** Funkcje PHP w FPM rejestruje SAPI
`cgi-fcgi` w `fpm_main.c`. W CLI ich nie ma. Jeśli metryki mają działać też
z CLI, muszą być **rozszerzeniem**, które pod fpm-ng używa backendu w pamięci
dzielonej, a poza nim lokalnego albo pustego. Przeniesienie tego później boli.

Pool supervisora nie obsługuje requestów, więc jego metryk nie ma jak wystawić
z niego samego — musi je czytać inny pool. Scoreboard jest per pool w shm,
więc da się, ale to nowy wzorzec dostępu.

## 3a. Self-runner: jedna binarka z aplikacją w środku

Cel: `fpm-ng pack app.phar php.ini fpm.conf -o myapp` daje jeden plik, który
zawiera wszystko. Wisienka na torcie, robiona na końcu.

**To nie jest kompilacja, tylko doklejenie.** Payload na koniec gotowej binarki
plus stopka na samym końcu: magiczne bajty, offset, rozmiar. Przy starcie
fpm-ng czyta `/proc/self/exe`, sprawdza ostatnie bajty, i jeśli znajdzie
magiczne — bierze `php.ini`, `fpm.conf` i phara stamtąd. Jak nie znajdzie,
działa normalnie z dysku.

Konsekwencja: **pakowanie nie wymaga żadnego toolchaina**. Ani kompilatora, ani
źródeł PHP. Trwa sekundę. To jest przewaga nad FrankenPHP, który też umie
wbudować aplikację, ale przez przebudowę z Go embed — czyli u użytkownika musi
być Go.

Nie wymyślamy formatu: phar dokładnie tak działa (stub doklejany do pliku,
phar potrafi się odnaleźć w środku większego pliku).

### Do przemyślenia

1. **Zapis — największy problem.** W phara nie da się pisać. Aplikacja
   potrzebuje cache, sesji, logów i uploadów. Konfiguracja musi twardo
   rozdzielić "kod, niezmienny, w binarce" od "stan, na wolumenie". Zdrowa
   dyscyplina, ale zaskoczy każdego z Laravelem czy Symfony, bo te domyślnie
   piszą do katalogu projektu. Rozwiązać projektowo i opisać, bo inaczej
   pierwsze zderzenie z realną aplikacją kończy się "nie działa".
2. Pliki statyczne z phara idą przez wrapper strumieniowy — wolniej niż z dysku.
   Przy naszym ruchu bez znaczenia, ale warto wczytać je do pamięci przy starcie.
3. Opcache z `validate_timestamps=0` — kod w binarce się nie zmienia, więc
   działa idealnie i szybciej niż z dysku.
4. `/proc/self/exe` to Linux; w kontenerze `/proc` jest montowane, więc scratch
   OK. Mieć zapasową ścieżkę przez `argv[0]` na uruchomienie poza kontenerem.
5. Opcjonalnie podpis payloadu (phar to potrafi) — odmowa startu przy podmianie
   kodu aplikacji.

### OGRANICZENIE PROJEKTOWE DLA PUNKTU 1 PLANU

Wczytywanie konfiguracji musi umieć wziąć dane **ze strumienia albo bufora
w pamięci**, nie tylko ze ścieżki na dysku. Jeśli zaszyjemy to na `open(path)`,
przerabianie potem boli.


## 3b. Rozszerzenia binarne innych dostawców (New Relic, ionCube, ...)

**Statyczna binarka nie może załadować żadnego `.so`.** W musl statyczny
`dlopen` jest zaślepką zwracającą błąd; z glibc statycznym jest formalnie
możliwy, ale zepsuty i niewspierany. Wkompilować się nie da, bo dostawcy dają
`.so`, nie źródła.

Dotyczy: New Relic, Datadog, Blackfire, ionCube, SourceGuardian, Zend Guard.

**Rozwiązanie: dwa warianty budowania z jednego źródła, różnica tylko w flagach
linkowania.**

1. *statyczny* — scratch, bez rozszerzeń innych dostawców
2. *dynamiczny* — mały obraz bazowy (Alpine/distroless, ~8 MB), `.so` się
   ładują. To zwykłe budowanie, `.so` wskazuje się przez `extension_dir`
   i `extension=` w `php.ini`. Nic specjalnego do napisania.

Ładnie łączy się z self-runnerem (sekcja 3a): `.so` może jechać **w payloadzie**
i być rozpakowane przy starcie do tmpfs, a `extension_dir` ustawione na ten
katalog. Nadal jeden plik do wysłania, tylko binarka dynamiczna. Podmiana
rozszerzenia = przepakowanie, bez przebudowy. **Prawdopodobnie to powinien być
domyślny wariant**, a pełna statyka wariantem dla tych, którzy nic nie potrzebują.

Priorytet: ionCube ważniejszy niż New Relic. Małe projekty rzadko mają APM
(drogi, enterprise), ale sporo z nich hostuje komercyjne oprogramowanie
zakodowane ionCube'em (WHMCS itp.).

Haczyk niezależny od statyki: te rozszerzenia są budowane pod konkretne ABI
(wersja PHP, NTS/ZTS, libc). Większość dostawców ma dziś warianty musl dla
Alpine, ale nie wszyscy — **sprawdzić z konkretnymi dostawcami zanim się
cokolwiek obieca**. Może się okazać, że wariant dynamiczny musi być w dwóch
odmianach, glibc i musl.

### DECYZJA DO PODJĘCIA WCZEŚNIE: `php_sapi_name()`

Pół ekosystemu sprawdza nazwę SAPI. Frameworki testują
`PHP_SAPI === 'fpm-fcgi'` żeby wiedzieć, czy jest `fastcgi_finish_request()`.
New Relic po niej rozpoznaje typ aplikacji. Narzędzia monitorujące tak samo.

Jeśli nazwiemy się inaczej, wszystko to przestaje działać **po cichu**.
Propozycja: raportować `fpm-fcgi`. To nie jest oszustwo — jesteśmy FPM
z dodanymi trybami. Ale decyzja musi być świadoma, a nie odkryta za pół roku,
gdy komuś nie zadziała `fastcgi_finish_request()`.

## 3c. Ustalenia z budowania (2026-09-05)

- **musl nie ma `sys/queue.h`** (to nagłówek BSD, glibc dołącza go z grzeczności).
  Bramka używała z niego makr TAILQ. Naprawione: `fpm_http.c` dołącza go przez
  `__has_include` i uzupełnia brakujące makra własnymi definicjami, każde pod
  osobnym `#ifndef` — bo nagłówki libevent potrafią wciągnąć częściowy
  `sys/queue.h`.
- **`LDFLAGS=-static` NIE WYSTARCZA.** Toolchain Alpine domyślnie robi PIE,
  `-pie` kłóci się z `-static`, więc linker po cichu produkuje binarkę
  dynamiczną, a build kończy się sukcesem. Trzeba sprawdzać `file`. Właściwa
  flaga to **`-static-pie`** — działa i zachowuje ASLR.
- Domyślny docroot bramki to `chdir` poola albo cwd. W scratchu cwd to `/`,
  więc bez `chdir` dostajesz "File not found". Do zastąpienia jawną dyrektywą.
- `php_sapi_name()` zwraca dziś `fpm-fcgi` — czyli decyzja z sekcji 3b sprowadza
  się do "nie zmieniać".

### WYNIK PUNKTU 0 (2026-09-05): ZIELONE

Statyczna binarka `static-pie` na musl uruchomiona w gołym `FROM scratch`:
HTTP 200, PHP wykonane, FPM jako PID 1, `user = 65534` bez `/etc/passwd`.
**Cały obraz 20 MB** razem z PHP, bramką i aplikacją. Konfiguracja testowa
w `~/ngbuild/scratch/` na poligonie (Dockerfile, fpm.conf, www/hello.php).

Czyli koncepcja stoi. Ale patrz niżej — scratch został zdegradowany do celu
drugorzędnego.

- Budowanie testowe: `~/ngbuild/build3.sh` (dynamiczne) i `build4.sh`
  (`-static-pie`) na poligonie, out-of-tree w Alpine
  (`docker run -v php-src:/src -v ngbuild/build:/build -v ngbuild:/out alpine`),
  `--disable-all --enable-fpm --with-fpm-http`, `LDFLAGS=-static`.


## 3d. DECYZJA (2026-09-05): bazą jest Alpine, nie scratch

Piotr: "scratch jest celem drugorzędnym — Alpine dla musl i obrazy GCP
wystarczą, jakieś tam minimal".

Konsekwencje, wszystkie na plus:
- **Punkt 0 planu przestaje być bramką.** Dynamiczne budowanie po prostu działa.
  Największe ryzyko projektu zdjęte.
- Rozszerzenia `.so` innych dostawców działają domyślnie — problem z sekcji 3b
  rozwiązany, ionCube i New Relic bez obejść.
- Znikają trzy brakujące pliki (`resolv.conf`, certyfikaty, `tzdata`) —
  przychodzą z obrazu bazowego.
- Statyka zostaje **opcjonalnym wariantem** dla tych, którzy nic nie potrzebują,
  a nie fundamentem. Zweryfikowana, że działa (patrz 3c), więc opcja istnieje.

## 3e. Szkielet SAPI — zweryfikowany (2026-09-05)

`sapi/fpmng/` zbudowane przeciw **nietkniętemu** php-src. `git status` w drzewie
upstreamu pokazuje wyłącznie `?? sapi/fpmng/` — żaden istniejący plik nie
ruszony. Binarka `php-fpm-ng` startuje, bramka HTTP odpowiada, `php_sapi_name()`
zwraca `fpm-fcgi`.

Nasze pliki (reszta kopiowana z `sapi/fpm/` przez `build/prepare.sh`):
`config.m4`, `Makefile.frag`, `fpm/fpm.c`, `fpm/fpm_http.c`, `fpm/fpm_http.h`.

### Co trzeba było przemianować, żeby oba SAPI współistniały

- makra `AC_DEFUN`: `PHP_FPM_*` → `PHP_FPMNG_*` (inaczej "already defined")
- zmienne: `SAPI_FPM_PATH`, `BUILD_FPM`, `FPM_EXTRA_LIBS`, `PHP_FPM_OBJS`
- **opcje configure też**: `--with-fpm-systemd` → `--with-fpmng-systemd` itd.
  Sama zmiana nazwy zmiennej nie wystarcza — `PHP_ARG_WITH` generuje zmienną
  z nazwy opcji, więc przemianowanie tylko zmiennej sprawia, że test czyta
  niezdefiniowaną wartość i odpala się mimo wyłączenia (systemd wywalił build).
- `Makefile.frag` — cel `fpm:` musi być `fpmng:`, inaczej
  `No rule to make target 'fpmng'`

### PLIKI ŹRÓDŁOWE GENEROWANE, NIE ZASZYTE

Pierwsza próba miała listę plików zaszytą w naszym `config.m4` i od razu pękła:
nasza kopia pochodziła z nowszego php-src, który nie ma `events/devpoll.c`,
a budowaliśmy przeciw starszemu, który go ma → `undefined reference to
fpm_event_devpoll_module`.

Dlatego `config.m4` ma placeholder `@FPMNG_SOURCES@`, a `build/prepare.sh`
wyciąga listę z `sapi/fpm/config.m4` **tego konkretnego php-src** i dopisuje
nasze pliki. Ta klasa dryfu jest przez to załatwiona na stałe — i to jest wzorzec
do powtórzenia wszędzie, gdzie kusi zaszycie czegoś z upstreamu.

### Drobiazg operacyjny

Katalog budowania powstaje w kontenerze i należy do roota — czyszczenie z hosta
wymaga `sudo rm -rf`.


## 4. Zmierzone: wydajność NIE jest argumentem

Poligon 192.168.8.103, k3d, i7-6700T. Pełne dane w pamięci projektu Claude
(`reference_k3d_bench_poligon.md`).

CPU na request, pomiar czysty (`wrk -t1 -c2`, węzeł nienasycony):

| skrypt | nginx+fpm | bramka | oszczędność |
|---|---|---|---|
| hello.php (~0) | 227 µs | 128 µs | 44% |
| work.php (~1 ms) | ~1590 µs | ~1400 µs | 12% |
| w5.php (7.7 ms) | 7917 µs | 7116 µs | 10% |
| w20.php (74 ms) | 74132 µs | 73548 µs | 0.8% |

Realna aplikacja (20-100 ms CPU) leży między dwoma ostatnimi wierszami —
**kilka procent**. Przepustowość analogicznie: 2.21x na pustym skrypcie,
1.06x przy 1 ms, 1.01x przy 5 ms.

Sama bramka kosztuje **najwyżej ~25 µs/request** (C razem 128 µs vs sam
php-fpm w B 104 µs). To cały budżet dla dalszych optymalizacji.

**Wnioski, do których nie wracamy:**
- io_uring — odpada, atakuje część z tych 25 µs, a wymaga drugiego backendu
  pętli zdarzeń tylko pod Linuksem
- wariant in-process (HTTP w workerze) — odpada, rząd 50-60 µs zysku za
  wejście HTTP w cykl życia workera (`max_requests`, `request_terminate_timeout`,
  brak miejsca na kolejkowanie)
- **żadnych dalszych optymalizacji wydajności**, cała energia w funkcje

Metodologia: poligon ma 4 rdzenie fizyczne / 8 wątków i wrk siedzi na tym samym
węźle. Przy `-t4 -c64` pomiar mierzy walkę o hyperthready, nie koszt hopa
(ta sama para dała 1670 µs vs 234 µs różnicy). Miarodajne jest dopiero
`wrk -t1 -c2`.

## 5. Otwarte pytania

### TLS — najważniejsze, do rozstrzygnięcia przed pierwszym commitem

Przy małym projekcie na VPS-ie nie ma load balancera. Trzy opcje:
1. Przed fpm-ng stoi Caddy/Traefik — ale wtedy "nic więcej nie potrzebujesz"
   przestaje być prawdą
2. fpm-ng umie HTTPS łącznie z ACME — spory kawałek roboty
3. Terminacja TLS poza zakresem, w dokumentacji wprost (Cloudflare / reverse proxy)

Kontekst konkurencyjny: FrankenPHP celuje w tę samą grupę i ma jedną binarkę,
HTTP, pliki statyczne, automatyczne HTTPS przez Caddy, tryb worker. **Nie ma**
deklaratywnych consumerów i cronów. Czyli bramka HTTP jest u nas częścią
najmniej wyróżniającą, a supervisor i cron tym, czego nie ma nikt inny.
Jeśli trzeba będzie gdzieś przyciąć ambicje — raczej w HTTP.

### Statyczna binarka — do sprawdzenia PIERWSZE

Cała koncepcja obrazu bez systemu stoi na tym, że da się zlinkować statycznie
PHP + FPM + libevent + pcre + zlib. Z musl idzie gładko, z glibc walka z NSS.
**Jeśli to okaże się piekłem, koncepcja bierze w łeb** — lepiej wiedzieć przed
napisaniem czterech typów poola.

Dobra wiadomość: `fpm_unix.c:51` ma `fpm_unix_is_id` — jeśli w `user`/`group`
wpisać liczbę, FPM nie woła `getpwnam`. Czyli `user = 65534` działa bez
`/etc/passwd`. To zwykle wywala scratch.

### Zakres hot-reloadu

Prawdziwy hot-reload (diff konfiguracji, dotknięcie tylko zmienionego poola)
jest realny, ale walczy z architekturą opartą na `exec` — kilka tygodni.
Nie na start. Patrz sekcja 7.

## 6. Znane problemy i braki

### BŁĄD w obecnym POC bramki

`fpm_http.c:1065` rejestruje `fpm_cleanup_add(FPM_CLEANUP_PARENT, ...)`, ale
przy reloadzie master idzie ścieżką `FPM_CLEANUP_PARENT_EXEC`
(`fpm_pctl_exec` robi `execvp(saved_argv[0], saved_argv)`). Czyli przy `reload`
bramki prawdopodobnie nie dostaną SIGTERM, zostaną osierocone i będą trzymać
port, a nowy master nie zdoła się zbindować. **Nie zweryfikowane
uruchomieniowo** — do sprawdzenia jednym testem. Poprawka to jedna linijka.

### Blokery (bez tego nie da się tego nikomu włączyć)

- Konfiguracja w zmiennych środowiskowych (`FPM_HTTP_LISTEN`, `FPM_HTTP_GATEWAYS`,
  `FPM_HTTP_REUSEPORT`, `FPM_HTTP_IDLE_MS`) — muszą być dyrektywy z walidacją
- Domyślnie włączone — teraz każdy pool TCP dostaje bramkę na porcie +1, czyli
  otwiera port, o który nikt nie prosił
- Brak kontroli dostępu — FastCGI ma `listen.allowed_clients`, bramka nie ma nic
- Brak respawnu bramek — forkowane raz w `fpm_run()`, master trzyma tylko pidy
  żeby je ubić. Padnięta bramka nie wraca, a przy `reuseport` połowa ruchu
  trafia w martwą kolejkę

### Funkcjonalne (bez tego nie zastąpi nginxa)

- **Pliki statyczne** — wszystko idzie do PHP. Przy koncepcji scratch to jest
  bloker, nie opcja: bez nginxa aplikacja nie ma skąd wziąć CSS-a. Spora robota:
  stat, sendfile/mmap, ETag, Range, cache headers, typy MIME
- Brak TLS (patrz sekcja 5)
- Brakujące zmienne CGI: `SERVER_PORT`, `SERVER_ADDR`, `HTTPS`, `REQUEST_SCHEME`,
  `AUTH_TYPE`, `REMOTE_USER`. `SERVER_PORT` boli najbardziej — frameworki budują
  z niego absolutne URL-e
- Brak obsługi `X-Forwarded-For` — za proxy `REMOTE_ADDR` to proxy
- Brak access logu
- `index.php` zaszyty na sztywno, brak odpowiednika `try_files`

### Twardość

- Sprawdzanie ścieżki jest tekstowe (NUL, `/../`, końcowe `/..`). Brak `realpath`
  i sprawdzenia, że wynik jest pod docrootem → symlink wyprowadza na zewnątrz.
  Ratuje dziś tylko `security.limit_extensions` po stronie FPM. Naprawić **bez**
  dokładania `stat` na request
- Limity zaszyte: 32 MB body, 64 KB nagłówków CGI
- Brak timeoutów po stronie klienta (slow loris)
- Przy pełnym poolu oddajemy 502, powinno być 503 z `Retry-After`
- Brak backpressure przy wysyłaniu body — duży upload ląduje w pamięci bramki

### Niezawodność supervisora i crona

Przy jednej instancji na VPS-ie **nie ma klastra, który wyłapie awarię**.
Backoff, próg błędów i widoczne logowanie to funkcja, nie higiena. Kod wyjścia
różny od zera musi być widoczny, nie tylko na poziomie debug.

### Wdzięczne zatrzymanie

Ważniejsze od hot-reloadu: zerwane zadanie zostawia śmieci w bazie, a sekunda
przerwy w HTTP nie zostawia nic. Consumer musi dostać SIGTERM i dokończyć
bieżące zadanie. Cron w trakcie przebiegu — albo dopalić, albo pominąć.

### Czego NIE robimy

HTTP/2, kompresja, cache. To są rzeczy, dla których istnieje nginx.

### Scratch: pliki, których zabraknie aplikacji (nie nam)

- brak `/etc/resolv.conf` → brak DNS, połączenie do bazy po nazwie hosta pada
- brak paczki certyfikatów → każdy `https://` do zewnętrznego API pada
- brak `tzdata` → `date()` w UTC niezależnie od `date.timezone`

Żadnego nie da się rozwiązać w kodzie — to dane, nie funkcje. Realnie jest to
`FROM scratch` + binarka + kod + trzy pliki danych. Mówić to uczciwie od początku.

### Konfiguracja

`include=` **już działa**, z globem (`fpm_conf.c:1383`, używa `php_glob`).
Czyli `include=/etc/fpmng/conf.d/*.conf` jest gotowe, nic nie trzeba pisać.

Wymaganie projektowe: plik dla "aplikacja + dwa consumery + trzy crony" musi
mieścić się w jakichś czterdziestu linijkach. Jeśli będzie dwieście,
przegrywamy z docker-compose i supervisord mimo lepszej techniki.

Brak shella w scratch = brak entrypointa. Konfiguracja musi być w pełni
deklaratywna plus podstawianie zmiennych środowiskowych w pliku. **Do
sprawdzenia**, na ile FPM to dziś potrafi — bez tego jeden obraz nie obsłuży
dev i prod.

## 7. Kolejność prac

0. **Statyczna binarka na musl w scratch, serwująca `hello.php`.** Dzień roboty,
   rozstrzyga czy koncepcja stoi. Reszta planu jest sensowna niezależnie, ale to
   jedyne założenie, które może zabrać ze sobą wszystko.
0b. Rozstrzygnąć TLS (sekcja 5).
1. `pool.type` jako szew: tablica operacji, domyślnie `fcgi`, `fcgi` i `http`
   tylko przenoszą istniejący kod. Zero nowej funkcjonalności — chodzi o to,
   żeby zobaczyć, czy abstrakcja jest dobra. Jeśli `http` nie wchodzi gładko,
   jest zła.
2. `supervisor`. Sprawdza abstrakcję na przypadku naprawdę innym od `fcgi`.
3. `cron`. Druga polityka odpalania na tej samej maszynerii. Dobry test szwu:
   jeśli trzeba pod niego ruszać `fpm_children.c`, polityka spawnowania nie
   została wydzielona czysto.
4. Metryki. Tu decyzja SAPI kontra rozszerzenie.
5. Pliki statyczne.
6. Self-runner (sekcja 3a) — niezależny od reszty, ale ograniczenie
   projektowe z 3a musi być spełnione już w punkcie 1.
7. Reload zrobiony porządnie: gniazdka przeżywają, requesty się dopalają,
   consumer dostaje SIGTERM i kończy zadanie. Opcjonalnie obserwowanie plików
   konfiguracyjnych. **Nie** prawdziwy hot-reload — zostawić na później,
   kiedy będzie wiadomo, czy ktoś na to narzeka.

## 8. Utrzymanie: nowa wersja PHP = przebudowa

SAPI kompiluje się **w** binarkę PHP i nie ma stabilnego ABI. Każde wydanie
(8.5.11 → 8.5.12) wymaga zbudowania binarki na nowo przeciw temu tagowi.

To nie jest wada tej architektury — dystrybucje robią dokładnie to samo ze
swoimi paczkami `php-fpm`. A skoro i tak wypuszczamy obraz kontenera, to
"przebudowa" znaczy "CI odpala się na nowym tagu i pushuje obraz".

Skala pracy:
- **patch** (8.5.11 → 8.5.12): mechaniczne, wewnętrzne API się nie zmienia,
  CI po prostu przechodzi
- **minor** (8.5 → 8.6): może wymagać poprawek, głównie w `fpm_main.c`
- **major** (9.0): na pewno wymaga

Dlatego: php-src pinowany **tagiem, nie gałęzią**, i podbijany świadomie.
CI z macierzą dwóch-trzech wersji PHP to jedyny sposób, żeby dowiedzieć się
wcześnie, że upstream coś zmienił.

## 9. Sprawy formalne

- **Licencja**: kod pochodzi z FPM → PHP License 3.01. Nie ma wyboru.
- **Nazwa**: "PHP" jest znakiem towarowym PHP Group i mają politykę jego
  używania. `php-fpm-ng` jako nazwa produktu prosi się o list z prośbą
  o zmianę. Roboczo może zostać, przed publikacją dać coś własnego.

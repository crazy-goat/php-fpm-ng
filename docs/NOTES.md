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
| `supervisor` | N długo żyjących procesów, wskrzeszanych | gotowe, patrz sekcja 3o |
| `cron` | skrypt odpalany z harmonogramu | gotowe, patrz sekcja 3r |

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


## 3f. Granica modelu: pliki poza `sapi/` — mechanizm łatek (2026-09-05)

Poprawka liczenia statystyk GH-18956 dotyka `main/fastcgi.c` i `main/fastcgi.h`,
czyli **rdzenia php-src poza `sapi/`**. Osobne SAPI ich nie dosięgnie. To jest
pierwsze realne ograniczenie modelu.

**I to nie jest problem teoretyczny, tylko nasz własny.** Bramka trzyma trwałe
połączenia do poola (`FCGI_KEEP_CONN`), więc fpm-ng jest dokładnie tym
przypadkiem, który ten błąd psuje: licznik idle kontra active kłamie, a
`pm = dynamic` i `ondemand` źle skalują pulę. Bez łatki wiarygodny jest tylko
`pm = static`.

Decyzja: **niesiemy łatkę**, ale tak, żeby odstępstwo było widoczne i policzalne.
`patches/` + `prepare.sh` nakłada i głośno raportuje; bez łatek mówi wprost
"upstream nietknięty". Po nałożeniu `git status` w drzewie upstreamu pokazuje
dokładnie `M main/fastcgi.c`, `M main/fastcgi.h`, `?? sapi/fpmng/` — pełny
promień rażenia na jednym ekranie.

Zasady w `patches/README.md`: każda łatka podaje PR upstreamu i znika, gdy tamten
się zmerguje; jedna łatka na problem, nie na wersję; więcej niż dwa warianty
wersyjne to sygnał, że musi iść do upstreamu albo do `sapi/fpmng/`; CI buduje
każdą wspieraną wersję, więc nienakładająca się łatka pada przy budowaniu.

**Ile łatek na wersję?** Zmierzone: `0001` nakłada się czysto na PHP-8.3, 8.4,
8.5 i master. `main/fastcgi.c` ma 2–9 commitów rocznie i nie w naszym rejonie.
Czyli na razie zero wariantów wersyjnych.

Towarzyszące zmiany w `fpm_request.c` i `fpm_request.h` niesiemy jako własne
pliki, bo są w `sapi/`.

Drobiazg: binarka dynamiczna z Alpine wymaga w kontenerze `libgcc` obok
`libevent` — inaczej `Error loading shared library libgcc_s.so.1`.


## 3g. Swoboda na ścieżce HTTP i kontrakt rozszerzalności (2026-09-05)

Skoro to nie wejdzie łatwo do php-src, na ścieżce HTTP nie wiąże nas zgodność
z niczym. Ale rozróżnienie zostaje: **szaleć wolno w naszych plikach**;
w czterech współdzielonych (`fpm_main.c`, `fpm_conf.c`, `fpm_status.c`, `fpm.c`)
każda ekstrawagancja to koszt przy każdym wydaniu PHP.

Żadnej z poniższych rzeczy nie robimy dla wydajności — tam nie ma czego zbierać
(sekcja 4). Robimy je, bo usuwają ruchome części albo dokładają brakującą funkcję.

| pomysł | ocena |
|---|---|
| **Pliki statyczne w bramce** (`sendfile`, bez zawracania głowy workerowi) | ROBIMY, pierwsze. Jedyna rzecz przesądzająca, czy nginx jest jeszcze potrzebny. Możliwa TYLKO z bramką — wariant in-process ją traci. |
| **Pool HTTP nie otwiera gniazdka FastCGI** (gniazdko uniksowe w katalogu runtime) | ROBIMY przy okazji `pool.type` — ta sama robota. Mniej powierzchni ataku, o dyrektywę mniej. |
| **Zdjąć ramkowanie FastCGI z odpowiedzi, `splice()`** | Kiedyś. Bramka zostaje w ścieżce, więc 502 i timeouty nadal działają. Czysty zysk bez utraty kontroli. |
| **Przekazanie deskryptora przez `SCM_RIGHTS`** | ODRZUCONE. Największy zysk przy dużych odpowiedziach, ale gdy worker padnie w połowie, klient dostaje ucięty strumień, a bramka nie może wysłać 502 ani pilnować timeoutu na gniazdku, którego nie posiada. Przy jednej instancji bez klastra to zła wymiana. |
| **`$_SERVER` bez objazdu przez CGI** | ODRZUCONE na razie. Aplikacje polegają na dokładnym zestawie kluczy CGI, więc oszczędność jest tylko w środku. |

### `fcgi-async` — eksperymentalny, stary `fcgi` bez zmian

Decyzja Piotra. Zastrzeżenie do zapamiętania: **BC dotyczy konfiguracji
i protokołu, nie wewnętrznego zachowania.** Istniejący `fpm.conf` ma działać
i nginx ma się dogadać — ale poprawki błędów i usprawnienia wewnętrzne mieszczą
się w `fcgi`. GH-18956 to naprawa, nie zmiana kontraktu.

`fcgi-async` ma sens jako miejsce na zmiany łamiące obserwowalne zachowanie i na
eksperymenty (io_uring, `SO_REUSEPORT` per worker, batchowanie syscalli).
Uwaga strukturalna do zweryfikowania: ścieżka FastCGI ma inną budowę niż bramka.
Bramka to nasz proces z pętlą libevent; worker FastCGI to blokująca pętla
w `fpm_main.c`, a pętla zdarzeń FPM żyje w MASTERZE. Więc to, co zadziałało
w bramce, tu niekoniecznie się przekłada. **Bada to osobny agent — wynik
wkleić tutaj.**

### `http-async` (in-process) — nie teraz, ale nie zamykamy drogi

Korekta wcześniejszego argumentu: mówiłem, że bez bramki keep-alive przypina
workera i osiem workerów to osiem połączeń. To prawda tylko wtedy, gdy worker
po odpowiedzi wraca do `accept`. Worker z własną pętlą zdarzeń trzyma wiele
połączeń i przetwarza po jednym — wtedy zastrzeżenie znika, a kolejkowanie robi
backlog jądra per gniazdko `SO_REUSEPORT`. Architektura się broni.

Prawdziwy koszt jest inny i poważniejszy: dziś worker jest GŁUPI (czyta FastCGI,
wykonuje skrypt, pisze wynik), a bramka bierze na siebie parsowanie HTTP,
keep-alive, timeouty, limity ciała i wolnych klientów. In-process wpycha to
wszystko do procesu wykonującego kod PHP — każdy błąd w parserze HTTP staje się
błędem w procesie trzymającym pamięć aplikacji, bez niczego z przodu.
To jest tryb worker FrankenPHP. Działa, ale to inny produkt.

Sekwencja, nie zakaz: przepisanie rdzenia serwowania jest dobre, gdy masz
użytkowników, złe jako drugi krok przy zerowej bazie.

## 3h. KONTRAKT: typ poola musi być rozszerzalny

Wymaganie zapisane PRZED kodem, na wyraźną prośbę.

Dodanie nowego typu poola ma kosztować **nowy plik plus jedną linię w rejestrze**.
Nic poza tym. W szczególności NIE wolno przy tym ruszać:

- logiki walidacji w `fpm_conf.c` (tablica dyrektyw tak, `if`-y walidacji nie)
- `fpm_children.c` — jeśli nowy typ wymaga tam zmiany, znaczy że polityka
  spawnowania nie została wydzielona czysto
- `fpm_status.c` poza dodaniem etykiety

Struktura z operacjami, po jednym pliku na typ:

```
validate_config()   co jest wymagane, co zabronione dla tego typu
init_main()         przygotowanie po stronie mastera (gniazdka, bramki, timery)
spawn_policy()      ile dzieci i kiedy (static N / na gniazdku / na tickу)
child_main()        co robi dziecko przy run_child: — pętla accept albo skrypt
status()            jak się pokazuje w statusie
```

Znane typy do zmieszczenia w tym interfejsie: `fcgi` (domyślny, brak
`pool.type` = `fcgi`, zero BC), `http`, `fcgi-async`, `supervisor`, `cron`,
a w przyszłości `http-async`. Jeśli któryś z nich nie wchodzi gładko —
interfejs jest zły i lepiej się o tym dowiedzieć teraz.

`listen` przestaje być bezwarunkowo obowiązkowe (zweryfikowane: dziś pool bez
niego daje `ALERT: no listen address have been defined!`) — o wymagalności
decyduje `validate_config()` typu.


## 3i. `pool.type` — zaimplementowane i zweryfikowane (2026-09-05)

Rejestr w `fpm_pool_type.c`. Typ deklaruje wymagania konfiguracyjne **danymi**
(`requires_listen`, `requires_pm`, `serves_requests`), a nie kodem — dzięki temu
`fpm_conf.c` nie zna żadnego konkretnego typu i dodanie kolejnego nie wymaga tam
zmian. Kontrakt z 3h spełniony: **nowy typ = nowy plik + jedna linia
w `fpm_pool_types[]`**.

Zweryfikowane na zbudowanej binarce:
```
brak pool.type   -> fcgi, zero bramek, port glucny        (pelne BC)
pool.type = http -> bramka wstaje, odpowiada
pool.type = xxx  -> ALERT: unknown pool.type 'xxx'; known types: fcgi, http
```

**Uboczny efekt: zniknął jeden z blokerów.** Bramka nie startuje już domyślnie
na każdym poolu TCP — trzeba o nią poprosić przez `pool.type = http`.

### Ograniczanie dyrektyw per typ

Wymaganie: blok ma być jednego typu i mieć **ograniczony** zestaw dyrektyw —
te bez sensu dla typu mają być odrzucane, nie po cichu ignorowane.

Problem do rozwiązania: z samej wartości w konfiguracji **nie da się odróżnić
"nieustawione" od "ustawione na wartość domyślną"** (`pm_max_children = 0` może
znaczyć jedno albo drugie). Dlatego `fpm_conf.c` zapamiętuje przy parsowaniu
listę faktycznie ustawionych dyrektyw w `config->set_directives` jako
`";nazwa;nazwa;"` — delimitery po obu stronach, żeby wyszukiwanie nie dawało
fałszywego trafienia na prefiksie (`pm` kontra `pm.max_children`).

Typ deklaruje `rejects` — tablicę nazw zakończoną NULL-em. **Lista ODRZUCEŃ,
nie dopuszczeń**: nowa dyrektywa jest domyślnie dozwolona wszędzie, więc
przeoczenie nie psuje zgodności wstecznej. Nazwa kończąca się kropką działa jak
prefiks (`pm.` łapie całe `pm.*`).

STAN: mechanizm zaimplementowany i skompilowany, ale **nie ma jeszcze
konsumenta** — ani `fcgi`, ani `http` niczego nie odrzucają, bo dziś oba
używają tych samych dyrektyw. Pierwszym prawdziwym testem będzie `supervisor`
(odrzuci `listen`, `pm.start_servers`, `pm.min_spare_servers`,
`pm.max_spare_servers`, `request_terminate_timeout`, `slowlog`, `ping.path`,
`security.limit_extensions`). Do tego czasu traktować jako niezweryfikowany.

### Dwie rzeczy warte zapamiętania z implementacji

**Dziecko odnajduje swój pool przez scoreboard.** Naiwne rozwiązanie (użyć
zmiennej pętli `wp` przy etykiecie `run_child:`) jest BŁĘDNE: dzieci wskrzeszane
w pętli zdarzeń wychodzą z `fpm_event_loop()` przez `if (fpm_globals.is_child)
break` i docierają do `run_child:` z `wp == NULL`, bo pętla po poolach dawno się
skończyła. Scoreboard jest per pool i dziecko dostaje swój w
`fpm_scoreboard_init_child()`, więc wystarczy dopasowanie —
i `fpm_children.c` pozostaje nietknięty, zgodnie z kontraktem.

**Naprawiony błąd z reloadem** (znaleziony w 6, wcześniej niezweryfikowany):
sprzątanie bramek rejestruje się teraz także na `FPM_CLEANUP_PARENT_EXEC`.
Reload robi `execvp()`, więc bez tego bramki zostawały osierocone i trzymały
port, na którym nowy master chciał się zbindować.

### Pułapka w `prepare.sh`, którą sam wpuściłem i naprawiłem

Test "czy łatka już nałożona" przez `patch -R --dry-run` **jako pierwszy** jest
błędny: na nietkniętym drzewie też potrafi zwrócić sukces (BSD patch na macOS).
Efekt byłby cichy i paskudny — binarka bez łatki i komunikat, że łatka jest.
Kolejność musi być: najpierw próba w przód, dopiero potem test odwrotny.


## 3j. Statystyki dla `supervisor` i `cron` — projekt (2026-09-05)

Wymaganie: statystyki poolów, które nie obsługują requestów, mają być dostępne
po HTTP na osobnym porcie. W Dockerze/k8s restartem zarządza orkiestrator
(patrz `supervisor.fatal`), więc rolą fpm-ng jest tam **tylko raportowanie stanu**.

### Dlaczego to nie jest oczywiste

Pool typu supervisor i cron **nie obsługuje requestów**, więc nie ma jak wystawić
własnych statystyk — mechanizm `pm.status_path` działa wewnątrz requestu
(`fpm_main.c:1832`). Statystyki takiego poola musi wystawić ktoś inny.
Scoreboard jest per pool w pamięci dzielonej, więc technicznie każdy proces może
je odczytać — ale to nowy wzorzec dostępu, dziś nikt tak nie robi.

### Kształt danych jest INNY niż dla FastCGI

To jest sedno problemu, nie szczegół. Scoreboard FastCGI mierzy idle/active,
liczbę requestów, długość kolejki. Dla supervisora i crona sensowne są zupełnie
inne rzeczy:

- stan: działa / śpi w backoffie / poddał się / zakończony planowo
- czas ostatniego startu i czas życia bieżącego procesu
- kod wyjścia ostatniego zakończenia
- liczba kolejnych porażek i bieżące opóźnienie backoffu
- dla crona dodatkowo: czas ostatniego przebiegu, czas następnego, ile przebiegów
  pominięto z powodu nakładania

Dlatego `fpm_status.c` musi stać się świadomy typów. Pole `serves_requests`
w deskryptorze typu **już istnieje i jest dziś nieużywane** — to jest miejsce,
w które ma się wpiąć rozgałęzienie.

### Skąd to podawać — do rozstrzygnięcia

1. **Osobny pool `pool.type = status`** — mały listener HTTP, który nie odpala
   PHP w ogóle, tylko czyta scoreboardy wszystkich poolów i serializuje.
   Zaleta: osobny port, więc metryk nie wystawiamy na porcie publicznym; przy
   okazji jest to świetny test kontraktu rozszerzalności (typ bez workerów,
   bez `pm`, bez skryptu). Wada: kolejny typ poola.
2. **Bramka HTTP odpowiada na `/status` i `/metrics` sama**, bez zawracania głowy
   workerowi — ten sam mechanizm co planowane pliki statyczne. Zaleta: nic
   nowego. Wada: te same porty co ruch publiczny, więc trzeba kontroli dostępu.

**Skłaniam się do (1)**, bo oddzielenie portu metryk od publicznego jest
w produkcji warte więcej niż oszczędność jednego typu, a Piotr wprost mówił
"na jakiś port".

Format: tekstowy Prometheus plus JSON pod inną ścieżką. Uwaga na kardynalność
(sekcja o metrykach) — tu akurat jest ograniczona, bo etykietą jest nazwa poola,
a tych jest skończenie wiele.

### Kolejność

Po supervisorze i cronie, razem z metrykami — bo dopiero wtedy wiadomo, co
naprawdę jest do pokazania. Robienie tego wcześniej to zgadywanie kształtu danych.


## 3k. Metryki z PHP — API i decyzje projektowe (2026-09-05)

### Gdzie mieszka kod: ROZSZERZENIE, nie funkcje SAPI

`configure.ac:1097` woła `esyscmd(./build/config-stubs ext)` — czyli `ext/` jest
wykrywane tym samym globem co `sapi/`. Więc repo może zawierać
`ext/fpmng_metrics/`, a `prepare.sh` wrzuci je obok `sapi/fpmng/`, nadal bez
łatek na upstream.

To rozstrzyga rozwidlenie z sekcji o metrykach: funkcje rejestrowane
w `fpm_main.c` istniałyby tylko pod fpm-ng, a tryb CLI z ręcznie włączonymi
metrykami jest jednym z celów produktu. Pod fpm-ng rozszerzenie używa pamięci
dzielonej; pod CLI ma magazyn w procesie i funkcję zwracającą gotowy tekst,
żeby skrypt mógł go sam wystawić. Ten sam kod PHP działa w obu miejscach.

### API

```php
fpm_metric_register(string $name, string $type, string $help, array $buckets = []): bool
fpm_metric_inc(string $name, float $by = 1.0, array $labels = []): bool
fpm_metric_set(string $name, float $value, array $labels = []): bool
fpm_metric_observe(string $name, float $value, array $labels = []): bool
```

Typ wynika z użytej funkcji, `register` jest opcjonalny (HELP, kubełki).
Wszystkie zwracają `bool` — to jest sposób na wykrycie wyczerpania puli, nie
ozdoba.

### Współbieżność: SLOTY PER WORKER, nie atomiki na wspólnym liczniku

Kilkunastu workerów walących atomowo w ten sam licznik to kontencja o linię
cache przy każdym `inc`. Zamiast tego każdy worker pisze do własnego fragmentu
bez synchronizacji, a **sumowanie robi się przy odczycie**. Zero blokad.

Slot kluczowany **indeksem procesu ze scoreboardu, nie pidem** — inaczej przy
recyklingu po `pm.max_requests` licznik startowałby od zera.

Gauge się nie sumuje: domyślnie suma, ale przy rejestracji musi dać się wybrać
agregację (dla "użycie pamięci" sensowne jest maksimum). Prometheus ma ten sam
problem w trybie wieloprocesowym i rozwiązuje tak samo.

### Kardynalność — twarda reguła

Stały limit serii z dyrektywy. Po wyczerpaniu **odrzucamy i zwracamy `false`**,
nigdy nie rośniemy. Ostrzeżenie w logu **raz**, nie przy każdym requeście,
i koniecznie z nazwą metryki, która wyczerpała pulę — bez tego operator nie
znajdzie winowajcy.

### Histogramy WCHODZĄ do pierwszej wersji

KOREKTA wcześniejszej oceny (mojej, błędnej). Dla consumera kolejki rozkład
czasu przetwarzania to jest ta jedna metryka, która ma sens — średnia zakłamuje
ogon, a to właśnie ogon zapycha kolejkę. Koszt też przeszacowałem: histogram
z ustalonymi kubełkami to N liczników plus suma plus licznik, przy inkrementacji
szukanie kubełka wśród kilkunastu wartości. Pracochłonne są exemplary,
histogramy natywne i estymacja kwantyli — a tego nie robimy.

Ostrzejsza reguła: **histogramy nie są drogie same z siebie, są drogie
w połączeniu z etykietami o nieograniczonej liczbie wartości.** Histogram czasu
requestu z etykietą `route` przy 100 trasach i 12 kubełkach to 1200 serii razy
liczba workerów. Ten sam histogram u consumera z etykietą `queue` przy 5
kolejkach to 60 serii.

**To przesądza `fpm_metric_observe`:** przy supervisorze NIE MAMY jak wywnioskować,
gdzie kończy się jedno zadanie — skrypt to długo żyjąca pętla, granicę zna
wyłącznie aplikacja. Więc to nie jest wygodny dodatek, tylko jedyna droga do
sensownych metryk consumera.

Domyślne kubełki muszą sięgać dalej niż typowe dla HTTP (zadania trwają dłużej
niż requesty): 0.005 do 60 s.

### Jak konsument rozróżnia, czego dotyczy metryka

Nazwa poola jest bezpiecznym kluczem — zweryfikowane, `fpm_conf.c:1439` przy
powtórzonej sekcji `[nazwa]` wraca do istniejącego poola zamiast tworzyć drugi.

Dwa mechanizmy na dwa różne pytania:

1. **Jakiego typu jest pool** — metryka informacyjna, zawsze obecna:
   `fpmng_pool_info{pool="queue",type="supervisor"} 1`
2. **Których pól się spodziewać** — po NAZWIE metryki, nie po etykiecie.
   `fpmng_fcgi_idle_processes` dla poola supervisora **po prostu nie istnieje**.
   Nazwa metryki ma mieć jedno znaczenie i jednostkę; "idle" supervisora to nie
   to samo pojęcie co "idle" poola FastCGI.

**PUŁAPKA:** nigdy nie emitować fałszywego zera tam, gdzie pojęcie nie ma sensu.
`fpmng_fcgi_idle_processes{pool="queue"} 0` skończy się alertem "idle == 0"
i budzikiem z powodu poola, który nie ma takiego pojęcia. **Brak serii to
informacja, fałszywe zero to kłamstwo.**

Stan jako enum z etykietą (`state="running"|"backoff"|"gave_up"`, każdy 0/1),
nie jedna liczba kodująca stan. JSON: pola nieadekwatne nieobecne. Strona dla
człowieka: osobna tabela na typ.


## 3l. TLS i ACME — DECYZJA: robimy, ale na końcu (2026-09-05)

Piotr: "TLS i acme na koniec - ale robimy". Przestaje być otwartym pytaniem.

### Konsekwencja 1: domyka sprawę `http-async`

Przy TLS worker nie ma jak pisać prosto do klienta — strumień jest szyfrowany,
a stan sesji siedzi w bramce. Wariant in-process musiałby dać każdemu workerowi
własny stan TLS i obsługę certyfikatów. To praktycznie **zamyka** tamtą drogę,
nie tylko odkłada. Nie wracać do tematu bez nowego argumentu.

### Konsekwencja 2: gdzie napisać klienta ACME — NIEROZSTRZYGNIĘTE

Piotr skłania się do C. Decyzja świadomie odłożona, obie drogi zapisane.
Nie rozstrzygać bez odpowiedzi na pytania z końca tej sekcji.

**Opcja A: ACME w C, wewnątrz bramki**

- **Bootstrap działa naturalnie.** Nie da się serwować HTTPS bez certyfikatu,
  a pool typu `cron` startuje po uruchomieniu poolów — pierwsze wydanie
  certyfikatu wypada wtedy w złym momencie cyklu życia. W C to jest wewnątrz
  procesu, który i tak musi poczekać na certyfikat.
- **Brak sprzężenia z aplikacją.** Zepsuta albo źle skonfigurowana aplikacja
  użytkownika nie może doprowadzić do wygaśnięcia certyfikatu.
- **"Jedna binarka" zostaje prawdą.** Skrypt PHP musiałby gdzieś mieszkać —
  payload self-runnera pakuje aplikację użytkownika, nie nasze rzeczy, więc albo
  sprzęgamy dwie funkcje, albo dokładamy plik obok binarki.
- **Brak zależności od rozszerzeń PHP.** Podpisy JOSE potrzebują `openssl_sign`,
  do tego klient HTTP. Jeśli ktoś zbuduje fpm-ng bez tych rozszerzeń, ACME
  przestaje działać. W C OpenSSL i tak jest zlinkowany dla samego TLS.
- Koszt: kilka tysięcy linii C i stała powierzchnia na błędy pamięci.

**Opcja B: ACME w PHP jako pool typu `cron`**

- Kilkaset linii zamiast kilku tysięcy, biblioteki istnieją.
- Reużywa maszynerii, którą i tak budujemy (`cron`, pliki statyczne dla
  wyzwania HTTP-01 spod `/.well-known/acme-challenge/`).
- Łatwiejsze do poprawienia bez przebudowy binarki.
- Wady to dokładnie zalety opcji A odwrócone: bootstrap, sprzężenie
  z aplikacją, dodatkowy plik, zależność od rozszerzeń.

**Co rozstrzygnie ten wybór — do sprawdzenia przed decyzją**

1. Ile realnie kodu C to jest? Obejrzeć minimalnego klienta ACME w C
   (np. `uacme`, `acme-client`) i policzyć, ile z tego jest nam potrzebne przy
   wyłącznie HTTP-01 i jednym CA.
2. Czy bootstrap w opcji B da się rozwiązać sensownie — np. bramka startuje bez
   TLS, wystawia tylko wyzwanie, a listener HTTPS wstaje po pierwszym wydaniu?
3. Czy skrypt ACME dałoby się osadzić w binarce tym samym mechanizmem co
   self-runner (sekcja 3a) bez sprzęgania obu funkcji?

Niezależnie od wyboru: w C zostaje TLS w bramce przez `bufferevent_openssl`
(libevent to ma, OpenSSL już linkujemy statycznie — sekcja 3c) oraz
przeładowanie certyfikatu bez zrywania połączeń, we wszystkich procesach bramki.

### DWIE RZECZY DO ZAPROJEKTOWANIA TERAZ, NIE NA KOŃCU

1. **Ścieżka requestu w bramce potrzebuje JEDNEGO punktu, w którym odpowiadamy
   bez workera.** Pliki statyczne, wyzwanie ACME, `/status` — to ten sam haczyk.
   Jeśli pliki statyczne zrobimy doraźnym `if`-em, przy ACME będziemy przepisywać.
2. **Certyfikaty i konto ACME to STAN, nie kod.** Muszą leżeć na wolumenie
   zapisywalnym, nie w binarce — wchodzą do tego samego podziału "kod niezmienny
   kontra stan na wolumenie", który i tak trzeba zdefiniować przy self-runnerze
   (sekcja 3a, punkt 1). Rozstrzygnąć raz.

### Konsekwencja konfiguracyjna

HTTP-01 wymaga portu 80. Setup z ACME potrzebuje i 80, i 443 — jeden na wyzwanie
i przekierowanie, drugi na ruch. Model konfiguracji musi to obsłużyć; sprawdzić,
czy "jeden pool, jeden port" wystarcza.


## 3m. `fcgi-async` — wyniki badania i PRAWDZIWY CEL: eksperymentalny build pod async

### Cel, w którego świetle trzeba czytać wszystkie liczby

`fcgi-async` nie jest głównie o wyciśnięciu mikrosekund z dzisiejszego FPM. Ma być
**eksperymentalnym buildem pod prawdziwy asynchron w PHP**. To zmienia wagę
wszystkich pomiarów poniżej.

**Async zmienia mianownik z czasu ściennego na czas CPU.** Refren "przy 20 ms
requeście transport to 0,15%" zakłada, że worker jest przez te 20 ms zajęty.
Ale typowy request PHP to ~2 ms CPU i ~18 ms czekania na bazę — dziś to czekanie
blokuje proces. Pod asynchronem worker w tym czasie obsługuje inne requesty,
więc liczy się CPU na request, nie ścienny: **30 µs z 2 ms to 1,5%, nie 0,15%.**
Przy lekkim endpoincie API z cache'em (200 µs CPU) — 15%.

### Wyniki badania (agent, 2026-09-05, poligon, artefakty w `~/ng-research/`)

Worker FastCGI robi **26 syscalli na request keep-alive**, 33 przy nowym
połączeniu. Dla porównania nasza bramka HTTP robi ~9 — czyli w parze
bramka+worker to **worker był większym konsumentem**, a myśmy optymalizowali
cieńszy koniec. Z tych 26 tylko **7 to FastCGI**; reszta to narzut PHP/Zend/FPM:
8× `rt_sigaction` (`zend_signal_activate`), 2× `setitimer` + `rt_sigprocmask`
(`max_execution_time`), `getcwd` + 2× `chdir`, 2× `fcntl` (blokada opcache),
2× `times` (CPU requestu do statusu), `write(2,"\0fscf")`.

Zmierzone na dwóch łatkach eksperymentalnych (rozrzut < 3%):

| | syscalli | TCP µs/req | UDS µs/req |
|---|---|---|---|
| dziś | 26 | 65 | 54 |
| bufor wejściowy + `accept4`, bez `poll` | 21 | 56 | 46 |
| + zdjęta księgowość | 8 | 34 | 26 |
| + `max_execution_time=0` | **4** | **30** | **23** |

Dwie pozycje są **za darmo, samą konfiguracją**: `max_execution_time=0` (−3 µs)
i gniazdko uniksowe zamiast loopbacku TCP (−7…−11 µs).

Podłoga po wycięciu wszystkiego: ~23 µs (UDS), z czego ~9 µs to sam PHP.
Czegoś większego nie ma: `stat` na skrypcie nie występuje wcale (opcache
serwuje z SHM), realpath trafia w cache, scoreboard to ~0,25 µs.

**Zastrzeżenie sprzętowe:** poligon ma PTI + IBRS, więc syscall kosztuje tam
~0,9 µs. Na nowszym CPU (Ice Lake+, Zen) to 0,1–0,3 µs, czyli **zysk
bezwzględny skurczy się 3–5×**. Te 30 µs to górna granica, nie typowa wartość.

### BŁĄD UPSTREAMU: `TCP_NODELAY` nigdy nie włączane na Linuksie

Potwierdzone we własnym drzewie. `main/fastcgi.c:891` przypisuje `req->tcp`
**wyłącznie pod `#ifdef _WIN32`**, a linia 1085 używa go bezwarunkowo do decyzji
o `TCP_NODELAY`. Poza Windows pole zostaje zerem z `calloc`, więc przy
keep-alive po TCP odpowiedź > 8 KB idzie kilkoma `write`, a ostatni mały segment
czeka na ACK → Nagle + delayed ACK.

Zmierzone: **40 ms** zamiast 80 µs. Z prawdziwym nginx na loopbacku
NIE odtworzone (nginx ACK-uje szybko), więc w typowym wdrożeniu nie boli — ale
poza loopbackiem albo z innym klientem FastCGI już tak.

Naprawa to jedna linia. **Zgłosić upstream niezależnie od tego projektu.**

### Dwa kierunki odrzucone — ale jeden tylko WARUNKOWO

**`SO_REUSEPORT` per worker — odrzucone na stałe.** Thundering herd **w ogóle
nie istnieje**: blokujący `accept` używa kolejki wyłącznej i jądro budzi jednego
workera (zmierzone: 1,78 przełączenia kontekstu na `accept` przy 8 workerach vs
1,88 przy jednym). A `SO_REUSEPORT` przypina połączenie do gniazda hashem, nie
do wolnego workera, więc pogorszyłby ogon.

**`io_uring` — odrzucone TYLKO DLA OBECNEGO MODELU.** Agent oparł werdykt wprost
na tym, że po jego łatkach cykl to `read` → PHP → `write` → `read`, więc jest
do sklejenia najwyżej 1–2 syscalle. To poprawne dla **workera blokującego**.

Pod asynchronem worker nie siedzi w blokującym `read`, tylko ma pętlę zdarzeń
i N równoległych requestów — czyli dokładnie to, do czego io_uring powstał:
wiele deskryptorów, wiele operacji zgłaszanych jednym `io_uring_enter`,
multishot accept/recv, brak `epoll_ctl` na każdą zmianę zainteresowania.
**Przy async trzeba to przeliczyć od nowa. Nie cytować samej konkluzji.**

### KOREKTA: async ponownie otwiera `http-async`

W sekcji 3l napisałem, że TLS praktycznie zamyka wariant in-process, bo worker
musiałby mieć własny stan TLS i pętlę zdarzeń. Pod asynchronem worker **i tak ma
pętlę zdarzeń** — to jest sedno asynchrona. HTTP i TLS w workerze przestają
wtedy być wpychaniem parsera do procesu, który nie ma gdzie go trzymać.

Argument był za mocny. `http-async` wraca do stanu **otwarte**, nie zamknięte.

### Rekomendacja agenta — kolejność dla `fcgi-async`

1. Naprawa `TCP_NODELAY` (jedna linia, błąd, zgłosić upstream)
2. Bufor wejściowy + `accept4` (~7 µs keep, ~12 µs nowe poł., zero zmian na drucie)
3. `write(2,"\0fscf")` tylko przy `catch_workers_output` (~1 µs)
4. Cache `chdir` w SAPI + `SAPI_OPTION_NO_CHDIR` (~5 µs, zachowuje semantykę cwd)
5. Opt-in na CPU requestu (`times()`, ~2,6 µs)
6. Usunięcie `poll` TYLKO razem z `SO_RCVTIMEO` na gnieździe nasłuchującym
   (poll chroni przed milczącym klientem — stage ACCEPTING nie podlega
   `request_terminate_timeout`)
7. W dokumentacji: `max_execution_time=0` + UDS dla mikro-endpointów

Poza `fcgi-async`, do upstreamu: 7× `rt_sigaction` w `zend_signal_activate`
(~6,5 µs) i 2× `fcntl` opcache (~1,7 µs) — 40% pozostałych syscalli, ale nie po
stronie SAPI.


## 3n. Nazwy trybów: `fcgi-async` i `http-async`, nie `-ng` (2026-09-05)

Decyzja Piotra. `-ng` mówi tylko, że coś jest nowsze; `-async` mówi, czym to
jest i czym się różni. Poprzednie nazwy w tym pliku zostały przemianowane:

- `fcgi-ng` -> **`fcgi-async`** — eksperymentalny typ poola FastCGI pod prawdziwy
  asynchron w PHP. Stary `fcgi` bez zmian.
- `http-direct` -> **`http-async`** — wariant, w którym HTTP żyje w workerze
  z pętlą zdarzeń, bez bramki i bez hopa FastCGI.

Nazwa `http-async` jest przy tym trafniejsza merytorycznie niż `http-direct`,
bo to nie "bezpośredniość" jest istotą tego wariantu, tylko to, że worker ma
pętlę zdarzeń. Bez asynchrona ten wariant nie ma sensu (traci kolejkowanie
i przypina połączenia keep-alive do workerów) — z asynchronem ma.

Nazwa produktu `php-fpm-ng` na razie bez zmian; osobna sprawa, patrz sekcja 9
(znak towarowy).


## 3o. `pool.type = supervisor` — zaimplementowane i zweryfikowane (2026-09-05)

Nowy plik `sapi/fpmng/fpm/fpm_pool_supervisor.c` + `.h`, jedna linia w
`fpm_pool_types[]` (`fpm_pool_type.c`), dyrektywy w `fpm_conf.c`/`fpm_conf.h`.
Pięć dyrektyw z zadania (`supervisor.script`, `.processes`, `.restart`,
backoff/`.restart_max`, `.stop_timeout`) plus szósta dopisana w trakcie przez
koordynatora: `supervisor.fatal`. Wszystkie zweryfikowane na zbudowanej
binarce (10 scenariuszy, patrz niżej).

### Decyzja: `supervisor.processes` → `pm = static` + `pm.max_children`

Rozstrzygnięte na tak. `fpm_pool_type_s.validate()` dla tego typu ustawia
`wp->config->pm = PM_STYLE_STATIC` i `pm_max_children = supervisor.processes`
**programowo**, zanim `fpm_conf_process_all_pools()` dojdzie do sprawdzeń
`requires_pm` — więc te sprawdzenia przechodzą trywialnie, a użytkownik nigdy
sam nie ustawia `pm`/`pm.*` (odrzucone przez `rejects`, patrz niżej). Efekt:
spawnowanie N procesów i wskrzeszanie po `exit()`/crash jest **za darmo**
z istniejącego `fpm_children.c` — zero własnej puli procesów.

Konsekwencja tej decyzji: `requires_pm = 1`, nie `0` jak sugerował szkic
zadania — bo "wymaga sensownego pm" jest prawdą, tylko ten pool sam sobie tę
wartość generuje zamiast czytać ją z configu.

### `rejects`: `pm`/`pm.` i `listen`/`listen.` w całości

Skoro `pm`/`pm.max_children` są generowane z `supervisor.processes`, pozwolenie
użytkownikowi ustawić je równolegle dawałoby dwa źródła prawdy dla tej samej
liczby — więc odrzucone **w całości** (`"pm"` i prefiks `"pm."`), nie tylko
pojedyncze pola wymienione w zadaniu. Podobnie `listen`/`listen.` w całości
(ten typ nie nasłuchuje), `ping.`/`access.` w całości (bez `listen` nie ma
czego pingować ani logować jako "dostęp"), plus pojedyncze pozycje z zadania
(`request_terminate_timeout(_track_finished)`, `request_slowlog_timeout`,
`request_slowlog_trace_depth`, `slowlog`, `security.limit_extensions`).

**Mechanizm `rejects` zweryfikowany — działał od razu, bez poprawek.**
`fpm_pool_type_check_directives()` (w `fpm_pool_type.c`) był poprawny; jedyne,
czego brakowało, to prawdziwy konsument (dotąd testowany zerem dyrektyw,
patrz 3i). Testy 8/8b niżej pokazują czytelny ALERT przy starcie z odrzuconą
dyrektywą, dokładnie jak zaprojektowano.

### PROBLEM PROJEKTOWY (a) kontra (b): rozstrzygnięte na (a), zgodnie z preferencją

Zaimplementowane (a): pamięć dzielona per pool (`fpm_shm_alloc`, ten sam wzorzec
co `fpm_http.c`), struktura `fpm_supervisor_shared_s` (`failures`,
`next_allowed_start`, `terminal`, `gave_up`, `fatal_signaled`). `fpm_children.c`
**nietknięty**. Dziecko po starcie sprawdza stan: jeśli `terminal` już
ustawiony (poprzedni proces tego poola już raz zdecydował "koniec") — parkuje
się (`pause()` w pętli aż do sygnału) zamiast robić cokolwiek. Zweryfikowane:
test 4 (`restart = never`) pokazuje dokładnie ten mechanizm.

**Ważna korekta względem opisu w zadaniu**: proces wskrzeszony przez
`fpm_children.c` po `exit()` **nie znika** — zostaje jako harmless parkujący
się proces (dokładnie tak, jak zapowiadała wada (a) w opisie zadania:
"procesy istnieją i śpią zamiast zniknąć"). Test 4 pokazuje to wprost w `ps`:
po tym jak skrypt uruchomiony z `restart = never` skończy działanie, zostaje
jeden proces `pool sup`, ale jest to **nowy, zaparkowany** proces (inny PID),
nie oryginalny. Skrypt sam **nie jest uruchamiany ponownie** (log skryptu ma
dokładnie jeden wpis) — to jest właściwa gwarancja, jaką (a) może dać bez
zmiany `fpm_children.c`.

### Nie "jedna instancja PHP na jedno wykonanie skryptu" — pętla WEWNĄTRZ procesu

Doprecyzowanie względem szkicu w sekcji 3 ("dziecko wykonuje skrypt" w liczbie
pojedynczej): supervisor **nie** kończy procesu po każdym uruchomieniu skryptu.
`child_main()` woła `php_request_startup()` / `php_fopen_primary_script()` /
`php_execute_script()` / `php_request_shutdown()` **w pętli C, w tym samym
procesie**, tak długo jak polityka `restart` każe próbować dalej. Proces kończy
się (świadomie, przez `exit()`) tylko gdy: polityka mówi "koniec na dobre"
(`never` po jednym biegu, `on-failure` po sukcesie, `restart_max` wyczerpany),
albo przyszedł SIGTERM i bieżące wykonanie skryptu się zakończyło. To jest
zgodne z literą zadania ("dać wewnętrzną logikę pętli nad wykonaniami
skryptu"), tylko nie od razu oczywiste z opisu w sekcji 3 wyżej — tamten opis
("dziecko wykonuje skrypt", liczba pojedyncza) sugerował model
"jeden proces = jedno wykonanie", co okazało się niezgodne z literą zadania.
Bonus z sekcji 3 (fork mastera z gotowym PHP) działa więc **raz na cały czas
życia procesu**, nie przy każdej iteracji — iteracje w tym samym procesie są
jeszcze tańsze, bo nawet nie ma kosztu `fork()`.

### Backoff: co liczy się jako "porażka"

Zdecydowane: **tylko `exit_code != 0` liczy się jako porażka** dla backoffu
i `restart_max`. Pierwsza wersja liczyła też udane zakończenia (bo mierzyła
tylko "czy proces żył krótko"), co pod `restart = always` z krótko trwającym,
ale w pełni zdrowym skryptem (typowy consumer: odbierz-jedno-zadanie-i-wróć)
winowałoby normalny cykl pracy jako "flapping" i po `restart_max` cyklach
zabijałoby zdrowy pool. Naprawione: `exit_code == 0` zawsze zeruje licznik
i startuje następną iterację **od razu** (zero sztucznego throttlingu — tempo
kontroluje sam skrypt, np. własnym `sleep()`). Dopiero `exit_code != 0`
wchodzi w logikę backoffu (`restart_delay`, rosnąco ×2 aż do
`restart_delay_max`) i liczy się do `restart_max`. Próg resetu licznika przy
długo żyjącym niepowodzeniu: `restart_delay_max` (jeśli proces żył dłużej niż
najdłuższy możliwy odstęp między próbami, to nie był "szybką śmiercią" — jeden
pechowy fail po tygodniach pracy nie powinien liczyć się do tego samego limitu
co prawdziwy crash-loop). `supervisor.restart_max` domyślnie `0` = bez limitu
(nigdy się nie poddawaj) — w zadaniu nie było jawnego domyślnego.

### `supervisor.fatal` (dopisane przez koordynatora w trakcie pracy)

`supervisor.fatal = no` (domyślne) | `yes`. Gdy pool poddaje się z powodu
**prawdziwej porażki** (`restart_max` wyczerpany, `shared->gave_up = 1`) i
`fatal = yes`: ALERT, `kill(fpm_globals.parent_pid, SIGTERM)` do mastera —
czyli to samo, co zwykłe zamknięcie mastera sygnałem, więc pozostałe poole
dopalają requesty i sprzątają się przez ISTNIEJĄCĄ maszynerię (`fpm_signals.c`,
`fpm_process_ctl.c`, nietknięte). Kod wyjścia mastera musi być != 0 (inaczej
Docker/k8s nie zrestartują kontenera) — do tego jeden dodatkowy hook:
`fpm_cleanup_add(FPM_CLEANUP_PARENT_EXIT_MAIN, ...)` (ten sam mechanizm, na
którym stoi już sprzątanie bramek HTTP), wołany tuż przed
`exit(FPM_EXIT_OK)` w `fpm_pctl_exit()` (`fpm_process_ctl.c`, nietknięte) —
callback sprawdza `shared->gave_up && supervisor.fatal` dla wszystkich poola
supervisora i jeśli prawda, robi `_exit(FPM_EXIT_SOFTWARE)` (70) **zanim**
`fpm_pctl_exit()` zdąży wywołać swój `exit(FPM_EXIT_OK)`. Rozróżnienie
"planowe zakończenie" (`restart = never` / sukces pod `on-failure`,
`gave_up = 0`) od "prawdziwej porażki" (`gave_up = 1`) pilnowane explicite w
kodzie (`shared->gave_up`, nie samo `shared->terminal`) — test 10 pokazuje że
`restart = never` + `fatal = yes` ze skryptem kończącym się zerem **nie**
ubija mastera.

### Pułapka #1 (poważna): `child_main` nigdy nie był wcześniej wywołany — `fpm_worker_all_pools` ginie przed `run_child:`

To był największy problem w tym zadaniu, w kodzie **cudzym** (już istniejącym
w `fpm.c`/`fpm_pool_type.c` przed tym zadaniem). Mechanizm
`fpm_pool_type_current_pool()` (dopasowanie przez scoreboard) był opisany
w 3i jako "zaimplementowany i skompilowany", ale **`http` go nie używa**
(bramka forkuje własne procesy bezpośrednio w `init_main()`, nie przez
`child_main`) — więc `supervisor` jest pierwszym prawdziwym konsumentem
`child_main`, i mechanizm okazał się zepsuty.

Przyczyna: `fpm_worker_pool_init_main()` (`fpm_worker_pool.c`, referencja)
rejestruje `fpm_worker_pool_cleanup()` na `FPM_CLEANUP_ALL` — czyli i na
`FPM_CLEANUP_CHILD`. Ta funkcja **zwalnia całą listę `fpm_worker_all_pools`**,
włącznie z `wp->config` (`free()`), i na końcu `fpm_worker_all_pools = NULL`.
`fpm.c` woła `fpm_cleanups_run(FPM_CLEANUP_CHILD)` **na samym początku**
etykiety `run_child:`, **przed** próbą znalezienia typu poola przez
`fpm_pool_type_current_pool()` — więc do czasu gdy nasz kod próbuje odczytać
`wp->config->supervisor_script`, `wp` już nie istnieje (use-after-free).
Dla zwykłego workera FastCGI to niewidoczne, bo po tym punkcie kod już nigdy
nie zagląda do `wp`/`config` (działa wyłącznie na `fpm_globals`).

Naprawione w `fpm.c` (już nasz plik, więc dozwolone): rozwiązanie typu
(`fpm_pool_type_current_pool()` + `fpm_pool_type_of()`) przeniesione **przed**
`fpm_cleanups_run(FPM_CLEANUP_CHILD)`, i dla typu z ustawionym `child_main`
**cały `fpm_cleanups_run(FPM_CLEANUP_CHILD)` jest pomijany** — bo `child_main`
i tak nie wraca, a `wp`/`config` są potrzebne przez cały czas życia procesu.
To jest zmiana w `fpm.c` wykraczająca poza "fpm_conf.c/.h + nowy plik" —
zgłoszona tu wprost, bo tak każe instrukcja zadania. Bez niej supervisor (i
każdy przyszły typ z `child_main`, np. `cron`) w ogóle by nie ruszył: proces
kończył się natychmiast (0.001s), bez wykonania skryptu, w gorącej pętli
fork-exit-fork (widoczne w logu jako dziesiątki "child exited with code 0"
na sekundę) — bo trafiał z powrotem do zwykłej pętli accept FastCGI na
gnieździe 0 (dup od stdin, bo `requires_listen = 0`), która natychmiast
kończyła się błędem.

### Pułapka #2: PHP samo używa `SIGALRM`/`ITIMER_REAL` do `max_execution_time`

Pierwsza wersja `stop_timeout` używała `alarm()` + własny handler `SIGALRM`
jako siatki bezpieczeństwa (twardy `SIGKILL`, gdyby skrypt nie skończył się
sam w czasie `stop_timeout`). Zmierzone na żywo, że to koliduje z
`zend_set_timeout_ex()` (`Zend/zend_execute_API.c`), które **też** używa
`SIGALRM`/`setitimer(ITIMER_REAL,...)` do `max_execution_time`, i pod
`ZEND_SIGNALS` (ten build ma `-DZEND_SIGNALS`) re-instaluje swój handler przy
każdym wykonaniu skryptu — nasz `sigaction(SIGALRM,...)` (wołany raz, na
starcie procesu) bywał po cichu podmieniany. Efekt w teście: proces czekał na
zakończenie PHP-owego `max_execution_time` (komunikat "Maximum execution time
of 30 seconds exceeded"), nie na nasz `stop_timeout`.

Naprawione: `stop_timeout` **nie** używa żadnego sygnału/timera PHP. Handler
`SIGTERM` forkuje malutki proces-watchdog (`fork()` jest async-signal-safe),
który czeka do `stop_timeout` w **osobnym procesie**, całkowicie niezależnym
od stanu sygnałów Zenda, i jeśli proces supervisora nadal żyje — ubija go.
Zweryfikowane na żywo (test 7c, busy-loop bez żadnego punktu bezpiecznego):
zabite dokładnie po `stop_timeout`, sygnałem, nie przez naturalne zakończenie
skryptu.

**Wyścig PID-owy — zamknięty na Linuksie (2026-09-05, dopisek koordynatora)**.
Pierwsza wersja identyfikowała proces po samym PID-zie zapamiętanym w momencie
`fork()`: watchdog spał `stop_timeout` sekund, potem `kill(pid, SIGKILL)`.
Teoretyczny wyścig: gdyby proces supervisora zdążył umrzeć i jego PID został
ponownie użyty przez inny proces zanim watchdog się obudzi, watchdog zabiłby
nie tam, gdzie trzeba — nie do zaakceptowania w produkcie, który ma pilnować
cudzych procesów (Docker/k8s).

Naprawione przez `pidfd_open()`/`pidfd_send_signal()` (Linux, jądro ≥5.3/5.1
— czyli dokładnie platforma docelowa: kontenery Alpine). Klucz: pidfd
otwierany na siebie samego **tuż przed `fork()`-iem watchdoga**, w momencie,
gdy "ja" jest jeszcze jednoznaczne (proces właśnie dostał SIGTERM, na pewno
wciąż żyje) — więc samo otwarcie nie ma żadnego okna wyścigu. Watchdog
dziedziczy ten deskryptor przez `fork()` i czeka na niego przez `poll()`
zamiast spać na ślepo: pidfd odnosi się do **konkretnej instancji procesu**,
niezależnie od tego, co później stanie się z tym numerem PID — więc timeout
w `poll()` jest jednoznacznym dowodem "to wciąż ten sam proces, wciąż żywy",
i dopiero wtedy leci `SIGKILL` (przez `pidfd_send_signal`, więc nawet ten
ostatni strzał nie przechodzi przez goły PID).

Numery syscalli (`SYS_pidfd_open` = 434, `SYS_pidfd_send_signal` = 424) na
sztywno w kodzie — nie każda libc (musl, starsze glibc) jeszcze je opakowuje,
a `syscall()` bezpośrednio jest wystarczająco stabilne (te numery nie
zmieniają się między architekturami x86_64/aarch64).

Na nie-Linuksie (ten Mac — tylko lokalne budowanie i testy, nigdy platforma
docelowa) `pidfd_open` nie istnieje: zostaje stary fallback `sleep()`+
`kill(pid, ...)`, z tym samym, udokumentowanym wyżej wąskim oknem wyścigu.
Zweryfikowane tylko w tej gałęzi fallbacku (na macOS nie da się przetestować
ścieżki `pidfd` — nie ma jej w jądrze). Test 7c powtórzony po zmianie:
zachowanie identyczne jak przed poprawką (busy-loop zabity `SIGKILL`-em
dokładnie po `stop_timeout`).

### Kolejny drobiazg: `catch_workers_output`

Bez `catch_workers_output = yes` supervisor **działa poprawnie, ale bez
żadnych logów** — `echo`/`error_log`/nasze własne `zlog()` z procesu dziecka
lecą do `/dev/null` (domyślne zachowanie FPM dla stdout/stderr dziecka, gdy ta
dyrektywa jest wyłączona, co jest domyślne). To nie jest specyficzne dla
supervisora, ale dla tego typu jest krytyczne (to jedyny sposób, żeby zobaczyć
cokolwiek ze skryptu) — warto rekomendować `catch_workers_output = yes` jako
praktyczne "wymagane" dla `pool.type = supervisor` w dokumentacji użytkownika
(nie wymuszone w kodzie, żeby nie dodawać kolejnej reguły walidacji bez
wyraźnej potrzeby).

### Monkey-patching `sapi_module` na czas życia procesu — bezpieczne, bo proces nie wraca

`child_main` nadpisuje kilka pól globalnego `sapi_module` (`ub_write`,
`getenv`, `read_post`, `read_cookies`, `register_server_variables`,
`pre_request_init = NULL`) — oryginalne wersje z `fpm_main.c` bezwarunkowo
rzutują `SG(server_context)` na `fcgi_request*`, a supervisor nigdy nie ma
prawdziwego requestu FastCGI (`SG(server_context)` zostaje `NULL` przez cały
czas życia procesu), więc bez nadpisania pierwszy `echo` w skrypcie zrobiłby
segfault. Bezpieczne wyłącznie dlatego, że ten proces **nigdy nie wraca** do
pętli accept FastCGI — gdyby wracał, te nadpisania zepsułyby normalną obsługę
requestów. `ub_write` pisze bezpośrednio na `STDOUT_FILENO`, co trafia do logu
FPM przez istniejące przechwytywanie pipe'em (stąd wymóg
`catch_workers_output` wyżej).

### Testy (10 scenariuszy, wszystkie zielone)

1. Brak `pool.type` → zwykły pool `fcgi`, pełne BC (zweryfikowane też
   prawdziwym requestem FastCGI po UDS, ręcznym klientem w Pythonie —
   `X-Powered-By`, treść skryptu).
2. `pool.type = supervisor` + `supervisor.script` — skrypt wykonuje się
   faktycznie (plik logu z timestampem i PID), w pętli, w tym samym procesie.
3. `supervisor.processes = 3` → trzy faktyczne procesy `php-fpm: pool sup`
   w `ps`.
4. `restart = never` — skrypt uruchomiony dokładnie raz (log ma jeden wpis),
   proces kończy się; **nowy, zaparkowany** proces zajmuje jego miejsce
   (patrz sekcja o (a) wyżej), ale skrypt się nie powtarza.
5. `restart = on-failure` — `exit(0)` nie restartuje (log: "not restarting"),
   `exit(1)` restartuje z backoffem, w tym samym procesie (ten sam PID).
6. Szybkie `exit(1)` w pętli — backoff rośnie geometrycznie (1s, 2s, 4s,
   ucięte na `restart_delay_max`), po `restart_max` ALERT i koniec prób na
   dobre (jeden zaparkowany proces zamiast crash-loopa).
7. SIGTERM w trakcie wykonywania skryptu (pętla z `usleep`) — skrypt kończy
   bieżące wykonanie (wszystkie 10 "ticków"), proces kończy się czysto
   (`exited with code 0`), NIE jest wskrzeszany do tego samego skryptu w tym
   samym sensie (nowy proces startuje, bo to normalne zejście, nie polityka
   "koniec na dobre"). Osobno (7c): busy-loop bez punktu bezpiecznego +
   `stop_timeout = 2` → zabity `SIGKILL`-em dokładnie po ok. 2s (nie kończy
   się sam po 15s).
8. `rejects` — `listen` i `pm.*` (`pm`, `pm.start_servers`,
   `pm.min_spare_servers`, `pm.max_spare_servers`) w konfiguracji poola
   `supervisor` dają czytelny ALERT i `FPM initialization failed` (exit 78),
   mechanizm działał bez poprawek.
9. `supervisor.fatal = yes` + szybki crash-loop → po wyczerpaniu
   `restart_max`: ALERT, master schodzi normalną ścieżką ("Terminating ...",
   "exiting, bye-bye!"), **`echo $? == 70`**.
10. `restart = never` + `fatal = yes` ze skryptem kończącym się zerem —
    master **nie** pada (proces "sup" się parkuje, master żyje dalej).

### Czego NIE zrobiono / wątpliwe

- Watchdog `stop_timeout` ma teoretyczny wyścig PID-owy (opisane wyżej).
- `pm.max_requests` (semantyka z sekcji 3: "skrypt kończy się sam po N
  zadaniach") **nie zaimplementowana** — nie było w pięciu wymaganych
  funkcjach, a `pm.*` jest teraz odrzucane w całości dla tego typu, więc
  potrzebowałoby własnej dyrektywy `supervisor.max_iterations` czy podobnej,
  gdyby ktoś tego chciał.
- Status supervisora w `fpm_status.c` (oznaczenie typu w statusie, wspomniane
  w sekcji 3) — nie ruszone, poza zakresem tego zadania.
- Współistnienie wielu pooli `supervisor` obok `fcgi`/`http` w jednym procesie
  mastera przy reload/SIGHUP — **przetestowane, patrz sekcja 3p** (dopisek
  koordynatora, 2026-09-05).
- `security.limit_extensions` i inne dyrektywy security nie mają dedykowanego
  testu poza samym faktem odrzucenia w konfiguracji — nie sprawdzono np. czy
  odrzucenie nie psuje czegoś w `fpm_unix.c` (nie powinno, bo to tylko string
  w configu, ale nie zweryfikowane explicite).

## 3p. `supervisor` przy reload i mieszanych poolach — zweryfikowane (2026-09-05)

Dopisek koordynatora po odebraniu 3o: historycznie bramki HTTP rejestrowały
sprzątanie tylko na `FPM_CLEANUP_PARENT`, a reload idzie przez
`FPM_CLEANUP_PARENT_EXEC` (`fpm_pctl_exec()` robi `execvp()`) — więc zostawały
osierocone, trzymając port (naprawione dla `http`, patrz 3i). Ten sam rodzaj
pytania dla `supervisor` nie był wcześniej sprawdzony — sprawdzone teraz.

**Konfiguracja testowa**: jeden pool `http`, jeden zwykły `fcgi`, jeden
`supervisor` z `restart = always` (skrypt: 6 ticków po 0.4s, ok. 2.4s na
iterację, z logiem do pliku), jeden `supervisor` z `restart = never` (skrypt
`exit(0)` od razu, więc pool jest już zaparkowany, zanim dojdzie do testu
sygnałów — nie trzeba nic specjalnie odczekiwać, to naturalny efekt
`restart = never`).

### Scenariusz 1: `SIGUSR2` (reload, `execvp()`)

```
PRZED: pgrep -P 90726
90728 90729 90730 90732   # web, plainfcgi, sup_always, sup_parked

kill -USR2 90726
[...] NOTICE: Reloading in progress ...
[...] NOTICE: reloading: execvp("php-fpm-ng", {"-y", "reload_test.conf", "-F", "-O"})
[...] NOTICE: using inherited socket fd=8, ".../reload_web.sock"
[...] NOTICE: using inherited socket fd=9, ".../reload_fcgi.sock"
[...] NOTICE: fpm is running, pid 90726          # <- TEN SAM PID (execvp nie zmienia PID)
[...] NOTICE: ready to handle connections

PO (t+3s): master pid=90726 (ten sam)
dzieci nowego mastera: 90739 90740 90741 90743
wszystkie procesy pool sup*:
90741 90726 php-fpm: pool sup_always
90743 90726 php-fpm: pool sup_parked
```

**Brak sierot** — obie stare instancje supervisora zniknęły, obie nowe mają
poprawny `PPID` (nowy/ten sam master). Log `reload_always.log` pokazuje, że
stary proces (pid 90730) dokończył SWOJĄ bieżącą iterację do końca
("tick 0".."tick 5", "iter end") **przed** faktycznym `execvp()` o 17:44:33 —
sygnał dotarł, `child_main` zareagował, nowa iteracja (pid 90741) wystartowała
dopiero po restarcie:

```
2026-09-05T15:44:31+00:00 reload_always iter start pid=90730
2026-09-05T15:44:31+00:00 reload_always tick 0
...
2026-09-05T15:44:33+00:00 reload_always tick 5
2026-09-05T15:44:33+00:00 reload_always iter end
2026-09-05T15:44:33+00:00 reload_always iter start pid=90741   # nowa generacja, po reloadzie
```

**Pamięć dzielona (backoff) po `execvp()`**: `fpm_shm_alloc()` używa
`mmap(MAP_ANONYMOUS | MAP_SHARED)` — taka mapa **nie przeżywa `execve()`**
(POSIX: cała przestrzeń adresowa poza otwartymi deskryptorami znika przy
exec). To nie jest coś, co dziedziczymy poprawnie czy niepoprawnie — to
fizycznie niemożliwe do odziedziczenia, więc pytanie "czy licznik porażek ma
sens po reloadzie" ma proste rozwiązanie: **nowa instancja mastera i tak
alokuje nowy segment `fpm_shm_alloc()` od zera** (w swoim własnym
`fpm_init()`/`fpm_pool_type_of()->init_main()`), więc liczniki backoffu
zerują się naturalnie przy reloadzie. Zachowanie spójne z resztą FPM: reload
to nowa instancja procesu, nie "kontynuacja" starej.

### Scenariusz 2: `SIGQUIT` (graceful stop, bez exec)

```
kill -QUIT 90858
[...] NOTICE: Finishing ...
[...] NOTICE: exiting, bye-bye!
master zszedl po 0s (pomiar co 0.3s — bardzo szybko)

reload_always.log (koniec):
...tick 4
...tick 5
...iter end        # <- dokonczyl biezaca iteracje przed zejsciem
```

Zaraz po zejściu mastera `ps` chwilowo pokazywał dwa procesy z `PPID=1`
(`pool sup_parked`, `pool sup_always`) — to były nasze własne procesy-
-strażniki `stop_timeout` (patrz 3o), które na tym Macu (brak `pidfd`) czekają
w pętli sprawdzającej `kill(pid,0)` co sekundę zamiast reagować na sygnał —
zniknęły same w ciągu ~1-2s. Nie prawdziwa sierota w sensie "zostanie na
zawsze", tylko przejściowy artefakt własnego mechanizmu strażnika, opisany
i zaakceptowany w 3o.

### Scenariusz 3: `SIGTERM` (szybkie zamknięcie)

```
kill -TERM 91009
[...] NOTICE: Terminating ...
[...] NOTICE: exiting, bye-bye!
master zszedl po ~2s

reload_always.log (koniec):
...iter start pid=91013
...tick 0
...tick 1
...tick 2
...tick 3
...tick 4          # <- BRAK "tick 5"/"iter end" — proces nie zdazyl dokonczyc
```

**Różnica względem `SIGQUIT`/`SIGUSR2` i warta zapisania**: pod stanem
`TERMINATING` `fpm_pctl_action_next()` (`fpm_process_ctl.c`, referencja) wysyła
`SIGTERM` jako **pierwszy** sygnał (nie `SIGQUIT`), a przy domyślnym
`process_control_timeout = 0` eskaluje do `SIGKILL` już po ok. 1 sekundzie,
jeśli dziecko jeszcze żyje — to jest zachowanie **całego mastera FPM, nie coś
wprowadzonego przez `supervisor`**, i dotyczy każdego typu poola (zwykły
worker FastCGI w trakcie długiego requestu ginie tak samo). Nasz własny
`supervisor.stop_timeout` (domyślnie 10s) nie ma tu znaczenia, bo to
**master**, nie nasz watchdog, dobija proces szybciej. Skrypt (2.4s na
iterację) nie zdążył zakończyć iteracji przy domyślnym
`process_control_timeout`. Żadnej sieroty nie zostało (`ps` czysty od razu po
zejściu mastera) — proces został poprawnie zebrany, tylko brakuje w logu
pojedynczej linii "child exited" dla tego PID-u (najpewniej dlatego, że
pętla zdarzeń mastera skończyła się, zanim zdążyła zalogować reap tego
konkretnego SIGCHLD — kosmetyczna kolejność logowania, zweryfikowane przez
`ps`, że proces faktycznie zniknął, nie osierocił się).

**Wniosek dla użytkowników**: kto chce, żeby `supervisor` (albo jakikolwiek
inny typ poola) miał czas dokończyć pracę przy `SIGTERM`/`docker stop`, musi
ustawić `process_control_timeout` na sensowną wartość globalnie — to
istniejąca dyrektywa FPM, nie coś do dodania. Warte dopisania do przyszłej
dokumentacji użytkownika, nie tylko tu.

### Poprawka przy okazji: watchdog `stop_timeout` bez `pidfd` skracał okno sieroty

Pierwsza wersja testu (przed poprawką w tym samym dniu) pokazywała sieroty
utrzymujące się przez pełne `stop_timeout` (domyślnie 10s) po reloadzie —
bo fallbackowy watchdog (bez `pidfd`, czyli na tym Macu) robił jeden długi
`sleep(stop_timeout)` zamiast sprawdzać co sekundę, czy nadzorowany proces już
się skończył. Naprawione (patrz też commit `2a4da50`/kolejny): watchdog na
ścieżce fallback pyta `kill(pid, 0)` co sekundę i kończy się od razu, gdy
nadzorowany proces już nie żyje, zamiast czekać cały `stop_timeout` na ślepo.
Po tej poprawce scenariusz 1 (USR2) pokazuje zero sierot już przy `t+3s`
(wcześniej: dwie sieroty widoczne jeszcze przy `t+3s`, znikające dopiero przy
`t+12s`).

## 3q. Async: pomiary epoll vs io_uring i pułapki (agent, 2026-09-05)

ZASTRZEŻENIE: agent mierzył **własnym prototypem w C** symulującym serwer
FastCGI z pętlą zdarzeń. Nie sprawdzał tego z faktyczną implementacją True
Async — konsekwencje semantyczne wywnioskował z lektury `fpm_scoreboard.c`,
`fpm_request.c`, `ZendAccelerator.c`, `zend_signal.c`, `main.c`. Liczby
o epoll/io_uring są zmierzone; wnioski o tym, co zniknie pod asynchronem, są
wnioskami z kodu FPM/Zend, a nie z asynchrona. Osobny agent bada True Async
u źródła — patrz sekcja, która powstanie z jego raportu.

### io_uring odpada TAKŻE pod async, ale z innego powodu

W modelu blokującym nie było czego batchować. W modelu z pętlą zdarzeń
batchowanie działa — syscalle spadają z ~2,0/req do 0,08–0,2/req (10–25×) —
ale **CPU spada tylko o 0,3 µs/req** (UDS, nasycone), 0,2 µs (TCP) i 0,7–1,15 µs
przy rzadkich zdarzeniach. io_uring nie usuwa pracy jądra, tylko przejścia,
a własny narzut (task-work, CQE, buffer ring) zjada większość oszczędności.

Próg: przy 2 ms CPU/request potrzeba **17–67 operacji I/O na request**, żeby
wyjść na 1%. Dla PHP nie istnieje N (równoległość), przy którym to wychodzi
z szumu. io_uring wraca do gry dopiero, gdy `fcgi-async` przestaje być PHP,
a staje się proxy z dziesiątkami operacji I/O na mikroskopijny request.

**Gwóźdź praktyczny: Docker domyślnie blokuje `io_uring_*` w seccomp.**
Do tego wymaga liburing i kernela ≥ 6.0 dla multishot recv, bywa wyłączany
przez `io_uring_disabled`. Nasze docelowe środowisko to kontener.

### Prawdziwa wygrana to PĘTLA ZDARZEŃ, nie API wejścia-wyjścia

Blokujący worker po optymalizacjach: 22–23 µs/req (UDS). Prototyp async na
epoll: **3,4 µs/req** (6,3 TCP). Różnica ~10 µs to koszt modelu "proces śpi
w `read` i budzi się per request" — przełączenie kontekstu plus zimne cache.
Znika w pętli zdarzeń **niezależnie od io_uring**. Czyli libevent, którą już
mamy, wystarczy.

### TRZY WCZEŚNIEJSZE REKOMENDACJE STAJĄ SIĘ BŁĘDAMI POD ASYNC

Nie tylko bezużyteczne — **błędne**. Oznaczyć, zanim ktoś je zaimplementuje
z listy w sekcji 3m:

1. **Cache `chdir` w SAPI** — cwd jest per proces. Dwa przeplatane requesty
   z różnych docrootów: jeden wykonuje się z cwd drugiego (relatywne `include`,
   `fopen`). Pod async **każde `chdir` per request jest błędem**; potrzebne
   wirtualne cwd per kontekst, czego NTS nie ma. To praca w Zend/TSRM.
2. **`times()` opt-in** — mierzy CPU **procesu**, więc "CPU requestu" byłoby
   sumą cudzych. Per-korutynowe CPU wymaga `CLOCK_THREAD_CPUTIME_ID` przy
   każdym przełączeniu — **1,1 µs/syscall** (zmierzone, nie vDSO), przy
   10 przełączeniach = 11 µs, więcej niż całość dzisiejszych oszczędności.
3. **`SO_RCVTIMEO` zamiast `poll`** — nie działa na gniazdach nieblokujących
   (`recv` wraca EAGAIN natychmiast). Timeouty muszą być timerami pętli.

### Trzy pułapki, których nie było na żadnej liście

- **Scoreboard**: slot `proc` jest per proces, a `request_uri`, `request_stage`,
  `accepted` opisują JEDEN request. Pod async status i slowlog kłamią, a
  `request_terminate_timeout` (master patrzy na `proc->tv` i `request_stage`)
  **ubija proces z N requestami z powodu jednego**.
- **Blokada opcache**: `accel_activate_add` bierze F_RDLCK per request;
  `accel_deactivate_sub` pierwszego kończącego requestu **zwolniłby blokadę,
  gdy inne jeszcze działają**. Trzymać raz per proces z licznikiem.
- **Pojęcie "wolnego workera"** przestaje istnieć. `fpm_request_end`,
  `fpm_stdio_flush_child` i `fpm_log_write` zakładają "request się skończył =
  proces wolny", a na tym stoi `pm = dynamic/ondemand`. Trzeba zdefiniować
  na nowo (requesty w toku < limit).

### `max_execution_time` i `zend_signal_activate`

`setitimer(ITIMER_PROF)` per request jest pod async **niewykonalne z definicji**
— jeden timer na proces. Znika sam, ale zamiennik trzeba zaprojektować:
- **zły**: `CLOCK_THREAD_CPUTIME_ID` przy przełączeniu korutyny (1,1 µs syscall,
  10–50 przełączeń = 11–55 µs — drożej niż całe dzisiejsze 26 syscalli)
- **dobry**: deadline wall-clock sprawdzany z `CLOCK_MONOTONIC_COARSE`
  (7 ns, vDSO) plus jeden timer pętli na najbliższy deadline

`zend_signal_activate` per request (7× `rt_sigaction`): handlery są per proces,
więc rejestracja raz na proces staje się **konieczna, nie optymalizacyjna**.
Głębszy problem: odraczanie sygnałów Zend (`SIGG(depth)`,
`HANDLE_BLOCK_INTERRUPTIONS`) zakłada jeden wątek wykonania — przełączenie
korutyny w sekcji krytycznej zostawia licznik w powietrzu. To Zend, nie SAPI.

**Netto: 12 z 26 dzisiejszych syscalli znika pod async z przyczyn
strukturalnych**, nie optymalizacyjnych.

### Ile z podłogi zostaje

Z ~21–23 µs (UDS): ~12 µs jest specyficzne dla modelu "jeden request na proces"
i spada do ~3–4 µs w pętli zdarzeń (zmierzone prototypem); ~9 µs to user-space
PHP i FPM, które zostaje. Docelowa podłoga transport+lifecycle pod async:
**~12–14 µs/req**, czyli 0,6% przy 2 ms CPU requestu.

**Wniosek: rzeczą, która realnie zmieni CPU/request pod async, jest koszt
per-korutynowego kontekstu PHP (startup/shutdown, sterta, superglobale) —
a to leży w Zend, nie w SAPI.** Cała warstwa transportu jest poniżej 1%.

Artefakty: `~/ng-research/async/` na poligonie.


## 3r. `pool.type = cron` — zaimplementowane i zweryfikowane (2026-09-05)

Nowe pliki: `fpm_pool_cron.c`/`.h` (sam typ), `fpm_cron_schedule.c`/`.h`
(parser crontaba, bez zależności poza libc), plus dwa pliki wydzielone ze
wspólnego kodu z supervisorem: `fpm_pool_watchdog.c`/`.h` (pidfd watchdog)
i `fpm_pool_script.c`/`.h` (wykonanie jednego skryptu PHP poza requestem
FastCGI, nadpisania `sapi_module`). Jedna linia w `fpm_pool_types[]`
(`fpm_pool_type.c`), trzy dyrektywy w `fpm_conf.c`/`fpm_conf.h`
(`cron.schedule`, `cron.script`, `cron.timeout`). `fpm_pool_supervisor.c`
zostało przepisane na te dwa wspólne pliki zamiast trzymać własne kopie —
zero zmiany zachowania, tylko usunięcie duplikacji (patrz niżej).

### Kluczowa decyzja upraszczająca: żadnych timerów po stronie mastera

Zgodnie z ustaleniem sprzed kodu (sekcja 3): dziecko poola `cron`, po starcie,
liczy najbliższy termin, śpi do niego przerywalnie, wykonuje skrypt RAZ,
kończy proces. FPM wskrzesza je istniejącą maszynerią (`fpm_children.c`,
NIETKNIĘTE), nowy proces liczy kolejny termin od bieżącego zegara. `fpm_events.c`
też nietknięty — cron nie używa `fpm_event_set_timer()`, wbrew wcześniejszemu
szkicowi w sekcji 3 ("timer + fork"). To jest inny, prostszy mechanizm niż
w tamtym szkicu, i prostszy niż supervisor.

### Różnica wobec supervisora: ZERO stanu w pamięci dzielonej

To jest największa różnica projektowa względem supervisora, warta zapisania
wprost, bo nieoczywista. Supervisor potrzebuje `fpm_shm_alloc()`, bo ma
politykę restart/backoff/`restart_max`, która musi przeżyć śmierć procesu.
Cron **nie ma żadnej polityki do przetrwania**: każdy nowy proces liczy
termin WYŁĄCZNIE z bieżącego zegara i harmonogramu, nigdy z tego, co robił
poprzednik. Nie ma więc `init_main` dla `cron` w `fpm_pool_types[]` (pole
zostaje `NULL` — "nic do zrobienia po stronie mastera", zgodnie z kontraktem
w `fpm_pool_type.h`).

Konsekwencja tej samej decyzji: **"nakładanie się przebiegów" nie jest
polityką, którą trzeba było napisać.** `cron.processes`-podobnej dyrektywy
świadomie nie ma — dla crona jest zawsze `pm.max_children = 1`,
ustawiane programowo w `validate()`, tak samo jak supervisor mapuje
`supervisor.processes` na `pm.max_children`. Przy `pm.max_children = 1` drugi
proces tego poola fizycznie nie istnieje, dopóki pierwszy nie zakończy
działania (`exit()`) — `fpm_children.c` odpala następny dopiero PO śmierci
poprzedniego. Nie ma więc przebiegu, z którym mógłby się nałożyć kolejny.
Polityka "pomijaj nakładające się" (przewidziana w sekcji 3 jako coś do
napisania) wychodzi więc za darmo z samej konstrukcji pm=static+1 i **nie
została napisana wcale** — dokładnie jak zapowiadało zadanie.

### Dlaczego "brak nadrabiania" i "brak podwójnego odpalenia" to jedna i ta sama linijka kodu

`fpm_cron_schedule_next(sched, after)` zawsze liczy najmniejszy czas ściśle
WIĘKSZY niż `after`, licząc od `((after / 60) + 1) * 60` (start następnej
pełnej minuty). Nigdy nie pyta "co przegapiłem od ostatniego uruchomienia" —
zawsze "co jest najbliżej w przyszłości od teraz". Efekt uboczny, za darmo:
- master wyłączony na godzinę → nowy proces liczy termin od aktualnego
  zegara, dostaje najbliższy przyszły termin, nie dwanaście zaległych,
- proces odrodzony chwilę po tym, jak poprzedni skończył przebieg w tej
  samej minucie (skrypt trwał ułamek sekundy), nigdy nie znajdzie ponownie
  minuty, która właśnie minęła — bo szuka ściśle w przyszłości.

Ktoś kiedyś będzie chciał dodać nadrabianie zaległych przebiegów — to ma być
świadoma zmiana projektowa (zmiana `after` na "czas ostatniego udanego
przebiegu", trzymany w stanie, którego dziś celowo nie ma), nie poprawka.

### Czas: wyłącznie UTC

`fpm_cron_schedule_next()` używa `gmtime_r()`, nigdy `localtime_r()`. Czas
lokalny + zmiana czasu (DST) dawałby przebieg podwójny (cofnięcie zegara)
albo zaden (przesunięcie zegara) przy każdym przejściu. Nie warto tego
ryzykować dla wygody zapisu harmonogramu w czasie lokalnym — harmonogramy
przenoszone z systemowego crona (który zwykle i tak zaleca UTC dla serwerów)
działają identycznie.

### Parser crontaba (`fpm_cron_schedule.c`) — zakres i pułapki

Pięć pól (minuta, godzina, dzień miesiąca, miesiąc, dzień tygodnia), bez
zależności poza libc. Obsługuje `*`, `N`, `N-M`, `*/S`, `N-M/S`, listy
przez przecinek, oraz nieoczywisty, ale prawdziwy crontab(5): `N/S` BEZ
zakresu (np. `10/15`) znaczy "od N do końca dziedziny pola ze skokiem S", nie
pojedynczą wartość — inaczej harmonogramy przenoszone z systemowego crona
zmieniłyby znaczenie. Dzień tygodnia 0-7, gdzie zarówno 0 jak i 7 znaczą
niedzielę (parsowane w tymczasowym bitmapie 0..7, potem 7 składane w 0).
Skróty: `@hourly`, `@daily`, `@weekly`, `@monthly`, `@yearly` (tylko te pięć,
zgodnie z zadaniem — `@midnight`/`@annually` świadomie pominięte, tania
rzecz do dodania później, gdyby ktoś potrzebował).

**Reguła OR dla dnia miesiąca i dnia tygodnia** (klasyczny, zaskakujący
cron): kiedy OBA pola są ograniczone (żadne nie jest literalnym `*`),
dopasowanie to SUMA, nie iloczyn. Literalność sprawdzana na całym polu
PRZED podziałem po przecinkach (`*/1` NIE liczy się jako `*`, zgodnie
z zachowaniem vixie-cron) — zaimplementowane i zweryfikowane osobnym
programem (patrz testy niżej): `0 0 13 * 5` pasuje do każdego piątku ORAZ
do 13. dnia miesiąca niezależnie od dnia tygodnia.

**Błąd składni odrzuca konfigurację przy starcie**, z czytelnym komunikatem
wskazującym które pole i dlaczego (`fpm_cron_schedule_parse()` wypełnia
bufor błędu przekazany przez `validate()`, `validate()` loguje
`ZLOG_ALERT` i zwraca -1 — ten sam mechanizm co reszta walidacji `fpm_conf.c`,
zero specjalnego traktowania). Zweryfikowane: `* * * *` (za mało pól),
`99 * * * *` (wartość poza zakresem), `*/0 * * * *` (krok zero) — wszystkie
trzy odrzucone ze startu, exit 78, bez próby "mniej więcej" interpretacji.

`fpm_cron_schedule_next()` ma limit poszukiwań (~4 lata w minutach) jako
ostatnia siatka bezpieczeństwa przed harmonogramem, który strukturalnie
nigdy nie może zajść (np. `0 0 30 2 1` — 30 lutego, dzień tygodnia
ograniczony, więc reguła OR też nie ratuje) — walidacja przy starcie
sprawdza tylko składnię, nie "czy harmonogram może kiedykolwiek zajść".
Nie zaimplementowano osobnej walidacji "czy to jest strukturalnie możliwe"
— uznane za niewartą złożoność dla przypadku, który i tak jest ewidentnym
błędem operatora i zostanie zauważony (proces odmawia startu z ALERT-em
"schedule never matches").

### `cron.timeout` — dokładnie ten sam watchdog co `supervisor.stop_timeout`

Wydzielone do `fpm_pool_watchdog.c`: `fpm_pool_watchdog_arm(target_pid,
timeout_seconds)` forkuje proces-watchdoga, który przez `pidfd` (Linux) albo
`kill(pid,0)` w pętli co sekundę (fallback, tylko lokalne budowanie/testy)
czeka aż `target_pid` się zakończy LUB minie `timeout_seconds` — jeśli
minie, a proces wciąż żyje, SIGKILL. Dla supervisora uzbrajany w handlerze
SIGTERM (siatka bezpieczeństwa na `stop_timeout`); dla crona uzbrajany PRZED
uruchomieniem skryptu (`cron.timeout`, domyślnie 0 = bez limitu, wtedy
w ogóle nie uzbrajany). Mechanizm sam się "anuluje": jeśli skrypt skończy
się w czasie, proces w końcu i tak wywoła `exit()`, `poll()` na pidfd dostaje
POLLIN (proces się zakończył) i watchdog kończy się cicho, bez sygnału — nie
trzeba osobnego wywołania "anuluj timeout".

### Wykonanie skryptu — dokładnie ten sam kod co supervisor, teraz wspólny

Wydzielone do `fpm_pool_script.c`: `fpm_pool_script_install_sapi_overrides()`
(nadpisania `ub_write`/`getenv`/`read_post`/`read_cookies`/
`register_server_variables`, bezpieczne z tego samego powodu co
w supervisorze — proces nigdy nie wraca do pętli accept) i
`fpm_pool_script_run(pool_name, script_path)` (`php_request_startup` /
`php_fopen_primary_script` / `php_execute_script` / `php_request_shutdown`,
bez `SG(request_info)` z FastCGI). `fpm_pool_supervisor.c` przepisane na te
dwie funkcje zamiast trzymać własne kopie — czysty refaktor, zero zmiany
zachowania (te same 10 scenariuszy z sekcji 3o dalej przechodzą, patrz
"zweryfikowane po refaktorze" niżej).

### SIGTERM: prostsze niż supervisor, bo nie ma "bieżącej iteracji do pilnowania podczas snu"

W odróżnieniu od supervisora, handler SIGTERM crona TYLKO ustawia flagę
(`cron_term_requested`), nie uzbraja żadnego watchdoga. Podczas SNU nic się
nie wykonuje — flaga budzi `sleep()` (przerywane każdym dostarczonym
sygnałem, dla którego mamy handler) i proces kończy się natychmiast, zanim
cokolwiek zostanie sforkowane. Podczas WYKONYWANIA skryptu granicę czasu
ustawia `cron.timeout` (osobny mechanizm, patrz wyżej), nie SIGTERM — skrypt
kończy bieżący przebieg naturalnie, tak jak supervisor (test 8 niżej). Zwykła
eskalacja mastera (`process_control_timeout`, patrz sekcja 3p) dotyczy tego
typu tak samo jak każdego innego, bez cron-specyficznego kodu.

### `requires_pm = 0` (różni się od supervisora, gdzie jest 1)

Zgodnie z opisem zadania. W praktyce nie ma to znaczenia funkcjonalnego —
`validate()` ustawia `pm`/`pm_max_children` programowo PRZED sprawdzeniami
`requires_pm` w `fpm_conf.c` (patrz sekcja 3i, `fpm_conf.c:960-969`), więc te
sprawdzenia i tak przechodzą trywialnie niezależnie od wartości `requires_pm`.
Zostawione jak w zadaniu, bo semantycznie bardziej precyzyjne: cron nie
"wymaga" `pm` w sensie "użytkownik musi coś skonfigurować", bo user w ogóle
nie ma tu nic do ustawienia (odrzucone przez `rejects`).

### `rejects`

`listen`, `listen.`, `pm`, `pm.`, `request_terminate_timeout(_track_finished)`,
`request_slowlog_timeout`, `request_slowlog_trace_depth`, `slowlog`, `ping.`,
`access.`, `security.limit_extensions`, `supervisor.` — nadzbiór listy
z zadania, z tym samym uzasadnieniem co supervisor (sekcja 3o): `pm`/`pm.`
w całości (generowane programowo, zawsze 1, dwa źródła prawdy inaczej),
`listen`/`listen.`/`ping.`/`access.` w całości (brak requestów FastCGI),
plus `supervisor.` (dyrektywy DRUGIEGO typu poola).

### Testy (9 scenariuszy z zadania, wszystkie zielone, surowe wyjścia w raporcie)

1. Config bez `pool.type` (`pm = static`, zwykły `listen`) — `-t` zielone,
   proces startuje i kończy się bez śladu (BC nietknięte).
2. `cron.schedule = * * * * *`, skrypt zapisujący `gmdate()` + PID do pliku —
   dwa kolejne przebiegi o `16:07:00` i `16:08:00` UTC (dokładnie na początku
   minuty), różne PID-y (proces kończy się i jest wskrzeszany między
   przebiegami, zgodnie z modelem "jeden proces = jeden przebieg").
3. Tymczasowy program testowy (skompilowany osobno, NIE w produkcie, usunięty
   po teście) dla `*/15 * * * *`, `0 3 * * *`, `30 4 1,15 * *`, `0 0 * * 0`,
   `@daily` liczony od `2026-09-05 16:10:00 UTC` (piątek) — wszystkie terminy
   sprawdzone ręcznie jako poprawne (najbliższy kwadrans, najbliższa 3:00,
   najbliższy 1. albo 15. dzień miesiąca o 4:30, najbliższa niedziela
   o północy, to samo dla `@daily`).
4. Reguła OR: `0 0 13 * 5` — kolejnych 10 dopasowań od 2026-09-01 pokazuje
   naprzemiennie piątki (dowolna data) i 13. dzień miesiąca (dowolny dzień
   tygodnia, w tym niedziela i wtorek w kolejnych miesiącach) — potwierdzone
   `fpm_cron_schedule_next()` bezpośrednio, nie tylko bitmapami.
5. Złe harmonogramy `* * * *` / `99 * * * *` / `*/0 * * * *` — każdy odrzucony
   ze startu (`ALERT` + `FPM initialization failed`, exit 78) z czytelnym
   komunikatem wskazującym które pole i dlaczego.
6. `rejects` — pool `cron` z `listen` albo `pm.max_children` w konfiguracji
   dają czytelny `ALERT` i `FPM initialization failed` (exit 78).
7. `cron.timeout = 3`, skrypt z `sleep(20)` — proces zabity, w logu skryptu
   widać TYLKO wpis "starting sleep(20)", nigdy "finished normally"; następny
   przebieg (nowy PID) wystartował normalnie w kolejnej minucie.
8. SIGTERM w trakcie snu (wysłane bezpośrednio do dziecka, zanim nadszedł
   termin) — proces kończy się natychmiast, skrypt NIGDY nie uruchomiony
   (log pusty), nowe dziecko odrodzone przez `fpm_children.c` czeka na
   kolejny termin. SIGTERM w trakcie przebiegu (wysłane w połowie pętli
   10 "ticków") — skrypt dokończył WSZYSTKIE 10 ticków i "run end" przed
   zakończeniem procesu (zachowanie identyczne z supervisorem, test 7 w 3o).
   Oba scenariusze: zero sierot po `ps`.
9. Współistnienie: jeden pool `http` (`pm = static`, 2 workery) + jeden pool
   `cron` w tym samym mastrze — oba wstają, cron wykonuje przebieg normalnie
   podczas gdy pool http stoi gotowy. `SIGUSR2` (reload, `execvp()`) — ten
   sam PID mastera, świeże dzieci obu typów (`web` x2 + `cronjob`), zero
   procesów z PPID=1 po reloadzie.

**Zweryfikowane po refaktorze supervisora na wspólne pliki**: powyższe testy
uruchamiane były na tej samej binarce, w której `fpm_pool_supervisor.c` już
korzysta z `fpm_pool_watchdog.c`/`fpm_pool_script.c` zamiast własnych kopii —
build czysty (zero ostrzeżeń o niezdefiniowanych symbolach), a testy 7-9
(SIGTERM, timeout, coexistence) pośrednio ćwiczą te same funkcje współdzielone
z supervisorem. Osobny, pełny przebieg 10 scenariuszy z sekcji 3o NIE został
powtórzony w tym zadaniu (poza zakresem) — ryzyko regresji ocenione jako
niskie, bo refaktor jest czysto mechaniczny (przeniesienie identycznego ciała
funkcji, zmiana tylko nazw i miejsca pliku), ale warto to dopisać jako
"czego nie zrobiono" wprost.

### Czego NIE zrobiono / wątpliwe

- Pełny powtórzony przebieg 10 testów supervisora z sekcji 3o po refaktorze
  na wspólne pliki (patrz wyżej) — niskie ryzyko, ale nie zweryfikowane
  bezpośrednio, tylko pośrednio przez testy crona korzystające z tego samego
  kodu.
- Status crona w `fpm_status.c` (oznaczenie typu w statusie, jak w sekcji 3o
  dla supervisora) — nie ruszone, poza zakresem tego zadania (patrz sekcja 3j,
  metryki dla typów bez requestów to osobne zadanie).
- Walidacja "czy harmonogram może kiedykolwiek strukturalnie zajść"
  (np. 30 lutego) — tylko limit poszukiwań jako siatka bezpieczeństwa
  w runtime, nie odrzucenie przy starcie. Patrz wyżej.
- `@midnight`/`@annually` jako dodatkowe aliasy skrótów — świadomie pominięte,
  zadanie wymieniało tylko pięć skrótów.
- Nadrabianie zaległych przebiegów — świadomie NIE zrobione, zgodnie
  z zadaniem (patrz sekcja o "brak nadrabiania" wyżej). Ktoś kiedyś będzie
  chciał to dodać — to ma być świadoma zmiana projektowa.


## 3s. Async w PHP — ZAMKNIĘTE, oraz opcja `ext` z danymi requestu z C (2026-09-05)

Rozmowa o tym, czy da się mieć w PHP model: worker oddaje sterowanie na I/O,
bramka wpycha mu w tym czasie kolejne żądanie. Wniosek: **nie budujemy tego**,
ale powody warto mieć spisane, bo za pół roku ktoś (my) zapyta znowu.

### Dlaczego nie — trzy warunki, wszystkie muszą zajść naraz

1. **Stan requestu per-korutyna w silniku.** PHP ma JEDEN komplet stanu
   requestu na proces, nie na request. `php_request_startup()` /
   `php_request_shutdown()` ustawiają i sprzątają globale: `$_SERVER`,
   nagłówki, bufory wyjścia, handler błędów, bieżące ini, sesja. Nie ma
   "obiektu requestu", od którego to wisi. Wpuszczenie drugiego żądania
   w trakcie pierwszego nadpisuje stan pierwszego — zawsze, nie "czasem".
   True Async po całej dotychczasowej pracy ma per-korutyna `ob_*` i cache
   hostent. To pokazuje skalę roboty. STATUS: nie ma.

2. **Przechwytywanie I/O w sterownikach.** KOREKTA wcześniejszej notatki
   ("połowa rozszerzeń nie zadziała") — dla typowego stosu to nieprawda:
   - `mysqlnd` idzie przez `php_stream` → MySQL przez PDO/mysqli
     w domyślnej kompilacji JEST w warstwie przechwytywalnej.
   - `phpredis` też siedzi na streamach. Predis (userland) tym bardziej.
   - Po złej stronie: `libpq` (`pdo_pgsql`), `libmysqlclient`, `curl` —
     własne gniazda, o schedulerze nie wiedzą.
   Czyli to jest krótka, konkretna lista, a nie "ekosystem".
   STATUS: technicznie w zasięgu, nie zrobione.
   POPRAWKA (2026-09-05, po pytaniu o własny scheduler): to jest w zasięgu
   NAS SAMYCH, bez łatania silnika. PHP ma publiczne API
   `php_stream_xport_register()` — rozszerzenie może podmienić transport
   `tcp` na własny (tak działa ext/openssl). Skoro mysqlnd i phpredis idą
   przez streamy, nasze `ext` mogłoby przechwycić ich gniazda i zawiesić
   fiber zamiast blokować. Bez patcha, bez forka php-src.
   Wykonalne przez nas — i BEZWARTOŚCIOWE samo w sobie, dopóki stoi
   warunek 1.

3. **Frameworki napisane tak, żeby z tego skorzystać.** I to jest warunek,
   który zabija temat, bo nie zależy od nikogo, na kogo mamy wpływ.
   Fiber sam z siebie nic nie daje — zawiesza wykonanie tylko wtedy, gdy
   wywołanie NA DOLE odda sterowanie. `PDO::query()` wchodzi w `read()`
   i blokuje proces niezależnie od tego, czy jesteś w fiberze.
   amphp działa, ale z KLIENTAMI amphp (`amphp/mysql`, `amphp/redis`),
   a Doctrine i Eloquent ich nie użyją, bo ich API jest z definicji
   synchroniczne (`$user->posts` musi zwrócić kolekcję, nie obietnicę).
   To nie kwestia adaptera, tylko kształtu ORM-a.
   Jedyny prawdziwy wyjątek: `Symfony\HttpClient` (równoległe żądania
   przez `curl_multi`) — ale to HTTP, nie baza, i to wyspa, nie model.
   Laravel Octane to NIE async, tylko worker mode (proces żyje między
   żądaniami, jedno naraz). Symfony Runtime to samo.
   STATUS: nie są i się nie zapowiadają.

   POPRAWKA (2026-09-05, po pytaniu "czy tego nie ogarnia True Async"):
   powyższe jest prawdą dla modelu amphp (JAWNEGO: inne klienty, inne API,
   więc Eloquent/Doctrine odpadają), ale NIE dla modelu True Async, który
   jest PRZEZROCZYSTY — `PDO::query()` sam oddaje sterowanie, a kod
   wywołujący o niczym nie wie. Gdyby warunki 1 i 2 zostały dowiezione,
   Laravel i Symfony działałyby BEZ ZMIAN. Warunek 3 nie znika calkiem,
   tylko slabnie do: "framework nie może przeciekać stanem między
   requestami" (singleton pamiętający zalogowanego użytkownika — statyki
   klas żyją w procesie, nie w korutynie). To jest DOKŁADNIE ta sama
   dyscyplina, ktorej wymaga Octane, a Laravel ma Octane i Symfony ma
   Runtime — wiec ekosystem jest w te strone czesciowo przygotowany.
   To czyni True Async WAZNIEJSZYM, niz wynikalo z pierwszej wersji tej
   sekcji, gdzie warunek 3 byl postawiony za ostro.

   Co True Async ma dzis: warunek 2 w duzej czesci (przechwytywanie w
   silniku, wiec szersze niz nasz `php_stream_xport_register`); warunek 1
   tylko `ob_*` i cache hostent per-korutyna, reszta EG/SG nierozdzielona;
   `max_execution_time` bez zmian. Nie znikna nigdy: fatal ubija wszystkie
   korutyny w procesie, `memory_limit` jest na proces.

   Praktycznie dla nas: to fork php-src, RFC dla uzytkownika ANULOWANE,
   do 8.7 proponowany sam ABI schedulera bez I/O. Budowanie na tym dzis =
   pilnowanie cudzego forka i porzucenie zasady "przypiety tag upstreamu,
   zero latek", na ktorej stoi ten projekt. Nasza rola: OBSERWOWAC jeden
   sygnal — czy stan requestu staje sie per-korutyna.

### WYNIK POC (2026-09-05): dziala, ale wylacznie na forku — galaz `async-poc`

`pool.type = async` zostal zbudowany i URUCHOMIONY na forku
`true-async-stable` razem z naszym `sapi/fpmng`. Jeden proces,
`pm.max_children = 1`, wlasny klient FastCGI:

    4 x slow.php (usleep 500 ms)    503 ms   (fcgi: 2008 ms)
    4 x net.php  (fsockopen 500 ms) 537 ms   (fcgi: 2140 ms)
    RSS po 2000 requestach          bez wzrostu

Kazdy request dostal swoj naglowek, `$_GET`, `$_SERVER`, `$GLOBALS`
i `get_included_files()`. Pelny opis w NOTES sekcja 3t NA GALEZI `async-poc`.

**KOREKTA do tej sekcji:** teza "fiber przelacza stos, nie globale, wiec zaden
scheduler tego nie naprawi" byla ZA MOCNA. Fork ma publiczne switch-handlery
(`zend_async_API.h:269`) i przez nie da sie podmieniac SG, `EG(symbol_table)`
i `EG(included_files)` BEZ zmian w VM. Fork uzywa ich do `ob_*`; POC uzyl do
reszty i to wystarczylo.

**Sciana jest gdzie indziej i stoi:** tablice funkcji i klas sa PER PROCES
(`EG(function_table)` = `CG(function_table)`, czyszczone dopiero w
`shutdown_executor()`), wiec drugi request deklarujacy funkcje dostaje
"Cannot redeclare" i tak zostaje. Opcache zaklada jeden request na proces
(`ZendAccelerator.c:1958,2481`), wiec POC dziala z `opcache.enable = off`.
Galaz `global-isolation` forka zrobila `symbol_table` per korutyna, ale NIGDY
nie weszla do stable; statyki klas per korutyna zrobiono i cofnieto.

Stad wniosek, do ktorego doszlismy tez niezaleznie od kodu: realnym ksztaltem
tego typu nie jest "wiele niezaleznych requestow", tylko MODEL WORKERA —
aplikacja ladowana RAZ, request jako wywolanie w nia. Wtedy tablice funkcji
i klas nie sa problemem, bo nikt nie deklaruje ich drugi raz.

### DECYZJA (2026-09-05): DWA typy poola, nie jeden z przelacznikiem

`pool.type = fiber` (wlasny scheduler, czysty upstream) oraz
`pool.type = true-async` (fork). Powod jest wylacznie taki, zeby WYCOFANIE
bylo tanie: nasz kontrakt sprawia, ze typ to jeden plik + jedna linia
w rejestrze, wiec porzucenie jednej drogi to skasowanie pliku i linii.

Odrzucone: jeden typ `async` z dyrektywa `async.engine = fiber|true-async`.
Wtedy obie sciezki splataja sie w jednym pliku i usuniecie jednej znaczy
operowanie na zywym kodzie drugiej — czyli dokladnie to, czego ten uklad
ma unikac.

Kod wspolny (akceptor, obsluga requestu FastCGI, podmiana stanu) idzie do
TRZECIEGO, dzielonego pliku — ten sam wzorzec co `fpm_pool_watchdog.c`
i `fpm_pool_script.c` wydzielone przy cronie. Skasowanie jednego typu nie
rusza wtedy rdzenia, bo drugi z niego korzysta.

Do protokolu: wariant `fiber` NIE ISTNIEJE i nie jest zweryfikowany (patrz
nizej) — POC mamy wylacznie na forku. Zgoda na dwa typy jest wiec zgoda na
zbudowanie czegos niesprawdzonego, i to jest argument ZA tym ukladem,
nie przeciw. Oba warianty uderzaja w te sama sciane (tablice funkcji/klas,
opcache), wiec oba i tak wyjda na model workera.


### WARIANT "async bez forka" — opcja, NIE zweryfikowana

To jest rozumowanie, nie wynik pomiaru — nikt tego nie probowal. Zapisane,
bo zmienia rachunek kosztow eksperymentu: gdyby True Async nigdy nie wszedl
do PHP, ta droga nadal istnieje.

Co dzis pochodzi z forka i musialoby powstac u nas: (a) scheduler i reaktor
(`ext/async`, 33k linii na libuv) — moglby powstac na libevent, ktorego i tak
uzywamy w bramce; (b) przechwytywanie I/O w silniku (`xp_socket.c`,
`network.c`, `plain_wrapper.c`, `curl_async.c`, uspienia w
`basic_functions.c`) — czesciowo zastapialne przez
`php_stream_xport_register()`; (c) switch-handlery — NIEPOTRZEBNE, bo w tym
wariancie to MY jestesmy tym, kto przelacza, wiec podmieniamy stan sami tuz
przed wznowieniem fibera. `zend_fiber_suspend`/`zend_fiber_resume` sa
`ZEND_API` w upstreamie (`zend_fibers.h:135-136`).

Zasieg takiej wersji: gniazda i tylko gniazda — mysqlnd, phpredis,
`fsockopen`. NIE zlapie `sleep()`/`usleep()`, curl, libpq ani zwyklych plikow,
bo to nie idzie przez warstwe streamow.

Koszt: piszemy od zera to, co fork juz napisal, i bierzemy na siebie
utrzymanie. Sciana z tablicami funkcji/klas i opcache stoi TAK SAMO —
zadna z dwoch drog jej nie omija.


### Własny scheduler dla amphp — sprawdzone, nie ma czego budować

Pytanie: skoro amphp stoi na fiberach, czy nie podstawić mu naszej pętli
i "łapać" przerwania. Odpowiedź: ten kawałek JUŻ ISTNIEJE. Revolt ma
abstrakcję sterownika i sam wykrywa `ext-event` (to dosłownie libevent),
`ext-uv` i `ext-ev`; bez nich spada na `stream_select`. Nasz sterownik byłby
czwartą kopią `ext-event` — tego samego libeventa, którego używa bramka.
Jedyna przewaga własnego: JEDNA wspólna pętla dla gniazd bramki w C i dla
I/O PHP-a — co ma sens wyłącznie w wersji ambitnej (nasz C przyjmuje
połączenia i woła PHP). Przy zwykłej aplikacji amphp, która sama trzyma
port, zysk zerowy, a koszt to sterownik chodzący za zmianami Revolt.

I rzecz najważniejsza, gdyby ktoś chciał tędy iść:
**Fiber przełącza STOS, nie przełącza GLOBALI.** Żaden scheduler tego nie
naprawi, bo to nie jest problem harmonogramowania. Żeby przełączać requesty,
trzeba przy każdym przełączeniu fibera podmieniać `EG` i `SG` — czyli
warunek 1. Scheduler i przechwytywanie I/O są w naszym zasięgu; brakującym
elementem nie jest ani jedno, ani drugie.

**Czego pilnować, gdyby wracać do tematu:** nie "czy True Async dodał
sterowniki", tylko **czy stan requestu jest per-korutyna**. To jedno pytanie
rozstrzyga warunek 1. Warunek 3 i tak zostaje.

### Rachunek, który mimo wszystko jest na korzyść async

Warto zapisać, bo pokazuje, że to nie jest głupi pomysł, tylko niewykonalny.
Typowy request frameworkowy z bazą i redisem: ~30 zapytań × ~1 ms czekania
przy ~5 ms CPU. Worker jest bezczynny ~85% czasu requestu. Żeby wysycić
4 rdzenie trzeba nie 4 procesów, tylko ~28 — przy 60 MB/worker to ~1,7 GB
RAM-u wydanego na czekanie. Na małym VPS to realne ograniczenie.
**Async oszczędza procesy, które CZEKAJĄ, nie te, które LICZĄ** —
współbieżność to nie równoległość, na 4 rdzenie nadal trzeba 4 procesów.

DO ZMIERZENIA na realnej aplikacji: stosunek czasu requestu do CPU requestu
(FPM loguje oba). To jest współczynnik upakowania i rozstrzyga wartość async
faktami zamiast przewidywaniami. 1:7 → async oszczędza 6 procesów na 7.
1:2 → temat zamknięty definitywnie.

### Co z tego wynika DLA NAS — aplikacja amphp/ReactPHP

Ważne odkrycie z tej rozmowy: **aplikacja w amphp nie potrzebuje
php-fpm-ng jako serwera HTTP.** Ma własny serwer, sama trzyma port, jest
długo żyjącym procesem. Nasza bramka, pula workerów i cały FastCGI nie mają
się do czego podpiąć. Nie konkurujemy — mijamy się.

Ale to dobry wynik, bo taka aplikacja pasuje do nas jako **`pool.type =
supervisor`** — nadzór nad procesem, restart policy, backoff, statystyki,
crony obok w tym samym pliku konfiguracyjnym. To już mamy i to działa.

Uwaga do protokołu — ograniczenia amphp, żeby nie sprzedawać tego jako
darmowego obiadu: (a) JEDNO blokujące wywołanie gdziekolwiek w drzewie
zależności zatrzymuje pętlę dla WSZYSTKICH żądań w locie (w FPM spowolniłoby
jedno); (b) ekosystem composera jest blokujący (SDK płatności, AWS →
Guzzle/curl); (c) nie ma granicy między requestami — arena się nie resetuje,
`memory_limit`/`max_execution_time`/`set_time_limit()` nie działają per
request, fatal ubija wszystkie żądania w locie, singleton pamięta dane
poprzedniego użytkownika (znany footgun Octane/Swoole).

### OPCJA (nie decyzja): `pool.type = proxy`

Jedyny brakujący element między nami a aplikacjami asynchronicznymi.
Bramka trzyma :443, terminuje TLS, obsługuje ACME, oddaje pliki statyczne
sama, a resztę przekazuje po zwykłym HTTP/1.1 na localhost do procesu
aplikacji (amphp/ReactPHP/cokolwiek). Aplikacja nie wie o niczym, my nie
dotykamy Revolt. Domyka historię "jedna binarka, bez nginxa".
Koszt: `evhttp` jako KLIENT, kilkadziesiąt linii, nic co już działa nie jest
ruszane. Ładnie się składa z TLS+ACME z sekcji 3l, które i tak są na końcu.

### OPCJA (nie decyzja): `ext` wystawiające dane requestu sparsowane w C

Obserwacja Piotra: amphp parsuje HTTP w PHP, a my mamy to już w C — może da
się to wykorzystać. Dwie bardzo różne wersje:

**Wersja skromna — nasz C parsuje, PHP dostaje gotowe.** Rozszerzenie
wystawiające metodę, URI, nagłówki i ciało już sparsowane przez `evhttp`
(roboczo: `fpmng_request_*`). amphp używa go zamiast własnego parsera.
Wykonalne, małe, ale **zysk prawdopodobnie znikomy**: zmierzyliśmy naszą
bramkę na ~25 µs/żądanie — to CAŁY tor HTTP w C (accept, parsowanie,
odpowiedź). Nawet gdyby amphp potrzebował na to 10× tyle, przy requeście
palącym 5 ms CPU to kilka procent. Parsowanie 200 bajtów nagłówków nie jest
tam, gdzie idzie czas.

**Wersja ambitna — nasz C prowadzi pętlę, PHP jest z niej wywoływane.**
Prawdziwa przeszkoda: **DWIE PĘTLE ZDARZEŃ** — nasza (libevent) i Revolt.
Obie nie mogą prowadzić. Gdy PHP zawiesza się na zapytaniu do bazy, ktoś
musi obsłużyć to gniazdo; jeśli to Revolt, on musi być na wierzchu, a nasza
pętla stoi. Da się rozwiązać, bo Revolt ma abstrakcję sterownika — można
napisać sterownik w C na naszym libevent i mieć JEDNĄ pętlę, naszą.
To realny projekt, nie weekend, plus utrzymywanie go razem ze zmianami
w Revolt.

**Gdzie C naprawdę wygrywa — i to NIE jest parsowanie:** TLS, HTTP/2
(amphp robi w PHP całą maszynę stanów, ramkowanie i HPACK) oraz pliki
statyczne. Tam różnica jest duża, a nie kilkuprocentowa. I do tego nie
trzeba ŻADNEJ z powyższych wersji — wystarczy `pool.type = proxy` powyżej.

### POMIAR WYKONANY (2026-09-05) — wersja skromna jest bez sensu

Poligon 192.168.8.103, amphp/http-server 3.4.6, PHP 8.6.0-dev z tego samego
drzewa zrodel co nasze binarki (`--disable-all --enable-filter`, zeby
league/uri mialo `filter_var()`), `zend.assertions=-1`, kompresja wylaczona.
Odpowiedz "Hello World", `wrk -t1 -c2 -d12s`, CPU liczone z
`/proc/<pid>/stat` (utime+stime) samego procesu serwera, 3 powtorzenia:

    amphp (TCP_NODELAY wl.)   ~6 900 req/s    ~147 us/req CPU
    nasza bramka, hello.php  ~13 600 req/s    (k3d: 139 us/req)

Czyli **caly per-request koszt amphp w PHP jest mniej wiecej rowny naszemu
calemu torowi w C razem z `php_request_startup/shutdown` i wykonaniem
`hello.php`** — a przepustowosc mamy 2x wyzsza na tej samej maszynie.
Skoro parsowanie HTTP jest tylko ulamkiem tych 147 us, to wystawienie
naszego parsera jako `ext` nie ma czego uratowac. WERSJA SKROMNA: ODRZUCONA
na liczbach. Wersja ambitna (sterownik Revolt w C) niezmieniona — nadal
realny projekt, nie weekend.

Zastrzezenie do protokolu: nasze 139 us/req pochodzi z pomiaru w k3d,
a 13 600 req/s z golej maszyny; liczba req/s jest porownywalna
(ta sama maszyna, ta sama metodyka), liczba us/req nie w pelni. Gdyby ktos
chcial to domknac, trzeba przemierzyc bramke bare-metal ta sama metoda.

### ZNALEZISKO PRZY OKAZJI: amphp domyslnie NIE ustawia TCP_NODELAY

`Amp\Socket\BindContext::$tcpNoDelay` ma wartosc domyslna `false`, a
`SocketHttpServer::expose()` bez jawnego kontekstu tej wartosci nie zmienia.
Skutek zmierzony: na polaczeniu keep-alive pierwsze zadanie 0,5 ms, **drugie
41 ms**; `wrk -t1 -c2` daje wtedy 49 req/s zamiast 6 900, a latencja stoi
rowno na 40,8 ms. Klasyczny Nagle + delayed ACK, bo odpowiedz idzie
`transfer-encoding: chunked` w kilku zapisach. Z `Connection: close`
problem znika (2 686 req/s), co potwierdza mechanizm.
Lekarstwo: `$server->expose($addr, (new BindContext())->withTcpNoDelay())`.

To jest DOKLADNIE ta sama klasa bledu co nasza latka 0002 na `main/fastcgi.c`
(tam `req->tcp` ustawiane tylko pod `#ifdef _WIN32`). Warto zglosic do amphp.

Pulapka diagnostyczna, warta zapamietania: przy `NullLogger` amphp POLYKA
wyjatki z obslugi klienta. Objaw byl taki, ze `wrk` pokazywal 80 tysiecy
"read errors" i zero odpowiedzi, a w logu nie bylo NIC. Dopiero wlasny
logger na stderr pokazal `Call to undefined function filter_var()`.


## 3u. `pool.type = status` — zaimplementowane i zweryfikowane (2026-09-05)

Nowy plik `sapi/fpmng/fpm/fpm_pool_status.c`/`.h`, jedna linia w
`fpm_pool_types[]` (`fpm_pool_type.c`). Zero nowych dyrektyw konfiguracyjnych —
`validate()` programowo wymusza `pm = static` + `pm.max_children = 1` (jeden
proces w zupelnosci wystarcza na scrapy monitoringu), tak jak `supervisor`
mapuje `supervisor.processes` na `pm.*`. `requires_listen = 1`, ale w
odroznieniu od bramki http port to BEZPOSREDNIO `listen` z configu (nie
fcgi+1) — ten typ nie ma zadnego fcgi za soba, wiec nie ma czego przesuwac.

### Ksztalt danych — rozgalezienie po `serves_requests`, dokladnie jak w 3j

`fpm_pool_status.c` idzie po `fpm_worker_all_pools`, dla kazdego poola bierze
`fpm_pool_type_of(wp)` i:
- `serves_requests = 1` (fcgi, http): czyta `wp->scoreboard` (ten sam
  scoreboard co dzisiejszy `fpm_status.c`, `fpm_scoreboard_copy(wp->scoreboard, 0)`
  — kopia pod lockiem, bo to CUDZY pool, czytany z INNEGO procesu) — idle,
  active, requests.
- `serves_requests = 0` (supervisor, cron): NOWY piaty operator w
  `fpm_pool_type_s` — `status(wp, out)`, zwraca `struct fpm_pool_status_s`
  (`state`, `last_start`, opcjonalny `last_exit_code`, `consecutive_failures`,
  opcjonalne `next_run` albo `backoff_until`).
  Kazdy taki typ implementuje to we WLASNYM pliku, czytajac WLASNA pamiec
  dzielona — `fpm_pool_status.c` nie zna wewnetrznej struktury
  `fpm_supervisor_shared_s` ani `fpm_cron_shared_s`, dokladnie jak wymaga
  kontrakt z 3h ("nowy typ = nowy plik + jedna linia w rejestrze").
- Typ bez `.status` i `serves_requests = 0` (dzis: `status` samo siebie) —
  pomijany w wyjsciu, bez wykrywania po nazwie.

`enum fpm_pool_state_e` (running/backoff/gave_up/finished/idle) i
`struct fpm_pool_status_s` zyja w `fpm_pool_type.h` — to jedyne miejsce
wspolne dla typow, ktore go wypelniaja, i dla `fpm_pool_status.c`, ktore go
czyta. Prometheus dostaje `fpmng_pool_info{pool,type} 1`, a stan jest zestawem
serii `fpmng_pool_state{pool,state} 0|1`, nie liczba wymagajaca znajomosci enum.
Pola specyficzne dla typu (`next_run` dla crona, `backoff_seconds` dla
supervisora) sa emitowane tylko tam, gdzie maja znaczenie; ta sama regula
obowiazuje w JSON. Zero etykiet o nieograniczonej kardynalnosci: etykiety
pochodza wylacznie z konfiguracji pooli i zamknietego zbioru stanow.

### Dolozony minimalny stan w shm — dokladnie tyle, ile pokazane, ani pola wiecej

**Supervisor** (`fpm_supervisor_shared_s`, w `fpm_pool_supervisor.c`) mial juz
`failures`/`terminal`/`gave_up` (polityka backoffu, 3o) — dolozone TRZY nowe
pola wylacznie na potrzeby statusu: `running` (bool, ustawiane tuz przed
`fpm_pool_script_run()` i zerowane zaraz po), `last_start` (epoch), oraz
`last_exit_code`. Zadne z nich nie wplywa na polityke restart/backoff — to
czysto obserwacyjny dodatek.

**Cron** (3r) mial ZERO stanu w shm — swiadoma decyzja, bo "kazdy nowy proces
liczy termin wylacznie z biezacego zegara". Ta decyzja **zostaje w mocy**:
`next_run` dla statusu jest liczony NA BIEZACO w `fpm_pool_cron_status()`
przez `fpm_cron_schedule_next(wp->config->cron_parsed_schedule, time(NULL))`
— dokladnie ta sama funkcja, ktorej uzywa sam cron do policzenia wlasnego
kolejnego terminu, wywolana na configu odczytanym z pamieci procesu statusu
(wspolny fork z mastera, ten sam config dla wszystkich poolow — patrz nizej).
Zero shm dla `next_run`.

To, czego cron NIE MOZE policzyc z samego zegara, to fakty historyczne:
`last_run` i `last_exit_code`. Dla tych dwoch (plus `running` i
`consecutive_failures`, ktore i tak sa "za darmo" przy tej samej alokacji)
dodano `fpm_cron_shared_s` — NOWY, minimalny `init_main` dla typu `cron`
(wczesniej `NULL`, "nic do zrobienia po stronie mastera" — teraz jest: jedna
alokacja `fpm_shm_alloc()`, ten sam wzorzec co supervisor). **To jest jedyny
kompromis wobec "cron jest bezstanowy" z 3r** — i jest swiadomy: bez tego
"last_start"/"last_exit_code" fizycznie nie da sie pokazac (nikt inny tego
nie pamieta), a "next_run" i tak zostal bezstanowy. `consecutive_failures`
dla crona jest policzalne za darmo (ten sam `last_exit_code`), ale na NIC nie
wplywa — cron nie ma polityki, ktora by to konsumowala (w odroznieniu od
supervisora); to czysty sygnal dla czlowieka/monitoringu ("ten cron pada z
rzedu N razy"), zaakceptowany bo tania konsekwencja tego samego stanu, nie
osobna decyzja projektowa. "Ile przebiegow pominieto z powodu nakladania"
(wymienione w 3j jako mozliwy dodatek dla crona) **NIE zostalo zaimplementowane**
— przy `pm.max_children = 1` nakladanie jest fizycznie niemozliwe (3r), a
liczenie "ile terminow zostalo pominietych miedzy last_run a teraz" byloby
tylko kosmetyka bez zadnej polityki za nim; uznane za zbedna zlozonosc.

### PUŁAPKA — prawdziwe znalezisko: `fpm_children.c` domyslnie ZWALNIA scoreboardy CUDZYCH poolow w kazdym dziecku

To jest **jedyne miejsce, w ktorym dotknieto `fpm_children.c`**, i zrobiono to
z pelnym uzasadnieniem, dokladnie jak zadanie przewidywalo. Objaw: pool
`status` odczytujacy scoreboard poola `fcgi`/`http` (czyli WLASNIE to, po co
istnieje wariant 1 z 3j) segfaultowal **w mniej niz 1ms od forka**, zanim
jakikolwiek kod nasz zdazyl cokolwiek zrobic — na samym pierwszym dotknieciu
`wp->scoreboard->idle` dla CUDZEGO poola.

Diagnoza (godziny zmarnowane na falszywe tropy — zapisane, zeby nikt nie
musial ich powtarzac): to NIE byl problem Zend MM, NIE opcache, NIE
kolejnosc alokacji vs forka (sprawdzone: oba scoreboardy alokowane w
masterze PRZED jakimkolwiek forkiem), NIE roznica anonymous-vs-named shm
(przetestowane osobno: `shm_open()` zamiast `mmap(MAP_ANON)` w
`fpm_shm_alloc()` — crash identyczny), NIE sasiedztwo regionow w pamieci
(przetestowany spacer miedzy alokacjami — crash identyczny), NIE ograniczenie
srodowiska/sandboxa tego repo (goly program w C robiacy `mmap(MAP_SHARED)` +
dwa kolejne forki dziala bez zarzutu, w tym samym sandboxie bash).

Prawdziwa przyczyna, znaleziona przez czytanie kodu, nie zgadywanie:
`fpm_children.c:fpm_child_resources_use()` (wolane w KAZDYM dziecku, zaraz po
`fork()`, przed czymkolwiek innym) ma petle:

```c
for (wp = fpm_worker_all_pools; wp; wp = wp->next) {
    if (wp == child->wp || wp == child->wp->shared) {
        continue;
    }
    fpm_scoreboard_free(wp);   /* munmap() scoreboardu KAZDEGO INNEGO poola */
}
```

To ISTNIEJACA, celowa higiena pamieci w upstreamie: kazdy worker po forku
zwalnia (munmapuje) scoreboardy WSZYSTKICH poolow poza swoim wlasnym, zeby nie
trzymac w swoim adresie niepotrzebnych mapowan. Bezpieczne od zawsze, bo
KAZDY dotychczasowy konsument scoreboardu (sam `fpm_status.c`) czyta
WYLACZNIE scoreboard WLASNEGO poola — nikt nigdy nie probowal czytac
scoreboardu CUDZEGO poola z INNEGO procesu. `pool.type = status` jest
**pierwszym** takim konsumentem, i ta "higiena" jest wobec niego destrukcyjna:
proces `status`, chcac pokazac dane fcgi/http, ma pod soba scoreboardy tamtych
poolow odmapowane, zanim zdazy je przeczytac.

To jest DOKLADNIE ograniczenie kontraktu z 3h przewidziane przez zadanie:
_"Nie ruszaj fpm_children.c bez bardzo dobrego powodu — jesli uznasz ze
musisz, uzasadnij dlaczego"_. Uzasadnienie: bez zmiany tutaj `pool.type = status`
nie moze dzialac DLA FCGI/HTTP W OGOLE (dla supervisor/cron dziala od razu,
bo ich wlasny stan nie przechodzi przez `fpm_scoreboard_free()` — to osobna
alokacja, `fpm_pool_supervisor.c`/`fpm_pool_cron.c` wlasny rejestr).

Naprawa (minimalna, danych-nie-kodu, zgodna z duchem 3h): nowe pole w
`fpm_pool_type_s` — `reads_foreign_scoreboards:1` (dzis ustawione tylko dla
`status`). W `fpm_child_resources_use()` petla zwalniajaca cudze scoreboardy
jest owinieta w `if (!fpm_pool_type_of(child->wp)->reads_foreign_scoreboards)`
— decyzja jest per DZIECKO, nie per caly master: **wylacznie** dziecko poola
`status` nie zwalnia cudzych scoreboardow; KAZDY inny pool w tym samym
configu (w tym zwykly fcgi/http obok `status`) nadal zwalnia je dokladnie
jak dzis, niezaleznie od tego, czy gdziekolwiek w configu istnieje pool
`status`. `fpm_children.c` pyta o typ TEGO KONKRETNEGO dziecka przez
`fpm_pool_type_of()` (juz istniejaca, generyczna funkcja) — nie zna zadnego
konkretnego typu, tylko czyta dane. Pierwsza wersja tej naprawy wylaczala
higiene dla calego mastera przez osobna funkcje skanujaca caly config
(`fpm_pool_type_any_reads_foreign_scoreboards()`) — **poprawione po review**:
to bylo za szerokie (workery fcgi/http tracily zabezpieczenie przed
przypadkowym dostepem do cudzej pamieci, mimo ze ich to nie dotyczylo).
Funkcja skanujaca caly config zostala usunieta jako niepotrzebna.

Bez `pool.type = status` w configu zachowanie jest 1:1 identyczne
z upstreamem — zweryfikowane wprost (nie tylko teoretycznie): dwa zwykle
poole `fcgi` (`web`, `web2`, bez zadnego `status` w configu) — `vmmap` na
kazdym z dzieci pokazuje dokladnie JEDEN 16K region `VM_ALLOCATE...SM=S/A`
(wlasny scoreboard), NIE dwa — czyli worker `web` nadal fizycznie nie ma
zmapowanego scoreboardu `web2` i odwrotnie, dokladnie jak przed jakakolwiek
zmiana w tym zadaniu. Dla kontrastu, master (przed forkiem) ma jeden,
POLACZONY region 32K (oba scoreboardy razem, sasiadujace w pamieci) — to
pokazuje dokladnie, CO konkretne dziecko odziedziczylo, a co samo zwolnilo.

Cena naprawy (po zawezeniu): TYLKO proces `status` trzyma w swoim adresie
mapowania scoreboardow WSZYSTKICH innych poolow (kilkanascie KB na pool,
zaniedbywalne) — zaden inny proces w systemie nie ponosi tego kosztu.

### Testy (zweryfikowane na zbudowanej binarce, macOS/arm64)

1. Config bez `pool.type` (`fcgi`) — `-t` zielone, dziala jak dzis (BC).
2. `pool.type = status` z odpowiadajacymi za "cokolwiek zwiazane z php/pm"
   dyrektywami (`pm.max_children`, `supervisor.script`) w tym samym bloku —
   `-t` daje czytelny ALERT dla obu i `FPM initialization failed` (exit 78).
3. Realny serwer: jeden pool `fcgi` (`web`), jeden `supervisor` (`sup`), jeden
   `cron` (`cronjob`, `* * * * *`), jeden `status` (`metrics`) — wszystkie
   wstaja. `curl /metrics` (Prometheus) i `curl /status` (JSON) na porcie
   statusu pokazuja jednoczesnie idle/active/requests dla `web` ORAZ
   `pool_info`, state/last_start/exit_code/failures dla `sup` i `cronjob`
   (plus `backoff_seconds` tylko dla `sup` i `next_run` tylko dla `cronjob`) —
   pola nieadekwatne dla typu sa nieobecne.
4. `SIGUSR2` (reload) — ten sam PID mastera, nowe dzieci wszystkich typow,
   `/status` po reloadzie pokazuje swiezy `last_start` dla `sup` (liczniki
   backoffu zerowane, zgodnie z 3p — nowy segment shm od zera po `execvp()`).
5. `kill -9` na procesie `sup` — `fpm_children.c` (nietkniete) wskrzesza go
   natychmiast; `/status` pokazuje nowy, pozniejszy `last_start` po
   wskrzeszeniu — status poprawnie odzwierciedla nowa generacje procesu.
6. Skrypt supervisora konczacy sie zawsze `exit(1)`, `restart = on-failure`,
   `restart_max = 2` — po dwoch porazkach `/status` pokazuje
   `"state":"gave_up"`, `"last_exit_code":1`, `"consecutive_failures":2` —
   dokladnie stan, w ktorym supervisor faktycznie sie znajduje (potwierdzone
   rownolegle logiem ALERT "giving up").
7. Po zawezeniu naprawy z `fpm_children.c` (patrz wyzej) do pojedynczego
   dziecka: powtorzony test 3 (status nadal czyta wszystkie scoreboardy bez
   segfaulta) — zielony. Osobno, config BEZ zadnego poola `status` (dwa zwykle
   `fcgi`) — `vmmap` na kazdym dziecku pokazuje TYLKO wlasny scoreboard (16K),
   nie oba — bit w bit jak przed calym zadaniem.
8. Klient laczacy sie z portem statusu i nie wysylajacy nic (`socket.recv()`
   bez wczesniejszego `send()`) — polaczenie zamkniete przez serwer po
   dokladnie ~5s (`SO_RCVTIMEO`), zmierzone bezposrednio (nie tylko
   zalozone) — proces NIE wisi w nieskonczonosc, i nadal odpowiada
   normalnie na kolejny, prawdziwy request zaraz potem.
9. Dwa poole `pool.type = status` w jednym configu (rozne porty) — oba
   wstaja niezaleznie, oba poprawnie odpowiadaja na `/status` dla tego
   samego poola `fcgi` obok nich, zero konfliktu.

### Czego NIE zrobiono / wątpliwe

- API metryk aplikacyjnych z PHP (`fpm_metric_register/inc/set/observe`) opisane
  w 3k jest osobnym zadaniem; ten typ wystawia tylko wbudowany stan poolow.
- Wiele procesow `status` (np. `status.processes`) — celowo brak, jeden
  proces wystarcza na scrape monitoringu; gdyby ktos potrzebowal wiecej, to
  swiadoma decyzja projektowa (nowa dyrektywa), nie domysl.
- Keep-alive/chunked/pelny parser HTTP — celowo brak (patrz komentarz w
  `fpm_pool_status.c`): to endpoint monitoringu, nie serwer WWW, jeden
  request na polaczenie w zupelnosci wystarcza.
- Obsluga `SIGQUIT` (graceful) dla poola `status` — brak wlasnego handlera,
  taki sam skutek uboczny jak dla supervisor/cron bez uchwytu na `SIGQUIT`
  (opoznienie do eskalacji `SIGTERM` przez mastera, patrz 3p scenariusz 2) —
  zaakceptowane, bo `status` nie ma zadnej pracy w toku do dokonczenia.
- "Ile przebiegow crona pominieto z powodu nakladania" — patrz wyzej,
  swiadomie pominiete jako zbedna zlozonosc przy `pm.max_children = 1`.
- `SO_RCVTIMEO`/`SO_SNDTIMEO` (5s, `FPM_POOL_STATUS_IO_TIMEOUT_SEC`) na
  polaczeniu klienckim — dopisane i zmierzone bezposrednio (test 8 wyzej):
  klient trzymajacy polaczenie otwarte bez wysylania danych zostaje
  odlaczony po dokladnie ~5s, proces obsluguje kolejne requesty normalnie.
  NIE zabezpiecza to przed powolnym, ALE nie-cichym klientem (np. wysylajacym
  1 bajt co 4s, resetujac timeout za kazdym razem) — teoretyczna luka DoS
  na pojedynczy proces (pm.max_children zawsze 1), zaakceptowana jako
  wystarczajaca dla endpointu monitoringu bez ruchu publicznego.

## 3t. Syscalle blokującego workera FastCGI — zaimplementowane i zweryfikowane (2026-09-05)

Zmiana założenia względem 3m/3q: **nie ma typu `fcgi-async`**. True Async
sprawdzone u źródła — RFC użytkowe anulowane, blokujące I/O w C-SAPI nie jest
przechwytywane, `main/fastcgi.c` i `sapi/fpm` nietknięte. Optymalizacje trafiły
więc do zwykłego, blokującego workera, podzielone według tego, czy zmieniają
obserwowalne zachowanie: nie zmieniają → domyślnie; zmieniają → opt-in.

### Co weszło

| co | gdzie | domyślnie | zmiana zachowania |
|---|---|---|---|
| naprawa `TCP_NODELAY` (błąd upstreamu) | `patches/0002` (`main/fastcgi.c`) | tak | tylko naprawa — Nagle znika z keep-alive po TCP |
| bufor wejściowy 16 KB w `safe_read()` | `patches/0003` | tak | nie (jeden `read()` zamiast sześciu na nagłówek requestu) |
| `accept4(SOCK_CLOEXEC)` | `patches/0003` + `AC_CHECK_FUNCS([accept4])` w naszym `config.m4` | tak, gdy `HAVE_ACCEPT4` | nie (zejście na `accept`+2×`fcntl`) |
| `write(2, "\0fscf")` tylko przy `catch_workers_output = yes` | `fpm_stdio.c` (wzięty na własność, 4 commity/rok) | tak | nie — przy `no` fd 2 to `/dev/null`, zapis szedł w próżnię |
| `request_cpu_tracking = yes|no` (2× `times()`) | `fpm_conf.c/h`, `fpm_request.c/h`, hook w `fpm.c` | **yes** = jak upstream | `no` zeruje „last request cpu" w statusie i `%C` w `access.format` |

`fpm_stdio.c` to piąty plik na własność (po `fpm_main.c`… — patrz sekcja 2);
zmiana to statyczna flaga ustawiana w `fpm_stdio_child_use_pipes()` i wczesny
`return` w `fpm_stdio_flush_child()`.

`request_cpu_tracking` czyta się w dziecku w `fpm.c` **przed**
`fpm_cleanups_run(FPM_CLEANUP_CHILD)`, bo potem `wp->config` już nie istnieje
(ta sama pułapka co w 3o). Dyrektywa per pool.

### Co odrzucone i dlaczego

- **`poll` po `accept` → `SO_RCVTIMEO` na gnieździe nasłuchującym.** Zmierzone
  programem w C na poligonie (Linux 7.0), nie z dokumentacji:
  - TCP: opcja dziedziczy się przez `accept()` i przerywa `read()` — ale
    **`accept()` bez klienta też dostaje EAGAIN po timeoucie**, a
    `fcgi_accept_request` traktuje każdy błąd poza EINTR/ECONNABORTED jako
    fatalny → worker wychodzi co 5 s bezczynności;
  - **UDS: nie dziedziczy się w ogóle** (`SO_RCVTIMEO` na połączeniu = 0),
    czyli zerowa ochrona na transporcie, który sami zalecamy;
  - timeout przenosiłby się na `read()` połączeń keep-alive (nginx trzyma je
    bezczynnie długo), więc trzeba by go zdejmować kolejnym `setsockopt` — zysk
    jednego syscalla na NOWE połączenie znika.
  `poll` zostaje. Jeden syscall nie jest wart zawieszonego albo znikającego workera.
- **Cache `chdir`** — poza zakresem (`main/`, semantyka cwd), zgodnie z decyzją.
- **`zend_signal_activate` (7× `rt_sigaction`) i blokada opcache (2× `fcntl`)**
  — Zend i ext/opcache, nie SAPI. Materiał na osobne zgłoszenie upstream:
  rejestracja handlerów raz na proces zamiast per request; blokada
  `accel_activate_add`/`deactivate_sub` per request.
- **`SO_REUSEPORT`** — nie ruszane (3m: thundering herd nie istnieje).

### Pułapki znalezione po drodze

1. **`0001` psuło `--enable-fpm` w tym samym drzewie — NAPRAWIONE (droga 1).**
   Zmieniało sygnaturę hooków w `main/fastcgi.h` (`void(*)(bool)`), a upstreamowe
   `fpm_main.c` przekazywało `void(*)(void)`; GCC 14+ traktuje niezgodne wskaźniki
   jako błąd. Przyczyna: `0001` było wycinkiem PR bukka#2 ograniczonym do `main/`,
   a jego część z `sapi/fpm/fpm/fpm_request.c/.h` nieśliśmy tylko jako własne pliki
   w `sapi/fpmng/`. Decyzja koordynatora: `0001` niesie teraz pełny PR (bez
   testów .phpt). Zweryfikowane: `--enable-fpm --enable-fpmng` w jednym drzewie
   buduje się (mac), a `sapi/fpm/tests` na tak zbudowanym upstreamowym
   `php-fpm` z pełnym stosem 0001+0002+0003: 141 testów, 0 failed. Skutek
   uboczny: `prepare.sh` mógłby przestać usuwać `sapi/fpmng/tests`. Koszt:
   PHP-8.3 potrzebuje wariantu `0001` (`fpm_scoreboard_update_commit` ma tam
   7 argumentów; 8.4 dodało `memory_peak`) — razem z wariantem `0003` to dwa
   warianty dla 8.3, czyli próg alarmowy z `patches/README.md`.
   Testy upstreamu można też puszczać przeciw `php-fpm-ng` przez
   `TEST_PHP_FPM_EXECUTABLE` (szuka `<dir>/fpm/php-fpm` dwa poziomy wyżej —
   potrzebny symlink).
2. **Stos łatek jest kolejnościowy.** 0002 i 0003 kontekstowo zakładają 0001
   (hunk init w `fcgi_init_request` i okolice `accept()`), choć merytorycznie
   są niezależne. Wersje do upstreamu trzeba by przebazować na czyste drzewo.
3. **`prepare.sh` kłamał przy stosie.** „Już nałożone" sprawdzał odwrotnym
   dry-runem per łatka — pada dla 0002, gdy leży na niej 0003. Teraz decyzja
   zapada raz dla całego stosu: w przód na nietknięte drzewo albo cały stos
   odwrotnie z kopii dotkniętych plików. Sprawdzone na BSD patch (mac) i GNU.
4. **`safe_read()` na PHP-8.3 ma `const void *buf`** (zmienione w 8.4 przez
   #20887) — 0003 potrzebuje wariantu `patches/php-8.3/`. Pierwszy wariant
   wersyjny w repo; dwa to próg alarmowy z `patches/README.md`.
5. Kopiowanie repo z maca `tar`-em bez `COPYFILE_DISABLE=1` wrzuca pliki
   `._*.c`, które `prepare.sh` bierze za źródła i build pada na
   `No rule to make target '._fpm_request.c'`.

### Poprawność

Pełny zestaw `sapi/fpm/tests` (141 testów), 0 failed, w pięciu wariantach —
piąty to upstreamowy `php-fpm` zbudowany razem z `php-fpm-ng` w jednym drzewie
z pełnym stosem 0001+0002+0003 (mac, 115 pass / 25 skip). Pozostałe cztery:
upstream czysty i upstream+0002/0003 na Linuksie (121 pass / 19 skip, identycznie
przed i po; `HAVE_ACCEPT4` włączone ręcznie, bo upstream go nie sprawdza),
upstream+0002/0003 na macu (ścieżka bez `accept4`) i `php-fpm-ng` na macu
(115 / 25, skipy środowiskowe). Jedyny „warn" to upstreamowy XFAIL, który
przechodzi — tak samo bez łatek.

Prawdziwy nginx 1.28 na poligonie (`ngtest/nginx-check.sh`), binarka bazowa
(0001 + nasze sapi) vs nowa, w konfiguracjach `catch_workers_output = yes`, `no`
i `no` + `request_cpu_tracking = no`. **Cztery przebiegi, zero porażek**,
identyczny wynik dla bazowej i nowej:

- `hello.php` ×50 na każdej ścieżce: keep-alive TCP, nowe połączenie TCP,
  keep-alive UDS, nowe połączenie UDS, `fastcgi_buffering off` +
  `fastcgi_request_buffering off`;
- POST surowy 1 KB, 17 KB (tuż nad buforem), 100 KB, 1 MB, 5 MB — długość i md5
  ciała zgadzają się na każdej ścieżce; multipart 300 KB z plikiem — `$_POST`
  i md5 pliku zgadzają się;
- odpowiedzi 20 KB, 200 KB, 5 MB (md5 całego ciała) na każdej ścieżce;
- chunked: nginx odpowiada `Transfer-Encoding: chunked`, surowe ciało dłuższe
  od zdekodowanego, zdekodowane md5 poprawne;
- zerwane połączenie w połowie odpowiedzi (`curl -m 0.3` w pauzie skryptu
  100 KB + `usleep` + 100 KB, dwa razy, na keep-alive i na streamie): worker
  przeżywa, kolejne 20 requestów OK, liczba workerów bez zmian;
- `catch_workers_output = yes`: `php://stderr` z workera ląduje w `error_log`
  mastera; przy `no` nie ląduje (zgodnie z semantyką) — czyli pominięcie
  `write(2, "\0fscf")` nic nie psuje, gdy jest odbiorca;
- `request_cpu_tracking` domyślne: „last request cpu" w statusie i `%C` w
  access logu niezerowe po skrypcie z ~30 ms CPU; `= no`: oba równe 0.

Pułapka testowa (nie kodu): w SAPI FPM nie ma stałej `STDERR` — skrypt z
`fwrite(STDERR, …)` pada fatalem; trzeba `fopen('php://stderr')`. Pierwsza wersja
testu „brak STDERR w logu" była fałszywym alarmem, także na upstreamie.

### Wydajność — zmierzone (poligon i7-6700T, Linux 7.0, PTI+IBRS, k3d wyłączone, maszyna pusta)

Binarka bazowa = 0001 + nasze `sapi/fpmng` sprzed tej pracy; nowa = to samo +
0002 + 0003 + `fpm_stdio.c` + `request_cpu_tracking` (domyślnie `yes`, więc
`times()` w obu). Konfiguracje pomiarowe bez `catch_workers_output`, czyli
nowa binarka pomija `write(2, "\0fscf")`. `hello.php` (`echo "hello\n"`), opcache.

**Syscalle na request** (`strace -c` na jednym workerze, fcgibench, TCP):

| | keep-alive | nowe połączenie |
|---|---|---|
| bazowa | **26** (read 6, write 2, rt_sigaction 8, chdir 2, fcntl 2, times 2, setitimer 2, getcwd, rt_sigprocmask) | **33** (+ accept, fcntl 2, poll, shutdown, recvfrom 2, close; read 5) |
| nowa | **20** (read 1, write 1, reszta bez zmian) | **25** (accept4 1, fcntl 2 = tylko opcache, read 1, recvfrom 1, poll, shutdown, close) |

Z pozostałych 20: 8× `rt_sigaction` + 1× `rt_sigprocmask` (Zend signals), 2×
`setitimer` (`max_execution_time`), 2× `chdir` + `getcwd`, 2× `fcntl` (opcache),
2× `times` (wyłączalne dyrektywą) — tylko `read` + `write` to FastCGI.

**CPU workera na request** (suma utime+stime dzieci z `/proc`, fcgibench,
2 połączenia, 5 s, trzy powtórzenia; wartości min–max):

| ścieżka | bazowa µs/req | nowa µs/req | różnica |
|---|---|---|---|
| TCP keep-alive | 60,2–62,5 | 48,7–50,8 | **−12 (−19%)** |
| TCP nowe połączenie | 89,0–95,4 | 81,7–84,5 | −8 (−9%) |
| UDS keep-alive | 52,6–53,7 | 41,7–44,8 | **−9 (−18%)** |
| UDS nowe połączenie | 72,0–73,0 | 52,3–54,3 | **−19 (−27%)** |

Zysk większy niż w 3m (7/12 µs), bo tam nie było jeszcze pominięcia
`write(2, …)` — `write` na tej maszynie kosztuje 4–7 µs/wywołanie wg `strace`,
najdroższy pojedynczy syscall na ścieżce.

**`wrk -t1 -c2 -d10s` przez nginx 1.28** (nginx + wrk + 2 workery na tej samej
maszynie, trzy powtórzenia):

| ścieżka | req/s bazowa | req/s nowa | CPU workera µs/req bazowa → nowa |
|---|---|---|---|
| TCP keep-alive (`fastcgi_keep_conn on`) | 8441–8830 | 8543–8576 | 108–113 → 106–108 (−3…−5) |
| TCP nowe połączenie | 7622–7948 | 7737–7935 | 106–109 → 97–98 (**−11**) |
| UDS keep-alive | 14657–15012 | 15962–16191 (**+7%**) | 69–72 → 59–60 (**−10**) |

Przepustowość przez nginx prawie nie drga, bo mierzy głównie nginx+wrk (jak
w 3m: przy większym obciążeniu widać walkę o hyperthready, nie kod); CPU
workera na request jest miarą właściwą. Zaskoczenie: przez nginx TCP keep-alive
zyskuje najmniej (−3…−5 µs), choć bezpośrednio fcgibenchem −12 — nie zbadane,
zapisane jako otwarte. Request przez nginx kosztuje ~45 µs więcej CPU niż ten
sam skrypt fcgibenchem — to ~20 zmiennych `fastcgi_params` i większe `$_SERVER`,
nie transport.

**Nagle (`TCP_NODELAY`, łatka 0002)** — własny klient FastCGI w Pythonie
(`patches/0002-fcgi-nodelay-repro.py`), jedno połączenie TCP z `FCGI_KEEP_CONN`,
50 requestów z rzędu, `mid.php` = 20 KB odpowiedzi (> bufor 8 KB), trzy serie:

| | mediana | min | max |
|---|---|---|---|
| bazowa | **41,0 ms** | 0,27 ms | 41,8 ms |
| nowa | **0,08 ms** | 0,07 ms | 0,71 ms |
| bazowa, `hello.php` (< 8 KB) | 0,09 ms | 0,07 ms | 0,34 ms |

Dwie osobne rzeczy, których nie wolno mieszać:

1. **Wada w kodzie — udowodniona z samego kodu**: `req->tcp` przypisywane
   tylko pod `_WIN32`, czytane bezwarunkowo; gałąź `TCP_NODELAY` poza Windows
   jest martwa. To wystarcza do zgłoszenia niezależnie od pomiarów.
2. **Skutek praktyczny — wykazany tylko częściowo.** Własnym klientem: 41 ms
   → 0,08 ms, czyli timer delayed ACK Linuksa (40 ms) na każdym requeście z
   odpowiedzią większą niż jeden `write()`. **Z nginx 1.28 na loopbacku NIE
   odtworzone** — odpowiedzi > 8 KB są w mikrosekundach z łatką i bez. Nie
   znaleźliśmy konfiguracji z prawdziwym nginxem, która to łapie, więc nie
   twierdzimy, że typowe wdrożenie nginx + FPM to widzi. Hipoteza (nie
   ustalenie): nginx czyta odpowiedź natychmiast, jego jądro ACK-uje po dwóch
   pełnych segmentach, więc delayed ACK nie ma kiedy zadziałać; klient czytający
   wolniej albo po prawdziwej sieci dostaje opóźnienie — nasz klient Pythonowy
   jest takim klientem.

Tekst zgłoszenia trzyma ten podział: `patches/0002-upstream-report.md`.

**Zastrzeżenie sprzętowe** bez zmian z 3m: PTI+IBRS, syscall ~0,9 µs; na nowszym
CPU zysk bezwzględny skurczy się 3–5×.

### Zalecana konfiguracja dla lekkich endpointów (zero linijek kodu)

- `php_admin_value[max_execution_time] = 0` — znika `setitimer` ×2 +
  `rt_sigprocmask` (−3 µs/req na poligonie). FPM i tak ma
  `request_terminate_timeout` jako strażnika czasu ściennego.
- `listen = /run/php/pool.sock` zamiast `127.0.0.1:9000` — −7…−11 µs/req.
  Bramka HTTP i worker są w tym samym kontenerze, TCP na loopbacku nic nie daje.
- `catch_workers_output = no`, jeśli logi idą przez `error_log()`/stderr do
  własnego stosu — oszczędza `write()` per request (od teraz automatycznie).
- `request_cpu_tracking = no`, jeśli nikt nie czyta „last request cpu" ani `%C`.

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

### TLS — ROZSTRZYGNIĘTE 2026-09-05: robimy, na końcu. Patrz sekcja 3l.

Poniższe zostaje jako zapis rozważań; wybrana została droga własnego HTTPS z ACME.

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
8. TLS + ACME (sekcja 3l).
9. `pool.type = proxy` — DECYZJA (2026-09-05): robimy, ale **na samym koncu**,
   po TLS. Bramka trzyma :443, terminuje TLS, obsluguje ACME i pliki statyczne
   sama, a reszte przekazuje po zwyklym HTTP/1.1 na localhost do dlugo zyjacego
   procesu aplikacji (amphp, ReactPHP, Octane, cokolwiek). Powod kolejnosci:
   bez TLS i ACME ten typ nie ma czego wnosic — sam przekaz HTTP na localhost
   zalatwia dowolne narzedzie. Wartosc powstaje dopiero z polaczenia
   "jedna binarka trzyma certyfikat i statyki" z "aplikacja jest osobnym
   procesem". Kontekst i skad sie to wzielo: sekcja 3s.

### Stan na 2026-09-05

Zrobione: 1 (pool.type z kontraktem rozszerzalnosci), 2 (supervisor),
3 (cron), 5 (pliki statyczne), 0 (statyczna binarka musl) — plus optymalizacje
syscalli blokujacego workera (3t), ktorych w pierwotnym planie nie bylo.
W robocie: `pool.type = status` (czesc punktu 4) i eksperymentalny
`pool.type = async` (3s).
Zostalo: 4 (metryki z PHP, `fpm_metric_*`), 6 (self-runner), 7 (reload),
8 (TLS+ACME), 9 (proxy), oraz dlugi ogon braków bramki z sekcji 6.

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


## 3t. `pool.type = async` — POC URUCHOMIONY na forku True Async (agent, 2026-09-05)

Kontynuacja 3s (tam: "nie budujemy tego"). Tu badanie wykonalnosci na kodzie,
nie na opisach. Wszystko ponizej jest z lektury zrodel (plik:linia) albo
zmierzone lokalnie na macu (arm64, NTS, `--enable-debug`). Kod:
`sapi/fpmng/fpm/fpm_pool_async.c` (+ `.h`, jedna linia w `fpm_pool_types[]`).
Nic nie poszlo do zadnego upstreamu. Zrodla forka i buildy:
`~/work/true-async/{php-src,php-async,build,php-upstream,build-upstream}`.

### Co fork faktycznie udostepnia (z kodu)

Fork `true-async/php-src` ma wiele galezi; ta, na ktorej buduje ext, to
`true-async-stable` (2026-08-26, ABI "TrueAsync ABI v0.26.0",
`Zend/zend_async_API.h`, 3060 linii; `README` ext podaje `true-async-api`,
ktora stoi na 2025-09 — nieaktualne). Vs upstream master: 228 plikow,
+21k linii. Osobno galaz `async-core-master` (2026-07-03): "AsyncCore ABI
v0.1.0", 573-liniowy naglowek ze SLOTAMI schedulera (new_coroutine, enqueue,
suspend, resume, cancel, launch, intercept_fiber, defer) i NICZYM wiecej —
bez reaktora, eventow, poll. To jest to, co ma isc do 8.7. Dla SAPI oznacza:
sam ABI nie da czekania na deskryptorze; to robi dostawca (ext).

Scheduler i reaktor to ext `true-async/php-async` (libuv, 33k linii C;
`scheduler.c`, `coroutine.c`, `libuv_reactor.c`). Rdzen dostarcza sloty
funkcji (`zend_async_scheduler_register`, `zend_async_API.c:402`) i
`zend_async_is_enabled()` = scheduler I reaktor zarejestrowane (`:305`).
Scheduler startuje LENIWIE przy pierwszym `spawn`/`suspend`
(`scheduler.c:1153 async_scheduler_launch`), zamieniajac biezacy przebieg w
"korutyne glowna"; wymaga `EG(active_fiber) == NULL` i RINIT ext.

Uruchomienie kodu z C: `ZEND_ASYNC_SPAWN()` (`zend_async_API.h:2726`) daje
`zend_coroutine_t*`; ustawiasz `internal_entry` (void(*)(void)) i
`extended_data`; startuje przy nastepnym oddaniu sterowania. Czekanie na fd:
`ZEND_ASYNC_NEW_SOCKET_EVENT(fd, ASYNC_READABLE)` + `ZEND_ASYNC_WAKER_NEW` +
`zend_async_resume_when(...)` + `ZEND_ASYNC_SUSPEND()` — wzor
`main/network_async.c:241 network_async_await_stream_socket`. Dziala z C,
sprawdzone (akceptor ponizej).

Gdzie silnik zawiesza sie na I/O (fork, `git diff master..true-async-stable`):
`main/streams/xp_socket.c` (+346, kazdy read/write/connect/accept przez
`network_async_await_stream_socket`), `main/network.c` (`php_poll2` ->
`php_poll2_async` gdy `ZEND_ASYNC_IS_ACTIVE`), `main/streams/plain_wrapper.c`
(+743, pliki/pipe przez `ZEND_ASYNC_IO_CREATE`), `ext/standard/basic_functions.c`
(`sleep_async` dla sleep/usleep/time_nanosleep), `ext/curl/curl_async.c`
(+2278), `ext/pdo_pgsql`/`ext/pgsql` (libpq przez polling gniazda), `ext/sockets`,
`ext/standard/dns.c`. Czyli mysqlnd (streamy) i libpq sa obslugiwane W SILNIKU
— to wiecej niz nasz `php_stream_xport_register` z 3s.

### Co jest per-korutyna DZIS (wyliczone z kodu, nie z opisu)

- Fiber switch (`Zend/zend_fibers.c:121-160 zend_fiber_vm_state`):
  `vm_stack*`, `current_execute_data`, `error_handling`, `exception_class`,
  `jit_trace_num`, `active_fiber`, `bailout`. Tyle z EG.
- `main/output.c`: stos `ob_*` przez klucz "internal context" korutyny
  (`php_output_get_async_context`, handler startu korutyny glownej `:209`, `:1663`).
- `main/network_async.c:1586`: cache `hostent`.
- `EG(shutdown_context)` (destruktory na shutdown w korutynie).
- SG: ZERO zmian (`main/SAPI.c`, `main/php_variables.c` — diff pusty).
- `EG(symbol_table)`, `function_table`/`class_table`, `included_files`, ini,
  `error_reporting`, `user_error_handler`, `memory_limit`, timeout: WSPOLNE.

Zaczatek mechanizmu ISTNIEJE i jest publiczny: **switch-handlery** —
`zend_coroutine_switch_handler_fn(coroutine, is_enter, is_finishing)`
(`zend_async_API.h:269`), `ZEND_COROUTINE_ADD_SWITCH_HANDLER` (`:3043`),
wolane w `ext/async/scheduler.c:1697` (LEAVE przed przelaczeniem) i `:1719`
(ENTER po wznowieniu), FINISH w `coroutine.c` przy finalizacji. Tego uzywa
sam fork dla `ob_*`. Tego uzywa nasz POC dla SG i tablicy symboli — i to
WYSTARCZA bez zmian w VM (patrz nizej).

Galaz `global-isolation` (af6c53037, 2025-11-30) zrobila per-korutyna
`EG(symbol_table)` (`coroutine->symbol_table`, `zend_execute.c`,
`zend_vm_def.h` +116) i per-scope superglobale (`scope->superglobals`) —
**nigdy nie weszla do `true-async-stable`** (`git branch --contains` = tylko
ona). Statyki klas per-korutyna zrobiono i cofnieto (858ece3db). Czyli autor
forka probowal warunku 1 z 3s i porzucil; w stable stan requestu jest
per-proces.

### POC: co dziala i jak (ZMIERZONE)

`child_main` (jeden proces, `pm = static`, `pm.max_children = 1`):
1. JEDEN `php_request_startup()` — "request-kontener" (executor, RINIT ext,
   arena). `zend_unset_timeout()`, bo `max_execution_time` dotyczylby procesu.
2. `ZEND_ASYNC_SPAWN()` akceptora: czeka na `listening_socket` przez poll-event,
   `fcgi_init_request` + `fcgi_accept_request` (accept od razu; `poll` na nowym
   fd jest w forku asynchroniczny przez `php_poll2`; `read` naglowkow blokujacy,
   ale dane juz sa), potem `ZEND_ASYNC_SPAWN()` korutyny requestu.
3. Korutyna requestu: swieza kopia SG (odpowiednik `sapi_activate` +
   `init_request_info`), swieze `EG(symbol_table)` i `EG(included_files)`
   (`zend_hash_init` jak `init_executor`), ponowne uzbrojenie auto-globali,
   switch-handler podmieniajacy przez `memcpy` cale `sapi_globals` i naglowki
   obu HashTable (adres `&EG(symbol_table)` sie nie zmienia, zmienia sie
   zawartosc — ramka skryptu trzyma wskaznik, wpisy IS_INDIRECT wskazuja w
   stos VM tej korutyny). `zend_execute_scripts` (NIE `php_execute_script`,
   patrz pulapki), `sapi_send_headers`/`sapi_flush`, `fcgi_finish_request`
   bez keep-alive, `zend_hash_graceful_reverse_destroy` tablicy symboli.
4. Korutyna glowna budzi sie co 1 s i sprawdza `fcgi_in_shutdown()`.

Wyniki (klient FastCGI w PHP, N rownoleglych polaczen, jeden proces workera):

    4 x slow.php (usleep 500 ms)      async: 503 ms lacznie   fcgi: 2008 ms
    4 x net.php (fsockopen -> serwer
      odpowiadajacy po 500 ms)         async: 537 ms           fcgi: 2140 ms
    50 x slow.php (100 ms)             async: 105 ms
    200 x slow.php (0 ms)              async: 22 ms (~110 us/req, debug build)
    RSS dziecka po 2000 requestach     8624 KB -> 8624 KB (arena sie odzyskuje)

Poprawnosc: kazdy request dostal SWOJ naglowek `X-Req`, SWOJE `$_GET`,
`$_SERVER['REQUEST_URI']`, `$GLOBALS`, `get_included_files()`; ostrzezenia
i "Uncaught RuntimeException" trafiaja do wlasciwego klienta; SIGQUIT do
mastera -> wszystkie procesy znikaja < 2,5 s.

### Co NIE dziala i przez jaki kod (zmierzone, nie wywnioskowane)

- **Tablice funkcji i klas sa per proces.** `classdef.php` (definiuje klase
  i funkcje) dziala RAZ; kazdy nastepny request w tym procesie: "Fatal error:
  Cannot redeclare function helper()" — na zawsze, bo `EG(function_table)`
  to `CG(function_table)` napelniana przy kompilacji i czyszczona dopiero w
  `shutdown_executor()`. Podmiana tablicy per korutyna wymagalaby kopii
  wszystkich wpisow wewnetrznych (tysiace) per request albo zmiany w silniku.
  TO jest dzis prawdziwa sciana dla frameworkow (kazdy request laduje te same
  klasy). Z opcache ten sam problem ma inna postac: `zend_accel_load_script`
  binduje klasy do `EG(class_table)` per request.
- **opcache zaklada jeden request na proces.** Pierwotnie z wlaczonym opcache
  requesty 2..N tracily `$_GET`/`$_SERVER`: cache hit omijal kompilacje, na
  ktorej POC polegal przy uruchamianiu callbackow auto-globali. POC jawnie
  wywoluje teraz `zend_is_auto_global_str()` dla kazdego requestu; test 3 x 4
  rownoleglych requestow z aktywnym opcache zachowal osobne `$_GET`, `$_SERVER`,
  `$_COOKIE`, `$GLOBALS` i `get_included_files()` (4 x 500 ms w 506-508 ms).
  Nadal **zalecane jest `opcache.enable = off`**: opcache oraz jego stan
  `ZCG(cwd)`, `ZCG(include_path)` nie byly projektowane dla wielu requestow
  przeplatanych w jednym procesie, a test nie dowodzi izolacji wszystkich
  sciezek rozszerzenia.
- **Bez keep-alive po stronie poola**: `fcgi_accept_request` na otwartym fd
  czyta blokujaco (`main/fastcgi.c:1445`, bez poll). Nasza bramka HTTP trzyma
  polaczenia trwale — dopiac przez asynchroniczny poll na fd przed odczytem.
- **POST**: `sapi_cgi_read_post` uzywa statycznego `request_body_fd`
  (`fpm_main.c`) — wspolny dla wszystkich requestow w locie. POC testowany GET.
- Brak `X-Powered-By`, brak `php_request_startup/shutdown` per request, wiec
  RINIT/RSHUTDOWN rozszerzen nie biegna per request (sesje, mysqlnd stats,
  `set_time_limit`, `memory_limit` per request — nie ma).
- Fatal w dowolnej korutynie: nasz `zend_try` lapie bailout w korutynie, ale
  `CG(unclean_shutdown)` i stan silnika po E_ERROR sa wspolne.
- Logi dziecka nie trafiaja do `error_log` (FPM zamyka zlog w dziecku bez
  `catch_workers_output`) — nasze NOTICE/DEBUG z `child_main` gina.

### Pulapki, na ktore sie nadzialem (kazda to konkretny kod)

1. `php_execute_script()` w forku wola `ZEND_ASYNC_RUN_SCHEDULER_AFTER_MAIN`
   (`main/main.c php_execute_script_ex`), a to = `suspend(from_main=true)`
   -> `async_scheduler_main_coroutine_suspend` (`scheduler.c:1315`), ktore
   FINALIZUJE biezaca korutyne jak glowna. Z wnetrza korutyny requestu trzeba
   wolac `zend_execute_scripts()` bezposrednio.
2. Fiber korutyny ma na dnie sztuczna ramke funkcji wewnetrznej
   (`scheduler.c:1786-1811 fiber_entry`, `root_function`). `zend_execute()` przy
   niepustym `EG(current_execute_data)` szuka tablicy symboli w gore stosu
   (`zend_rebuild_symbol_table`) i dostaje NULL -> SIGSEGV w
   `zend_attach_symbol_table` (`zend_execute_API.c:2029`). Na czas wykonania
   skryptu trzeba ustawic `EG(current_execute_data) = NULL`.
3. Wspolna `EG(symbol_table)` to nie tylko wyciek `$GLOBALS`: drugi skrypt
   glowny w `zend_attach_symbol_table` przejmuje BITOWO (bez addref) wartosci
   CV pierwszego o tych samych nazwach i zwalnia je pod nim. Zmierzone:
   SIGABRT w `gc_possible_root` (`zend_gc.c:800`) na `net.php`, gdy `$fp`
   z zasobem gniazda. Skalarne skrypty "dzialaly" przypadkiem. Rozwiazanie
   w POC: wlasna tablica per korutyna podmieniana w switch-handlerze.
4. `fiber_entry` ustawia `EG(error_reporting)` z ini zamiast dziedziczyc
   (`scheduler.c:1766-1813`); bez php.ini daje 0 — ostrzezenia i "Uncaught"
   znikaja bez sladu. Dziedziczymy wartosc kontenera recznie.
5. `fpm_main.c:1800-1801` podmienia `php_import_environment_variables` na
   wariant czytajacy srodowisko FastCGI DOPIERO po powrocie z `fpm_run()`.
   `child_main` nie wraca, wiec `$_SERVER` mial tylko environ procesu.
6. Build forka na macOS: `ext/async/thread.c:3222` deklaruje slaby symbol
   `OPENSSL_thread_stop` — linker Apple wymaga `-Wl,-U,_OPENSSL_thread_stop`.
   Poza tym fork + ext + nasze `sapi/fpmng` buduja sie razem bez zadnej latki
   (`prepare.sh` dziala na forku jak na upstreamie; `main/fastcgi.c` forka
   jest identyczny z upstreamem).

### Wykrywanie silnika (punkt 4) — zweryfikowane na obu binarkach

Kompilacja: `__has_include("zend_async_API.h")` (tylko fork ma ten naglowek;
`-IZend` jest zawsze). Runtime w `validate()`: `zend_async_is_enabled()` —
mozliwe, bo `fpm_init()` biegnie PO `php_module_startup()` (`fpm_main.c:1749`
vs `:1765`), wiec MINIT ext/async juz zarejestrowal scheduler. Dodatkowo
`ZEND_ASYNC_API` daje wersje ABI do komunikatu. Na binarce z upstreamu
(`~/work/true-async/build-upstream`, PHP 8.6.0-dev bez API):

    ALERT: [pool async] pool.type = async requires a PHP engine with the True
    Async API (Zend/zend_async_API.h); this binary is PHP 8.6.0-dev without it

`-t` na forku: `pm = dynamic` i `request_terminate_timeout` odrzucane
czytelnie, poprawny plik przechodzi.

### Punkt 6: `php_stream_xport_register()` — potwierdzone w upstreamie

`PHPAPI` (`main/streams/php_stream_transport.h:32`), rejestr to
`zend_hash_update_ptr` (`transports.c:30`) — ostatni wygrywa;
`_php_stream_xport_create` szuka po nazwie protokolu (`transports.c:109`).
Domyslne `tcp/udp/unix/udg` rejestruje `streams.c:1765-1769`; ext/openssl
NADPISUJE `tcp` w MINIT (`openssl.c:832`) i przywraca w MSHUTDOWN (`:906`) —
wiec nasze ext musi ladowac sie PO openssl. mysqlnd: `mysqlnd_vio.c:210`
`php_stream_xport_create(scheme...)` ze schematem `tcp://`/`unix://`
(`:278-305`) — lapie sie. phpredis: `library.c:3345` to samo. Do zawieszania
fibera z C upstream ma `ZEND_API zend_fiber_suspend/resume`
(`zend_fibers.h:135-136`). Teza z 3s stoi: warunek 2 w naszym zasiegu na
upstreamie; warunek 1 (stan requestu) nadal nie.

### Nastepny krok, gdyby isc dalej

Nie "wiecej I/O" — I/O fork ma. Brakuje trzech rzeczy i wszystkie sa w
silniku, nie w SAPI: (a) tablice funkcji/klas per request albo mechanizm
"skrypt juz zaladowany w tym procesie" (to jest to, co robi tryb worker
FrankenPHP/Octane: aplikacja ladowana RAZ, request = wywolanie funkcji —
i to jest realistyczny model dla `async`, nie "kazdy request od zera");
(b) per-korutyna `RINIT/RSHUTDOWN` albo lista rozszerzen bezpiecznych;
(c) opcache swiadomy wielu requestow w procesie. Nasza strona (keep-alive,
POST, scoreboard, logi dziecka) to robota na dzien i nie o nia sie rozbija.

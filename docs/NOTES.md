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

### `fcgi-ng` — eksperymentalny, stary `fcgi` bez zmian

Decyzja Piotra. Zastrzeżenie do zapamiętania: **BC dotyczy konfiguracji
i protokołu, nie wewnętrznego zachowania.** Istniejący `fpm.conf` ma działać
i nginx ma się dogadać — ale poprawki błędów i usprawnienia wewnętrzne mieszczą
się w `fcgi`. GH-18956 to naprawa, nie zmiana kontraktu.

`fcgi-ng` ma sens jako miejsce na zmiany łamiące obserwowalne zachowanie i na
eksperymenty (io_uring, `SO_REUSEPORT` per worker, batchowanie syscalli).
Uwaga strukturalna do zweryfikowania: ścieżka FastCGI ma inną budowę niż bramka.
Bramka to nasz proces z pętlą libevent; worker FastCGI to blokująca pętla
w `fpm_main.c`, a pętla zdarzeń FPM żyje w MASTERZE. Więc to, co zadziałało
w bramce, tu niekoniecznie się przekłada. **Bada to osobny agent — wynik
wkleić tutaj.**

### `http-direct` (in-process) — nie teraz, ale nie zamykamy drogi

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
`pool.type` = `fcgi`, zero BC), `http`, `fcgi-ng`, `supervisor`, `cron`,
a w przyszłości `http-direct`. Jeśli któryś z nich nie wchodzi gładko —
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

### Konsekwencja 1: domyka sprawę `http-direct`

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

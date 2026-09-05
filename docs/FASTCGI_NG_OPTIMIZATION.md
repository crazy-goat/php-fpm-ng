# Plan optymalizacji `fastcgi-ng`

## Cel

`pool.type = fastcgi-ng` ma być zoptymalizowanym frontendem FastCGI, podczas gdy:

```ini
pool.type = fastcgi
```

ma zachować możliwie pełną zgodność z klasycznym upstreamowym PHP-FPM.

Executor jest osobnym wymiarem:

```ini
pool.executor = classic | fiber | async
```

Optymalizacje opisane tutaj dotyczą przede wszystkim:

```ini
pool.type = fastcgi-ng
pool.executor = classic
```

Executory `fiber` i `async` pozostają eksperymentalne i nie są przeznaczone do produkcji.

## Stan początkowy

Optymalizacje transportu znajdują się w `main/fastcgi.c`, ale są chronione procesowym przełącznikiem ustawianym przez worker po rozpoznaniu poola. Domyślnie przełącznik jest wyłączony, więc `pool.type = fastcgi` używa upstreamowej ścieżki odczytu i `accept() + fcntl()`. `pool.type = fastcgi-ng` oraz wewnętrzny transport frontendu `http` włączają buforowane odczyty i `accept4()` po forku dziecka.

Rozdzielenie nie duplikuje całego `main/fastcgi.c`: mały interfejs `fcgi_set_optimized_transport()` zachowuje jedną implementację protokołu i wybiera wyłącznie zoptymalizowane operacje transportowe. Ponieważ każdy worker jest osobnym procesem przypisanym do jednego poola, ustawienie nie przecieka między poolami.

Obecne zmiany względem czystego upstreamu:

1. bufor wejściowy FastCGI 16 KB: typowy nagłówek requestu jest pobierany jednym `read()` zamiast około sześciu;
2. `accept4(..., SOCK_CLOEXEC)` zamiast `accept()` i dwóch `fcntl()`, jeśli platforma udostępnia `accept4`;
3. pominięcie technicznego zapisu do fd 2, gdy `catch_workers_output = no`;
4. opcjonalne `request_cpu_tracking = no`, usuwające dwa `times()` na request;
5. naprawa ustawiania `TCP_NODELAY` dla połączeń FastCGI keep-alive po TCP;
6. poprawka GH-18956 dla liczników idle/active na połączeniach keep-alive.

Naprawy `TCP_NODELAY` i GH-18956 są poprawkami błędów. Mogą pozostać wspólne dla `fastcgi` i `fastcgi-ng`. Optymalizacje zmieniające implementację transportu powinny być aktywowane tylko przez `fastcgi-ng`.

## Zmierzony punkt odniesienia

Poligon: `192.168.8.103`, użytkownik `piotr`, i7-6700T, 4 rdzenie fizyczne / 8 wątków logicznych, Linux 7.0.

Kod bazowy PHP: upstream master, commit `5be4de10`. Porównywane binarki muszą być release buildami z tego samego checkoutu i wykonywać identyczny kod PHP.

### Przepustowość przy nasyceniu

`hello.php`, nginx, 4 workery, `wrk -t2 -c32`, siedem serii po 10 sekund:

| wariant | mediana |
|---|---:|
| upstream `php-fpm` | 11 583,73 req/s |
| `php-fpm-ng`, `fastcgi-ng + classic` | 11 631,00 req/s |

Różnica `+0,41%` jest na poziomie szumu. Ten wariant testu nasyca maszynę i mierzy również walkę nginx, `wrk` i workerów o rdzenie oraz hyperthready.

### CPU workera bez nasycania maszyny

`hello.php`, nginx, 4 workery, `wrk -t1 -c2`, pięć naprzemiennych serii po 10 sekund:

| wariant | mediana CPU workera/request |
|---|---:|
| upstream `php-fpm` | 114,53 us |
| `php-fpm-ng`, `fastcgi-ng + classic` | 105,01 us |

Aktualny zysk wynosi około `9,5 us/request`, czyli `8,3%` CPU samego workera. Mediana przepustowości wyniosła około 8812 req/s dla upstreamu i 8910 req/s dla fpm-ng (`+1,1%`).

Wyniki na poligonie:

```text
/home/piotr/opencode-fiber-poligon/bench-fastcgi/results.txt
/home/piotr/opencode-fiber-poligon/bench-fastcgi/cpu-results.txt
```

## Plan prac

### 1. Faktycznie rozdzielić `fastcgi` i `fastcgi-ng` — wykonane, do pełnej regresji wersji PHP

Wprowadzono wybór implementacji transportu na podstawie efektywnego `pool.type`.

Dla `fastcgi`:

- pozostawić upstreamową ścieżkę FastCGI;
- nie obsługiwać `pool.executor`;
- nie włączać optymalizacji specyficznych dla `fastcgi-ng`;
- zachować domyślne zachowanie istniejących konfiguracji bez `pool.type`.

Dla `fastcgi-ng`:

- aktywować buforowane odczyty FastCGI;
- aktywować `accept4(SOCK_CLOEXEC)`;
- aktywować przyszłe optymalizacje transportu;
- domyślnie używać `pool.executor = classic`.

Preferowane rozwiązanie nie powinno duplikować całego `main/fastcgi.c`. Należy znaleźć najmniejszy interfejs pozwalający ustawić wariant zachowania per worker przed rozpoczęciem pętli accept. Flaga nie może zmieniać zachowania innych pooli w tym samym procesie mastera.

Po rozdzieleniu należy ponownie wykonać benchmark trzech wariantów:

1. czysty upstream `php-fpm`;
2. `php-fpm-ng` z `pool.type = fastcgi`;
3. `php-fpm-ng` z `pool.type = fastcgi-ng` i `pool.executor = classic`.

Wariant 2 powinien być wydajnościowo i behawioralnie równoważny upstreamowi. Wariant 3 powinien zachować obecny spadek CPU workera.

### 2. Ustalić zalecaną konfigurację `fastcgi-ng`

Dla lekkich endpointów mierzyć i dokumentować konfigurację:

```ini
pool.type = fastcgi-ng
pool.executor = classic
listen = /run/php/pool.sock
catch_workers_output = no
request_cpu_tracking = no
php_admin_value[max_execution_time] = 0
```

Każdą opcję mierzyć także osobno:

- UDS zamiast TCP loopback: dotychczas około `7-11 us/request` mniej;
- `request_cpu_tracking = no`: usuwa dwa `times()`;
- `max_execution_time = 0`: usuwa dwa `setitimer()` i część obsługi masek sygnałów;
- `catch_workers_output = no`: pozwala pominąć techniczny `write()`.

Nie zmieniać wartości domyślnych klasycznego `fastcgi` w celu uzyskania lepszego wyniku benchmarku.

### 3. Ponownie sprofilować gorącą ścieżkę

Po rozdzieleniu frontendów zebrać dla obu wariantów:

- `strace -c` na request dla TCP keep-alive;
- `strace -c` dla nowych połączeń TCP;
- te same dwa pomiary dla UDS;
- CPU dzieci FPM z `/proc`, nie całego hosta;
- `perf record` i `perf report` dla lekkiego `hello.php`;
- przepustowość i latency przez prawdziwy nginx.

Poprzedni profil po optymalizacjach zawierał około 20 syscalli na request keep-alive. Największe pozostałe grupy to:

- 8 x `rt_sigaction` i 1 x `rt_sigprocmask`;
- 2 x `setitimer`;
- 2 x `chdir` oraz `getcwd`;
- 2 x `fcntl` pochodzące z OPcache;
- 2 x `times`;
- pojedyncze `read` i `write` FastCGI.

Dalsze prace wybierać dopiero na podstawie nowego profilu.

### 4. Rozważyć rejestrację sygnałów raz na proces

Największym potencjalnym kosztem pozostaje ponowne wykonywanie `rt_sigaction` dla każdego requestu.

Należy sprawdzić:

- które handlery są rzeczywiście niezmienne między requestami;
- czy można je zainstalować raz podczas inicjalizacji workera;
- które elementy muszą być resetowane per request;
- zachowanie po fatal error, timeout, przerwaniu requestu i reloadzie;
- zgodność z rozszerzeniami instalującymi własne handlery.

To jest zmiana w Zend, nie lokalna optymalizacja FPM. Nie implementować jej bez osobnego reproduktora, testów regresji i pomiaru zysku. Preferowana droga to zmiana nadająca się do upstreamu, a nie trwały fork Zend.

### 5. Zbadać blokady OPcache per request

Dwa `fcntl()` pozostające na gorącej ścieżce pochodzą z aktywacji i dezaktywacji OPcache.

Należy ustalić:

- czego dokładnie chronią te blokady;
- czy w klasycznym modelu jednego requestu naraz na worker można ograniczyć ich częstotliwość;
- czy zmiana zachowuje poprawność przy restartach, invalidacji i współdzieleniu pamięci między workerami;
- czy rozwiązanie może zostać wysłane do upstreamowego OPcache.

Nie omijać blokad tylko na podstawie benchmarku Hello World.

### 6. Opcjonalny tryb bez zmiany CWD

`getcwd()` i dwa `chdir()` można potencjalnie usunąć dla aplikacji używających wyłącznie ścieżek absolutnych.

Jeżeli profil potwierdzi istotny koszt, rozważyć jawną opcję tylko dla `fastcgi-ng`, domyślnie wyłączoną. Opcja musi jasno dokumentować zmianę semantyki względnych ścieżek, `include`, `require` i operacji plikowych.

Nie stosować cache CWD jako niewidocznej optymalizacji, ponieważ może zmienić zachowanie aplikacji.

### 7. Sprawdzić rozmiar bufora wejściowego — wykonane dla 8/16/32 KB

Pomiar CPU/request nie wykazał istotnej przewagi żadnego wariantu:

| bufor | CPU/request |
|---|---:|
| 8 KB | 86,591 us |
| 16 KB | 87,321 us |
| 32 KB | 86,564 us |

Różnice pozostały poniżej 1%, dlatego bufor wejściowy pozostaje bez zmian: 16 KB.

## Zaakceptowana optymalizacja dużych odpowiedzi

Dla dużego rekordu FastCGI zoptymalizowany transport na Unixie wysyła nagłówek i body jednym `writev()`. Klasyczny `fastcgi` oraz Windows zachowują dotychczasową ścieżkę `write()`.

Dla odpowiedzi 262 144 B liczba operacji transportowych spadła z 11 do 6. Test `strace` na PHP 8.5 potwierdził 11 zapisów i brak `writev()` dla `fastcgi` oraz 5 `writev()` i końcowy zapis rekordu dla `fastcgi-ng`.

Pięć naprzemiennych serii na PHP 8.5, `wrk -t1 -c2 -d10s`:

| frontend | metryka | baseline | `writev` | zmiana |
|---|---|---:|---:|---:|
| `fastcgi-ng` | CPU workera/request | 195,433 us | 178,824 us | **-8,5%** |
| `fastcgi-ng` | req/s | 2018,63 | 2037,15 | **+0,9%** |
| `http`, `Connection: close` | łączny CPU gatewaya i workera/request | 525,209 us | 490,612 us | **-6,6%** |
| `http`, `Connection: close` | req/s | 2682,67 | 2705,14 | **+0,8%** |

Spadek CPU wystąpił we wszystkich pięciu parach obu benchmarków. Pierwszego pomiaru HTTP z keep-alive, około 50 req/s, nie użyto do oceny `writev()`, ponieważ brak `TCP_NODELAY` na listenerze HTTP uruchamiał Nagle/delayed ACK. Ustawienie tej opcji raz na listenerze (dziedziczonej przez zaakceptowane sockety) podniosło medianę dużej odpowiedzi keep-alive z 49,74 do 2686,13 req/s i obniżyło łączny CPU gatewaya i workera/request z 643,939 do 494,200 us.

Regresja PHP 8.5 przeszła dla małej i dużej odpowiedzi, binarnego POST 65 792 B z kontrolą SHA-256, keep-alive/close oraz zerwanego odbiorcy. Odpowiedzi od 1 B do 1 MiB, w tym granice rekordów FastCGI, zostały wcześniej porównane bajt w bajt na masterze.

Batching małej odpowiedzi odrzucono: kompletna mała odpowiedź FastCGI już trafia do jednego `write()`, a obserwowany drugi zapis dotyczy innego deskryptora. Nie daje to bezpiecznej oszczędności transportowej.

## Metodologia benchmarków

Każde porównanie musi spełniać wszystkie warunki:

1. ten sam commit upstreamowego PHP;
2. release build, bez `--enable-debug`;
3. ten sam kompilator i flagi kompilacji;
4. identyczny kod PHP i konfiguracja OPcache;
5. identyczna liczba workerów;
6. ten sam frontend nginx i ustawienia FastCGI;
7. naprzemienna kolejność serii;
8. warm-up przed pomiarem;
9. minimum pięć serii, raportowanie mediany i rozrzutu;
10. osobny pomiar CPU workerów oraz całkowitego throughputu;
11. `wrk -t1 -c2` jako podstawowy pomiar kosztu CPU bez nasycania poligonu;
12. testy o wysokiej współbieżności raportowane osobno jako test przepustowości całego systemu.

Dla każdego wyniku zapisywać:

- commit PHP i commit php-fpm-ng;
- pełne polecenia configure/build;
- konfiguracje FPM i nginx;
- wersje nginx i `wrk`;
- surowe wyniki;
- liczbę rdzeni i stan innych obciążeń hosta.

## Kryteria poprawności

Każda optymalizacja musi przejść:

- pełne testy `sapi/fpm/tests`;
- GET i POST przez nginx;
- małe i duże request body;
- małe i wielomegabajtowe odpowiedzi;
- keep-alive i nowe połączenia;
- TCP i UDS;
- `fastcgi_request_buffering` oraz `fastcgi_buffering` włączone i wyłączone;
- zerwanie połączenia podczas requestu i odpowiedzi;
- restart/reload mastera;
- `pm.max_requests`;
- status, slowlog, access log i timeouty;
- kontrolę braku wycieków deskryptorów i pamięci.

Klasyczny `pool.type = fastcgi` musi dodatkowo przechodzić test porównawczy z czystym upstreamem.

## Kryteria przyjęcia optymalizacji

Zmianę przyjmujemy tylko wtedy, gdy:

- nie zmienia protokołu FastCGI ani zachowania aplikacji bez jawnej opcji;
- ma test regresji;
- daje powtarzalny zysk CPU workera lub naprawia udowodniony problem;
- zysk nie wynika z innej konfiguracji lub nasycenia hosta;
- koszt utrzymania i odchylenia od upstreamu jest proporcjonalny do efektu;
- istnieje plan wysłania do upstreamu dla zmian poza `sapi/fpmng`.

Nie przyjmujemy mikrooptymalizacji wyłącznie na podstawie wzrostu req/s w nasyconym teście.

## Świadomie odrzucone kierunki

Na obecnym etapie nie wracamy do:

- `SO_RCVTIMEO` zamiast `poll` po `accept`: psuje bezczynne TCP i nie dziedziczy się poprawnie dla UDS;
- `SO_REUSEPORT` jako lekarstwo na thundering herd: blokujący accept nie wykazał takiego problemu;
- `io_uring`: zbyt duża złożoność, osobny backend i problemy z domyślnym Docker seccomp;
- niewidocznego cache `chdir`: ryzyko zmiany semantyki aplikacji;
- optymalizowania wyłącznie wyniku Hello World kosztem BC;
- dalszego rozwijania forka Zend bez drogi do upstreamu.

## Oczekiwany rezultat

Najbliższy konkretny rezultat to:

1. `fastcgi` rzeczywiście używa ścieżki zgodnej z upstreamem;
2. `fastcgi-ng + classic` jawnie włącza zoptymalizowany transport;
3. zachowany zostaje obecny zysk około `9-10 us` CPU workera na lekki request;
4. zalecana konfiguracja UDS bez zbędnej telemetrii zostaje zmierzona osobno;
5. dalsze zmiany są wybierane na podstawie `perf` i `strace`, a nie przypuszczeń.

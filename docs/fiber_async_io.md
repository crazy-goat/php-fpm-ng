# Fiber: co jest nieblokujace, a co blokuje caly proces

Stan na 2026-09-06, `pool.executor = fiber`. Dotyczy `pool.type = fastcgi-ng`
i `pool.type = http`.

Rzecz do zapamietania: w tym executorze jeden proces obsluguje N requestow
naraz, wiec **jedno blokujace wywolanie zatrzymuje wszystkie requesty w locie**,
a nie tylko swoj wlasny. W klasycznym FPM spowolniloby jeden request.

## Przechwycone dzis

`fpm_pool_fiber_xport.c` podmienia fabryki transportow `tcp` i `unix` przez
`php_stream_xport_register()` i zawiesza fiber na read/write/connect zamiast
blokowac. Z tego korzysta wszystko, co idzie przez warstwe strumieni PHP:

- `fsockopen()`, `stream_socket_client()`;
- **mysqlnd**, czyli `PDO` (mysql) i `mysqli` w domyslnej kompilacji — a przez
  nie Doctrine i Eloquent, bez zmiany linijki kodu aplikacji;
- **phpredis** i Predis;
- wrapper `http://` (`file_get_contents`, `fopen`).

Zmierzone: 4 rownolegle requesty z `fsockopen()` do serwera odpowiadajacego po
500 ms konczyly sie w 508 ms w jednym procesie. Kontrolnie 4 x `usleep(500 ms)`
trwaly 2009 ms.

## Blokuje caly proces

- `sleep()`, `usleep()`, `time_nanosleep()`;
- **curl** — wlasne gniazda, poza warstwa strumieni;
- **libpq**, czyli `pdo_pgsql` i `pgsql`;
- **TLS**: `ssl://`, `tls://`, `https://` — `ext/openssl` ma wlasne pollowanie;
- DNS (`getaddrinfo` w connect) — patrz nizej, w toku;
- zwykle pliki, `ext/sockets` (`socket_*`), libmemcached.

## curl: odkladamy, obchodzimy przez handler strumieniowy

Przechwycenie curla wymagaloby podmiany handlera `curl_exec` i przepisania
transferu na `curl_multi` z `CURLMOPT_SOCKETFUNCTION`/`TIMERFUNCTION` wpietym
w nasza petle libevent. W forku True Async ten kawalek to `ext/curl/curl_async.c`,
+2278 linii. **Swiadomie odkladamy** — to osobny projekt i kod, ktory psuje sie
przy kazdej zmianie w ext/curl.

Obejscie po stronie aplikacji: uzywac klienta HTTP w trybie **strumieniowym**
zamiast curlowego, bo strumienie juz sa przechwycone.

- **Guzzle** domyslnie wybiera handler curlowy, gdy `ext-curl` jest dostepny.
  Wymuszenie handlera strumieniowego (`GuzzleHttp\Handler\StreamHandler`
  podany do `HandlerStack::create()`) przenosi caly ruch na wrappery PHP.
- **Symfony HttpClient** ma ten sam podzial: `NativeHttpClient` (strumienie)
  kontra `CurlHttpClient`.
- Kod pollujacy wiele strumieni naraz uzywa `stream_select()`, ktore dzis
  **jeszcze blokuje** — jest na liscie do przechwycenia i jest tanie
  (podmiana handlera funkcji, zawieszenie zamiast `select()`).

### Uczciwe zastrzezenie: to placi dopiero po TLS

Handler strumieniowy daje wspolbieznosc **tylko dla `http://`**. `https://`
idzie przez transport `ssl`, ktorego nie przechwytujemy, wiec dzis wywolanie
do zewnetrznego API po HTTPS blokuje caly proces tak samo jak curl.

Wniosek, ktory z tego wynika i ktory zmienia priorytety: **przechwycenie TLS
jest wazniejsze, niz wygladalo**, bo to ono odblokowuje obejscie curla. Bez
niego rada "uzywaj Guzzle na strumieniach" jest prawdziwa tylko na papierze.

Zamiana `ext-curl` na handler strumieniowy nie da sie wymusic naszym
`zend_disable_functions` — Guzzle sprawdza `extension_loaded('curl')`, a tego
disable_functions nie rusza. To musi byc decyzja aplikacji.

## Kolejnosc prac

1. **DNS** — w toku (branch `fiber-async-dns`). Najtansze z waznych i naprawia
   dziure w tym, co juz dziala: polaczenie do hosta PO NAZWIE blokuje proces na
   czas `getaddrinfo`, wiec wspolbiezny MySQL po nazwie hosta dzis nie jest
   w pelni wspolbiezny.
2. **TLS** — odblokowuje obejscie curla (patrz wyzej) i kazde wychodzace HTTPS.
3. `sleep`/`usleep` i `stream_select` — tanie, podmiana handlerow funkcji.
4. **curl** — dopiero gdy 1-3 sa zrobione i nadal brakuje pokrycia.
5. `pdo_pgsql`/libpq — poza zasiegiem bez latki na `ext/pdo_pgsql`; PDO wola
   synchroniczne `PQexec`, wiec nie ma gdzie sie wpiac.

## Uwaga nadrzedna

Ta lista mowi o WSPOLBIEZNOSCI I/O. Nie zmienia nic w sprawie redeklaracji:
aplikacja z `require vendor/autoload.php` nadal pada na drugim requescie
("Cannot redeclare class ComposerAutoloaderInit..."), bo `included_files` jest
per request, a tablice funkcji i klas per proces. Te dwie rzeczy sa niezalezne
i async nie ma na czym dzialac, dopoki tamto stoi. Patrz [fiber_errors.md](fiber_errors.md).

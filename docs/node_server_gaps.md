# Różnice względem serwera aplikacyjnego Node.js

Stan na 2026-09-06. Dokument jest materiałem do analizy, a nie zatwierdzoną roadmapą.

Porównanie dotyczy typowego serwera aplikacyjnego Node.js (np. `node:http` z Express, Fastify lub Nest), a nie samego runtime'u bez bibliotek. Celem jest wychwycenie funkcji, które mogą mieć sens dla FPM-NG, bez automatycznego rozszerzania zakresu projektu.

## Różnica modelu

Produkcyjny FPM-NG `classic` realizuje model request-response: gateway przyjmuje żądanie, przekazuje je do workera PHP, worker wykonuje request i zwraca odpowiedź.

Node.js daje aplikacji bezpośrednią kontrolę nad event loopem, listenerem, połączeniem i cyklem życia odpowiedzi. Nie każdą funkcję Node należy kopiować do FPM-NG — część z nich wymagałaby zmiany modelu SAPI, a nie tylko rozbudowy gatewaya.

## Braki istotne dla zwykłych aplikacji HTTP

### Routing i front controller

Node może programowo mapować dowolną metodę i ścieżkę na handler. FPM-NG nadal opiera się na mapowaniu URI do skryptu lub `index.php`.

Brakuje konfigurowalnego odpowiednika:

```nginx
try_files $uri $uri/ /index.php?$query_string;
```

Ten punkt już znajduje się w planie FPM-NG i jest ważny dla frameworków PHP.

### Timeouty klientów

Typowy serwer Node pozwala osobno kontrolować timeout nagłówków, requestu, bezczynnego socketu i keep-alive. HTTP gateway FPM-NG nie ma jeszcze kompletnej ochrony przed wolnymi klientami i slow loris.

Ten punkt już znajduje się w planie.

### Streaming request body i backpressure

Node udostępnia request body jako strumień i może wstrzymywać odbiór, gdy konsument jest wolniejszy. Gateway FPM-NG buforuje body w pamięci przed przekazaniem requestu.

Brakuje:

- strumieniowego przekazywania body;
- backpressure;
- bezpiecznej obsługi dużych i wolnych uploadów;
- kontrolowania wzrostu pamięci gatewaya.

Backpressure już znajduje się na liście znanych braków. Sposób implementacji, np. bufor plikowy, nie jest jeszcze decyzją projektową.

### Pełny stan przeciążenia

Przy pełnym poolu FPM-NG zwraca obecnie `502`. Plan zakłada poprawne `503 Service Unavailable` z `Retry-After`.

Ewentualne kolejkowanie krótkich burstów nie jest obecnie częścią zatwierdzonego planu.

## Długie i dwukierunkowe połączenia

### Server-Sent Events

Node naturalnie obsługuje długą odpowiedź, okresowe flushowanie danych i wykrywanie rozłączenia klienta. FPM-NG nie ma obecnie produkcyjnie potwierdzonego modelu dla SSE.

Do zbadania:

- czy odpowiedź jest przekazywana strumieniowo bez nieograniczonego buforowania;
- czy flush dociera do klienta;
- zachowanie po rozłączeniu klienta;
- timeouty i backpressure zapisu;
- wpływ długiego requestu na zajętość workera.

SSE nie jest obecnie zatwierdzonym punktem roadmapy.

### WebSocket

Node może wykonać HTTP Upgrade i przejąć dwukierunkowy socket. FPM-NG nie obsługuje WebSocketów ani przekazania połączenia aplikacji PHP.

Potencjalne warianty:

1. nie obsługiwać WebSocketów i zostawić je reverse proxy;
2. dodać tunelowanie WebSocket w przyszłym `pool.type = proxy`;
3. udostępnić osobny model aplikacyjny, co byłoby znacznie większą zmianą.

Najbardziej zgodny z obecną architekturą jest wariant drugi. WebSocket nie jest obecnie zatwierdzonym punktem roadmapy.

### Long polling

Technicznie może działać jako długi request, ale zajmuje worker w executorze `classic`. Wymaga testów limitów, shutdownu, rozłączenia klienta i zachowania pełnego poola.

## Kontrola transportu z poziomu aplikacji

Node daje aplikacji bezpośredni dostęp do:

- chunked encoding;
- flushowania fragmentów;
- trailerów HTTP;
- HTTP Upgrade;
- zamknięcia lub przejęcia socketu;
- zdarzeń rozłączenia klienta.

PHP za SAPI kontroluje status, nagłówki i treść, ale gateway pozostaje właścicielem transportu. Nie należy dodawać bezpośredniego dostępu aplikacji do socketu bez osobnego projektu bezpieczeństwa i lifecycle.

### Anulowanie pracy po rozłączeniu klienta

Node może propagować anulowanie przez zdarzenia socketu i `AbortSignal`. PHP ma `connection_aborted()`, ale FPM-NG nie ma spójnego mechanizmu anulowania aktywnych operacji aplikacyjnych lub asynchronicznych.

Do zbadania:

- kiedy worker dowiaduje się o rozłączeniu;
- czy blokujące I/O można przerwać;
- czy anulowanie może bezpiecznie wywołać bailout;
- jak zachowują się `classic`, Fiber i True Async.

Nie jest to obecnie zatwierdzony punkt roadmapy.

## Protokół i funkcje serwera

### TLS

Node ma moduły `tls` i `https`. FPM-NG nie ma jeszcze TLS ani ACME. TLS + ACME są już częścią planu i poprzedzają `pool.type = proxy`.

### HTTP/2

Node posiada moduł `http2`. FPM-NG obsługuje HTTP/1.1.

HTTP/2 zapisano jako nice to have po realizacji podstawowego planu. Ewentualna implementacja powinna używać sprawdzonej biblioteki, np. `nghttp2`, i nastąpić dopiero po TLS/ALPN, timeoutach oraz backpressure.

### Kompresja

W ekosystemie Node kompresję zwykle dodaje middleware. FPM-NG nie kompresuje odpowiedzi.

Streamingowe `gzip` zapisano jako nice to have po realizacji podstawowego planu. Brotli nie jest obecnie częścią planu.

### Middleware gatewaya

Frameworki Node oferują łańcuch middleware dla uwierzytelniania, logowania, rate limiting, routingu i modyfikowania odpowiedzi. FPM-NG nie ma rozszerzalnego systemu middleware w procesie gatewaya.

Większość logiki aplikacyjnej powinna pozostać w frameworku PHP. Middleware gatewaya ma sens tylko dla funkcji transportowych lub wykonywanych przed uruchomieniem PHP. Nie jest obecnie częścią planu.

## Model wykonania aplikacji

### Stan między requestami

Node naturalnie utrzymuje stan procesu między requestami. Produkcyjny executor `classic` uruchamia pełny lifecycle requestu PHP, zachowując oczekiwaną izolację PHP-FPM.

Zmiana tego modelu grozi wyciekami stanu aplikacji i rozszerzeń. Nie jest to brak, który należy automatycznie usuwać — izolacja jest właściwością kompatybilności.

### Asynchroniczne I/O

Node może obsługiwać wiele operacji I/O w jednym procesie. `classic` przypisuje request do workera. FPM-NG ma eksperymentalne executory Fiber i True Async, ale nie są one gotowe produkcyjnie.

Znane problemy Fiber znajdują się w `docs/fiber_errors.md`. True Async wymaga osobnego forka silnika PHP.

### Własne protokoły

Node może otwierać TCP/UDP i implementować dowolne protokoły. FPM-NG jest celowo skoncentrowany na FastCGI, HTTP i uruchamianiu PHP. MQTT, raw TCP/UDP czy własne protokoły nie są obecnie celem projektu.

## Funkcje, których Node zwykle nie daje bez dodatkowego stosu

Porównanie nie oznacza, że Node ma wszystko w standardowej konfiguracji. Typowa instalacja potrzebuje dodatkowych bibliotek lub usług dla:

- ACME;
- routingu frameworka;
- access logu;
- kompresji middleware;
- zarządzania procesami i restartami;
- cronów i procesów nadzorowanych;
- metryk;
- konfiguracji graceful shutdown;
- obsługi plików statycznych na poziomie reverse proxy.

FPM-NG integruje już część tych funkcji w jednej binarce: zarządzanie workerami, supervisor, cron, status, statyki, ACL, trusted proxies i access log.

## Kandydaci do dalszego rozpoznania

Bez wpisywania ich automatycznie do roadmapy warto wykonać małe testy techniczne w tej kolejności:

1. **SSE i flush odpowiedzi** — sprawdzić, co już działa oraz gdzie występuje buforowanie.
2. **Rozłączenie klienta** — zmierzyć, kiedy gateway i worker wykrywają przerwanie.
3. **Streaming uploadu** — opisać obecny przepływ pamięci i możliwe punkty backpressure.
4. **Long polling** — sprawdzić shutdown, timeouty i zachowanie pełnego poola.
5. **WebSocket przez przyszły proxy** — ocenić tunelowanie zamiast implementacji w SAPI.
6. **API anulowania** — dopiero po poznaniu zachowania rozłączeń w każdym executorze.

Największą szansę na użyteczną funkcję bez zmiany modelu PHP mają SSE, poprawne wykrywanie rozłączenia klienta i streaming body. WebSocket najlepiej rozważać jako funkcję przyszłego reverse proxy, a nie executora PHP.

## Klasyfikacja

### Już w planie

- routing/odpowiednik `try_files`;
- timeouty klientów;
- backpressure request body;
- `503 Retry-After` przy pełnym poolu;
- TLS + ACME;
- `pool.type = proxy`;
- HTTP/2 i gzip jako końcowe nice to have.

### Do rozpoznania, ale nie w roadmapie

- SSE;
- WebSocket/tunelowanie WebSocket;
- long polling;
- anulowanie pracy po rozłączeniu klienta;
- rozbudowane streaming API;
- middleware gatewaya.

### Świadomie nie traktować jako brak kompatybilności

- trwały globalny stan aplikacji między requestami;
- bezpośrednie przejęcie socketu przez zwykły skrypt PHP;
- dowolne serwery TCP/UDP w modelu requestowym;
- własna implementacja HTTP/2.

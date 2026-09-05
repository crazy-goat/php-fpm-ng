# patches/

Łatki na pliki php-src **poza `sapi/`**. Normalnie fpm-ng nie tyka upstreamu —
`prepare.sh` tworzy wyłącznie `sapi/fpmng/`. Te łatki są odstępstwem i mają być
widoczne, policzalne i tymczasowe.

## Zasady

1. **Każda łatka ma termin ważności.** W nagłówku podaje PR upstreamu, na który
   czeka. Znika w dniu, w którym tamten się zmerguje. Łatka bez drogi wyjścia
   to ukryty fork.
2. **Jedna łatka na jeden problem**, nie na jedną wersję PHP. Wersje obsługuje
   się dopiero wtedy, gdy ta sama łatka przestaje się nakładać.
3. **Więcej niż dwa warianty wersyjne jednej łatki to sygnał alarmowy** — wtedy
   albo idzie do upstreamu, albo trzeba ją przeprojektować tak, żeby zmieściła
   się w `sapi/fpmng/`.
4. **CI buduje każdą wspieraną wersję PHP**, więc nienakładająca się łatka pada
   przy budowaniu, a nie przy wydaniu.

## Układ

```
patches/*.patch              nakładane zawsze
patches/php-8.4/*.patch      tylko dla tej wersji, nadpisuje łatkę o tej nazwie
```

## Stan

| łatka | dotyczy | czeka na | sprawdzone na |
|---|---|---|---|
| `0001-gh18956-fastcgi-keepalive-counting.patch` | `main/fastcgi.c`, `main/fastcgi.h`, **`sapi/fpm/fpm/fpm_request.c`, `fpm_request.h`** (pełny PR, nie wycinek) | https://github.com/bukka/php-src/pull/2 (GH-18956) | 8.4, 8.5, master; **8.3 przez wariant** `php-8.3/` (7 argumentów `fpm_scoreboard_update_commit`) |
| `0002-fastcgi-tcp-nodelay-never-set.patch` | `main/fastcgi.c` | zgłoszenie do php/php-src — tekst gotowy w `0002-upstream-report.md`, jeszcze nie wysłane | 8.3, 8.4, 8.5, master |
| `0003-fastcgi-buffered-read-accept4.patch` | `main/fastcgi.c` | kandydat na PR do php/php-src, nie zgłoszone | 8.4, 8.5, master; **8.3 przez wariant** `php-8.3/` (inna sygnatura `safe_read`) |
| `0004-fastcgi-ng-transport-switch.patch` | `main/fastcgi.c`, `main/fastcgi.h` | przeniesienie przełącznika za API należące do `sapi/fpmng` | 8.5, master; starsze wersje do weryfikacji |
| `0005-fastcgi-writev-large-response.patch` | `main/fastcgi.c` | kandydat na PR do php/php-src, nie zgłoszone | 8.5, master; starsze wersje do weryfikacji |

Stos jest kolejnościowy: 0002 i 0003 zakładają nałożone 0001 (kontekst przy
`accept()`), choć merytorycznie są od niego niezależne. `prepare.sh` nakłada
wszystko po kolei na nietknięte drzewo, a „już nałożone" sprawdza dla całego
stosu naraz (odwrotnie, z kopii dotkniętych plików) — test per łatka kłamie,
gdy dwie łatki siedzą w tym samym miejscu.

### Dlaczego 0001 jest konieczna

Bramka trzyma trwałe połączenia do poola (`FCGI_KEEP_CONN`), więc fpm-ng jest
dokładnie tym przypadkiem, który błąd GH-18956 psuje: licznik idle kontra active
kłamie, a `pm = dynamic` i `ondemand` źle skalują pulę. Bez tej łatki wiarygodny
jest tylko `pm = static`.

Informacja o tym, czy `accept` przyszedł z połączenia trzymanego przy życiu,
żyje w `main/fastcgi.c` — nie da się jej wyprodukować z samego `sapi/`.
Towarzyszące zmiany w `fpm_request.c` i `fpm_request.h` niesiemy jako własne
pliki w `sapi/fpmng/fpm/`, bo te są w `sapi/`.

### Dlaczego 0002 (`TCP_NODELAY`)

Błąd upstreamu, nie nasza optymalizacja: `req->tcp` jest przypisywane tylko pod
`_WIN32`, więc na Linuksie `TCP_NODELAY` nigdy nie trafia na połączenie
keep-alive. Odpowiedź > 8 KB przez TCP idzie kilkoma `write()`, ostatni mały
segment czeka na ACK: Nagle + delayed ACK, dziesiątki milisekund zamiast
mikrosekund. Bramka trzyma połączenia do poola po TCP, więc to nas dotyczy
wprost. Opis do zgłoszenia i reprodukcja: `0002-upstream-report.md`.

### Dlaczego 0003 (bufor wejściowy + `accept4`)

Czysto transportowa oszczędność syscalli w workerze (jeden `read()` na nagłówek
requestu zamiast sześciu, `accept4(SOCK_CLOEXEC)` zamiast `accept` + 2×`fcntl`),
zero zmian na drucie. Liczby przed/po: `docs/NOTES.md`, sekcja 3t. Wykrywanie
`accept4` robi nasz `sapi/fpmng/config.m4` (`AC_CHECK_FUNCS([accept4])`),
bo upstream sprawdza to tylko w `ext/sockets`; bez `HAVE_ACCEPT4` kompiluje się
stara ścieżka. Wersja dla upstreamu musiałaby dodać ten check do `configure.ac`.

### ROZWIĄZANE (droga 1): 0001 łamało `--enable-fpm --enable-fpmng` w jednym drzewie

Decyzja koordynatora: droga 1. `0001` niesie teraz także hunki PR-a na
`sapi/fpm/fpm/fpm_request.c/.h`, czyli jest PEŁNYM odpowiednikiem PR bukka#2
(bez testów .phpt) i znika w całości w dniu jego merge'u. Oba SAPI budują się
razem, stary FPM dostaje tę samą poprawkę. Weryfikacja: build
`--enable-fpm --enable-fpmng` + pełny zestaw `sapi/fpm/tests` — wynik w
`docs/NOTES.md` 3t. Poniżej pierwotna analiza, zachowana dla kontekstu.


`0001` zmienia w `main/fastcgi.h` sygnaturę hooków `fcgi_init_request()` z
`void(*)(void)` na `void(*)(bool)`. Upstreamowe `sapi/fpm/fpm/fpm_main.c`
przekazuje tam `fpm_request_accepting`/`fpm_request_reading_headers` ze starymi
sygnaturami z upstreamowego `fpm_request.h`, więc GCC 14+ przerywa kompilację
(`-Wincompatible-pointer-types` jest błędem). Obietnica „stary FPM działa dalej
obok" jest w tym miejscu złamana: dziś działa tylko `--enable-fpmng` bez
`--enable-fpm`. Nasz `sapi/fpmng` kompiluje się, bo niesie własne
`fpm_request.c/.h` z sygnaturami `bool`.

Rozmiar: **mały**, bo to dokładnie to, co robi upstreamowy PR. PR bukka#2
(GH-18956) zmienia `main/fastcgi.c` **oraz** `sapi/fpm/fpm/fpm_request.c`,
`sapi/fpm/fpm/fpm_request.h` (plus testy). Nasze `0001` to jego część
ograniczona do `main/`; brakującą część niesiemy jako własne pliki w
`sapi/fpmng/` — i właśnie dlatego upstreamowe `sapi/fpm` zostaje w tyle.

Dwie drogi:

1. **Dołożyć do `0001` hunki PR-a na `sapi/fpm/fpm/fpm_request.c` i
   `fpm_request.h`** (~33 linie, identyczne z tym, co mamy w `sapi/fpmng/`).
   Wtedy oba SAPI budują się razem, a upstreamowy FPM w tym samym drzewie
   dostaje poprawkę liczenia GH-18956 — czyli zachowanie, które upstream i tak
   zmerguje. Koszt: łatka po raz pierwszy dotyka `sapi/fpm/`, ale to nadal
   jeden problem = jedna łatka, z tym samym terminem ważności. Zmiana
   zachowania starego FPM ogranicza się do poprawnych liczników idle/active
   przy keep-alive.
2. **Przeprojektować `0001` bez zmiany sygnatur**: zostawić stare hooki, dodać
   w `fastcgi.c` osobny setter (np. `fcgi_request_set_hooks_ex(req,
   on_accept(bool), on_read(bool))`). Upstreamowe `sapi/fpm` kompiluje się
   nietknięte, ale nasz skopiowany `fpm_main.c` woła `fcgi_init_request()` ze
   starymi prototypami i naszymi `bool`-funkcjami — więc trzeba by wziąć
   `fpm_main.c` na własność (20 commitów/rok) albo dodać do niego hook.
   Więcej kodu, dalej od kształtu upstreamu.

Rekomendacja: droga 1. Zero nowego kodu, zbieżne z upstreamem, a
`prepare.sh` może przestać usuwać `sapi/fpmng/tests` tylko dlatego, że
„testy odwołują się do binarki php-fpm" — z `--enable-fpm` obok testy upstreamu
biegną w tym samym drzewie. Do zrobienia po decyzji koordynatora, nie w tym
zadaniu.

### Stan wariantów wersyjnych

PHP-8.3 ma dziś DWA warianty (`0001` — 7 argumentów `fpm_scoreboard_update_commit`;
`0003` — `const void *buf` w `safe_read()`). To jest dokładnie próg alarmowy
z zasady 3. Oba znikną razem z łatkami po merge'u upstreamu; jeśli 8.3 rozjedzie
się bardziej, tańsze będzie wypisanie 8.3 ze wspieranych wersji niż trzeci wariant.

**DECYZJA (2026-09-05): 8.3 ZOSTAJE.** Ma nadal wsparcie bezpieczeństwa, a dwa
warianty nie kosztują nas realnie nic — nakładają się czysto i są pokryte CI.
Próg z zasady 3 pozostaje w mocy jako ostrzeżenie, nie jako automat: sam fakt,
że jesteśmy na progu, NIE jest powodem do wypisania wersji. Powodem będzie
dopiero trzeci wariant albo wariant, który wymaga innej logiki, a nie innej
sygnatury. Do tego czasu nie wracamy do tej dyskusji.

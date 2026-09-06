# SPIKE: czy executor `fiber` moze dzialac WYLACZNIE w ZTS, z osobnym blokiem TSRM na kazdy request?

Status: SPIKE, nie do zmergowania. Kod eksperymentu: `spike/ext-tsrm-spike/`
(rozszerzenie `tsrm_spike`, `--enable-tsrm-spike`, statyczne, wolane z CLI:
`tsrm_spike_run()`, `tsrm_spike_mem(n)`, `tsrm_spike_require_in_ctx(path, which)`).
Nie implementuje executora na kontekstach TSRM — tylko sprawdza, czy fundament
w ogole dziala.

Repo: php-fpm-ng @ `89c5151`, branch `spike/a5-tsrm-ctx`, worktree
`/Users/piotr.halas/work/php-fpm-ng-spike-a5`. Poligon: `~/rd/a5` na
192.168.8.103. Build: PHP 8.5.11-dev (ten sam php-src co `~/rd/phpsrc`, kopia
w `~/rd/a5/phpsrc`), `./configure --disable-all --enable-fpmng --enable-zts
--enable-tsrm-spike --enable-session=shared --enable-mbstring --enable-ctype
--enable-tokenizer --enable-phar --with-openssl --with-zlib
--prefix=/home/piotr/rd/a5/inst`.

Weryfikacja, ze mierze SWOJA binarke: `strings sapi/cli/php | grep -c 'Q4a:
przed utworzeniem'` -> `1` (nasz string jest w binarce), `php -v` -> `(ZTS)`,
`php -m | grep tsrm_spike` -> `tsrm_spike` (modul zaladowany).

## Ustalenia wstepne (research, nie pomiar — ale poparte configure.ac i readelf)

- `tsrm_new_interpreter_context()` / `tsrm_set_interpreter_context()` faktycznie
  nie istnieja w drzewie (`bd73607b9e4`, "TSRM cleanup for PHP8"). Zero
  wystapien w `TSRM/`, `Zend/`, `main/`.
- `TSRMG` (uzywane m.in. przez `PS()` w session, gdy nie ustawiona jest static
  cache) i `TSRMG_STATIC` (uzywane przez `EG`/`PG`/`SG` i przez `PS()` GDY
  ustawiona) to dwie rozne sciezki (`TSRM/TSRM.h:178-192`):
  - `TSRMG_STATIC` czyta `TSRMLS_CACHE` (symbol `_tsrm_ls_cache`, `__thread`,
    extern) z BIEZACEGO pliku kompilacji — to jest nieoficjalny, ale wciaz
    dzialajacy "setter" (mozna do niego przypisac).
  - `TSRMG` (bez `_STATIC`) woła `tsrm_get_ls_cache()` — funkcje TSRM, ktora
    czyta prawdziwy, wewnetrzny `pthread_getspecific` (patrz `TSRM/TSRM.c:847`
    i `tsrm_tls_get()`), NIE zmienna `_tsrm_ls_cache`.
  - Ktora sciezke dostaje dany modul, decyduje `ZEND_ENABLE_STATIC_TSRMLS_CACHE`
    (`Zend/zend.h:56-69`), flaga per-plik ustawiana w `config.m4`.
    `ext/session/config.m4:20` ustawia ja TAKZE dla builda `shared` (a wiec
    session UZYWA fast/static path, nie `tsrm_get_ls_cache()`).
  - Dla `COMPILE_DL_SESSION`, `ext/session/session.c:3319-3323` robi
    `ZEND_TSRMLS_CACHE_DEFINE()` (NOWA definicja symbolu `_tsrm_ls_cache` W TYM
    PLIKU, tzn. w `session.so`) + `ZEND_GET_MODULE`, a `session.c:2884-2885`
    robi `ZEND_TSRMLS_CACHE_UPDATE()` w MINIT — czyli RAZ, przy starcie
    procesu / dlopen tego `.so`. To dokladnie hipoteza z zadania.
  - **Zweryfikowane na poziomie binarki** (nie tylko przez czytanie kodu):
    `readelf -sW` na `sapi/cli/php` i na `ext/session/.libs/session.so`
    pokazuje `_tsrm_ls_cache` jako `TLS LOCAL` w OBU plikach — czyli to sa
    FIZYCZNIE dwie oddzielne zmienne watkowo-lokalne, bez mozliwosci
    interpozycji symboli ELF. Powod: kompilacja idzie z `-fvisibility=hidden`
    (widac w linii kompilacji `make`), a `_tsrm_ls_cache` nie ma zadnego
    atrybutu widocznosci w definicji (`TSRM_TLS void *_tsrm_ls_cache = NULL;`),
    wiec dziedziczy `hidden` z flagi kompilacji per-plik. `session.so` fizycznie
    NIE MOZE zobaczyc zmiany tej zmiennej w binarce glownej, i odwrotnie.

## Q1: czy da sie utworzyc drugi blok TSRM bez usunietego API?

**TAK.** `ts_resource_ex(0, &fake_tid)` z podstawionym (nieprawdziwym) `th_id`
tworzy nowy blok — `TSRM/TSRM.c:527-530` (`allocate_new_resource`), gdy
`thread_id` nie jest jeszcze w hashtable. `id == 0` przekazane do
`ts_resource_ex` zwraca `&thread_resources->storage` (patrz
`TSRM_SAFE_RETURN_RSRC`, `offset==0` -> `return &array`), czyli dokladnie to,
co normalnie zwraca `tsrm_get_ls_cache()` dla PRAWDZIWEGO watku.
`allocate_new_resource` wywoluje ctor KAZDEGO zarejestrowanego modulu
(`TSRM/TSRM.c:470-480`) — czyli nowy blok dostaje pelny, swiezy zestaw
globali (jak nowy prawdziwy watek w SAPI watkowym).

Surowe wyjscie (`tsrm_spike_run()`):
```
Q1: prawdziwy watek=130144153527872, tsrm_get_ls_cache() (ctxA)=0x617b00d055d0, TSRMLS_CACHE=0x617b00d055d0
Q1: ts_resource_ex(0, fake_tid=130144153551077) -> ctxB=0x617b00f17b30 (utworzony=tak, rozny od A=tak)
Q1: po utworzeniu B, tsrm_get_ls_cache()=0x617b00f17b30, TSRMLS_CACHE=0x617b00f17b30 (oczekiwane: oba == ctxB - efekt uboczny allocate_new_resource przelaczyl PRAWDZIWY watek)
```

**Zastrzezenie odkryte PRZY OKAZJI (wazniejsze niz sama odpowiedz TAK):**
`allocate_new_resource()` NIE tworzy tylko nowego bloku obok istniejacego —
jego skutkiem ubocznym jest natychmiastowe PRZELACZENIE prawdziwego watku
(realnego `pthread_setspecific` UZYWANEGO przez `tsrm_get_ls_cache()`, plus
`TSRMLS_CACHE`) na nowo utworzony blok. Nie ma zadnej publicznej funkcji, ktora
pozwala wrocic do poprzedniego bloku dla sciezki `tsrm_get_ls_cache()` —
`ts_resource_ex()` dla JUZ ISTNIEJACEGO bloku (branch "znaleziono w
hashtable") NIE wywoluje `set_thread_local_storage_resource_to()`, wiec nie
przestawia real TLS z powrotem. Jedyny sposob powrotu, jaki znalazlem, to
reczne przypisanie `TSRMLS_CACHE = adres_starego_bloku` — a to dziala TYLKO
dla fast/static path (patrz Q2), NIE dla `tsrm_get_ls_cache()`.

## Q2: czy reczne `TSRMLS_CACHE = ...` przelacza EG/PG (kod statyczny)?

**TAK**, dla kodu wkompilowanego statycznie (main binary, bez `COMPILE_DL_*`),
bo to jest jedna, wspoldzielona zmienna (`TSRMLS_MAIN_CACHE_EXTERN()` w
`zend.h:73-76` dla plikow bez `ZEND_COMPILE_DL_EXT`).

Surowe wyjscie:
```
Q2: ustawilem EG(precision)=111 w A, =222 w B. W B odczytuje=222 (oczekiwane 222)
Q2: po powrocie TSRMLS_CACHE=ctxA, EG(precision)=111 (oczekiwane 111, NIE 222)
Q2: WNIOSEK: przelaczanie EG() (kod statyczny) przez reczne TSRMLS_CACHE dziala = TAK
```
(Kolejnosc linii w terminalu byla przestawiona wzgledem kolejnosci
wykonania kodu — patrz sekcja "Efekt uboczny na warstwie output" nizej;
wartosci sa poprawne, tylko FLUSH bufora wyjscia jest nieprzewidywalny.)

## Q3: czy session (`--enable-session=shared`) idzie za przelaczeniem?

**NIE — zgodnie z hipoteza, i to na dwa sposoby, jeden gorszy niz drugi.**

1. Build: `session.so` uzywa `ZEND_ENABLE_STATIC_TSRMLS_CACHE=1`
   (`ext/session/config.m4:20`), wiec `PS(v)` idzie przez fast/static path —
   ale WLASNA, sfrozenowana (raz, w MINIT tego `.so`) kopie `_tsrm_ls_cache`.
   Reczne przelaczenie `TSRMLS_CACHE` w naszym pliku NIE dotyka tej kopii
   (potwierdzone przez `readelf`, patrz wyzej: dwie fizycznie oddzielne
   zmienne `TLS LOCAL`).
2. Runtime, sciezka A<->A (test dziala): ustawiamy `session.save_path` w A,
   odczytujemy w A — poprawnie, izolacja OK dopoki nie ruszamy sie z A:
```
Q3: w A ustawilem save_path=/spike/A, session_save_path() zwraca teraz: /spike/A
Q3: po powrocie TSRMLS_CACHE=ctxA (statyczny EG/PG juz widzi A), session_save_path() (dynamicznie zaladowany session.so) zwraca: /spike/A
```
3. Runtime, proba odczytu/zapisu W kontekscie B: **`call_user_function()` dla
   `session_save_path` ZAWIODLO** (dwukrotnie — set i get):
```
SPIKE: call_user_function(session_save_path, /spike/B) FAILED
SPIKE: call_user_function(session_save_path) FAILED
```
   Nie udalo mi sie ustalic dokladnej przyczyny w ramach tego spike'u (nie
   drazylem glebiej, zgodnie z zasada "nie naprawiaj po drodze") — najbardziej
   prawdopodobne wytlumaczenie: `CG(function_table)` w kontekscie B jest
   swiezy z GINIT (bo `allocate_new_resource` woła ctor kazdego modulu, ale to
   NIE jest to samo, co pelna rejestracja funkcji przez `php_module_startup()`
   danego watku), wiec `session_save_path` moze tam po prostu nie istniec.
   To jest WAZNIEJSZE od samego Q3: **kontekst B, nawet poprawnie utworzony
   (Q1), NIE jest dzialajacym interpreterem** — patrz takze Q4b, gdzie proba
   uruchomienia PRAWDZIWEGO kodu PHP w kontekscie B konczy sie SEGFAULTEM.

**Wniosek dla Q3**: tej czesci pytania (czy session PO CICHU widzi stary
kontekst zamiast nowego) NIE udalo sie jednoznacznie zmierzyc w runtime, bo
probe blokuje glebszy problem — kontekst B nie jest w stanie wykonac zadnego
wywolania funkcji PHP. To, co jest zmierzone i pewne: session.so ma WLASNA,
fizycznie oddzielna kopie `_tsrm_ls_cache` (dowod `readelf`), wiec zaden
zewnetrzny "switch" (nawet gdyby kontekst B dzialal) nie moze jej dotknac —
session zawsze widzi kontekst, ktory mial w momencie wlasnego MINIT (zwykle
start procesu), niezaleznie od czegokolwiek pozniej.

## Efekt uboczny odkryty przy okazji: warstwa output tez jest per-kontekst

Linie z `php_printf()` wydrukowane W TRAKCIE gdy `TSRMLS_CACHE` wskazywal na
B pojawily sie w terminalu w INNEJ kolejnosci niz w kodzie (np. linia "Q2: po
powrocie... 111" pojawila sie w logu PRZED linia "Q1: ts_resource_ex(...) ->
ctxB", mimo ze w kodzie jest odwrotnie). `php_printf` idzie przez SAPI
output buffering (`OG()`/`output_globals`), ktore rowniez jest per-kontekst
TSRM (fast/static) — czyli przelaczenie `TSRMLS_CACHE` w trakcie requestu
przelacza TEZ bufor wyjscia na nowy, "sierocy" bufor kontekstu B, ktory flushuje
sie w innym momencie niz normalny strumien stdout kontekstu A. Nie zbadalem
tego dogłębniej (poza zakresem spike'u), ale to kolejny, niezalezny dowod na
to, ze "po prostu przelacz TSRMLS_CACHE" rusza znacznie wiecej stanu, niz
tylko globalne zmienne, ktorych celowo dotykamy.

## Q4: koszt pamieci

### (a) sam blok zasobow TSRM, zmierzone

Metoda: `VmRSS` z `/proc/self/status` przed i po utworzeniu N blokow przez
`ts_resource_ex(0, &fake_tid)` w petli (rozne `fake_tid` kazda iteracja), BEZ
zadnego kodu PHP w srodku (zaden request, zaden `require`).

Surowe wyjscie:
```
N=1: Q4a: przed utworzeniem 1 dodatkowych blokow: VmRSS=16564 kB
     Q4a: po utworzeniu 1 dodatkowych blokow: VmRSS=16856 kB (delta=292 kB, ~292.0 kB/blok)
N=8: Q4a: przed utworzeniem 8 dodatkowych blokow: VmRSS=16664 kB
     Q4a: po utworzeniu 8 dodatkowych blokow: VmRSS=18944 kB (delta=2280 kB, ~285.0 kB/blok)
```
~285-292 kB na dodatkowy, PUSTY blok TSRM (same struktury modulow po GINIT,
zero kodu uzytkownika). To JEST mierzalny koszt bazowy, ale nie odpowiada na
pytanie o realny koszt (patrz (b)).

### (b) z Symfony (`require vendor/autoload.php`) w kazdym kontekscie

**NIE ZMIERZONE.** Proba (`tsrm_spike_require_in_ctx()`, uzywajac
`zend_eval_string()` bo `require` to konstrukcja jezyka, nie funkcja) na
prostym pliku `tiny.php` w BIEZACYM kontekscie zadziala poprawnie:
```
Q4b: zend_eval_string(require /home/piotr/rd/a5/tiny.php) w kontekscie biezacym -> rv=0 (SUCCESS), exception=nie
Q4b: VmRSS przed=16632 kB po=16640 kB (delta=8 kB)
PHP script survived to the end.
```
ale ta sama operacja W KONTEKSCIE B (utworzonym przez `ts_resource_ex`, tak
jak w Q1) **konczy sie SEGFAULTEM procesu, natychmiast, przed jakimkolwiek
printem**:
```
$ php -n spike_require.php tiny.php 1
Segmentation fault (core dumped)
EXIT=139
```
Nie draze dalej przyczyny (poza zakresem spike'u) — ale to zamyka sprawe (b):
skoro nawet trywialny plik w kontekscie B segfaultuje, `require
vendor/autoload.php` z Symfony w tym samym kontekscie nie ma szans zadzialac,
a probowanie tego dalej byloby marnowaniem czasu na cos, co juz wiadomo ze nie
dziala. Punkt odniesienia z `docs/frameworks.md` (8 rownoleglych requestow
Symfony = 42 MB RSS w jednym procesie, obecny model) pozostaje jedynym
zmierzonym punktem po tej stronie.

## Q5: czy build ZTS przechodzi z `patches/` i `sapi/fpmng`?

**NIE.** `./configure --enable-zts` + `build/prepare.sh` (patche + skladanie
`sapi/fpmng`) przechodza bez ostrzezen, ale `make` pada na
`sapi/fpmng/fpm/fpm_pool_coop.c` (mechanizm izolacji sesji request<->request
na executorze coop/fiber — swap globali "przez wartosc"):

```
/home/piotr/rd/a5/phpsrc/sapi/fpmng/fpm/fpm_pool_coop.c: In function 'fpm_coop_container_start':
fpm_pool_coop.c:384:36: error: 'sapi_globals' undeclared (first use in this function); did you mean 'fpm_globals'?
        memcpy(&fpm_coop_base_sg, &sapi_globals, sizeof(sapi_globals));
fpm_pool_coop.c:385:36: error: 'output_globals' undeclared (first use in this function); did you mean 'output_globals_id'?
        memcpy(&fpm_coop_base_og, &output_globals, sizeof(output_globals));
... (dokladnie ta sama para bledow w fpm_coop_accept_kept, fpm_coop_req_new,
     fpm_coop_req_enter, fpm_coop_req_leave — 8 wystapien lacznie)
make: *** [Makefile:663: sapi/fpmng/fpm/fpm_pool_coop.lo] Error 1
```

Przyczyna: `sapi_globals` i `output_globals` jako GOLE zmienne globalne
istnieja TYLKO w buildzie NTS. W ZTS to makra (`SG(...)`, `OG(...)`) idace
przez TSRM, nie symbole do wziecia adresu / `memcpy`. Kod
`fpm_pool_coop.c` (mechanizm "swap globali przez wartosc" miedzy requestami w
tym samym procesie, patrz `fpm_coop_req_enter`/`fpm_coop_req_leave`) zaklada
NTS wprost — spojne z tym, ze `fpm_coop_validate()` juz dzis ODRZUCA ZTS w
runtime (`fpm_pool_coop.c:172-176`). To znaczy: kod nigdy nie byl pisany pod
ZTS i dzis nie przechodzi nawet kompilacji, nie tylko walidacji.

Reszta drzewa (CLI SAPI, Zend, nasze rozszerzenie `tsrm_spike`, `session`
jako `shared`) kompiluje sie i linkuje czysto pod ZTS (`make sapi/cli/php`
przeszlo bez bledow, `php -v` pokazuje `(ZTS)`).

## Podsumowanie

| Pytanie | Odpowiedz | Pewnosc |
|---|---|---|
| Q1: da sie stworzyc 2. blok TSRM | TAK, przez `ts_resource_ex(0,&fake_tid)` | wysoka — dziala, ale ma efekt uboczny (auto-przelaczenie) i tworzy kontekst, ktory NIE jest w pelni sprawnym interpreterem |
| Q2: reczny `TSRMLS_CACHE` przelacza EG/PG | TAK | wysoka, zmierzone bezposrednio |
| Q3: session idzie za przelaczeniem | NIE (wlasna, fizycznie oddzielna kopia `_tsrm_ls_cache`, potwierdzone `readelf`) | build-level: wysoka. Runtime "co dokladnie widzi w B": nie zmierzone, bo B nie wykonuje wywolan funkcji |
| Q4a: koszt pustego bloku | ~285-292 kB/blok | zmierzone |
| Q4b: koszt z Symfony | nie zmierzone (segfault juz na trywialnym pliku w kontekscie B) | — |
| Q5: build ZTS przechodzi | NIE — `fpm_pool_coop.c` uzywa gołych `sapi_globals`/`output_globals`, ktore nie istnieja w ZTS | wysoka, konkretny blad kompilacji |

**Ogolny wniosek tego spike'u**: pomysl "executor fiber wylacznie w ZTS,
osobny kontekst TSRM per request" napotyka na PRZYNAJMNIEJ trzy niezalezne
przeszkody, kazda z osobna wystarczajaca do odrzucenia pomyslu w obecnej
formie:
1. Nie ma publicznego, dwukierunkowego sposobu przelaczania kontekstu dla
   WSZYSTKICH sciezek dostepu (fast-static dziala, `tsrm_get_ls_cache()` nie
   ma odpowiednika settera).
2. Rozszerzenia zaladowane dynamicznie (jak `session`) maja wlasna, fizycznie
   oddzielna kopie cache'u, sfrozenowana raz przy starcie procesu — zaden
   spike-owy hack tego nie zmieni bez ingerencji w kazde takie rozszerzenie.
3. Blok utworzony przez `ts_resource_ex` z podstawionym `th_id` NIE jest
   samodzielnym, dzialajacym interpreterem — proba wykonania w nim
   PRAWDZIWEGO kodu PHP (nawet trywialnego) konczy sie segfaultem. Zeby to
   naprawic, trzeba by odtworzyc znaczna czesc tego, co normalnie robi
   `php_module_startup()`/`php_request_startup()` per-thread w watkowym SAPI —
   co jest dokladnie tym, co usunieto z publicznego API w PHP8 (Q1's
   wstepne ustalenie) i co ten spike mial celowo pominac.

Do tego dochodzi (4) build ZTS obecnego `sapi/fpmng` i tak nie przechodzi
(Q5) niezaleznie od powyzszego, bo `fpm_pool_coop.c` nigdy nie byl pisany pod
ZTS.

## Czego NIE zrobiono

- Nie zaimplementowano executora fiber-na-kontekstach TSRM — poza zakresem.
- Nie naprawiono `fpm_pool_coop.c` pod ZTS (Q5) — zanotowane, nie naprawiane.
- Nie zdiagnozowano dokladnej przyczyny segfaulta w Q4b/niepowodzenia
  `call_user_function` w Q3 (poza zakresem — "nie naprawiaj po drodze").
- Q4(b) z prawdziwym Symfony — nie zmierzone, patrz wyzej dlaczego probowanie
  dalej nie mialo sensu.

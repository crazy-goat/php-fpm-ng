/* fpm-ng: izolacja stanu ext/session per request na executorze coop (fiber).
 *
 * Problem (docs/frameworks.md, "Symfony — sesje PHP"): ext/session trzyma
 * caly swoj stan w PROCESIE — PS(session_status), PS(in_save_handler),
 * $_SESSION (PS(http_session_vars)), otwarty handler zapisu (np. polaczenie
 * do Redisa) — a executor coop robi JEDEN php_request_startup() na zycie
 * procesu (fpm_pool_coop.c), wiec RINIT/RSHUTDOWN modulu session NIGDY sie
 * nie wykonuja per request. Zmierzone: fiber A wisi w I/O Redisa wewnatrz
 * read() handlera zapisu, fiber B wola session_start() na tym samym procesie
 * i dostaje "Cannot call session save handler in a recursive manner" —
 * 20/20 rund, HTTP 500. Sekwencyjnie (jeden request w locie) dziala.
 *
 * Rozwiazanie: dokladnie ten sam wzorzec co SG/OG/symbol_table w
 * fpm_pool_coop.c — swap globali modulu przy enter/leave — plus wolanie
 * RINIT/RSHUTDOWN modulu session PER REQUEST (swap daje izolacje danych,
 * RINIT/RSHUTDOWN daje cykl zycia: sesja startuje i jest FLUSHOWANA do
 * storage'u dla KAZDEGO requestu, nie raz na proces).
 *
 * --- Adres ps_globals bez zaleznosci linkera ------------------------------
 *
 * Twardy `extern ps_globals` (ZEND_EXTERN_MODULE_GLOBALS(ps) po nazwie w
 * naszym kodzie) tworzylby zaleznosc linkera od symbolu, ktory przy
 * --enable-session=shared siedzi w session.so, ktorego nasz .o nie linkuje —
 * build calej binarki padlby na linkowaniu (patrz odrzucona wersja tego
 * pomyslu, commit ed79db8 na branchu req-isolation, ktory zamiast tego
 * blokowal session_start() przez zend_disable_functions).
 *
 * Trop, ktory tu uzywamy: STD_PHP_INI_ENTRY (Zend/zend_ini.h) zapisuje w
 * KAZDYM wpisie ini modulu:
 *   mh_arg1 = offsetof(zend_ps_globals, <pole>)
 *   mh_arg2 = w buildzie NIE-ZTS wskaznik NA BAZE globali modulu, czyli
 *             wprost &ps_globals (patrz STD_ZEND_INI_ENTRY w zend_ini.h,
 *             galaz #else — nasz build jest NTS, sprawdzone przez brak
 *             --enable-maintainer-zts / --enable-zts w config.nice na
 *             poligonie i przez fpm_coop_validate(), ktore juz dzis
 *             odrzuca ZTS dla tego executora z tego samego powodu).
 * Wpis "session.save_path" (ext/session/session.c, PHP_INI_BEGIN) jest
 * rejestrowany przez STD_PHP_INI_ENTRY, wiec:
 *   zend_hash_str_find_ptr(EG(ini_directives), "session.save_path", ...)
 * daje zend_ini_entry*, ktorego mh_arg2 to adres ps_globals. Zero
 * zaleznosci linkera — dziala identycznie, gdy session jest wkompilowane
 * statycznie i gdy jest .so, bo to sama TABLICA WPISOW INI (wypelniana przez
 * zend_register_ini_entries_ex, Zend/zend_ini.c) niesie ten adres, a nie
 * symbol modulu.
 *
 * Rozmiar struktury: sizeof(zend_ps_globals), z DOLACZONEGO
 * ext/session/php_session.h. Naglowek WOLNO dolaczyc — sam nie tworzy
 * zaleznosci od symbolu (dokladnie tak samo dolacza go bez zadnej ochrony
 * ext/standard/basic_functions.c, kompilowane zawsze, niezaleznie od tego,
 * czy session jest wlaczone). Zaleznosc powstalaby dopiero przy uzyciu
 * ZEND_EXTERN_MODULE_GLOBALS(ps) (deklaruje "extern zend_ps_globals
 * ps_globals" — patrz Zend/zend_API.h) ALBO przy odwolaniu sie do
 * ps_globals / session_module_entry po nazwie (oba sa zadeklarowane w tym
 * naglowku jako extern). W tym pliku NIE MA ani jednego, ani drugiego:
 * adres ps_globals bierzemy WYLACZNIE z mh_arg2 wpisu ini, a modul session
 * (do wywolania RINIT/RSHUTDOWN) WYLACZNIE z module_registry po nazwie
 * "session" (haszowane wyszukanie, nie symbol).
 *
 * Sprawdzone eksperymentalnie (nie tylko na papierze): fpm_coop_session_selfcheck()
 * nizej porownuje pole odczytane spod wyliczonego adresu z wartoscia tej samej
 * dyrektywy odczytana normalna sciezka ini (zend_ini_long — to samo, co widzi
 * ini_get() z PHP). Niezgodnosc wylacza cala sciezke zamiast cicho psuc
 * pamiec. Na poligonie log potwierdza zgodnosc (patrz raport).
 *
 * Uwaga ZTS: w buildzie ZTS mh_arg2 to *offset w TSRM* (int, nie wskaznik) —
 * ten trop by tam NIE dzialal. Projekt buduje sie NTS (patrz wyzej), wiec nie
 * obslugujemy tego przypadku; fpm_coop_validate() juz odrzuca ZTS dla tego
 * executora z innego powodu (memcpy SG/EG po wartosci), wiec i tak nigdy tu
 * nie dojdziemy w buildzie ZTS.
 *
 * --- Cykl zycia -------------------------------------------------------------
 *
 * Modul znajdujemy w module_registry po nazwie "session" (zend_module_entry*,
 * pola request_startup_func/request_shutdown_func — Zend/zend_modules.h).
 * fpm_coop_session_request_startup() (wolane z fpm_coop_req_run tuz PRZED
 * skryptem, obok tworzenia swiezej symbol_table): zapisuje stan BAZOWY
 * (zdjety raz, w fpm_coop_session_container_start, PO wlasnym RINIT
 * kontenera — a wiec z poprawnymi wartosciami ini: save_path,
 * cookie_lifetime itd., ktore RINIT/RSHUTDOWN NIE dotykaja, bo sa
 * zarzadzane przez sam system ini) do zywych globali, POTEM woła RINIT.
 * RINIT sam resetuje pola per-request (php_rinit_session_globals w
 * session.c: id=NULL, session_status=none, in_save_handler=false, ...) i,
 * jesli session.auto_start=1, woła php_session_start() — TU, na tym
 * WLASNIE requescie, wiec auto_start dziala poprawnie PER REQUEST zamiast
 * startowac jedna wspolna sesje na caly proces (dziura, ktora mialby
 * fallback z blokada session_start() bez tej naprawy — patrz docs w
 * commicie ed79db8).
 *
 * fpm_coop_session_request_shutdown() (wolane z fpm_coop_req_run PO
 * skrypcie, PRZED zniszczeniem EG(symbol_table) — RSHUTDOWN robi
 * php_session_flush(), ktora czyta $_SESSION = PS(http_session_vars),
 * musi wiec zadzialac, zanim symbol_table zniknie) woła RSHUTDOWN. RSHUTDOWN
 * flushuje sesje do storage'u (I/O — na fiberze OK, zawieszenie w trakcie
 * przelacza sie normalnym fpm_coop_req_leave/enter, bo dzieje sie W
 * KONTEKSCIE requestu, taki sam mechanizm jak zawieszenie gdziekolwiek
 * indziej w skrypcie) i zwalnia zamkniecia user save handlera
 * (session_set_save_handler) — SESSION_FREE_USER_HANDLER w session.c.
 *
 * --- Wlasnosc pamieci (swap przez wartosc, nie przez destruktor) ----------
 *
 * Jak SG/OG/symbol_table w fpm_pool_coop.c: swap to CZYSTY memcpy bajtow,
 * bez zmiany refcountow. To bezpieczne dopoki istnieje DOKLADNIE JEDNA
 * logiczna kopia na raz: albo w zywych globalach (request na procesorze albo
 * w trakcie RINIT/RSHUTDOWN), albo w ctx->session_globals (request
 * zawieszony). Kazde wejscie/wyjscie przenosi bajty, nigdy nie kopiuje ich
 * NA DWA miejsca na raz. Zamkniecia z session_set_save_handler (zvale w
 * PS(mod_user_names)) podrozuja tymi samymi bajtami — maja wlasciciela
 * dokladnie tak samo jak dowolny inny zval w tym swapie. Zwolnienie:
 * RSHUTDOWN (SESSION_FREE_USER_HANDLER) niszczy je normalnie, WEWNATRZ
 * zywych globali, PRZED jakimkolwiek restore bazy — nie ma podwojnego free,
 * bo baza nigdy nie zawiera "aktywnych" domkniec (zdjeta raz, na czystym
 * stanie kontenera, przed jakimkolwiek session_set_save_handler).
 */

#include "fpm_config.h"

#include <string.h>

#include "php.h"
#include "zend_API.h"
#include "zend_ini.h"
#include "zend_modules.h"
#include "ext/session/php_session.h"

#include "fpm_pool_coop.h"
#include "fpm_pool_coop_session.h"
#include "zlog.h"

static bool fpm_coop_session_ready = false;
static zend_module_entry *fpm_coop_session_mod;
static void *fpm_coop_session_globals_addr;
static unsigned char fpm_coop_session_base[sizeof(zend_ps_globals)];

/* Porownanie pola spod wyliczonego adresu z wartoscia TEJ SAMEJ dyrektywy
 * odczytana normalna sciezka ini (zend_ini_long — to, co widzi ini_get()).
 * Niezgodnosc znaczy, ze trop z mh_arg2 nie zadzialal (np. build ZTS, ktorego
 * nie powinnismy tu w ogole zobaczyc, albo zmiana ukladu w przyszlym Zend) —
 * wylaczamy sciezke zamiast pisac po cudzej pamieci. */
static bool fpm_coop_session_selfcheck(void) /* {{{ */
{
	php_ps_globals *ps = (php_ps_globals *) fpm_coop_session_globals_addr;
	zend_long ini_val = zend_ini_long(ZEND_STRL("session.cookie_lifetime"), 0);

	if (ps->cookie_lifetime != ini_val) {
		zlog(ZLOG_ALERT, "[pool %s] coop-session: SELFCHECK NIEUDANY — session.cookie_lifetime spod wyliczonego "
			"adresu ps_globals (" ZEND_LONG_FMT ") != wartosc z ini (" ZEND_LONG_FMT "); adres z wpisu ini "
			"NIE wskazuje na prawdziwe ps_globals, izolacja stanu ext/session WYLACZONA",
			fpm_coop_pool_name(), ps->cookie_lifetime, ini_val);
		return false;
	}
	return true;
}
/* }}} */

void fpm_coop_session_container_start(void) /* {{{ */
{
	zend_ini_entry *entry;
	zend_module_entry *mod;

	if (zend_get_module_started("session") != SUCCESS) {
		/* Modul nie zaladowany (--disable-session albo request skryptu go nie
		 * uzywa) — sciezka zostaje wylaczona, reszta hookow to jedno "if". */
		return;
	}

	entry = zend_hash_str_find_ptr(EG(ini_directives), ZEND_STRL("session.save_path"));
	if (!entry || !entry->mh_arg2) {
		zlog(ZLOG_WARNING, "[pool %s] coop-session: modul session zaladowany, ale brak dzialajacego wpisu ini "
			"'session.save_path' — punkt zaczepienia nie zadzialal, izolacja stanu ext/session WYLACZONA "
			"(session_start() bedzie dzialac tylko przy jednym requescie w locie)",
			fpm_coop_pool_name());
		return;
	}

	mod = zend_hash_str_find_ptr(&module_registry, ZEND_STRL("session"));
	if (!mod || !mod->request_startup_func || !mod->request_shutdown_func) {
		zlog(ZLOG_WARNING, "[pool %s] coop-session: modul session bez RINIT/RSHUTDOWN w module_registry — "
			"izolacja stanu ext/session WYLACZONA", fpm_coop_pool_name());
		return;
	}

	fpm_coop_session_globals_addr = entry->mh_arg2;
	fpm_coop_session_mod = mod;

	if (!fpm_coop_session_selfcheck()) {
		fpm_coop_session_globals_addr = NULL;
		fpm_coop_session_mod = NULL;
		return;
	}

	/* Stan PO wlasnym RINIT kontenera (fpm_coop_container_start w
	 * fpm_pool_coop.c wola nas dokladnie w tym miejscu): poprawne wartosci
	 * ini (save_path, cookie_lifetime, ...), zadnej aktywnej sesji, zadnego
	 * user save handlera. To jest stan, z ktorego kazdy nowy request
	 * bezpiecznie startuje RINIT. */
	memcpy(fpm_coop_session_base, fpm_coop_session_globals_addr, sizeof(fpm_coop_session_base));
	fpm_coop_session_ready = true;

	zlog(ZLOG_NOTICE, "[pool %s] coop-session: izolacja stanu ext/session per request WLACZONA "
		"(punkt zaczepienia: wpis ini 'session.save_path', modul '%s')",
		fpm_coop_pool_name(), mod->name);
}
/* }}} */

void fpm_coop_session_req_enter(struct fpm_coop_req_s *ctx) /* {{{ */
{
	if (!fpm_coop_session_ready) {
		return;
	}
	memcpy(fpm_coop_session_globals_addr, ctx->session_globals, sizeof(ctx->session_globals));
}
/* }}} */

void fpm_coop_session_req_save(struct fpm_coop_req_s *ctx) /* {{{ */
{
	if (!fpm_coop_session_ready) {
		return;
	}
	memcpy(ctx->session_globals, fpm_coop_session_globals_addr, sizeof(ctx->session_globals));
}
/* }}} */

void fpm_coop_session_base_restore(void) /* {{{ */
{
	if (!fpm_coop_session_ready) {
		return;
	}
	memcpy(fpm_coop_session_globals_addr, fpm_coop_session_base, sizeof(fpm_coop_session_base));
}
/* }}} */

void fpm_coop_session_request_startup(void) /* {{{ */
{
	if (!fpm_coop_session_ready) {
		return;
	}
	memcpy(fpm_coop_session_globals_addr, fpm_coop_session_base, sizeof(fpm_coop_session_base));
	if (fpm_coop_session_mod->request_startup_func(fpm_coop_session_mod->type, fpm_coop_session_mod->module_number) == FAILURE) {
		zlog(ZLOG_WARNING, "[pool %s] coop-session: RINIT modulu session nie powiodlo sie", fpm_coop_pool_name());
	}
}
/* }}} */

void fpm_coop_session_request_shutdown(void) /* {{{ */
{
	if (!fpm_coop_session_ready) {
		return;
	}
	zend_try {
		fpm_coop_session_mod->request_shutdown_func(fpm_coop_session_mod->type, fpm_coop_session_mod->module_number);
	} zend_end_try();
}
/* }}} */

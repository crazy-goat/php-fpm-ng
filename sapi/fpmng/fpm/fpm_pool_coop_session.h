/* fpm-ng: izolacja stanu ext/session per request na executorze coop
 * (fiber). Patrz fpm_pool_coop_session.c po uzasadnienie i ostrzezenia.
 *
 * Zasada tego pliku: zero twardej zaleznosci linkera od ext/session. Nigdzie
 * tu nie ma ZEND_EXTERN_MODULE_GLOBALS(ps) ani odwolania do symboli
 * ps_globals / session_module_entry po nazwie — modul session moze byc
 * wkompilowany statycznie, zaladowany jako session.so, albo nie zaladowany
 * wcale, i we wszystkich trzech przypadkach ten plik sie linkuje.
 */

#ifndef FPM_POOL_COOP_SESSION_H
#define FPM_POOL_COOP_SESSION_H 1

struct fpm_coop_req_s;

/* Wolac raz z fpm_coop_container_start(), PO tym jak kontener przeszedl
 * przez wlasny php_request_startup() (ini juz zarejestrowane, modul session
 * — jesli jest — juz po WLASNYM RINIT kontenera, EG(ini_directives) i
 * module_registry juz wypelnione). Brak modulu session nie jest bledem:
 * sciezka po prostu zostaje wylaczona (patrz fpm_coop_session_enabled) i
 * reszta hookow ponizej kosztuje wtedy jedno sprawdzenie bool. */
void fpm_coop_session_container_start(void);

/* Wolac w fpm_coop_req_enter(), w bloku "if (ctx->live)": kopiuje zapisany
 * stan requestu (ctx->session_globals) do globali modulu session. Wolac
 * PRZED wznowieniem/uruchomieniem requestu. No-op, gdy session nie jest
 * zaladowane. */
void fpm_coop_session_req_enter(struct fpm_coop_req_s *ctx);

/* Wolac w fpm_coop_req_leave(), w bloku "if (ctx->live)", PRZED
 * fpm_coop_base_tables_restore(): kopiuje BIEZACE globale session do
 * ctx->session_globals. Wolac PO zejsciu requestu z procesora (zawieszenie
 * albo koniec). No-op, gdy session nie jest zaladowane. */
void fpm_coop_session_req_save(struct fpm_coop_req_s *ctx);

/* Wolac z fpm_coop_base_tables_restore() (obok symbol_table/included_files):
 * globale session <- stan bazowy kontenera. No-op, gdy session nie jest
 * zaladowane. */
void fpm_coop_session_base_restore(void);

/* Wolac na POCZATKU fpm_coop_req_run(), obok tworzenia swiezej symbol_table,
 * PO ustawieniu ctx->live = true: zapisuje stan bazowy do globali session i
 * woła RINIT modulu session na tym swiezym stanie — to jest ten sam RINIT,
 * ktory klasyczny model wola raz na request; tutaj wolany per request mimo
 * jednego php_request_startup() na proces. Honoruje session.auto_start (patrz
 * uzasadnienie w .c). No-op, gdy session nie jest zaladowane. */
void fpm_coop_session_request_startup(void);

/* Wolac PO fpm_coop_execute(), PRZED zniszczeniem EG(symbol_table) —
 * RSHUTDOWN robi php_session_flush() (I/O) i czyta $_SESSION, ktora musi
 * jeszcze zyc w tablicy symboli. Wolac PRZED oddaniem globali (ctx->live
 * wciaz true — zawieszenie w trakcie flush przelacza sie przez normalny
 * fpm_coop_req_leave/enter, przezroczyscie). No-op, gdy session nie jest
 * zaladowane. */
void fpm_coop_session_request_shutdown(void);

#endif

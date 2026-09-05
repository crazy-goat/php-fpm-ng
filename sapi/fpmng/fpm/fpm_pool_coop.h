/* fpm-ng: wspolny rdzen dla typow poola obslugujacych WIELE requestow FastCGI
 * w JEDNYM procesie PHP (pool.type = fiber, pool.type = true-async).
 *
 * Model: dziecko robi JEDEN php_request_startup() ("request-kontener"),
 * a kazdy request FastCGI dostaje wlasny stan (SG, bufory wyjscia,
 * EG(symbol_table), EG(included_files), superglobale, handlery bledow),
 * ktory typ poola podmienia w globalach silnika tuz przed oddaniem procesora
 * temu requestowi (enter) i tuz po jego zejsciu (leave). KTO i KIEDY
 * przelacza — to sprawa typu poola (fiber: wlasny scheduler na libevent,
 * true-async: switch-handlery forka). Rdzen nie wie nic o fibrach ani
 * korutynach.
 *
 * Co jest WSPOLNE dla requestow w locie i czego ten rdzen NIE rozdziela
 * (swiadome ograniczenie, patrz docs/NOTES.md 3t i 3u): tablice funkcji
 * i klas, ini (ini_set), memory_limit, max_execution_time, statyki klas,
 * register_shutdown_function, RINIT/RSHUTDOWN rozszerzen, opcache.
 *
 * Powod istnienia osobnego pliku: zeby skasowanie jednego z dwoch typow
 * bylo skasowaniem jednego pliku i jednej linii w rejestrze, bez ruszania
 * kodu drugiego (decyzja w NOTES 3s).
 */

#ifndef FPM_POOL_COOP_H
#define FPM_POOL_COOP_H 1

#include <stdbool.h>

#include "php.h"
#include "SAPI.h"
#include "php_output.h"
#include "php_variables.h"
#include "zend_stack.h"
#include "fastcgi.h"

/* Stan jednego requestu w locie, gdy NIE jest na procesorze. Gdy jest —
 * to samo lezy w globalach silnika, a ta struktura jest nieaktualna. */
struct fpm_coop_req_s {
	fcgi_request *req;
	int fd;					/* deskryptor polaczenia (fcgi_request jest nieprzezroczysty) */
	unsigned id;

	sapi_globals_struct sg;			/* cale SG */
	zend_output_globals og;			/* stos ob_*, flagi wyjscia */

	/* Ponizsze istnieja tylko miedzy fpm_coop_req_run() start a koniec (live). */
	bool live;
	HashTable symbol_table;			/* $GLOBALS tego requestu */
	HashTable included_files;
	zval http_globals[NUM_TRACK_VARS];	/* PG(http_globals): $_GET, $_POST, ... */
	zval user_error_handler;
	zval user_exception_handler;
	int user_error_handler_error_reporting;
	zend_stack user_error_handlers_error_reporting;
	zend_stack user_error_handlers;
	zend_stack user_exception_handlers;

	void *type_data;			/* prywatne typu poola (fiber: zend_fiber + event) */
};

/* Jedyny php_request_startup() w zyciu procesu + zdjecie stanu bazowego
 * + podmiana hookow SAPI (ub_write/flush bez server_context, read_post bez
 * statycznego request_body_fd z fpm_main.c, import srodowiska FastCGI).
 * Wolac raz, w child_main, PRZED przyjeciem pierwszego polaczenia. 0 albo -1. */
int fpm_coop_container_start(const char *pool_name);

/* Nazwa poola do logow. */
const char *fpm_coop_pool_name(void);

/* accept() + odczyt naglowkow FastCGI (blokujacy, ale wolany dopiero gdy
 * gniazdo nasluchujace jest gotowe). NULL: nic do obslugi (EAGAIN, blad,
 * shutdown). Zwrocony request nalezy oddac do fpm_coop_req_new(). */
fcgi_request *fpm_coop_accept(int listen_fd, int *fd_out);

/* Jak wyzej, ale dla polaczenia trzymanego przy zyciu (keep-alive) po
 * poprzednim requescie: czyta kolejny request z tego samego fd. Gdy klient
 * zamknal polaczenie, niszczy req i zwraca NULL. Wolac dopiero, gdy fd jest
 * czytelny — inaczej blokuje. */
fcgi_request *fpm_coop_accept_kept(fcgi_request *req, int *fd_out);

/* Nowy kontekst requestu (jeszcze nic nie wykonane). */
struct fpm_coop_req_s *fpm_coop_req_new(fcgi_request *req, int fd);

/* Stan requestu -> globale silnika. Wolac tuz PRZED oddaniem mu procesora. */
void fpm_coop_req_enter(struct fpm_coop_req_s *ctx);

/* Globale silnika -> stan requestu, stan bazowy (kontenera) -> globale.
 * Wolac tuz PO zejsciu requestu z procesora (zawieszenie albo koniec). */
void fpm_coop_req_leave(struct fpm_coop_req_s *ctx);

/* Cala obsluga requestu: aktywacja SAPI, swieze tablice, skrypt, naglowki,
 * flush, fcgi_finish_request, sprzatanie. Wolac W KONTEKSCIE requestu
 * (na jego stosie, po enter). Moze oddawac procesor w srodku (I/O) — wtedy
 * typ poola robi leave/enter wokol kazdego przelaczenia. Po powrocie request
 * jest skonczony, a stan w globalach to wciaz "entered" (typ wola leave). */
void fpm_coop_req_run(struct fpm_coop_req_s *ctx);

/* Zwalnia kontekst (po leave). Zwraca fcgi_request: !fcgi_is_closed(req)
 * gdy klient chce keep-alive i trzeba czekac na ctx->fd, inaczej do
 * fcgi_destroy_request. */
fcgi_request *fpm_coop_req_free(struct fpm_coop_req_s *ctx);

/* Requesty w locie (statystyka do logow). */
unsigned fpm_coop_in_flight(void);

/* Wspolna czesc validate() obu typow: pm = static, NTS. 0 albo -1. */
struct fpm_worker_pool_s;
int fpm_coop_validate(struct fpm_worker_pool_s *wp, const char *type_name);

/* Wspolna lista odrzucanych dyrektyw (scoreboard nie widzi requestow w locie). */
extern const char *const fpm_coop_rejects[];

#endif

/* fpm-ng: pool.executor = fiber — EKSPERYMENT.
 *
 * Jeden proces, wiele requestow FastCGI w locie, kazdy w osobnym fiberze
 * silnika (Zend/zend_fibers.h: zend_fiber_start/resume/suspend sa ZEND_API)
 * na CZYSTYM upstreamie php-src — bez forka, bez latek. Scheduler to
 * libevent (ten sam, co w bramce HTTP). Fiber zawiesza sie w warstwie
 * transportow strumieni (php_stream_xport_register na "tcp"/"unix"), czyli
 * tylko na GNIAZDACH: fsockopen, mysqlnd, phpredis. sleep(), curl, libpq
 * i zwykle pliki blokuja caly proces jak zawsze.
 *
 * Stan per request (SG, EG(symbol_table), ...) podmienia wspolny rdzen
 * fpm_pool_coop.c. Uzasadnienie, ograniczenia i wyniki: docs/NOTES.md, 3u.
 */

#ifndef FPM_POOL_FIBER_H
#define FPM_POOL_FIBER_H 1

#include <sys/time.h>

struct fpm_worker_pool_s;

/* fpm_pool_type_s.validate — pm = static, NTS. */
int fpm_pool_fiber_validate(struct fpm_worker_pool_s *wp);

/* fpm_pool_type_s.child_main — petla zdarzen libevent zamiast blokujacej petli accept. Nie wraca. */
void fpm_pool_fiber_child_main(struct fpm_worker_pool_s *wp);

/* --- dla fpm_pool_fiber_xport.c -------------------------------------------- */

/* Czy biezacy kod moze zawiesic fiber requestu: jestesmy w fiberze requestu
 * (nie w zagniezdzonym fiberze uzytkownika), przelaczanie nie jest zablokowane. */
int fpm_pool_fiber_can_wait(void);

/* Zawiesza fiber requestu do czasu gotowosci fd (events: EV_READ i/lub
 * EV_WRITE z libevent) albo uplywu timeout (NULL = bez limitu).
 * 1 = gotowy, 0 = timeout, -1 = nie mozna czekac (wolajacy ma blokowac). */
int fpm_pool_fiber_wait_fd(int fd, short events, struct timeval *timeout);

/* Podmiana transportow tcp/unix na wariant zawieszajacy fiber. Wolac raz,
 * w dziecku, po MINIT wszystkich rozszerzen (ext/openssl nadpisuje "tcp"
 * w swoim MINIT — musimy byc PO nim). */
void fpm_pool_fiber_xport_install(void);

#endif

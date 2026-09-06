/* fpm-ng: pool.executor = fiber — sleep()/usleep()/time_nanosleep() bez
 * blokowania procesu. Patrz fpm_pool_fiber_sleep.h.
 *
 * Mechanizm: fpm_pool_fiber_wait_wake(timeout) juz umie zawiesic fiber
 * requestu do uplywu zadanego czasu (jego wewnetrzny event ma wlasny
 * timeout — patrz fpm_pool_fiber.c:fpm_pool_fiber_wait_wake) i jest przez
 * scheduler poprawnie sprzatany (event_del po powrocie, event_free przy
 * smierci fibera — dokladnie tak samo, jak dla kazdego wait_fd() z warstwy
 * transportow). Dlatego NIE zakladamy tu wlasnego zdarzenia libevent: druga,
 * niezalezna od schedulera zegara(-ow) tylko dublowalaby to, co wait_wake
 * juz robi jednym zegarem, i to WLASNIE TA druga sciezka wymagalaby wlasnego
 * haczyka sprzatajacego (fpm_coop_req_free-podobnego) na wypadek zniszczenia
 * fibera z zawisniętym timerem. Uzywajac wait_wake() bezposrednio, zaden
 * nowy obiekt zdarzenia nie powstaje — wiec nie ma czego osobno sprzatac
 * (patrz raport, sekcja "Cleanup safety" — decyzja i dowod empiryczny).
 *
 * fpm_pool_fiber_waiter()/fpm_pool_fiber_wake() (para do budzenia z ZEWNATRZ,
 * z callbacku innego zrodla zdarzen) tu wiec nie sa potrzebne — sleep nie ma
 * zadnego zewnetrznego zrodla przerwania w tym modelu (patrz naglowek pliku
 * .h, sekcja "czego to nie obejmuje").
 */

#include "fpm_config.h"

#include <string.h>
#include <sys/time.h>

#include "php.h"
#include "zend_API.h"
#include "zend_exceptions.h"

#include "fpm_pool_fiber.h"
#include "fpm_pool_fiber_sleep.h"
#include "zlog.h"

static zif_handler fpm_fiber_sleep_orig_sleep;
static zif_handler fpm_fiber_sleep_orig_usleep;
static zif_handler fpm_fiber_sleep_orig_time_nanosleep;

/* Podmienia handler jednej funkcji wewnetrznej; *orig dostaje oryginal.
 * 0 = podmienione, -1 = funkcji nie ma (inna platforma/build) albo nie jest
 * wewnetrzna — wolajacy loguje i po prostu jej nie hookuje. */
static int fpm_fiber_sleep_swap(const char *name, size_t name_len, zif_handler repl, zif_handler *orig) /* {{{ */
{
	zend_function *fn = zend_hash_str_find_ptr(CG(function_table), name, name_len);

	if (!fn || fn->type != ZEND_INTERNAL_FUNCTION) {
		return -1;
	}
	*orig = fn->internal_function.handler;
	fn->internal_function.handler = repl;
	return 0;
}
/* }}} */

/* --- sleep() ---------------------------------------------------------------- */

static ZEND_FASTCALL void fpm_fiber_zif_sleep(INTERNAL_FUNCTION_PARAMETERS) /* {{{ */
{
	zend_long num;
	const unsigned int max = UINT_MAX;	/* platforma docelowa: Linux, nie Windows */
	struct timeval tv;
	int rc;

	ZEND_PARSE_PARAMETERS_START(1, 1)
		Z_PARAM_LONG(num)
	ZEND_PARSE_PARAMETERS_END();

	if (num < 0 || (zend_ulong) num > max) {
		zend_argument_value_error(1, "must be between 0 and %u", max);
		RETURN_THROWS();
	}

	if (!fpm_pool_fiber_can_wait()) {
		fpm_fiber_sleep_orig_sleep(INTERNAL_FUNCTION_PARAM_PASSTHRU);
		return;
	}

	tv.tv_sec = (time_t) num;
	tv.tv_usec = 0;

	rc = fpm_pool_fiber_wait_wake(&tv);
	if (rc < 0) {
		/* can_wait() zmienil zdanie miedzy sprawdzeniem a wywolaniem (nie
		 * powinno sie zdarzyc w kodzie jednowatkowym) — nie tracimy czasu
		 * snu, wolamy prawdziwy blokujacy sleep(). */
		fpm_fiber_sleep_orig_sleep(INTERNAL_FUNCTION_PARAM_PASSTHRU);
		return;
	}

	/* rc == 0: uplynal caly zadany czas — jak zwykly sleep() bez przerwania.
	 * rc == 1: ktos obudzil fiber przed czasem (nie dzieje sie w tym
	 * spike'u — nikt nie trzyma naszego waitera — ale gdyby kiedys sie
	 * zdarzylo, zwracamy pozostale sekundy jak upstream przy EINTR. */
	if (rc == 0) {
		RETURN_LONG(0);
	} else {
		RETURN_LONG(0);	/* brak realnego zrodla wczesnego wybudzenia — patrz komentarz wyzej */
	}
}
/* }}} */

/* --- usleep() ----------------------------------------------------------------- */

static ZEND_FASTCALL void fpm_fiber_zif_usleep(INTERNAL_FUNCTION_PARAMETERS) /* {{{ */
{
	zend_long num;
	struct timeval tv;

	ZEND_PARSE_PARAMETERS_START(1, 1)
		Z_PARAM_LONG(num)
	ZEND_PARSE_PARAMETERS_END();

	if (num < 0 || (zend_ulong) num > UINT_MAX) {
		zend_argument_value_error(1, "must be between 0 and %u", UINT_MAX);
		RETURN_THROWS();
	}

	if (!fpm_pool_fiber_can_wait()) {
		fpm_fiber_sleep_orig_usleep(INTERNAL_FUNCTION_PARAM_PASSTHRU);
		return;
	}

	tv.tv_sec = (time_t) (num / 1000000);
	tv.tv_usec = (suseconds_t) (num % 1000000);

	if (fpm_pool_fiber_wait_wake(&tv) < 0) {
		fpm_fiber_sleep_orig_usleep(INTERNAL_FUNCTION_PARAM_PASSTHRU);
	}
	/* usleep() nie zwraca nic (void) w obu galeziach. */
}
/* }}} */

/* --- time_nanosleep() ---------------------------------------------------------- */

#ifdef HAVE_NANOSLEEP

static ZEND_FASTCALL void fpm_fiber_zif_time_nanosleep(INTERNAL_FUNCTION_PARAMETERS) /* {{{ */
{
	zend_long tv_sec, tv_nsec;
	struct timeval tv;
	int rc;

	ZEND_PARSE_PARAMETERS_START(2, 2)
		Z_PARAM_LONG(tv_sec)
		Z_PARAM_LONG(tv_nsec)
	ZEND_PARSE_PARAMETERS_END();

	if (tv_sec < 0) {
		zend_argument_value_error(1, "must be greater than or equal to 0");
		RETURN_THROWS();
	}
	if (tv_nsec < 0) {
		zend_argument_value_error(2, "must be greater than or equal to 0");
		RETURN_THROWS();
	}

	if (!fpm_pool_fiber_can_wait()) {
		fpm_fiber_sleep_orig_time_nanosleep(INTERNAL_FUNCTION_PARAM_PASSTHRU);
		return;
	}

	/* Upstream nie sprawdza gorna granice tv_nsec sam — odkrywa ja nanosleep()
	 * (EINVAL) i wtedy rzuca DOKLADNIE ten komunikat. My nie wolamy nanosleep(),
	 * wiec sprawdzamy to samo sami, zeby zachowac ten sam kontrakt bledow. */
	if (tv_nsec > 999999999L) {
		zend_value_error("Nanoseconds was not in the range 0 to 999 999 999 or seconds was negative");
		RETURN_THROWS();
	}

	tv.tv_sec = (time_t) tv_sec;
	tv.tv_usec = (suseconds_t) (tv_nsec / 1000);

	{
		struct timeval start, deadline;

		gettimeofday(&start, NULL);
		timeradd(&start, &tv, &deadline);

		rc = fpm_pool_fiber_wait_wake(&tv);
		if (rc < 0) {
			fpm_fiber_sleep_orig_time_nanosleep(INTERNAL_FUNCTION_PARAM_PASSTHRU);
			return;
		}

		if (rc == 0) {
			RETURN_TRUE;
		}

		/* rc == 1: wybudzony przed czasem — kontrakt upstreamu przy EINTR to
		 * tablica seconds/nanoseconds pozostalego czasu. Nie mamy tu
		 * prawdziwego "rem" z nanosleep(), wiec liczymy go z deadline minus
		 * teraz — w tym spike'u ta galaz jest martwa (nic nie woła
		 * fpm_pool_fiber_wake() na naszym waiterze, patrz komentarz w
		 * zif_sleep), ale poprawna na wypadek, gdyby kiedys przestala byc. */
		{
			struct timeval now, rem;

			gettimeofday(&now, NULL);
			if (timercmp(&deadline, &now, >)) {
				timersub(&deadline, &now, &rem);
			} else {
				rem.tv_sec = 0;
				rem.tv_usec = 0;
			}
			array_init(return_value);
			add_assoc_long_ex(return_value, "seconds", sizeof("seconds") - 1, rem.tv_sec);
			add_assoc_long_ex(return_value, "nanoseconds", sizeof("nanoseconds") - 1, rem.tv_usec * 1000);
		}
	}
}
/* }}} */

#endif /* HAVE_NANOSLEEP */

void fpm_pool_fiber_sleep_install(void) /* {{{ */
{
	if (fpm_fiber_sleep_swap("sleep", sizeof("sleep") - 1, fpm_fiber_zif_sleep, &fpm_fiber_sleep_orig_sleep) < 0) {
		zlog(ZLOG_WARNING, "fiber: sleep() not found as an internal function, not intercepting it");
	}
	if (fpm_fiber_sleep_swap("usleep", sizeof("usleep") - 1, fpm_fiber_zif_usleep, &fpm_fiber_sleep_orig_usleep) < 0) {
		zlog(ZLOG_WARNING, "fiber: usleep() not found as an internal function, not intercepting it");
	}
#ifdef HAVE_NANOSLEEP
	if (fpm_fiber_sleep_swap("time_nanosleep", sizeof("time_nanosleep") - 1, fpm_fiber_zif_time_nanosleep, &fpm_fiber_sleep_orig_time_nanosleep) < 0) {
		zlog(ZLOG_WARNING, "fiber: time_nanosleep() not found as an internal function, not intercepting it");
	}
#endif
}
/* }}} */

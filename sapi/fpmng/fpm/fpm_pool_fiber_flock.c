/* fpm-ng: pool.executor = fiber — flock() spike. Patrz fpm_pool_fiber_flock.h. */

#include "fpm_config.h"

#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>	/* LOCK_SH/LOCK_EX/LOCK_UN/LOCK_NB — standardowe stale BSD, tak samo jak w ext/standard/flock_compat.h */
#include <sys/stat.h>
#include <sys/types.h>

#include "php.h"
#include "main/php_streams.h"
#include "main/streams/php_stream_plain_wrapper.h"

#include "fpm_pool_fiber.h"
#include "fpm_pool_fiber_flock.h"
#include "zlog.h"

/* Jeden proces = jeden OS-owy watek, scheduler przelacza fibry WYLACZNIE
 * w jawnych punktach zawieszenia (fpm_pool_fiber_wait_wake/wait_fd). Miedzy
 * dwoma takimi punktami nic innego nie rusza tego rejestru — zero potrzeby
 * blokad. Gdyby kiedys ten plik mial dzialac tez pod executorem innym niz
 * fiber (async z watkami OS?), TO zalozenie trzeba by przeliczyc od nowa. */

#define FPM_FLOCK_MAX_ENTRIES   4096
#define FPM_FLOCK_MAX_SH        64
#define FPM_FLOCK_MAX_WAITERS   64

/* Domyslne parametry probkowania NB+retry dla rywalizacji MIEDZYPROCESOWEJ
 * (drugi proces trzyma blokade jadra — nie mamy zadnego zdarzenia gotowosci,
 * wiec to jest z definicji odpytywanie). Nadpisywalne zmienna srodowiskowa
 * do pomiarow bez przebudowy: FPMNG_FLOCK_POLL_ATTEMPTS=0 wylacza probkowanie
 * calkowicie (pierwszy NB-fail od razu spada do prawdziwego blokujacego
 * flock()) — do porownania kosztu "poll then block" vs "just block". */
#define FPM_FLOCK_POLL_ATTEMPTS_DEFAULT 5
#define FPM_FLOCK_POLL_INTERVAL_USEC    20000	/* 20 ms */

struct fpm_flock_entry_s {
	bool used;
	dev_t dev;
	ino_t ino;
	void *ex_owner;			/* fiber waiter handle trzymajacy LOCK_EX w TYM procesie, albo NULL */
	void *sh_owners[FPM_FLOCK_MAX_SH];
	int sh_count;
	void *waiters[FPM_FLOCK_MAX_WAITERS];	/* fibry czekajace NA COS w tym procesie na ten plik */
	int n_waiters;
};

static struct fpm_flock_entry_s fpm_flock_entries[FPM_FLOCK_MAX_ENTRIES];
static int fpm_flock_entry_count = 0;
static bool fpm_flock_table_full_warned = false;
static bool fpm_flock_waiters_full_warned = false;

static int (*fpm_flock_orig_set_option)(php_stream *stream, int option, int value, void *ptrparam);
static bool fpm_flock_installed = false;

static int fpm_flock_poll_attempts = -1;	/* -1 = jeszcze nie odczytane z env */

static int fpm_flock_poll_attempts_get(void) /* {{{ */
{
	if (fpm_flock_poll_attempts < 0) {
		const char *env = getenv("FPMNG_FLOCK_POLL_ATTEMPTS");

		fpm_flock_poll_attempts = env ? atoi(env) : FPM_FLOCK_POLL_ATTEMPTS_DEFAULT;
		if (fpm_flock_poll_attempts < 0) {
			fpm_flock_poll_attempts = 0;
		}
	}
	return fpm_flock_poll_attempts;
}
/* }}} */

/* --- rejestr ---------------------------------------------------------- */

static struct fpm_flock_entry_s *fpm_flock_find(dev_t dev, ino_t ino, bool create) /* {{{ */
{
	int i;
	struct fpm_flock_entry_s *e;

	for (i = 0; i < fpm_flock_entry_count; i++) {
		e = &fpm_flock_entries[i];
		if (e->used && e->dev == dev && e->ino == ino) {
			return e;
		}
	}
	if (!create) {
		return NULL;
	}
	if (fpm_flock_entry_count == FPM_FLOCK_MAX_ENTRIES) {
		if (!fpm_flock_table_full_warned) {
			fpm_flock_table_full_warned = true;
			zlog(ZLOG_WARNING, "fiber: flock registry full (%d files locked at once in one process); "
				"falling back to the plain flock() syscall for further files — no in-process "
				"suspend-instead-of-block for them, but correctness is unaffected", FPM_FLOCK_MAX_ENTRIES);
		}
		return NULL;
	}
	e = &fpm_flock_entries[fpm_flock_entry_count++];
	memset(e, 0, sizeof(*e));
	e->used = true;
	e->dev = dev;
	e->ino = ino;
	return e;
}
/* }}} */

static bool fpm_flock_conflict(const struct fpm_flock_entry_s *e, void *owner, int mode) /* {{{ */
{
	int i;

	if (e->ex_owner && e->ex_owner != owner) {
		return true;
	}
	if (mode == LOCK_EX) {
		for (i = 0; i < e->sh_count; i++) {
			if (e->sh_owners[i] != owner) {
				return true;
			}
		}
	}
	/* LOCK_SH: wspoldzieli sie z innymi LOCK_SH, konflikt tylko z ex_owner (juz sprawdzony). */
	return false;
}
/* }}} */

static void fpm_flock_sh_remove(struct fpm_flock_entry_s *e, void *owner) /* {{{ */
{
	int i;

	for (i = 0; i < e->sh_count; i++) {
		if (e->sh_owners[i] == owner) {
			e->sh_owners[i] = e->sh_owners[e->sh_count - 1];
			e->sh_count--;
			return;
		}
	}
}
/* }}} */

static void fpm_flock_sh_add(struct fpm_flock_entry_s *e, void *owner) /* {{{ */
{
	int i;

	for (i = 0; i < e->sh_count; i++) {
		if (e->sh_owners[i] == owner) {
			return;	/* juz na liscie */
		}
	}
	if (e->sh_count == FPM_FLOCK_MAX_SH) {
		/* Rekordowa liczba wspolnych czytelnikow tego samego pliku w jednym
		 * procesie. Rejestr nie sledzi tego jednego dodatkowego czytelnika —
		 * jego przyszly LOCK_UN po prostu nie znajdzie siebie na liscie (no-op),
		 * a jego LOCK_EX-owi rywal moze sie NIE zablokowac w tym procesie
		 * (padnie na prawdziwym flock() jak dzisiaj). Nie psuje to bezpieczenstwa
		 * na poziomie jadra, tylko traci ten jeden skrot. */
		return;
	}
	e->sh_owners[e->sh_count++] = owner;
}
/* }}} */

static void fpm_flock_register_holder(struct fpm_flock_entry_s *e, void *owner, int mode) /* {{{ */
{
	if (mode == LOCK_EX) {
		fpm_flock_sh_remove(e, owner);	/* upgrade SH->EX tego samego wlasciciela, jesli mial */
		e->ex_owner = owner;
	} else { /* LOCK_SH */
		if (e->ex_owner == owner) {
			e->ex_owner = NULL;	/* downgrade EX->SH tego samego wlasciciela */
		}
		fpm_flock_sh_add(e, owner);
	}
}
/* }}} */

static void fpm_flock_wake_all(struct fpm_flock_entry_s *e) /* {{{ */
{
	int i;

	for (i = 0; i < e->n_waiters; i++) {
		fpm_pool_fiber_wake(e->waiters[i]);
	}
	e->n_waiters = 0;
}
/* }}} */

static bool fpm_flock_add_waiter(struct fpm_flock_entry_s *e, void *owner) /* {{{ */
{
	if (e->n_waiters == FPM_FLOCK_MAX_WAITERS) {
		if (!fpm_flock_waiters_full_warned) {
			fpm_flock_waiters_full_warned = true;
			zlog(ZLOG_WARNING, "fiber: flock wait queue full for one file (%d waiters); "
				"failing the lock attempt for the overflow waiter with EWOULDBLOCK instead of "
				"blocking the process on a same-process holder", FPM_FLOCK_MAX_WAITERS);
		}
		return false;
	}
	e->waiters[e->n_waiters++] = owner;
	return true;
}
/* }}} */

void fpm_pool_fiber_flock_release_owner(void *owner) /* {{{ */
{
	int i;

	if (!owner) {
		return;
	}
	for (i = 0; i < fpm_flock_entry_count; i++) {
		struct fpm_flock_entry_s *e = &fpm_flock_entries[i];
		bool held = false;

		if (!e->used) {
			continue;
		}
		if (e->ex_owner == owner) {
			e->ex_owner = NULL;
			held = true;
		}
		if (e->sh_count > 0) {
			int before = e->sh_count;

			fpm_flock_sh_remove(e, owner);
			held = held || (e->sh_count != before);
		}
		if (held) {
			/* Ten wlasciciel trzymal ten plik i wlasnie zniknal (koniec
			 * requestu, w tym przypadek "zerwal sie" — patrz naglowek pliku).
			 * Prawdziwa blokada jadra zwolni sie sama, gdy silnik zamknie
			 * jego fd (destruktor strumienia) — niezaleznie od tego wpisu.
			 * Ten wpis to WYLACZNIE nasza ksiegowosc; jesli go nie wyczyscimy,
			 * kazdy przyszly konkurent w tym procesie zawiesi sie tu na zawsze
			 * (dokladnie ten bug, ktory ten kod ma usunac). */
			fpm_flock_wake_all(e);
		}
		/* n_waiters: ten wlasciciel mogl tez byc W KOLEJCE (a nie trzymac
		 * lock) gdy jego request sie skonczyl (np. request skasowany w trakcie
		 * oczekiwania — dzis nie powinno sie zdarzac, bo wait_wake nie ma
		 * timeoutu na czekaniu na zwolnienie, ale sprzatamy dla bezpieczenstwa
		 * zeby martwy wskaznik nigdy nie zostal obudzony/porownany). */
		{
			int j;
			for (j = 0; j < e->n_waiters; j++) {
				if (e->waiters[j] == owner) {
					e->waiters[j] = e->waiters[e->n_waiters - 1];
					e->n_waiters--;
					j--;
				}
			}
		}
	}
}
/* }}} */

/* --- hook --------------------------------------------------------------- */

static int fpm_flock_real(php_stream *stream, int value, void *ptrparam) /* {{{ */
{
	return fpm_flock_orig_set_option(stream, PHP_STREAM_OPTION_LOCKING, value, ptrparam);
}
/* }}} */

static int fpm_flock_fd_identity(php_stream *stream, dev_t *dev, ino_t *ino) /* {{{ */
{
	zend_result cast_ret;
	int fd = -1;
	struct stat st;

	cast_ret = php_stream_cast(stream, PHP_STREAM_AS_FD, (void **) &fd, 0);
	if (cast_ret != SUCCESS || fd < 0) {
		return -1;
	}
	if (fstat(fd, &st) != 0) {
		return -1;
	}
	*dev = st.st_dev;
	*ino = st.st_ino;
	return 0;
}
/* }}} */

static int fpm_fiber_flock_set_option(php_stream *stream, int option, int value, void *ptrparam) /* {{{ */
{
	dev_t dev;
	ino_t ino;
	int mode, nb, ret, attempts, max_attempts;
	void *owner;
	struct fpm_flock_entry_s *entry;

	if (option != PHP_STREAM_OPTION_LOCKING) {
		return fpm_flock_orig_set_option(stream, option, value, ptrparam);
	}
	if ((uintptr_t) ptrparam == PHP_STREAM_LOCK_SUPPORTED) {
		/* Zapytanie o wsparcie (php_stream_supports_lock), nie akcja blokady. */
		return fpm_flock_orig_set_option(stream, option, value, ptrparam);
	}

	owner = fpm_pool_fiber_waiter();
	if (!owner) {
		/* Poza kontekstem requestu fibera (np. skrypt kontenera przed
		 * pierwszym requestem) — brak sensu w rejestrze procesowym, zachowaj
		 * dokladnie oryginalne zachowanie. */
		return fpm_flock_orig_set_option(stream, option, value, ptrparam);
	}

	mode = value & ~LOCK_NB;
	nb = value & LOCK_NB;

	if (mode == LOCK_UN) {
		ret = fpm_flock_real(stream, value, ptrparam);
		if (fpm_flock_fd_identity(stream, &dev, &ino) == 0) {
			entry = fpm_flock_find(dev, ino, false);
			if (entry) {
				bool was_ex = (entry->ex_owner == owner);

				if (was_ex) {
					entry->ex_owner = NULL;
				}
				fpm_flock_sh_remove(entry, owner);
				fpm_flock_wake_all(entry);
			}
		}
		return ret;
	}

	if (fpm_flock_fd_identity(stream, &dev, &ino) != 0) {
		/* Nie mozemy poznac tozsamosci pliku (fstat padl) — nie ma jak
		 * bezpiecznie prowadzic rejestru kluczowanego dev+inode; oddaj
		 * dokladnie oryginalne zachowanie (nadal poprawne, tylko bez
		 * zawieszania zamiast blokowania). */
		return fpm_flock_orig_set_option(stream, option, value, ptrparam);
	}

	entry = fpm_flock_find(dev, ino, true);
	if (!entry) {
		return fpm_flock_orig_set_option(stream, option, value, ptrparam);
	}

	/* UWAGA (znaleziono i naprawiono W TRAKCIE tego spike'u, patrz raport):
	 * Faza wewnatrzprocesowa i faza miedzyprocesowa MUSZA byc JEDNA petla,
	 * ktora za kazdym razem od nowa sprawdza konflikt wewnatrzprocesowy PRZED
	 * kolejna proba prawdziwego flock() — nie dwie oddzielne fazy uruchamiane
	 * raz. Pierwsza wersja tego pliku robila to jako dwie fazy: (1) sprawdz
	 * konflikt wewnatrzprocesowy RAZ, (2) jesli brak konfliktu, probkuj
	 * prawdziwy flock() az do wyczerpania prob, potem zablokuj sie na
	 * prawdziwym flock(). Miedzy kolejnymi probami w fazie (2) fiber
	 * ODDAJE procesor (fpm_pool_fiber_wait_wake) — w tym oknie INNY fiber
	 * W TYM SAMYM procesie mogl wejsc, tez nie zastac konfliktu (bo ten
	 * pierwszy fiber jeszcze nic nie trzymal), wygrac prawdziwy flock() i
	 * zawiesic sie na gniazdzie (poprawnie). Pierwszy fiber, wracajac z
	 * probkowania, widzial dalej EWOULDBLOCK (bo TERAZ w tym samym procesie
	 * ktos trzyma), ale jego petla probkowania nie sprawdzala juz rejestru
	 * wewnatrzprocesowego — po wyczerpaniu prob wchodzil w PRAWDZIWY
	 * BLOKUJACY flock(), zamrazajac caly OS-owy watek. Poniewaz posiadaczem
	 * byl fiber W TYM SAMYM procesie zawieszony na gniazdzie, nigdy juz nie
	 * odzyskiwal procesora, zeby dokonczyc i zwolnic — trwaly bezruch,
	 * zmierzony (dwa osobne procesy, oba sparaliowane, gdb na zywym procesie
	 * pokazal ramke dokladnie w tym prawdziwym blokujacym flock(), z
	 * entry->ex_owner ustawionym na INNY fiber W TYM SAMYM procesie).
	 * Naprawa: kazda iteracja najpierw sprawdza rejestr wewnatrzprocesowy
	 * (i idzie w prawdziwe zawieszenie, jesli jest konflikt) i DOPIERO gdy
	 * nikt w tym procesie nie trzyma, probuje prawdziwy flock(). */
	max_attempts = fpm_flock_poll_attempts_get();
	attempts = 0;
	for (;;) {
		/* Wewnatrzprocesowo: dopoki ktos INNY w TYM procesie trzyma
		 * sprzeczny tryb, zawieszamy sie na kolejce w pamieci — zero
		 * probkowania, prawdziwe wait/wake, budzone przez LOCK_UN
		 * posiadacza (fpm_flock_wake_all). */
		while (fpm_flock_conflict(entry, owner, mode)) {
			if (nb) {
				errno = EWOULDBLOCK;
				return -1;	/* wywolujacy prosil o LOCK_NB — nie wolno zawieszac ani blokowac */
			}
			if (!fpm_pool_fiber_can_wait() || !fpm_flock_add_waiter(entry, owner)) {
				/* FIX (was: fall through to a real BLOCKING flock() here).
				 * We just confirmed, above, that another fiber IN THIS SAME
				 * PROCESS holds a conflicting lock. That holder cannot run
				 * again — the one OS thread this process has is about to
				 * be the caller of a blocking flock() — until this call
				 * returns, so a real blocking flock() here can never be
				 * satisfied: permanent, whole-process deadlock, exactly the
				 * bug this file exists to remove, just reached from the
				 * "can't suspend" or "queue full" edge instead of the
				 * two-phase-loop edge fixed earlier in this same spike (see
				 * the report). There is no safe blocking fallback for an
				 * in-process conflict, ever, by construction: the process
				 * has one thread and the holder is one of this process's
				 * own fibers.
				 *
				 * So: fail the lock attempt instead, exactly the way a
				 * real non-blocking flock() fails when it cannot be
				 * granted right away (see the `nb` branch a few lines
				 * above, and php_flock_common() in ext/standard/file.c:
				 * flock($fp, LOCK_EX) returns false and, if $wouldblock
				 * was passed, sets it to true when errno is EWOULDBLOCK;
				 * file_put_contents(..., LOCK_EX) returns false with an
				 * E_WARNING). This is documented, ordinary, userland-
				 * visible lock failure, not a crash and not silent data
				 * loss — it is materially better than losing the whole
				 * worker and every request in flight.
				 *
				 * This applies identically to BOTH reasons for landing
				 * here: a fiber that structurally cannot suspend (a nested
				 * user Fiber, a destructor running under GC) has exactly
				 * the same "the holder can't run" problem as a full
				 * waiter queue -- neither can be turned into a safe block.
				 */
				errno = EWOULDBLOCK;
				return -1;
			}
			fpm_pool_fiber_wait_wake(NULL);
			/* Po obudzeniu wracamy na SAM POCZATEK tej wewnetrznej petli —
			 * sprawdzamy konflikt jeszcze raz (moglo obudzic sie kilku
			 * czekajacych naraz, tylko jeden faktycznie wygra). */
		}

		/* Nikt w TYM procesie nie trzyma sprzecznego trybu: mozemy probowac
		 * prawdziwy flock(). Nie mamy zadnego zdarzenia gotowosci dla
		 * advisory locka — LOCK_NB albo od razu wygrywa, albo od razu
		 * przegrywa. */
		if (nb) {
			ret = fpm_flock_real(stream, value, ptrparam);
			if (ret == 0) {
				fpm_flock_register_holder(entry, owner, mode);
			}
			return ret;
		}

		ret = fpm_flock_real(stream, mode | LOCK_NB, ptrparam);
		if (ret == 0) {
			fpm_flock_register_holder(entry, owner, mode);
			return 0;
		}
		if (errno != EWOULDBLOCK) {	/* EAGAIN == EWOULDBLOCK on Linux; one check covers both there */
			return ret;	/* prawdziwy blad, nie rywalizacja — nie ma sensu probowac dalej */
		}

		/* EWOULDBLOCK: albo rywalizacja MIEDZYPROCESOWA (inny proces trzyma
		 * blokade jadra), albo — wlasnie to naprawiamy — ktos w TYM procesie
		 * zdazyl wygrac prawdziwy flock() W TRAKCIE naszego poprzedniego
		 * oddania procesora. Zamiast zakladac ktore to jest, po prostu
		 * wracamy na goure petli "for": jesli ktos w tym procesie faktycznie
		 * wygral, pierwsza rzecz, ktora zrobimy, to WLASNIE go zobaczymy w
		 * fpm_flock_conflict() i pojdziemy w prawdziwe zawieszenie zamiast
		 * dalej probkowac/blokowac sie na flock(). To jest calosc naprawy. */
		attempts++;
		if (attempts >= max_attempts || !fpm_pool_fiber_can_wait()) {
			break;
		}
		{
			struct timeval iv;

			iv.tv_sec = FPM_FLOCK_POLL_INTERVAL_USEC / 1000000;
			iv.tv_usec = FPM_FLOCK_POLL_INTERVAL_USEC % 1000000;
			fpm_pool_fiber_wait_wake(&iv);	/* uspienie fibera bez blokowania procesu; wraca po timeoucie */
		}
	}

	/* Wyczerpane proby odpytywania (albo probkowanie wylaczone przez
	 * FPMNG_FLOCK_POLL_ATTEMPTS=0, albo nie mozemy zawiesic fibera) I w tej
	 * chwili fpm_flock_conflict() dalej nie widzi nikogo w TYM procesie
	 * (sprawdzone na gorze tej samej iteracji petli, wiec to na pewno
	 * rywalizacja MIEDZYPROCESOWA): ostatnia deska ratunku to prawdziwy
	 * BLOKUJACY flock(). To zawiesza caly OS-owy watek jak dzisiaj, ale
	 * tylko na czas trzymania blokady przez INNY PROCES, ktory z definicji
	 * jest dzialajacy i skonczony w czasie (patrz E3 w
	 * docs/flock-fiber-deadlock-report.md) — to jest ograniczone, nie martwa
	 * petla, w przeciwienstwie do bugu, ktory ten plik usuwa (rywalizacja
	 * MIEDZY FIBRAMI W TYM SAMYM procesie, gdzie posiadacz nigdy nie
	 * odzyska procesora). */
	ret = fpm_flock_real(stream, mode, ptrparam);
	if (ret == 0) {
		fpm_flock_register_holder(entry, owner, mode);
	}
	return ret;
}
/* }}} */

void fpm_pool_fiber_flock_install(void) /* {{{ */
{
	if (fpm_flock_installed) {
		return;
	}
	fpm_flock_installed = true;
	fpm_flock_orig_set_option = php_stream_stdio_ops.set_option;
	php_stream_stdio_ops.set_option = fpm_fiber_flock_set_option;
}
/* }}} */

/* fpm-ng: pool types.
 *
 * Dodanie nowego typu poola to nowy plik plus jedna linia w tablicy
 * fpm_pool_types[] w fpm_pool_type.c. Nic poza tym — w szczegolnosci ani
 * logika walidacji w fpm_conf.c, ani fpm_children.c nie moga o typie wiedziec.
 * Dlatego wymagania konfiguracyjne typ deklaruje DANYMI (pola ponizej),
 * a nie kodem rozsianym po walidacji.
 */

#ifndef FPM_POOL_TYPE_H
#define FPM_POOL_TYPE_H 1

#include <time.h>

struct fpm_worker_pool_s;

/* Stan poola, ktory NIE obsluguje requestow (serves_requests = 0). Dla
 * poolow typu fcgi/http (serves_requests = 1) ksztalt danych jest inny
 * (idle/active/requests ze scoreboardu) i ten enum ich nie dotyczy — patrz
 * pool.status w fpm_pool_status.c. */
enum fpm_pool_state_e {
	FPM_POOL_STATE_RUNNING = 0,	/* aktualnie wykonuje skrypt */
	FPM_POOL_STATE_BACKOFF,		/* czeka w opoznieniu przed kolejna proba (supervisor) */
	FPM_POOL_STATE_GAVE_UP,		/* poddal sie na dobre, prawdziwa porazka (supervisor.restart_max) */
	FPM_POOL_STATE_FINISHED,	/* zakonczyl sie planowo, nie porazka (restart=never/on-failure+sukces) */
	FPM_POOL_STATE_IDLE		/* nic teraz nie robi, czeka na kolejny termin (cron miedzy przebiegami) */
};

/* Wypelniane przez fpm_pool_type_s.status() dla typow serves_requests = 0.
 * Dokladnie tyle pol, ile pool.type = status faktycznie pokazuje — patrz
 * docs/NOTES.md sekcja 3u. */
struct fpm_pool_status_s {
	enum fpm_pool_state_e state;
	time_t last_start;		/* epoch, 0 = jeszcze nigdy nie startowal */
	int last_exit_code;
	unsigned consecutive_failures;	/* kolejne exit_code != 0 z rzedu */
	time_t next_run;		/* tylko cron: najblizszy termin z harmonogramu */
	time_t backoff_until;		/* tylko supervisor: koniec biezacego backoffu */
	unsigned has_last_exit_code:1;
	unsigned has_next_run:1;
	unsigned has_backoff_until:1;
};

struct fpm_pool_type_s {
	const char *name;

	/* Wymagania konfiguracyjne — czytane przez fpm_conf.c, ktory nie zna typow. */
	unsigned requires_listen:1;		/* pool musi miec adres nasluchiwania */
	unsigned requires_pm:1;			/* pool musi miec sensowne pm/pm.max_children */
	unsigned serves_requests:1;		/* liczy sie w scoreboardzie requestow */

	/* Ten typ, w SWOIM WLASNYM dziecku, czyta scoreboard CUDZEGO poola
	 * (pool.type = status: idle/active/requests innych poolow serves_requests=1).
	 * Ustawione tylko dla "status". Patrz fpm_children.c:
	 * fpm_child_resources_use() domyslnie zwalnia (munmap) scoreboardy
	 * WSZYSTKICH poolow poza wlasnym zaraz po forku, jako higiena pamieci —
	 * bezpieczne, bo do dzis zaden typ nie czytal cudzego scoreboardu. Ta
	 * flaga wylacza to zwalnianie WYLACZNIE dla dziecka poola TEGO typu
	 * (sprawdzane przez fpm_pool_type_of(child->wp) w fpm_children.c) — kazdy
	 * inny pool w tym samym configu (w tym zwykly fcgi/http) nadal zwalnia
	 * cudze scoreboardy dokladnie jak dzis, niezaleznie od tego, czy
	 * gdziekolwiek w configu istnieje pool status. Patrz docs/NOTES.md 3u. */
	unsigned reads_foreign_scoreboards:1;

	/* Dyrektywy, ktorych ten typ nie obsluguje. Zakonczona NULL-em, moze byc NULL.
	 * Lista ODRZUCEN, nie dopuszczen — dzieki temu nowa dyrektywa jest domyslnie
	 * dozwolona wszedzie i nie psuje zgodnosci wstecznej przez przeoczenie.
	 * Nazwa konczaca sie kropka dziala jak prefiks: "pm." lapie cale pm.*. */
	const char *const *rejects;

	/* Sprawdzenia specyficzne dla typu; NULL = brak. Zwraca 0 albo -1. */
	int (*validate)(struct fpm_worker_pool_s *wp);

	/* Strona mastera, po walidacji a przed forkiem dzieci; NULL = nic. */
	int (*init_main)(struct fpm_worker_pool_s *wp);

	/* Co robi dziecko zamiast petli accept; NULL = zwykla petla FastCGI.
	 * Nie wraca. */
	void (*child_main)(struct fpm_worker_pool_s *wp);

	/* Jak sie ten typ pokazuje w pool.type = status, gdy serves_requests = 0.
	 * NULL dla typow serves_requests = 1 (te maja idle/active/requests ze
	 * scoreboardu, czytane bezposrednio przez fpm_pool_status.c) i dla
	 * typow bez sensownego stanu do pokazania (np. status sam siebie).
	 * Wolane z INNEGO procesu (poola status), wiec musi czytac wylacznie
	 * z pamieci dzielonej / configu, nigdy z pamieci lokalnej procesu. */
	void (*status)(struct fpm_worker_pool_s *wp, struct fpm_pool_status_s *out);
};

/* Typ o tej nazwie albo NULL. Pusta nazwa daje typ domyslny (fcgi) — bez tego
 * kazdy istniejacy fpm.conf przestalby dzialac. */
const struct fpm_pool_type_s *fpm_pool_type_get(const char *name);

/* Nazwy znanych typow, do komunikatu o bledzie. Bufor nalezy do wolajacego. */
void fpm_pool_type_list(char *buf, size_t len);

/* Typ danego poola; nigdy NULL po udanej walidacji konfiguracji. */
const struct fpm_pool_type_s *fpm_pool_type_of(struct fpm_worker_pool_s *wp);

/* Pool biezacego dziecka, albo NULL poza dzieckiem. */
struct fpm_worker_pool_s *fpm_pool_type_current_pool(void);

/* Odrzuca dyrektywy nieobslugiwane przez ten typ. 0 albo -1. */
int fpm_pool_type_check_directives(struct fpm_worker_pool_s *wp, const struct fpm_pool_type_s *type);

#endif

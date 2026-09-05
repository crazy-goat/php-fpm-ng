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

struct fpm_worker_pool_s;

struct fpm_pool_type_s {
	const char *name;

	/* Wymagania konfiguracyjne — czytane przez fpm_conf.c, ktory nie zna typow. */
	unsigned requires_listen:1;		/* pool musi miec adres nasluchiwania */
	unsigned requires_pm:1;			/* pool musi miec sensowne pm/pm.max_children */
	unsigned serves_requests:1;		/* liczy sie w scoreboardzie requestow */

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

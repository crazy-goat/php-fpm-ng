/* fpm-ng: pool.type = supervisor — N long-lived processes running one PHP
 * script in-process (no exec), resurrected by the existing pm=static
 * machinery in fpm_children.c. See docs/NOTES.md for the design writeup
 * (in particular: why processes map onto pm=static + pm.max_children, and
 * why "give up" is a per-pool shared-memory state instead of a change to
 * fpm_children.c).
 */

#ifndef FPM_POOL_SUPERVISOR_H
#define FPM_POOL_SUPERVISOR_H 1

struct fpm_worker_pool_s;

/* Dyrektywy odrzucane dla pool.type = supervisor. NULL-terminated,
 * uzywane jako .rejects w fpm_pool_types[]. */
extern const char *const fpm_pool_supervisor_rejects[];

/* fpm_pool_type_s.validate — sprawdzenia i wartosci domyslne specyficzne dla
 * supervisora (supervisor.script wymagane, supervisor.restart poprawne,
 * mapowanie supervisor.processes na pm=static + pm.max_children). */
int fpm_pool_supervisor_validate(struct fpm_worker_pool_s *wp);

/* fpm_pool_type_s.init_main — alokacja stanu w pamieci dzielonej (licznik
 * kolejnych porazek, backoff, terminal/gave_up) i rejestracja sprzatania
 * przy zamykaniu mastera (dla supervisor.fatal). Wolane w masterze, przed
 * forkiem dzieci. */
int fpm_pool_supervisor_init_main(struct fpm_worker_pool_s *wp);

/* fpm_pool_type_s.child_main — dziecko petli po wykonaniach skryptu zamiast
 * wracac do petli accept FastCGI. Nie wraca. */
void fpm_pool_supervisor_child_main(struct fpm_worker_pool_s *wp);

#endif

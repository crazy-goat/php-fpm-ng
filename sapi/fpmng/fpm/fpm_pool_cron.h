/* fpm-ng: pool.type = cron — a script run on a crontab-style schedule,
 * one run per process lifetime. See fpm_pool_cron.c and docs/NOTES.md for
 * the design writeup (in particular: why there is deliberately no timer and
 * no shared state on the master side, and why overlap is impossible by
 * construction rather than by policy).
 */

#ifndef FPM_POOL_CRON_H
#define FPM_POOL_CRON_H 1

struct fpm_worker_pool_s;

/* Dyrektywy odrzucane dla pool.type = cron. NULL-terminated, uzywane jako
 * .rejects w fpm_pool_types[]. */
extern const char *const fpm_pool_cron_rejects[];

/* fpm_pool_type_s.validate — cron.schedule/cron.script wymagane, harmonogram
 * parsowany RAZ tutaj (zly harmonogram = config odrzucony ze startu, nie
 * "mniej wiecej" w runtime), mapowanie na pm = static + pm.max_children = 1. */
int fpm_pool_cron_validate(struct fpm_worker_pool_s *wp);

/* fpm_pool_type_s.child_main — liczy najblizszy termin, spi do niego
 * (przerywalnie SIGTERM-em), wykonuje skrypt raz, konczy proces. Nie wraca.
 * Wskrzeszenie na kolejny przebieg to zwykly, bezwarunkowy respawn
 * fpm_children.c (pm = static, max_children = 1) — bez wlasnego stanu
 * w pamieci dzielonej, w odroznieniu od supervisora. */
void fpm_pool_cron_child_main(struct fpm_worker_pool_s *wp);

#endif

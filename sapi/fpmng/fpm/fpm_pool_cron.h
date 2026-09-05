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

/* fpm_pool_type_s.init_main — alokuje TYLKO to, czego pool.type = status
 * potrzebuje do pokazania last_run/last_exit_code (docs/NOTES.md 3u):
 * cron nadal NIE MA zadnej polityki restart/backoff, ktora musialaby
 * przetrwac smierc procesu (patrz uzasadnienie na gorze fpm_pool_cron.c) —
 * to jest stan wylacznie do ODCZYTU przez status, nigdy do sterowania
 * zachowaniem crona samego siebie. */
int fpm_pool_cron_init_main(struct fpm_worker_pool_s *wp);

/* fpm_pool_type_s.child_main — liczy najblizszy termin, spi do niego
 * (przerywalnie SIGTERM-em), wykonuje skrypt raz, konczy proces. Nie wraca.
 * Wskrzeszenie na kolejny przebieg to zwykly, bezwarunkowy respawn
 * fpm_children.c (pm = static, max_children = 1) — polityka crona samego
 * w sobie nadal nie zalezy od zadnego stanu w pamieci dzielonej, w
 * odroznieniu od supervisora (patrz init_main wyzej: to co dolozono sluzy
 * WYLACZNIE statusowi, nie sterowaniu). */
void fpm_pool_cron_child_main(struct fpm_worker_pool_s *wp);

struct fpm_pool_status_s;

/* fpm_pool_type_s.status — stan tego poola dla pool.type = status.
 * last_run/last_exit_code z pamieci dzielonej, next_run POLICZONE NA
 * BIEZACO z harmonogramu i biezacego zegara (nie z zadnego stanu) — patrz
 * docs/NOTES.md 3u. */
void fpm_pool_cron_status(struct fpm_worker_pool_s *wp, struct fpm_pool_status_s *out);

#endif

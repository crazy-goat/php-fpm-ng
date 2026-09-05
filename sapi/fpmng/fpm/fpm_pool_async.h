/* fpm-ng: pool.type = async — EKSPERYMENT.
 *
 * Jeden proces, wiele requestow FastCGI w locie, kazdy w osobnej korutynie
 * True Async (fork php-src true-async/php-src + ext/async). Na silniku bez
 * True Async typ istnieje, ale validate() odrzuca pool z czytelnym
 * komunikatem. Uzasadnienie, ograniczenia i wyniki: docs/NOTES.md, sekcja 3t.
 */

#ifndef FPM_POOL_ASYNC_H
#define FPM_POOL_ASYNC_H 1

struct fpm_worker_pool_s;

/* Dyrektywy odrzucane dla pool.type = async (patrz fpm_pool_type_check_directives). */
extern const char *const fpm_pool_async_rejects[];

/* fpm_pool_type_s.validate — odrzuca pool, gdy silnik nie ma True Async API
 * (kompilacja) albo nie zarejestrowano schedulera/reaktora (ext/async, runtime). */
int fpm_pool_async_validate(struct fpm_worker_pool_s *wp);

/* fpm_pool_type_s.child_main — petla zdarzen zamiast blokujacej petli accept. Nie wraca. */
void fpm_pool_async_child_main(struct fpm_worker_pool_s *wp);

#endif

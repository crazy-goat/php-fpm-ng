#ifndef FPM_HTTP_H
#define FPM_HTTP_H 1

struct fpm_worker_pool_s;

/* Bramki HTTP dla jednego poola (patrz fpm_http.c). Wolane przez typ poola
 * "http" z fpm_pool_type.c, ze strony mastera, przed forkiem workerow. */
int fpm_http_init_pool(struct fpm_worker_pool_s *wp);

/* Jak wyzej, ale dla executora obslugujacego wiele requestow na worker.
 * capacity = liczba rownoleglych polaczen FastCGI do calego poola. */
int fpm_http_init_pool_with_capacity(struct fpm_worker_pool_s *wp, unsigned capacity);

/* Walidacja dyrektyw http.* dla pool.type = http, wolana z fpm_pool_type.c
 * (.validate) w fazie sprawdzania configu, przed forkiem czegokolwiek. */
int fpm_http_validate_pool(struct fpm_worker_pool_s *wp);

#endif

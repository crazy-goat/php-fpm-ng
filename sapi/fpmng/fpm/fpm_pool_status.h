/* fpm-ng: pool.type = status — pool ktory nie odpala PHP w ogole. Nasluchuje
 * HTTP na wlasnym porcie (ten sam mechanizm co listen dla fcgi/http, ale
 * bezposrednio, bez fcgi+1 jak bramka http), czyta scoreboardy i pamiec
 * dzielona WSZYSTKICH innych poolow w tym samym masterze i serializuje w
 * dwoch formatach: tekstowy Prometheus (/metrics) i JSON (/status). Patrz
 * docs/NOTES.md sekcja 3u dla uzasadnienia projektowego.
 */

#ifndef FPM_POOL_STATUS_H
#define FPM_POOL_STATUS_H 1

struct fpm_worker_pool_s;

/* Dyrektywy odrzucane dla pool.type = status. NULL-terminated, uzywane jako
 * .rejects w fpm_pool_types[]. Uwaga: w odroznieniu od supervisor/cron,
 * "listen"/"listen." NIE sa tu odrzucane — status faktycznie nasluchuje. */
extern const char *const fpm_pool_status_rejects[];

/* fpm_pool_type_s.validate — status nie ma zadnych wlasnych dyrektyw do
 * sprawdzenia; jedyna robota to programowe wymuszenie pm = static + 1
 * (jeden proces w zupelnosci wystarcza do obslugi scrapow monitoringu, wiec
 * nie ma sensu dodawac dyrektywy "ile procesow"). */
int fpm_pool_status_validate(struct fpm_worker_pool_s *wp);

/* fpm_pool_type_s.child_main — petla accept na wlasnym gnieznie (wp->listening_socket),
 * surowy HTTP/1.0 bez keep-alive: parsuje tylko linie zadania (GET <path>),
 * odpowiada tekstem Prometheus na /metrics albo JSON-em na /status, 404 na
 * wszystko inne. Nie wraca. */
void fpm_pool_status_child_main(struct fpm_worker_pool_s *wp);

#endif

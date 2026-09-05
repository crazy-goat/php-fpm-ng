/* fpm-ng: access log bramki HTTP (http.access_log), format Combined Log Format,
 * niekonfigurowalny -- patrz docs/NOTES.md dlaczego (prostota jest tu wartoscia).
 *
 * Kazdy z procesow http.gateways ma wlasny deskryptor do TEGO SAMEGO pliku,
 * otwarty O_APPEND. Jeden write() na linie na regularnym pliku z O_APPEND
 * jest atomowy wzgledem innych pisarzy (POSIX) -- dlatego linie z roznych
 * procesow bramki nie mieszaja sie, mimo braku wspolnego locka. Zapis jest
 * synchroniczny (jak w nginx/Apache) -- lokalny dysk/page cache czyni to
 * tanim, wiec nie warto komplikowac kodu buforem asynchronicznym.
 */

#ifndef FPM_HTTP_ACCESS_LOG_H
#define FPM_HTTP_ACCESS_LOG_H 1

#include <stddef.h>

struct fpm_http_access_log_s;

/* path == NULL albo "" -> log wylaczony, zwraca NULL (poprawny, "wylaczony"
 * uchwyt dla fpm_http_access_log_write()). Blad otwarcia jest logowany przez
 * zlog i traktowany tak samo jak "wylaczony" -- brakujacy log nie powinien
 * uniemozliwic startu bramki. */
struct fpm_http_access_log_s *fpm_http_access_log_open(const char *pool, const char *path);

void fpm_http_access_log_close(struct fpm_http_access_log_s *log);

/* log == NULL -> no-op. remote_user moze byc NULL/pusty (staje sie "-").
 * status < 0 -> "-" zamiast kodu (np. polaczenie padlo przed odpowiedzia). */
void fpm_http_access_log_write(struct fpm_http_access_log_s *log, const char *remote_addr,
	const char *remote_user, const char *method, const char *uri, int http_major, int http_minor,
	int status, size_t bytes_sent, const char *referer, const char *user_agent);

#endif

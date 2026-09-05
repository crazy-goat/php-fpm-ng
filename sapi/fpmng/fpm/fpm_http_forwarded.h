/* fpm-ng: X-Forwarded-For / -Proto / -Port dla bramki HTTP (fpm_http.c).
 *
 * Za odwrotnym proxy REMOTE_ADDR widziany przez bramke to adres proxy, nie
 * klienta. Naglowkom X-Forwarded-* ufamy TYLKO gdy polaczenie przychodzi z
 * adresu na liscie http.trusted_proxies -- bez tego dowolny klient moglby
 * podszyc sie pod adres w logach i w kontroli dostepu aplikacji (ktora czesto
 * ufa REMOTE_ADDR). Brak dyrektywy = nikomu nie ufamy = bezpieczny domyslny.
 *
 * Uproszczenie: z X-Forwarded-For bierzemy TYLKO pierwszy adres (oryginalny
 * klient). Dobre dla jednego zaufanego proxy przed brama -- typowy przypadek
 * dla tego projektu (patrz docs/NOTES.md, sekcja 1: maly VPS, nie klaster
 * proxy). Lancuch kilku proxy nie jest specjalnie rozpoznawany ponad to.
 */

#ifndef FPM_HTTP_FORWARDED_H
#define FPM_HTTP_FORWARDED_H 1

struct fpm_http_acl_s;
struct evkeyvalq;

#define FPM_HTTP_FORWARDED_ADDR_LEN 46 /* INET6_ADDRSTRLEN */
#define FPM_HTTP_FORWARDED_PORT_LEN 6  /* "65535" + NUL */

struct fpm_http_forwarded_result_s {
	char remote_addr[FPM_HTTP_FORWARDED_ADDR_LEN]; /* [0] == '\0' -> nie nadpisuj REMOTE_ADDR */
	const char *scheme;                            /* "http" albo "https", nigdy NULL */
	int https;                                     /* 1 -> HTTPS powinno byc ustawione na "on" */
	char server_port[FPM_HTTP_FORWARDED_PORT_LEN]; /* [0] == '\0' -> nie nadpisuj SERVER_PORT */
};

/* trusted == NULL -> nikomu nie ufamy, X-Forwarded-* jest w calosci ignorowany
 * i *out dostaje czyste wartosci domyslne (http, bez nadpisan). W przeciwnym
 * razie sprawdza peer_addr (bezposredni adres TCP) wzgledem `trusted`
 * (fpm_http_acl_check()) i dopiero wtedy czyta naglowki z `headers`. */
void fpm_http_forwarded_resolve(struct fpm_http_acl_s *trusted, const char *peer_addr,
	struct evkeyvalq *headers, struct fpm_http_forwarded_result_s *out);

#endif

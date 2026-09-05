/* fpm-ng: X-Forwarded-For / -Proto / -Port dla bramki HTTP (fpm_http.c).
 *
 * Za odwrotnym proxy REMOTE_ADDR widziany przez bramke to adres proxy, nie
 * klienta. Naglowkom X-Forwarded-* ufamy TYLKO gdy polaczenie przychodzi z
 * adresu na liscie http.trusted_proxies -- bez tego dowolny klient moglby
 * podszyc sie pod adres w logach i w kontroli dostepu aplikacji (ktora czesto
 * ufa REMOTE_ADDR). Brak dyrektywy = nikomu nie ufamy = bezpieczny domyslny.
 *
 * Z X-Forwarded-For bierzemy pierwszy OD PRAWEJ adres, ktory sam nie jest na
 * liscie http.trusted_proxies. Nie pierwszy z lewej: nginx z domyslnym
 * $proxy_add_x_forwarded_for dopisuje adres klienta do naglowka, ktory
 * klient przyslal, wiec lewa strona listy jest wprost pod kontrola klienta
 * i wziecie jej pozwalaloby podszyc sie pod dowolny adres MIMO zaufanego
 * proxy. Dziala tak samo dla jednego proxy i dla lancucha.
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

/* fpm-ng: pool.executor = fiber — przechwycenie gniazd w warstwie strumieni.
 *
 * php_stream_xport_register() (main/streams/php_stream_transport.h, PHPAPI)
 * pozwala podmienic fabryke transportu "tcp"/"unix". Nasza fabryka wola
 * ORYGINALNA (generyczna z streams.c albo openssl-owa, gdy ext/openssl
 * jest wkompilowane — ono nadpisuje "tcp" w MINIT) i podmienia w gotowym
 * strumieniu tablice ops na opakowanie. Opakowanie przed kazdym read/write/
 * connect sprawdza, czy operacja by zablokowala, i jesli tak — zawiesza
 * fiber requestu do czasu gotowosci deskryptora (fpm_pool_fiber_wait_fd),
 * a potem woła oryginalna operacje, ktora juz nie blokuje.
 *
 * Tozsamosc ops ma znaczenie: xp_socket.c rozpoznaje unix/udp po adresie
 * tablicy (PHP_STREAM_XPORT_IS_UNIX), a ext/sockets sprawdza
 * PHP_STREAM_IS_SOCKET. Na czas kazdego delegowanego wywolania przywracamy
 * wiec oryginalny stream->ops. Poza wywolaniem strumien ma nasze ops —
 * ext/sockets socket_import_stream() go nie rozpozna (znane ograniczenie).
 *
 * DNS: getaddrinfo() siedzi w php_network_connect_socket_to_host, czyli
 * WEWNATRZ delegowanego connect, i blokuje caly proces. Dlatego dla transportu
 * tcp, gdy host nie jest literalem IP, connect najpierw rozwiazuje nazwe przez
 * evdns (libevent, ktory i tak linkujemy) na event_base schedulera, fiber czeka
 * na callback (fpm_pool_fiber_wait_wake), a oryginalny connect dostaje juz
 * literal IP — jego getaddrinfo() jest wtedy trywialny. Baza evdns powstaje
 * raz na proces z /etc/resolv.conf i /etc/hosts (evdns_getaddrinfo sprawdza
 * hosts przed siecia). Kolejne adresy A/AAAA probujemy po kolei, jak upstream.
 * Czego to nie zmienia: peer_name/SNI w ext/openssl biora nazwe z resourcename
 * w fabryce, nie z nazwy podawanej connectowi; stream_socket_get_name() to
 * getpeername(); "Unable to connect to <host>" sklada ext/standard z nazwy
 * od uzytkownika. Transporty ssl/tls (a wiec https://) nie sa hookowane.
 *
 * Czego to NIE lapie (bo nie idzie przez transporty strumieni): sleep(),
 * curl, libpq (pdo_pgsql), zwykle pliki, TLS handshake i SSL_read/SSL_write
 * (openssl ma wlasne pollowanie), DNS poza connectem (gethostbyname,
 * dns_get_record) i DNS w transportach ssl/tls.
 */

#include "fpm_config.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <event2/event.h>
#include <event2/dns.h>
#include <event2/util.h>

#include "php.h"
#include "php_network.h"
#include "php_streams.h"

#include "fpm_pool_coop.h"
#include "fpm_pool_fiber.h"
#include "zlog.h"

/* Opakowanie ops per oryginalna tablica (generyczna tcp, unix, openssl-owa).
 * dns: strumienie z tej tablicy lacza sie po host:port (fabryka "tcp"), wiec
 * connect ma rozwiazywac nazwe; fabryka "unix" daje sciezke. Flaga pochodzi
 * z fabryki, ktora tablice opakowala — nie z porownywania adresow ops. */
struct fpm_fiber_ops_map_s {
	const php_stream_ops *orig;
	php_stream_ops wrap;
	bool dns;
};
#define FPM_FIBER_OPS_MAX 4
static struct fpm_fiber_ops_map_s fpm_fiber_ops_map[FPM_FIBER_OPS_MAX];
static int fpm_fiber_ops_count = 0;

/* Ostrzezenie o odmowie polaczenia trwalego: raz na proces, zeby jedna zapetlona
 * aplikacja nie zalala logu. */
static bool fpm_fiber_persistent_warned = false;

static php_stream_transport_factory fpm_fiber_orig_tcp_factory;
static php_stream_transport_factory fpm_fiber_orig_unix_factory;

static const struct fpm_fiber_ops_map_s *fpm_fiber_ops_entry(const php_stream_ops *wrap) /* {{{ */
{
	int i;

	for (i = 0; i < fpm_fiber_ops_count; i++) {
		if (&fpm_fiber_ops_map[i].wrap == wrap) {
			return &fpm_fiber_ops_map[i];
		}
	}
	return NULL;
}
/* }}} */

static const php_stream_ops *fpm_fiber_orig_ops(const php_stream_ops *wrap) /* {{{ */
{
	const struct fpm_fiber_ops_map_s *m = fpm_fiber_ops_entry(wrap);

	return m ? m->orig : NULL;
}
/* }}} */

/* Czy gniazdo jest gotowe TERAZ (bez czekania). Jeden tani poll zamiast
 * rejestracji w libevent — dane czesto juz sa (odpowiedz w jednym pakiecie). */
static int fpm_fiber_ready_now(int fd, int pollev) /* {{{ */
{
	int r;

	do {
		r = php_pollfd_for_ms(fd, pollev, 0);
	} while (r < 0 && errno == EINTR);
	return r != 0;	/* >0 gotowy; <0 blad — niech oryginal go zglosi */
}
/* }}} */

/* Czeka na gotowosc gniazda strumienia z jego timeoutem. 1 gotowy, 0 timeout,
 * -1 nie czekalismy (blokuj jak zwykle). */
static int fpm_fiber_wait_sock(php_netstream_data_t *sock, int pollev, short ev) /* {{{ */
{
	struct timeval *tv;

	if (!sock || sock->socket == -1 || !sock->is_blocked) {
		return -1;
	}
	/* timeout 0 = nie czekaj (xp_socket.c: dont_wait) */
	if (sock->timeout.tv_sec == 0 && sock->timeout.tv_usec == 0) {
		return -1;
	}
	if (!fpm_pool_fiber_can_wait()) {
		return -1;
	}
	if (fpm_fiber_ready_now(sock->socket, pollev)) {
		return 1;
	}
	tv = (sock->timeout.tv_sec == -1) ? NULL : &sock->timeout;
	return fpm_pool_fiber_wait_fd(sock->socket, ev, tv);
}
/* }}} */

/* Delegacja z przywroceniem tozsamosci ops. */
#define FPM_FIBER_DELEGATE(stream, orig, call) do { \
		const php_stream_ops *fpm_wrap_ = (stream)->ops; \
		(stream)->ops = (orig); \
		call; \
		(stream)->ops = fpm_wrap_; \
	} while (0)

static ssize_t fpm_fiber_xop_read(php_stream *stream, char *buf, size_t count) /* {{{ */
{
	const php_stream_ops *orig = fpm_fiber_orig_ops(stream->ops);
	php_netstream_data_t *sock = (php_netstream_data_t *) stream->abstract;
	ssize_t ret;

	/* has_buffered_data: xp_socket.c i tak nie czeka */
	if (!stream->has_buffered_data) {
		int w = fpm_fiber_wait_sock(sock, PHP_POLLREADABLE, EV_READ);

		if (w == 0) {
			/* jak php_sockop_read po timeoutcie bez zbuforowanych danych */
			sock->timeout_event = true;
			return -1;
		}
	}
	FPM_FIBER_DELEGATE(stream, orig, ret = orig->read(stream, buf, count));
	return ret;
}
/* }}} */

static ssize_t fpm_fiber_xop_write(php_stream *stream, const char *buf, size_t count) /* {{{ */
{
	const php_stream_ops *orig = fpm_fiber_orig_ops(stream->ops);
	php_netstream_data_t *sock = (php_netstream_data_t *) stream->abstract;
	ssize_t ret;
	int w = fpm_fiber_wait_sock(sock, POLLOUT, EV_WRITE);

	if (w == 0) {
		sock->timeout_event = true;
		return -1;
	}
	FPM_FIBER_DELEGATE(stream, orig, ret = orig->write(stream, buf, count));
	return ret;
}
/* }}} */

static int fpm_fiber_xop_close(php_stream *stream, int close_handle) /* {{{ */
{
	const php_stream_ops *orig = fpm_fiber_orig_ops(stream->ops);
	int ret;

	FPM_FIBER_DELEGATE(stream, orig, ret = orig->close(stream, close_handle));
	return ret;
}
/* }}} */

static int fpm_fiber_xop_flush(php_stream *stream) /* {{{ */
{
	const php_stream_ops *orig = fpm_fiber_orig_ops(stream->ops);
	int ret;

	FPM_FIBER_DELEGATE(stream, orig, ret = orig->flush(stream));
	return ret;
}
/* }}} */

static int fpm_fiber_xop_cast(php_stream *stream, int castas, void **ret_ptr) /* {{{ */
{
	const php_stream_ops *orig = fpm_fiber_orig_ops(stream->ops);
	int ret;

	FPM_FIBER_DELEGATE(stream, orig, ret = orig->cast(stream, castas, ret_ptr));
	return ret;
}
/* }}} */

static int fpm_fiber_xop_stat(php_stream *stream, php_stream_statbuf *ssb) /* {{{ */
{
	const php_stream_ops *orig = fpm_fiber_orig_ops(stream->ops);
	int ret;

	FPM_FIBER_DELEGATE(stream, orig, ret = orig->stat(stream, ssb));
	return ret;
}
/* }}} */

/* --- DNS: evdns na event_base schedulera ------------------------------------ */

static struct evdns_base *fpm_fiber_dns;
static bool fpm_fiber_dns_tried;

/* evdns loguje kazde zapytanie ("Resolve requested for", "Sending request
 * for ... on ipv4/ipv6") i awarie nameserverow; pod log_level = debug widac
 * wiec, ktore connecty poszly przez DNS, a ktore nie. */
static void fpm_fiber_dns_log(int is_warning, const char *msg) /* {{{ */
{
	zlog(is_warning ? ZLOG_WARNING : ZLOG_DEBUG, "[pool %s] fiber: evdns: %s", fpm_coop_pool_name(), msg);
}
/* }}} */

/* Baza evdns, raz na proces, leniwie przy pierwszym connect z nazwa hosta.
 * NULL = uzywaj blokujacego getaddrinfo. */
static struct evdns_base *fpm_fiber_dns_base(void) /* {{{ */
{
	struct event_base *base;

	if (fpm_fiber_dns_tried) {
		return fpm_fiber_dns;
	}
	fpm_fiber_dns_tried = true;

	base = fpm_pool_fiber_event_base();
	if (!base) {
		return NULL;
	}
	evdns_set_log_fn(fpm_fiber_dns_log);
	/* INITIALIZE_NAMESERVERS = DNS_OPTIONS_ALL: nameserver/search/ndots
	 * z /etc/resolv.conf ORAZ /etc/hosts. Bez resolv.conf albo bez ani jednego
	 * nameservera evdns_base_new zwraca NULL — wtedy zostaje blokujaca sciezka,
	 * ktora w tej sytuacji tez nie ma czego pytac. */
	fpm_fiber_dns = evdns_base_new(base, EVDNS_BASE_INITIALIZE_NAMESERVERS | EVDNS_BASE_DISABLE_WHEN_INACTIVE);
	if (!fpm_fiber_dns) {
		zlog(ZLOG_WARNING, "[pool %s] fiber: evdns_base_new() failed (no /etc/resolv.conf or no nameservers?); DNS stays blocking",
			fpm_coop_pool_name());
		return NULL;
	}
	/* getaddrinfo nie robi randomizacji 0x20; resolvery, ktore nie zachowuja
	 * wielkosci liter w odpowiedzi, dostalyby od evdns odrzucenie odpowiedzi. */
	evdns_base_set_option(fpm_fiber_dns, "randomize-case", "0");

	zlog(ZLOG_DEBUG, "[pool %s] fiber: async DNS via evdns, %d nameserver(s)",
		fpm_coop_pool_name(), evdns_base_count_nameservers(fpm_fiber_dns));
	return fpm_fiber_dns;
}
/* }}} */

struct fpm_fiber_dns_req_s {
	void *waiter;
	struct evutil_addrinfo *res;
	int result;			/* kod EVUTIL_EAI_* (0 = ok) */
	bool done;
};

static void fpm_fiber_dns_cb(int result, struct evutil_addrinfo *res, void *arg) /* {{{ */
{
	struct fpm_fiber_dns_req_s *r = arg;

	r->result = result;
	r->res = res;
	r->done = true;
	fpm_pool_fiber_wake(r->waiter);
}
/* }}} */

enum fpm_fiber_dns_status {
	FPM_FIBER_DNS_OK,
	FPM_FIBER_DNS_FAIL,		/* *gai_err = EVUTIL_EAI_* */
	FPM_FIBER_DNS_TIMEOUT,	/* uplynal timeout connectu */
	FPM_FIBER_DNS_NOWAIT	/* nie moglismy czekac; wolajacy ma blokowac */
};

/* Rozwiazuje nazwe nie blokujac procesu. Wynik zwalnia evutil_freeaddrinfo. */
static enum fpm_fiber_dns_status fpm_fiber_dns_resolve(struct evdns_base *dns, const char *host, struct timeval *timeout, struct evutil_addrinfo **res, int *gai_err) /* {{{ */
{
	struct fpm_fiber_dns_req_s r = { fpm_pool_fiber_waiter(), NULL, 0, false };
	struct evutil_addrinfo hints;
	struct evdns_getaddrinfo_request *req;
	int w = 1;

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = IPPROTO_TCP;

	/* Trafienie w /etc/hosts albo natychmiastowy blad: callback przychodzi
	 * synchronicznie, req == NULL i r.done juz stoi. */
	req = evdns_getaddrinfo(dns, host, NULL, &hints, fpm_fiber_dns_cb, &r);
	if (!r.done) {
		w = fpm_pool_fiber_wait_wake(timeout);
		if (!r.done) {
			/* timeout albo brak mozliwosci czekania: cancel wola callback
			 * synchronicznie z EVUTIL_EAI_CANCEL, wiec r przestaje byc uzywane. */
			evdns_getaddrinfo_cancel(req);
			return w == 0 ? FPM_FIBER_DNS_TIMEOUT : FPM_FIBER_DNS_NOWAIT;
		}
	}
	if (r.result != 0) {
		*gai_err = r.result;
		return FPM_FIBER_DNS_FAIL;
	}
	*res = r.res;
	return FPM_FIBER_DNS_OK;
}
/* }}} */

/* Host z "host:port" wedlug regul xp_socket.c parse_ip_address_ex (pierwszy
 * dwukropek dzieli). NULL = literal IP, forma "[v6]:port", brak portu albo cos,
 * czego nie rozumiemy — wtedy oryginal dostaje nazwe nietknieta i robi swoje
 * (w tym zglasza wlasne bledy skladni). */
static char *fpm_fiber_dns_host_to_resolve(const char *name, size_t namelen, int *portno) /* {{{ */
{
	const char *colon;
	size_t hostlen, i;
	bool numeric = true;
	char *host;
	struct in_addr in4;

	if (namelen < 2 || name[0] == '[' || memchr(name, '\0', namelen)) {
		return NULL;
	}
	colon = memchr(name, ':', namelen - 1);
	if (!colon || colon == name) {
		return NULL;
	}
	hostlen = colon - name;
	/* Same cyfry i kropki: literal IPv4, takze w formach skroconych ("127.1"),
	 * ktore inet_pton odrzuca, a getaddrinfo (inet_aton) przyjmuje. */
	for (i = 0; i < hostlen; i++) {
		if (!((name[i] >= '0' && name[i] <= '9') || name[i] == '.')) {
			numeric = false;
			break;
		}
	}
	if (numeric) {
		return NULL;
	}
	host = estrndup(name, hostlen);
	if (inet_pton(AF_INET, host, &in4) == 1) {
		efree(host);
		return NULL;
	}
	*portno = atoi(colon + 1);
	return host;
}
/* }}} */

/* "ip:port" albo "[ip6]:port" dla jednego adresu z listy; 0 = rodzina, ktorej
 * nie znamy (upstream tez ja pomija). */
static size_t fpm_fiber_dns_format_name(const struct evutil_addrinfo *ai, int portno, char *buf, size_t buflen) /* {{{ */
{
	char ip[INET6_ADDRSTRLEN];
	int n;

	if (ai->ai_family == AF_INET) {
		if (!inet_ntop(AF_INET, &((struct sockaddr_in *) ai->ai_addr)->sin_addr, ip, sizeof(ip))) {
			return 0;
		}
		n = snprintf(buf, buflen, "%s:%d", ip, portno);
	} else if (ai->ai_family == AF_INET6) {
		if (!inet_ntop(AF_INET6, &((struct sockaddr_in6 *) ai->ai_addr)->sin6_addr, ip, sizeof(ip))) {
			return 0;
		}
		n = snprintf(buf, buflen, "[%s]:%d", ip, portno);
	} else {
		return 0;
	}
	return (n > 0 && (size_t) n < buflen) ? (size_t) n : 0;
}
/* }}} */

/* Ten sam komunikat i to samo E_WARNING, co php_network_getaddresses. */
static void fpm_fiber_dns_report(php_stream_xport_param *xparam, const char *host, const char *reason) /* {{{ */
{
	if (xparam->outputs.error_text) {
		zend_string_release_ex(xparam->outputs.error_text, 0);
		xparam->outputs.error_text = NULL;
	}
	if (xparam->want_errortext) {
		xparam->outputs.error_text = strpprintf(0, "php_network_getaddresses: getaddrinfo for %s failed: %s", host, reason);
		php_error_docref(NULL, E_WARNING, "%s", ZSTR_VAL(xparam->outputs.error_text));
	} else {
		php_error_docref(NULL, E_WARNING, "php_network_getaddresses: getaddrinfo for %s failed: %s", host, reason);
	}
	xparam->outputs.returncode = -1;
}
/* }}} */

/* Timeout connectu jak widzi go network.c: NULL albo ujemny = bez limitu. */
static struct timeval *fpm_fiber_connect_timeout(struct timeval *tv) /* {{{ */
{
	if (!tv || tv->tv_sec < 0) {
		return NULL;
	}
	return tv;
}
/* }}} */

/* Ile zostalo do deadline; false = juz po czasie. */
static bool fpm_fiber_time_left(const struct timeval *deadline, struct timeval *left) /* {{{ */
{
	struct timeval now;

	evutil_gettimeofday(&now, NULL);
	if (!evutil_timercmp(&now, deadline, <)) {
		return false;
	}
	evutil_timersub(deadline, &now, left);
	return true;
}
/* }}} */

/* --- connect ------------------------------------------------------------------ */

/* Jedna proba connect(): oryginal robi connect nieblokujaco i poll-uje
 * z timeoutem (network.c php_network_connect_socket). Prosimy go o wariant
 * ASYNC (wraca z EINPROGRESS bez czekania i zostawia gniazdo w O_NONBLOCK),
 * czekamy na zapisywalnosc w schedulerze, sprawdzamy SO_ERROR i
 * przywracamy tryb blokujacy, jak zrobilby oryginal. */
static int fpm_fiber_xop_connect_once(php_stream *stream, const php_stream_ops *orig, int option, int value, php_stream_xport_param *xparam) /* {{{ */
{
	php_netstream_data_t *sock;
	int ret, fl, w, err = 0;
	socklen_t len = sizeof(err);

	xparam->op = STREAM_XPORT_OP_CONNECT_ASYNC;
	FPM_FIBER_DELEGATE(stream, orig, ret = orig->set_option(stream, option, value, xparam));
	xparam->op = STREAM_XPORT_OP_CONNECT;

	if (ret != PHP_STREAM_OPTION_RETURN_OK || xparam->outputs.returncode != 1) {
		return ret;	/* od razu polaczone (returncode 0) albo blad (-1) */
	}

	sock = (php_netstream_data_t *) stream->abstract;
	w = fpm_pool_fiber_wait_fd(sock->socket, EV_WRITE, fpm_fiber_connect_timeout(xparam->inputs.timeout));
	if (w < 0) {
		/* nie mozemy czekac: dokoncz blokujaco jak oryginal */
		int events = PHP_POLLREADABLE | POLLOUT;
		int n;

		do {
			n = php_pollfd_for(sock->socket, events, xparam->inputs.timeout);
		} while (n < 0 && errno == EINTR);
		w = n > 0 ? 1 : 0;
	}

	if (w == 0) {
		err = ETIMEDOUT;
	} else if (getsockopt(sock->socket, SOL_SOCKET, SO_ERROR, (char *) &err, &len) != 0) {
		err = errno;
	}

	fl = fcntl(sock->socket, F_GETFL);
	if (fl >= 0 && (fl & O_NONBLOCK)) {
		fcntl(sock->socket, F_SETFL, fl & ~O_NONBLOCK);
	}

	if (err) {
		xparam->outputs.error_code = err;
		if (xparam->want_errortext) {
			xparam->outputs.error_text = php_socket_error_str(err);
		}
		close(sock->socket);
		sock->socket = -1;
		xparam->outputs.returncode = -1;
	} else {
		xparam->outputs.error_code = 0;
		xparam->outputs.returncode = 0;
	}
	return PHP_STREAM_OPTION_RETURN_OK;
}
/* }}} */

/* connect() z nazwa hosta: rozwiazujemy ja przez evdns, a potem probujemy
 * kolejne adresy jak php_network_connect_socket_to_host — kazdy z resztka
 * wspolnego timeoutu, ostatni blad wygrywa. Literal IP, unix, brak bazy evdns
 * albo brak mozliwosci czekania: jedna proba z nazwa nietknieta, czyli
 * dokladnie dotychczasowa sciezka. */
static int fpm_fiber_xop_connect(php_stream *stream, const struct fpm_fiber_ops_map_s *m, int option, int value, php_stream_xport_param *xparam) /* {{{ */
{
	struct evdns_base *dns;
	struct evutil_addrinfo *res = NULL, *ai;
	struct timeval *timeout, deadline, left;
	char *host = NULL;
	char name[INET6_ADDRSTRLEN + sizeof("[]:65535")];
	int portno = 0, gai_err = 0, ret;
	enum fpm_fiber_dns_status st;

	if (!m->dns || !(host = fpm_fiber_dns_host_to_resolve(xparam->inputs.name, xparam->inputs.namelen, &portno))) {
		return fpm_fiber_xop_connect_once(stream, m->orig, option, value, xparam);
	}
	dns = fpm_fiber_dns_base();
	if (!dns) {
		efree(host);
		return fpm_fiber_xop_connect_once(stream, m->orig, option, value, xparam);
	}

	timeout = fpm_fiber_connect_timeout(xparam->inputs.timeout);
	if (timeout) {
		evutil_gettimeofday(&deadline, NULL);
		evutil_timeradd(&deadline, timeout, &deadline);
	}

	st = fpm_fiber_dns_resolve(dns, host, timeout, &res, &gai_err);
	switch (st) {
		case FPM_FIBER_DNS_NOWAIT:
			efree(host);
			return fpm_fiber_xop_connect_once(stream, m->orig, option, value, xparam);
		case FPM_FIBER_DNS_TIMEOUT:
			fpm_fiber_dns_report(xparam, host, "timed out");
			xparam->outputs.error_code = ETIMEDOUT;
			efree(host);
			return PHP_STREAM_OPTION_RETURN_OK;
		case FPM_FIBER_DNS_FAIL:
			fpm_fiber_dns_report(xparam, host, evutil_gai_strerror(gai_err));
			xparam->outputs.error_code = 0;	/* jak upstream: getaddrinfo nie ustawia errno strumienia */
			efree(host);
			return PHP_STREAM_OPTION_RETURN_OK;
		case FPM_FIBER_DNS_OK:
			break;
	}

	{
		char *orig_name = xparam->inputs.name;
		size_t orig_namelen = xparam->inputs.namelen;
		struct timeval *orig_timeout = xparam->inputs.timeout;

		ret = PHP_STREAM_OPTION_RETURN_OK;
		xparam->outputs.returncode = -1;
		for (ai = res; ai; ai = ai->ai_next) {
			size_t n = fpm_fiber_dns_format_name(ai, portno, name, sizeof(name));

			if (n == 0) {
				continue;
			}
			if (timeout) {
				if (!fpm_fiber_time_left(&deadline, &left)) {
					break;	/* po czasie: nie probujemy dalszych adresow (upstream: fatal) */
				}
				xparam->inputs.timeout = &left;
			}
			if (xparam->outputs.error_text) {
				zend_string_release_ex(xparam->outputs.error_text, 0);
				xparam->outputs.error_text = NULL;
			}
			xparam->inputs.name = name;
			xparam->inputs.namelen = n;
			ret = fpm_fiber_xop_connect_once(stream, m->orig, option, value, xparam);
			if (ret != PHP_STREAM_OPTION_RETURN_OK || xparam->outputs.returncode != -1) {
				break;
			}
		}
		xparam->inputs.name = orig_name;
		xparam->inputs.namelen = orig_namelen;
		xparam->inputs.timeout = orig_timeout;
	}

	if (xparam->outputs.returncode == -1 && !xparam->outputs.error_text && xparam->want_errortext) {
		/* zadnego adresu nie dalo sie nawet sprobowac (rodziny, ktorych nie
		 * obslugujemy, albo deadline minal przed pierwsza proba) */
		xparam->outputs.error_text = strpprintf(0, "php_network_getaddresses: getaddrinfo for %s failed: %s", host,
			timeout && !fpm_fiber_time_left(&deadline, &left) ? "timed out" : "no usable address");
	}

	evutil_freeaddrinfo(res);
	efree(host);
	return ret;
}
/* }}} */

static int fpm_fiber_xop_set_option(php_stream *stream, int option, int value, void *ptrparam) /* {{{ */
{
	const struct fpm_fiber_ops_map_s *m = fpm_fiber_ops_entry(stream->ops);
	int ret;

	if (option == PHP_STREAM_OPTION_XPORT_API && ptrparam) {
		php_stream_xport_param *xparam = (php_stream_xport_param *) ptrparam;

		if (xparam->op == STREAM_XPORT_OP_CONNECT && fpm_pool_fiber_can_wait()) {
			return fpm_fiber_xop_connect(stream, m, option, value, xparam);
		}
	}
	FPM_FIBER_DELEGATE(stream, m->orig, ret = m->orig->set_option(stream, option, value, ptrparam));
	return ret;
}
/* }}} */

static const php_stream_ops *fpm_fiber_wrap_ops(const php_stream_ops *orig, bool dns) /* {{{ */
{
	int i;
	struct fpm_fiber_ops_map_s *m;

	for (i = 0; i < fpm_fiber_ops_count; i++) {
		if (fpm_fiber_ops_map[i].orig == orig) {
			return &fpm_fiber_ops_map[i].wrap;
		}
		if (&fpm_fiber_ops_map[i].wrap == orig) {
			return orig;	/* juz opakowane */
		}
	}
	if (fpm_fiber_ops_count == FPM_FIBER_OPS_MAX) {
		return NULL;
	}
	m = &fpm_fiber_ops_map[fpm_fiber_ops_count++];
	m->orig = orig;
	m->dns = dns;
	m->wrap = *orig;
	m->wrap.write = fpm_fiber_xop_write;
	m->wrap.read = fpm_fiber_xop_read;
	m->wrap.close = fpm_fiber_xop_close;
	m->wrap.flush = fpm_fiber_xop_flush;
	m->wrap.cast = orig->cast ? fpm_fiber_xop_cast : NULL;
	m->wrap.stat = orig->stat ? fpm_fiber_xop_stat : NULL;
	m->wrap.set_option = orig->set_option ? fpm_fiber_xop_set_option : NULL;
	return &m->wrap;
}
/* }}} */

static php_stream *fpm_fiber_xport_factory_ex(php_stream_transport_factory orig_factory, bool dns,
		const char *proto, size_t protolen,
		const char *resourcename, size_t resourcenamelen,
		const char *persistent_id, int options, int flags,
		struct timeval *timeout,
		php_stream_context *context STREAMS_DC) /* {{{ */
{
	php_stream *stream;

	/* Polaczenia TRWALE sa zabronione w tym executorze. Lista trwalych strumieni
	 * (EG(persistent_list)) jest PROCESOWA, a rdzen coop jej nie podmienia przy
	 * przelaczaniu requestow — wiec dwa requesty w locie moga dostac TEN SAM
	 * socket. Protokoly bazodanowe sa naprzemienne (zapytanie-odpowiedz), wiec
	 * to nie jest spowolnienie, tylko rozjechany protokol.
	 *
	 * Zmierzone na poligonie (Ubuntu 26.04, epoll, jeden worker, 4 rownolegle
	 * requesty, ten sam DSN): PDO z ATTR_PERSISTENT — w jednym przebiegu trzy
	 * requesty zawisly do timeoutu klienta, w drugim trzy dostaly 502, czwarty
	 * "PDOException: Trying to access array offset on false", a worker padl
	 * i zostal wymieniony. Bez persistent ten sam test: 1,019 s, czysto.
	 *
	 * mysqli z prefiksem "p:" w tym samym tescie NIE psul sie — wydawal kolejne
	 * polaczenia z puli (cztery rozne identyfikatory sesji) i konczyl w 1,017 s.
	 * Blokujemy mimo to OBA, bo bezpieczenstwo zalezy wtedy od tego, czy dany
	 * klient pilnuje zajetosci polaczenia, a tego nie kontrolujemy ani nie
	 * widzimy z tej warstwy. Lepiej odmowic glosno przy nawiazywaniu polaczenia
	 * niz rozjechac protokol w losowym requescie.
	 *
	 * Odmowa jest tutaj, a nie w validate(), bo persistent to atrybut polaczenia
	 * podawany w kodzie aplikacji, a nie dyrektywa konfiguracji — w momencie
	 * walidacji poola nie ma czego sprawdzac. */
	if (persistent_id && fpm_pool_fiber_can_wait()) {
		/* PDO lapie blad polaczenia i rzuca wlasny PDOException ("Unknown error
		 * while connecting"), wiec ostrzezenie ponizej NIE dociera do autora
		 * kodu — zmierzone. Zeby operator mial czego szukac, mowimy to raz na
		 * proces do logu workera. */
		if (!fpm_fiber_persistent_warned) {
			fpm_fiber_persistent_warned = true;
			/* NIE logujemy persistent_id: PDO sklada ten klucz z DSN wraz z
			 * uzytkownikiem i haslem, wiec trafiloby to do error logu. */
			zlog(ZLOG_NOTICE, "[pool %s] fiber: refused a persistent stream; "
				"the persistent stream list is per process while this process serves many "
				"requests at once, so two requests could share one socket. Measured with "
				"PDO::ATTR_PERSISTENT: hung requests, 502s and a dead worker. Drop "
				"PDO::ATTR_PERSISTENT or the \"p:\" prefix — see docs/fiber_errors.md",
				fpm_coop_pool_name());
		}
		php_error_docref(NULL, E_WARNING,
			"persistent connections are not supported with pool.executor = fiber: "
			"the persistent stream list is per process while this process serves many "
			"requests at once, so two requests could share one socket (see "
			"docs/fiber_errors.md); drop PDO::ATTR_PERSISTENT or the \"p:\" prefix");
		return NULL;
	}

	stream = orig_factory(proto, protolen, resourcename, resourcenamelen,
		persistent_id, options, flags, timeout, context STREAMS_REL_CC);

	if (stream) {
		const php_stream_ops *wrap = fpm_fiber_wrap_ops(stream->ops, dns);

		if (wrap) {
			stream->ops = wrap;
		}
	}
	return stream;
}
/* }}} */

static php_stream *fpm_fiber_tcp_factory(const char *proto, size_t protolen,
		const char *resourcename, size_t resourcenamelen,
		const char *persistent_id, int options, int flags,
		struct timeval *timeout,
		php_stream_context *context STREAMS_DC) /* {{{ */
{
	return fpm_fiber_xport_factory_ex(fpm_fiber_orig_tcp_factory, true, proto, protolen, resourcename, resourcenamelen,
		persistent_id, options, flags, timeout, context STREAMS_REL_CC);
}
/* }}} */

static php_stream *fpm_fiber_unix_factory(const char *proto, size_t protolen,
		const char *resourcename, size_t resourcenamelen,
		const char *persistent_id, int options, int flags,
		struct timeval *timeout,
		php_stream_context *context STREAMS_DC) /* {{{ */
{
	return fpm_fiber_xport_factory_ex(fpm_fiber_orig_unix_factory, false, proto, protolen, resourcename, resourcenamelen,
		persistent_id, options, flags, timeout, context STREAMS_REL_CC);
}
/* }}} */

void fpm_pool_fiber_xport_install(void) /* {{{ */
{
	HashTable *xports = php_stream_xport_get_hash();

	fpm_fiber_orig_tcp_factory = zend_hash_str_find_ptr(xports, "tcp", sizeof("tcp") - 1);
	fpm_fiber_orig_unix_factory = zend_hash_str_find_ptr(xports, "unix", sizeof("unix") - 1);

	if (fpm_fiber_orig_tcp_factory) {
		php_stream_xport_register("tcp", fpm_fiber_tcp_factory);
	}
	if (fpm_fiber_orig_unix_factory) {
		php_stream_xport_register("unix", fpm_fiber_unix_factory);
	}

	zlog(ZLOG_DEBUG, "[pool %s] fiber: stream transports hooked: tcp=%s unix=%s",
		fpm_coop_pool_name(),
		fpm_fiber_orig_tcp_factory ? (fpm_fiber_orig_tcp_factory == php_stream_generic_socket_factory ? "generic" : "other (openssl?)") : "none",
		fpm_fiber_orig_unix_factory ? "generic" : "none");
}
/* }}} */

/* fpm-ng: pool.type = fiber — przechwycenie gniazd w warstwie strumieni.
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
 * Czego to NIE lapie (bo nie idzie przez transporty strumieni): sleep(),
 * curl, libpq (pdo_pgsql), zwykle pliki, DNS (getaddrinfo w connect),
 * TLS handshake i SSL_read/SSL_write (openssl ma wlasne pollowanie).
 */

#include "fpm_config.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>

#include <event2/event.h>

#include "php.h"
#include "php_network.h"
#include "php_streams.h"

#include "fpm_pool_coop.h"
#include "fpm_pool_fiber.h"
#include "zlog.h"

/* Opakowanie ops per oryginalna tablica (generyczna tcp, unix, openssl-owa). */
struct fpm_fiber_ops_map_s {
	const php_stream_ops *orig;
	php_stream_ops wrap;
};
#define FPM_FIBER_OPS_MAX 4
static struct fpm_fiber_ops_map_s fpm_fiber_ops_map[FPM_FIBER_OPS_MAX];
static int fpm_fiber_ops_count = 0;

static php_stream_transport_factory fpm_fiber_orig_tcp_factory;
static php_stream_transport_factory fpm_fiber_orig_unix_factory;

static const php_stream_ops *fpm_fiber_orig_ops(const php_stream_ops *wrap) /* {{{ */
{
	int i;

	for (i = 0; i < fpm_fiber_ops_count; i++) {
		if (&fpm_fiber_ops_map[i].wrap == wrap) {
			return fpm_fiber_ops_map[i].orig;
		}
	}
	return NULL;
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

/* connect(): oryginal robi connect nieblokujaco i poll-uje z timeoutem
 * (network.c php_network_connect_socket). Prosimy go o wariant ASYNC
 * (wraca z EINPROGRESS bez czekania i zostawia gniazdo w O_NONBLOCK),
 * czekamy na zapisywalnosc w schedulerze, sprawdzamy SO_ERROR i
 * przywracamy tryb blokujacy, jak zrobilby oryginal. */
static int fpm_fiber_xop_connect(php_stream *stream, const php_stream_ops *orig, int option, int value, php_stream_xport_param *xparam) /* {{{ */
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
	w = fpm_pool_fiber_wait_fd(sock->socket, EV_WRITE, xparam->inputs.timeout);
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

static int fpm_fiber_xop_set_option(php_stream *stream, int option, int value, void *ptrparam) /* {{{ */
{
	const php_stream_ops *orig = fpm_fiber_orig_ops(stream->ops);
	int ret;

	if (option == PHP_STREAM_OPTION_XPORT_API && ptrparam) {
		php_stream_xport_param *xparam = (php_stream_xport_param *) ptrparam;

		if (xparam->op == STREAM_XPORT_OP_CONNECT && fpm_pool_fiber_can_wait()) {
			return fpm_fiber_xop_connect(stream, orig, option, value, xparam);
		}
	}
	FPM_FIBER_DELEGATE(stream, orig, ret = orig->set_option(stream, option, value, ptrparam));
	return ret;
}
/* }}} */

static const php_stream_ops *fpm_fiber_wrap_ops(const php_stream_ops *orig) /* {{{ */
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

static php_stream *fpm_fiber_xport_factory_ex(php_stream_transport_factory orig_factory,
		const char *proto, size_t protolen,
		const char *resourcename, size_t resourcenamelen,
		const char *persistent_id, int options, int flags,
		struct timeval *timeout,
		php_stream_context *context STREAMS_DC) /* {{{ */
{
	php_stream *stream = orig_factory(proto, protolen, resourcename, resourcenamelen,
		persistent_id, options, flags, timeout, context STREAMS_REL_CC);

	if (stream) {
		const php_stream_ops *wrap = fpm_fiber_wrap_ops(stream->ops);

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
	return fpm_fiber_xport_factory_ex(fpm_fiber_orig_tcp_factory, proto, protolen, resourcename, resourcenamelen,
		persistent_id, options, flags, timeout, context STREAMS_REL_CC);
}
/* }}} */

static php_stream *fpm_fiber_unix_factory(const char *proto, size_t protolen,
		const char *resourcename, size_t resourcenamelen,
		const char *persistent_id, int options, int flags,
		struct timeval *timeout,
		php_stream_context *context STREAMS_DC) /* {{{ */
{
	return fpm_fiber_xport_factory_ex(fpm_fiber_orig_unix_factory, proto, protolen, resourcename, resourcenamelen,
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

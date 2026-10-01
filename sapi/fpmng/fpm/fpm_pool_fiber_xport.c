/* fpm-ng: pool.executor = fiber — intercept sockets at the stream layer.
 *
 * php_stream_xport_register() (main/streams/php_stream_transport.h, PHPAPI)
 * lets us replace the "tcp"/"unix" transport factory. Our factory calls the
 * ORIGINAL (generic from streams.c or the OpenSSL one when ext/openssl is
 * compiled in — it overrides "tcp" in MINIT) and replaces the ops table in the
 * created stream with a wrapper. Before every read/write/connect, the wrapper
 * checks whether the operation would block and, if so, suspends the request
 * Fiber until the descriptor is ready (a POLL operation through the IO seam,
 * fpm_pool_fiber_io.h), then calls the original operation, which no longer
 * blocks.
 *
 * The identity of ops matters: xp_socket.c recognizes unix/udp from the table
 * address (PHP_STREAM_XPORT_IS_UNIX), while ext/sockets checks
 * PHP_STREAM_IS_SOCKET. We therefore restore the original stream->ops for every
 * delegated call. Outside the call the stream has our ops, so
 * ext/sockets' socket_import_stream() does not recognize it (a known limitation).
 *
 * DNS: getaddrinfo() lives inside php_network_connect_socket_to_host, that is,
 * INSIDE the delegated connect, and blocks the whole process. Therefore, for the
 * tcp transport, when the host is not an IP literal, connect first resolves the
 * name with a GETADDRINFO operation through the IO seam (its libevent backend
 * is evdns on the scheduler's event_base, created once per process from
 * /etc/resolv.conf and /etc/hosts — see fpm_pool_fiber_io.c), and the original
 * connect receives an IP literal, making its getaddrinfo() trivial. We try
 * subsequent A/AAAA addresses in order, as upstream does. What this does not
 * change: peer_name/SNI in ext/openssl takes the name from resourcename in the
 * factory, not from the name passed to connect; stream_socket_get_name() is
 * getpeername(); "Unable to connect to <host>" is assembled by ext/standard
 * from the user-supplied name.
 *
 * TLS (ssl/tls/https/...) is non-blocking too, but not from here: it needs code
 * inside ext/openssl (the handshake and SSL_read/SSL_write loops live there and
 * poll internally), so it is done by patch 0007 (patches/0007-fiber-tls-*.patch,
 * HAVE_FPMNG_FIBER_TLS), which wraps the ext/openssl ops table through our
 * fpm_fiber_xport_wrap() and re-arms the ssl/tls transports with its own
 * factory. That keeps every upstream behavior change in a gated patch with an
 * expiry path; this file only lends it the wrapper and its waits
 * (fpm_fiber_xport_tls_active/_wait), so the patch, too, suspends only through
 * the seam and is switched off with this entry ("xport").
 *
 * What this does NOT catch (because it does not go through stream transports):
 * sleep(), curl, libpq (pdo_pgsql), ordinary files, DNS outside connect
 * (gethostbyname, dns_get_record). The TLS handshake and SSL_read/SSL_write
 * block unless patch 0007 (HAVE_FPMNG_FIBER_TLS) is in — without it the ssl/tls
 * transports are upstream's and OpenSSL polls the whole process.
 */

#include "fpm_config.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/time.h>

#include "php.h"
#include "php_network.h"
#include "php_streams.h"

#include "fpm_pool_coop.h"
#include "fpm_pool_fiber_io.h"
#include "fpm_pool_fiber_xport.h"
#include "zlog.h"

static void fpm_pool_fiber_xport_install(void);

/* Registry entry "xport" (fpm_pool_fiber_intercept.c): tcp/unix streams, the
 * DNS lookup inside their connect, and the TLS transports of patch 0007. */
struct fpm_fiber_intercept_s fpm_fiber_xport_intercept = {
	.name = "xport",
	.install = fpm_pool_fiber_xport_install,
};

/* Ops wrapper per original table (generic tcp, unix, OpenSSL).
 * dns: streams from this table connect to host:port (the "tcp" factory), so
 * connect must resolve the name; the "unix" factory receives a path. The flag
 * comes from the factory that wrapped the table, not from comparing ops addresses. */
struct fpm_fiber_ops_map_s {
	const php_stream_ops *orig;
	php_stream_ops wrap;
	bool dns;
};
/* Two entries are enough (generic tcp + unix), but the TLS patch
 * (0007, HAVE_FPMNG_FIBER_TLS) wraps ext/openssl's tables through this map as
 * well — and then only from request fibers, so without headroom a wrap
 * failure would make TLS non-blocking under concurrency but silently blocking
 * outside fibers. One slot stays in reserve for that wrapper. */
#define FPM_FIBER_OPS_MAX 5
static struct fpm_fiber_ops_map_s fpm_fiber_ops_map[FPM_FIBER_OPS_MAX];
static int fpm_fiber_ops_count = 0;

/* Warning about refusing a persistent connection: once per process so one
 * looping application cannot flood the log. */
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

/* Is the socket ready NOW (without waiting)? One cheap poll instead of
 * registering with libevent — data is often already available (a response in
 * one packet). */
static int fpm_fiber_ready_now(int fd, int pollev) /* {{{ */
{
	int r;

	do {
		r = php_pollfd_for_ms(fd, pollev, 0);
	} while (r < 0 && errno == EINTR);
	return r != 0;	/* >0 ready; <0 error — let the original operation report it */
}
/* }}} */

/* One POLL operation through the seam. 1 = ready, 0 = timeout, -1 = did not
 * wait (the caller blocks as usual). CANCELLED (never produced by the libevent
 * backend) counts as "not ready", like a timeout. */
static int fpm_fiber_wait_fd(int fd, unsigned events, struct timeval *timeout) /* {{{ */
{
	struct fpm_fiber_io_op_s op;

	memset(&op, 0, sizeof(op));
	op.type = FPM_FIBER_IO_OP_POLL;
	op.timeout = timeout;
	op.u.poll.fd = fd;
	op.u.poll.events = events;
	switch (fpm_fiber_io_run(&fpm_fiber_xport_intercept, &op)) {
		case FPM_FIBER_IO_READY:
			return 1;
		case FPM_FIBER_IO_UNSUPPORTED:
			return -1;
		default:
			return 0;
	}
}
/* }}} */

/* Wait for the stream socket to become ready using its timeout. 1 = ready,
 * 0 = timeout, -1 = did not wait (block as usual). */
static int fpm_fiber_wait_sock(php_netstream_data_t *sock, int pollev, unsigned ev) /* {{{ */
{
	struct timeval *tv;

	if (!sock || sock->socket == -1 || !sock->is_blocked) {
		return -1;
	}
	/* timeout 0 = do not wait (xp_socket.c: dont_wait) */
	if (sock->timeout.tv_sec == 0 && sock->timeout.tv_usec == 0) {
		return -1;
	}
	if (!fpm_fiber_io_can_suspend(&fpm_fiber_xport_intercept)) {
		return -1;
	}
	if (fpm_fiber_ready_now(sock->socket, pollev)) {
		return 1;
	}
	tv = (sock->timeout.tv_sec == -1) ? NULL : &sock->timeout;
	return fpm_fiber_wait_fd(sock->socket, ev, tv);
}
/* }}} */

/* Delegate while restoring the ops identity. */
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

	/* has_buffered_data: xp_socket.c does not wait in this case anyway */
	if (!stream->has_buffered_data) {
		int w = fpm_fiber_wait_sock(sock, PHP_POLLREADABLE, FPM_FIBER_IO_READ);

		if (w == 0) {
			/* as php_sockop_read does after a timeout without buffered data */
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
	int w = fpm_fiber_wait_sock(sock, POLLOUT, FPM_FIBER_IO_WRITE);

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

/* --- DNS: a GETADDRINFO operation through the seam ------------------------ */

enum fpm_fiber_dns_status {
	FPM_FIBER_DNS_OK,
	FPM_FIBER_DNS_FAIL,		/* *gai_err for fpm_fiber_io_gai_strerror */
	FPM_FIBER_DNS_TIMEOUT,	/* connect timeout elapsed */
	FPM_FIBER_DNS_NOWAIT	/* could not wait, or no async resolver; caller must block */
};

/* Resolve a name without blocking the process. The caller frees the result
 * with fpm_fiber_io_freeaddrinfo. */
static enum fpm_fiber_dns_status fpm_fiber_dns_resolve(const char *host, struct timeval *timeout, struct addrinfo **res, int *gai_err) /* {{{ */
{
	struct fpm_fiber_io_op_s op;

	memset(&op, 0, sizeof(op));
	op.type = FPM_FIBER_IO_OP_GETADDRINFO;
	op.timeout = timeout;
	op.u.getaddrinfo.host = host;
	switch (fpm_fiber_io_run(&fpm_fiber_xport_intercept, &op)) {
		case FPM_FIBER_IO_READY:
			if (op.u.getaddrinfo.gai_error != 0) {
				*gai_err = op.u.getaddrinfo.gai_error;
				return FPM_FIBER_DNS_FAIL;
			}
			*res = op.u.getaddrinfo.res;
			return FPM_FIBER_DNS_OK;
		case FPM_FIBER_IO_UNSUPPORTED:
			return FPM_FIBER_DNS_NOWAIT;
		default:
			return FPM_FIBER_DNS_TIMEOUT;
	}
}
/* }}} */

/* Host from "host:port" according to xp_socket.c parse_ip_address_ex (the
 * first colon is the separator). NULL = an IP literal, the "[v6]:port" form,
 * no port, or something we do not understand — the original then receives the
 * untouched name and handles it itself (including reporting its own syntax
 * errors). */
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
	/* Digits and dots only: an IPv4 literal, including abbreviated forms
	 * ("127.1") that inet_pton rejects but getaddrinfo (inet_aton) accepts. */
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

/* "ip:port" or "[ip6]:port" for one address from the list; 0 = an unknown
 * family (upstream skips it too). */
static size_t fpm_fiber_dns_format_name(const struct addrinfo *ai, int portno, char *buf, size_t buflen) /* {{{ */
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

/* The same message and E_WARNING as php_network_getaddresses. */
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

/* Connect timeout as network.c sees it: NULL or negative = unlimited. */
static struct timeval *fpm_fiber_connect_timeout(struct timeval *tv) /* {{{ */
{
	if (!tv || tv->tv_sec < 0) {
		return NULL;
	}
	return tv;
}
/* }}} */

/* Time remaining until the deadline; false = already expired. */
static bool fpm_fiber_time_left(const struct timeval *deadline, struct timeval *left) /* {{{ */
{
	struct timeval now;

	gettimeofday(&now, NULL);
	if (!timercmp(&now, deadline, <)) {
		return false;
	}
	timersub(deadline, &now, left);
	return true;
}
/* }}} */

/* --- connect ------------------------------------------------------------------ */

/* One connect() attempt: the original performs a non-blocking connect and
 * polls with a timeout (network.c php_network_connect_socket). Ask it for the
 * ASYNC variant (returns EINPROGRESS without waiting and leaves the socket in
 * O_NONBLOCK), wait for writability in the scheduler, check SO_ERROR, and
 * restore blocking mode as the original would. */
static int fpm_fiber_xop_connect_once(php_stream *stream, const php_stream_ops *orig, int option, int value, php_stream_xport_param *xparam) /* {{{ */
{
	php_netstream_data_t *sock;
	int ret, fl, w, err = 0;
	socklen_t len = sizeof(err);

	xparam->op = STREAM_XPORT_OP_CONNECT_ASYNC;
	FPM_FIBER_DELEGATE(stream, orig, ret = orig->set_option(stream, option, value, xparam));
	xparam->op = STREAM_XPORT_OP_CONNECT;

	if (ret != PHP_STREAM_OPTION_RETURN_OK || xparam->outputs.returncode != 1) {
		return ret;	/* connected immediately (returncode 0) or failed (-1) */
	}

	sock = (php_netstream_data_t *) stream->abstract;
	w = fpm_fiber_wait_fd(sock->socket, FPM_FIBER_IO_WRITE, fpm_fiber_connect_timeout(xparam->inputs.timeout));
	if (w < 0) {
		/* Cannot wait: finish with blocking behavior as the original does. */
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

/* connect() with a host name: resolve it through the seam, then try subsequent
 * addresses as php_network_connect_socket_to_host does — each with the
 * remaining shared timeout; the last error wins. An IP literal, unix, no async
 * resolver (UNSUPPORTED), or inability to wait: one attempt with the untouched
 * name, exactly the existing path. */
static int fpm_fiber_xop_connect(php_stream *stream, const struct fpm_fiber_ops_map_s *m, int option, int value, php_stream_xport_param *xparam) /* {{{ */
{
	struct addrinfo *res = NULL, *ai;
	struct timeval *timeout, deadline, left;
	char *host = NULL;
	char name[INET6_ADDRSTRLEN + sizeof("[]:65535")];
	int portno = 0, gai_err = 0, ret;
	enum fpm_fiber_dns_status st;

	if (!m->dns || !(host = fpm_fiber_dns_host_to_resolve(xparam->inputs.name, xparam->inputs.namelen, &portno))) {
		return fpm_fiber_xop_connect_once(stream, m->orig, option, value, xparam);
	}
	timeout = fpm_fiber_connect_timeout(xparam->inputs.timeout);
	if (timeout) {
		gettimeofday(&deadline, NULL);
		timeradd(&deadline, timeout, &deadline);
	}

	st = fpm_fiber_dns_resolve(host, timeout, &res, &gai_err);
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
			fpm_fiber_dns_report(xparam, host, fpm_fiber_io_gai_strerror(gai_err));
			xparam->outputs.error_code = 0;	/* as upstream: getaddrinfo does not set the stream errno */
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
					break;	/* expired: do not try further addresses (upstream: fatal) */
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
		/* No address could even be tried (unsupported families, or the deadline
		 * expired before the first attempt). */
		xparam->outputs.error_text = strpprintf(0, "php_network_getaddresses: getaddrinfo for %s failed: %s", host,
			timeout && !fpm_fiber_time_left(&deadline, &left) ? "timed out" : "no usable address");
	}

	fpm_fiber_io_freeaddrinfo(res);
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

		if (xparam->op == STREAM_XPORT_OP_CONNECT && fpm_fiber_io_can_suspend(&fpm_fiber_xport_intercept)) {
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
			return orig;	/* already wrapped */
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

	/* PERSISTENT connections are forbidden in this executor. The persistent
	 * stream list (EG(persistent_list)) is PROCESS-WIDE, and the coop core does
	 * not swap it when switching requests — so two in-flight requests may receive
	 * THE SAME socket. Database protocols are alternating (query-response), so
	 * this is not a slowdown but a corrupted protocol.
	 *
	 * Measured on the testbed (Ubuntu 26.04, epoll, one worker, 4 concurrent
	 * requests, same DSN): PDO with ATTR_PERSISTENT — in one run three requests
	 * hung until the client timeout; in another three returned 502, the fourth
	 * "PDOException: Trying to access array offset on false", and the worker died
	 * and was replaced. Without persistent, the same test: 1.019 s, clean.
	 *
	 * mysqli with the "p:" prefix did NOT fail in the same test — it opened
	 * subsequent connections from the pool (four different session identifiers)
	 * and finished in 1.017 s. We still block BOTH because safety then depends on
	 * whether the client correctly manages connection ownership, which we neither
	 * control nor see from this layer. It is better to refuse loudly while
	 * opening the connection than to corrupt the protocol in a random request.
	 *
	 * Refuse here rather than in validate() because persistent is a connection
	 * attribute supplied in application code, not a configuration directive —
	 * there is nothing to check when validating the pool. */
	if (persistent_id && fpm_fiber_io_can_suspend(&fpm_fiber_xport_intercept)) {
		/* PDO catches the connection error and throws its own PDOException
		 * ("Unknown error while connecting"), so the warning below does NOT reach
		 * the code author — measured. To give the operator something to search for,
		 * log it once per process in the worker log. */
		if (!fpm_fiber_persistent_warned) {
			fpm_fiber_persistent_warned = true;
			/* Do NOT log persistent_id: PDO builds this key from the DSN together
			 * with the username and password, so it would end up in the error log. */
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

/* Exported for the TLS patch (0007, patches/0007-fiber-tls-*.patch): its
 * ssl/tls factory returns a stream whose ops point DIRECTLY at ext/openssl's
 * table; wrapping happens on the first call from a request fiber
 * (fpm_fiber_tls_wrap_ops_once there calls into this). Returning NULL keeps
 * the stream functional but blocking — must never happen: we keep one map
 * slot in reserve (FPM_FIBER_OPS_MAX comment). */
const php_stream_ops *fpm_fiber_xport_wrap(const php_stream_ops *orig) /* {{{ */
{
	return fpm_fiber_wrap_ops(orig, false);
}
/* }}} */

/* Exported for the TLS patch (0007): its replacement for php_pollfd_for() and
 * its "are we in a request fiber" check, both through this entry, so the patch
 * never sees the scheduler and fiber.disable_interceptions = xport turns it
 * off too. poll_events are the poll(2) bits upstream passes: POLLOUT|POLLPRI
 * for WANT_WRITE and POLLIN|POLLPRI for WANT_READ. POLLPRI rides along in
 * BOTH, so it must not decide the direction: POLLOUT first, and POLLOUT|POLLIN
 * waits for either. Returns as php_pollfd_for: 1 ready, 0 timeout, -1 = did
 * not wait (the caller polls as upstream). */
int fpm_fiber_xport_tls_active(void) /* {{{ */
{
	return fpm_fiber_io_can_suspend(&fpm_fiber_xport_intercept);
}
/* }}} */

int fpm_fiber_xport_tls_wait(php_socket_t fd, int poll_events, struct timeval *timeout) /* {{{ */
{
	unsigned ev = (poll_events & POLLOUT)
		? ((poll_events & POLLIN) ? (FPM_FIBER_IO_READ | FPM_FIBER_IO_WRITE) : FPM_FIBER_IO_WRITE)
		: FPM_FIBER_IO_READ;

	return fpm_fiber_wait_fd(fd, ev, timeout);
}
/* }}} */

static void fpm_pool_fiber_xport_install(void) /* {{{ */
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
#ifdef HAVE_FPMNG_FIBER_TLS
	fpm_fiber_tls_xport_install();
#endif

	zlog(ZLOG_DEBUG, "[pool %s] fiber: stream transports hooked: tcp=%s unix=%s",
		fpm_coop_pool_name(),
		fpm_fiber_orig_tcp_factory ? (fpm_fiber_orig_tcp_factory == php_stream_generic_socket_factory ? "generic" : "other (openssl?)") : "none",
		fpm_fiber_orig_unix_factory ? "generic" : "none");
}
/* }}} */

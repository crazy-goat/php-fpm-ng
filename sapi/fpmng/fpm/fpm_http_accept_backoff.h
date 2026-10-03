/* Accept-error backoff for the gateway's listeners (issue #687).
 *
 * When accept() fails with EMFILE or ENFILE the pending connection stays in
 * the kernel queue, so the listening fd stays readable and the level-triggered
 * libevent read event fires again at once. libevent 2.1 listener_read_cb()
 * treats only EAGAIN/ECONNABORTED/EINTR as retriable; for anything else it
 * calls the evconnlistener error callback, or without one only logs through
 * event_sock_warn() and returns. Without a callback the loop therefore spins
 * on one core for as long as the process is out of descriptors, starving
 * every connection it already holds, and those are exactly the ones that
 * would release descriptors if they got any CPU.
 *
 * The callback installed here pauses the listener for a short interval and
 * then resumes it, which turns the spin into a poll every
 * FPM_HTTP_ACCEPT_BACKOFF_MS. The queued connections are not lost: they are
 * accepted as soon as a descriptor is free again.
 */

#ifndef FPM_HTTP_ACCEPT_BACKOFF_H
#define FPM_HTTP_ACCEPT_BACKOFF_H 1

#include <event2/event.h>
#include <event2/http.h>

#define FPM_HTTP_ACCEPT_BACKOFF_MS 100

/* Installs the error callback on the listener behind `bound`. `pool` and
 * `what` (for example "main" or "http.plain_listen") are kept by reference
 * for log lines and must outlive the event loop. Returns 0 on success, -1 on
 * allocation failure or when the resume timer cannot be created (the
 * listener then works as before, without the backoff). */
int fpm_http_accept_backoff_install(struct event_base *base, struct evhttp_bound_socket *bound,
		const char *pool, const char *what);

#endif

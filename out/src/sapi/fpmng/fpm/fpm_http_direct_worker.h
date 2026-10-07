/* fpm-ng: worker-mode HTTP-direct (POC, task 073). See fpm_http_direct_worker.c. */

#ifndef FPM_HTTP_DIRECT_WORKER_H
#define FPM_HTTP_DIRECT_WORKER_H 1

struct fpm_worker_pool_s;

/* Compile-time default for worker.max_pending (issue #331), used by
 * fpm_conf.c to seed wp->config->worker_max_pending before the pool's own
 * config is parsed. Bounds the memory a client burst can pin in
 * accepted-but-unanswered requests; beyond it the transport answers 503
 * itself, as the gateway does when its worker budget is exhausted
 * (fpm_http.c:153-155). Runtime checks in fpm_http_direct_worker.c use the
 * configured value, never this macro directly. */
#define FPM_WORKER_PENDING_MAX 256

/* Compile-time default for worker.accept_threshold (issue #338): how many
 * connections one worker may accept before it disables its own listener for
 * FPM_WORKER_ACCEPT_COOLDOWN_MS. libevent's listener_read_cb() accepts until
 * the queue is empty, so without a ceiling the first worker to wake takes the
 * whole backlog and, since keep-alive connections stay pinned to their
 * accepting worker, keeps it. This is a rate on the accept path only: it never
 * looks at how many requests a worker is already holding, so it does not cap
 * the concurrency inside one worker. 0 disables it entirely. Runtime checks use
 * wp->config->worker_accept_threshold, never this macro; see "THE ACCEPT
 * CEILING" in fpm_http_direct_worker.c and docs/http-direct.md. */
#define FPM_WORKER_ACCEPT_THRESHOLD 1

/* How long a worker that has just reached that ceiling stays out of the accept
 * race. A worker that answers in microseconds is idle again long before the
 * kernel has scheduled any sibling the same connection woke, so without this
 * delay the incumbent reopens and wins every wakeup -- measured, see the same
 * comment block. Classic's accept gate (issue #53) has the same shape in its
 * 10 ms fpm_direct_tick. The value bounds the accept rate of one worker at
 * worker.accept_threshold connections per this many milliseconds. */
#define FPM_WORKER_ACCEPT_COOLDOWN_MS 5

extern const char *const fpm_http_direct_worker_rejects[];

int fpm_http_direct_worker_validate(struct fpm_worker_pool_s *wp);
void fpm_http_direct_worker_child_main(struct fpm_worker_pool_s *wp);

#endif

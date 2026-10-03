	/* (c) 2007,2008 Andrei Nigmatulin */

#ifndef FPM_REQUEST_H
#define FPM_REQUEST_H 1

/* hanging in accept(). The void entry points are the ones the pristine upstream
 * fpm_main.c passes to fcgi_init_request(); they count every call as a state change. */
void fpm_request_accepting(void);
/* start reading fastcgi request from very first byte */
void fpm_request_reading_headers(void);
/* The same with an explicit counter policy, for the http-direct executors (once per
 * request in the classic executor, one long-lived state with pool.executor = worker).
 * fromActive = false: the worker was not counted active, so active is not decremented;
 * idle is still incremented, but only on the first call. keptAlive = true: the request
 * continues a kept-alive connection, so no counter changes; keptAlive = false applies
 * idle-1 / active+1 (see fpm_request.c). */
void fpm_request_accepting_ex(bool fromActive);
void fpm_request_reading_headers_ex(bool keptAlive);
/* not a stage really but a point in the php code, where all request params have become known to sapi */
void fpm_request_info(void);
/* the script is executing */
void fpm_request_executing(void);
/* request ended: script response have been sent to web server */
void fpm_request_end(void);
/* request processed: cleaning current request */
void fpm_request_finished(void);
/* fpm-ng: request_cpu_tracking — whether to measure request CPU through times() (2 syscalls/request).
 * Called in the child before the accept loop; enabled by default. */
void fpm_request_set_cpu_tracking(bool on);

struct fpm_child_s;
struct timeval;

void fpm_request_check_timed_out(struct fpm_child_s *child, struct timeval *tv, int terminate_timeout, int slowlog_timeout, int track_finished);
int fpm_request_is_idle(struct fpm_child_s *child);
const char *fpm_request_get_stage_name(int stage);
int fpm_request_last_activity(struct fpm_child_s *child, struct timeval *tv);

enum fpm_request_stage_e {
	FPM_REQUEST_CREATING,
	FPM_REQUEST_ACCEPTING,
	FPM_REQUEST_READING_HEADERS,
	FPM_REQUEST_INFO,
	FPM_REQUEST_EXECUTING,
	FPM_REQUEST_END,
	FPM_REQUEST_FINISHED
};

#endif

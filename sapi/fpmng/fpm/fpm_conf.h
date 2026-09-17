	/* (c) 2007,2008 Andrei Nigmatulin */

#ifndef FPM_CONF_H
#define FPM_CONF_H 1

#include <stdint.h>
#include "php.h"

#define PM2STR(a) ((a) == PM_STYLE_STATIC ? "static" : ((a) == PM_STYLE_DYNAMIC ? "dynamic" : "ondemand"))

#define FPM_CONF_MAX_PONG_LENGTH 64

struct key_value_s;
struct fpm_cron_schedule_s;	/* fpm-ng: defined in fpm_cron_schedule.h, here only a pointer */

struct key_value_s {
	struct key_value_s *next;
	char *key;
	char *value;
};

/*
 * Please keep the same order as in fpm_conf.c and in php-fpm.conf.in
 */
struct fpm_global_config_s {
	char *pid_file;
	char *error_log;
#ifdef HAVE_SYSLOG_H
	char *syslog_ident;
	int syslog_facility;
#endif
	int log_level;
	int log_limit;
	int log_buffering;
	int emergency_restart_threshold;
	int emergency_restart_interval;
	int process_control_timeout;
	int process_max;
	int process_priority;
	int daemonize;
	int rlimit_files;
	int rlimit_core;
	char *events_mechanism;
#ifdef HAVE_SYSTEMD
	int systemd_watchdog;
	int systemd_interval;
#endif
	/* issue #330: opt-in switch for selective reload -- default 0 (off)
	 * preserves today's all-pools-restart-together SIGUSR2 behaviour exactly.
	 * See fpm_conf_diff.h/fpm_reload_selective.h for what "on" changes. */
	int reload_selective;
};

extern struct fpm_global_config_s fpm_global_config;

/*
 * Please keep the same order as in fpm_conf.c and in php-fpm.conf.in
 */
struct fpm_worker_pool_config_s {
	char *name;
	char *type;			/* fpm-ng: pool.type, empty = fastcgi (see fpm_pool_type.h) */
	char *executor;			/* fpm-ng: pool.executor, empty = classic */
	char *set_directives;		/* fpm-ng: ";name;name;" of the directives that were actually set,
					 * so the pool type can reject the ones that do not
					 * apply to it — the value alone cannot distinguish
					 * "unset" from "set to the default" */
	char *prefix;
	char *user;
	char *group;
	char *listen_address;
	int listen_backlog;
	/* Using chown */
	char *listen_owner;
	char *listen_group;
	char *listen_mode;
	char *listen_allowed_clients;
	int process_priority;
	int process_dumpable;
	int pm;
	int pm_max_children;
	int pm_start_servers;
	int pm_min_spare_servers;
	int pm_max_spare_servers;
	int pm_max_spawn_rate;
	int pm_process_idle_timeout;
	int pm_max_requests;
	char *pm_status_path;
	char *pm_status_listen;
	char *pm_metrics_path;		/* fpm-ng: Prometheus text on the operator endpoint; see fpm_operator_endpoint.h */
	char *pm_metrics_listen;	/* fpm-ng: where that path binds; default 127.0.0.1:8080 */
	char *ping_path;
	char *ping_response;
	char *access_log;
	char *access_format;
	struct key_value_s *access_suppress_paths;
	char *slowlog;
	int request_slowlog_timeout;
	int request_slowlog_trace_depth;
	int request_terminate_timeout;
	int request_terminate_timeout_track_finished;
	int request_cpu_tracking;		/* fpm-ng: times() at request start and end; feeds "last request cpu" in the status and %C in access.format */
	int rlimit_files;
	int rlimit_core;
	char *chroot;
	char *chdir;
	int catch_workers_output;
	int decorate_workers_output;
	int clear_env;
	char *security_limit_extensions;
	/* fpm-ng: pool.type = supervisor, see fpm_pool_supervisor.c */
	char *supervisor_script;
	int supervisor_processes;
	char *supervisor_restart;		/* "always" (default) | "on-failure" | "never" */
	int supervisor_restart_delay;
	int supervisor_restart_delay_max;
	int supervisor_restart_max;		/* 0 = no limit, never give up */
	int supervisor_stop_timeout;
	int supervisor_fatal;			/* exhausting restart_max kills the entire master */
	/* issue #323: additive on top of the restart_delay/restart_delay_max backoff
	 * above, so several copies of the same pool (or several supervisor pools)
	 * do not retry in lockstep after a shared dependency blips. 0 (both fields
	 * unset) preserves today's exact backoff -- see fpm_pool_supervisor.c. */
	int supervisor_restart_jitter;		/* seconds, used when supervisor_restart_jitter_is_percent == 0 */
	int supervisor_restart_jitter_percent;	/* 0-100, used when supervisor_restart_jitter_is_percent == 1 */
	int supervisor_restart_jitter_is_percent;
	/* issue #323: applied only to the cold start of each of the supervisor.processes
	 * copies (see shared->cold_starts_issued in fpm_pool_supervisor.c), never to
	 * a later restart -- restart_jitter above already covers those. 0 = no jitter
	 * (default), preserving today's simultaneous cold start. */
	int supervisor_start_jitter;
	/* issue #324: memory-triggered recycle, the supervisor.* equivalent of
	 * pm.max_requests. 0 = disabled (default) -- see fpm_pool_supervisor.c. */
	size_t supervisor_max_memory;		/* bytes; ru_maxrss high-water mark, checked after every iteration */
	/* issue #324: the signal used to ask the current script execution to stop
	 * cleanly before supervisor.stop_timeout's SIGKILL -- both on an external
	 * termination request and on a memory-triggered recycle. Defaults to
	 * SIGTERM (today's behavior) in fpm_pool_supervisor_validate(). */
	int supervisor_stop_signal;
	/* issue #326: cap on a SINGLE iteration of supervisor.script, the
	 * supervisor.* equivalent of cron.timeout. 0 = disabled (default) -- see
	 * fpm_pool_supervisor.c for how it is armed/canceled around each iteration. */
	int supervisor_max_runtime;
	/* issue #328: redirects this pool's stdout/stderr straight to a file,
	 * bypassing catch_workers_output's shared pipe/reader thread entirely --
	 * see fpm_pool_output_log.c. Empty/NULL (default) = unchanged behavior:
	 * stdout/stderr are the catch_workers_output pipe when that is set, and
	 * /dev/null otherwise. Plain append, no rotation -- same expectation as
	 * cron_log below. */
	char *supervisor_output_log;
	/* fpm-ng: pool.type = cron, see fpm_pool_cron.c */
	char *cron_schedule;
	char *cron_script;
	int cron_timeout;			/* seconds, 0 = no limit (default) */
	struct fpm_cron_schedule_s *cron_parsed_schedule;	/* filled in by validate() */
	char *cron_timezone;			/* IANA name, e.g. "Europe/Warsaw"; empty/NULL = UTC (default), see fpm_pool_cron.c */
	char *cron_log;			/* optional: one line per run (start, exit code, duration) appended here; see fpm_pool_cron.c */
	int cron_jitter;			/* seconds, max delay added AFTER the scheduled time is due; 0 = no jitter (default),
						 * preserving today's exact-time fire. See fpm_pool_cron.c (issue #322). */
	int cron_jitter_mode;			/* FPM_CRON_JITTER_RANDOM (default) or _STABLE; only meaningful when cron_jitter > 0 */
	/* issue #325: the signal a shutdown/reload asks an actively-running (or
	 * sleeping) cron child to stop with, instead of the hardcoded SIGTERM
	 * every other non-request-serving pool still gets from
	 * fpm_pctl_kill_all() -- see fpm_pool_type_s.stop_signal and
	 * fpm_pool_cron_stop_signal() in fpm_pool_cron.c. Defaults to SIGTERM
	 * (today's behavior) in fpm_pool_cron_validate(). cron.timeout remains the
	 * hard fallback (SIGKILL) regardless of this directive. */
	int cron_stop_signal;
	int cron_expect_within;			/* seconds, 0 = disabled (default); see fpm_pool_cron.c (issue #327) */
	/* issue #328: same as supervisor_output_log above, see fpm_pool_output_log.c
	 * and fpm_pool_cron_child_main(). */
	char *cron_output_log;
	/* fpm-ng: pool.type = http, see fpm_http.c. The gateway starts ONLY when
	 * pool.type = http (see fpm_pool_type.c) — these directives merely tune it,
	 * they never enable it by themselves on another pool type. */
	char *http_listen;			/* empty = FastCGI port + 1 (or required when the pool listens on a UDS) */
	char *http_plain_listen;		/* optional redirect-only plain HTTP companion for a TLS listener */
	int http_gateways;			/* number of gateway processes, default 2 */
	int http_reuseport;			/* each gateway gets its own SO_REUSEPORT socket */
	int http_static;			/* serving static files without PHP, enabled by default */
	int http_fault_upstream_write;		/* test-only fault injection: fail the Nth write towards the pool with ECONNRESET;
						 * 0 = off, the default. See fpm_http_upstream_write_must_fail() for why this is a
						 * directive and not an environment variable. */
	int http_idle_timeout;			/* ms, releases an attached connection after this many idle ms; 0 = never */
	int http_read_timeout;			/* ms, one budget for the whole client-side read (headers + body); 0 = no client read timeout */
	int http_pool_full_policy;		/* FPM_HTTP_POOL_FULL_REJECT (default) or _WAIT; see fpm_http.c and docs/http-gateway-pool-full.md.
						 * "wait" is only a sane trade for IO-light pools -- opt in per pool, never globally. */
	int http_pool_full_queue_max;		/* wait policy only: bound on how many requests may sit on gw->waiting at once;
						 * a full queue rejects immediately rather than growing further (issue #309) */
	int http_pool_full_wait_ms;		/* wait policy only: bound on how long one request may sit on gw->waiting;
						 * an expired wait is rejected the same as a full queue (issue #309) */
	size_t http_max_body;			/* bytes, hard cap on a request body the gateway buffers whole; see fpm_http.c */
	int http_max_connections;		/* http-direct: connections one worker will hold at a time; 0 = unlimited. See fpm_http_direct_conn.h */
	int http_max_connections_per_client;	/* http-direct: connections one peer address may hold on one worker; 0 = unlimited */
	char *http_allowed_clients;		/* like listen.allowed_clients, but for the HTTP gateway; empty = no restriction */
	char *http_trusted_proxies;		/* addresses trusted for X-Forwarded-* headers; empty = trust nobody (safe default), see fpm_http_forwarded.c */
	char *http_access_log;			/* path to the HTTP gateway access log; empty = disabled, see fpm_http_access_log.c */
	char *http_front_controller;		/* nginx-style try_files: when the resolved SCRIPT_FILENAME does not exist, substitute this
						 * script and put the original path into PATH_INFO. Default "/index.php" — the built-in PHP
						 * server (php -S) gives the same effect with no configuration (see
						 * php_cli_server_request_translate_vpath()), so the HTTP gateway should not be less
						 * polite. Empty = disabled, today's behavior. */
	char *http_tls_cert;			/* path to a PEM certificate (with chain); empty = plain HTTP, as today */
	char *http_tls_key;			/* path to a PEM private key */
	char *http_tls_min_version;		/* "TLSv1.2" (default) or "TLSv1.3" */
	/* Additional certificates selected by SNI (task 041), on top of the
	 * default http.tls_cert/http.tls_key pair above. Comma-separated list of
	 * "servername:cert_path:key_path" entries, e.g.
	 * "example.org:/certs/example.org/fullchain.pem:/certs/example.org/privkey.pem,
	 *  example.net:/certs/example.net/fullchain.pem:/certs/example.net/privkey.pem".
	 * Whitespace around commas/colons is trimmed. Empty/unset = no extra SNI
	 * certificates, exactly today's single-certificate behaviour. A
	 * connection with no SNI, or an unrecognized servername, falls back to
	 * the default http.tls_cert/http.tls_key pair -- see fpm_tls_http.c. */
	char *http_tls_sni_cert;
	/* mTLS (issue #62): "none" (default), "optional" or "require". "none"
	 * leaves the listener exactly as it is today -- no CertificateRequest is
	 * sent, and fpm_connection_info() reports no client certificate fields.
	 * "optional" requests a client certificate but completes the handshake
	 * without one; "require" fails the handshake when none is presented.
	 * Either non-"none" value needs http.tls_client_ca, since there would
	 * otherwise be nothing to verify a presented certificate against. */
	char *http_tls_verify_client;
	char *http_tls_client_ca;		/* PEM file of trusted CA certificates for http.tls_verify_client */
	int http_tls_reload_check;		/* seconds between cert/key mtime checks on disk, without restarting the gateway (task 040);
						 * unset -> FPM_TLS_RELOAD_CHECK_DEFAULT (fpm_tls_reload.h), 0 = disabled */
	/* fpm-ng: start this pool before its certificate exists (issue #172).
	 * Off by default, and deliberately so: without it a missing or
	 * unparseable http.tls_cert is a startup failure of the whole master,
	 * which is the fail-closed behaviour a pool that is supposed to serve
	 * TLS must keep. With it, a cert path that does not exist YET puts the
	 * pool in NO_CERT -- the TLS listener is bound but not listening, so
	 * :443 refuses connections, and http.plain_listen answers HTTP-01
	 * challenges only -- until the certificate appears on disk, at which
	 * point every gateway picks it up through the http.tls_reload_check
	 * machinery and starts accepting. A cert path that exists but does not
	 * parse still fails startup: that is an operator error, not an
	 * unfinished issuance. See docs/tls.md. */
	int http_tls_wait_for_cert;
	/* fpm-ng: pool.type = http-direct only (the classic blocking executor).
	 * Off by default: the response is buffered whole and sent with a
	 * Content-Length, which is what every existing test measures. With it, the
	 * worker hands the body to the client as the script produces it, using
	 * chunked transfer encoding -- see fpm_http_direct.c and docs/http-direct.md
	 * (issue #56). */
	int http_stream;
	/* fpm-ng: milliseconds the worker will spend blocked on a client that is
	 * not taking a streamed response, summed over the whole response, before it
	 * gives up and truncates the message. A per-wait budget would not bound
	 * anything: a client reading one byte just before each deadline would renew
	 * it forever. Only meaningful with http.stream = yes; must be > 0, because
	 * a worker blocked forever on one slow client serves nobody else. */
	int http_stream_write_timeout;
	/* fpm-ng: pool.executor = fiber, see fpm_pool_coop_reval.c */
	int fiber_revalidate_freq;		/* seconds between mtime checks of loaded files; 0 = disabled (default) */
	/* fpm-ng: pool.executor = fiber, per-request isolation of listed class
	 * static properties; see fpm_pool_coop_statics.c. Comma-separated list of
	 * Class\Name::property (declaring class, PHP property syntax without the
	 * '$'); empty = mechanism off, see FPM_COOP_STATICS_MAX. */
	char *fiber_isolate_statics;
	/* fpm-ng: pool.executor = worker, see fpm_http_direct_worker.c. Ceiling on
	 * requests accepted but not yet answered (fw.pending / fw.ready[]); past it
	 * the worker answers 503 and asks to stop so the master respawns it
	 * (issue #184's contract, made configurable by issue #331). Validated > 0
	 * in fpm_http_direct_worker_validate(); FPM_WORKER_PENDING_MAX is the
	 * compile-time default used when the directive is unset. */
	int worker_max_pending;
	/* fpm-ng: pool.executor = worker, milliseconds a request may sit accepted
	 * but unanswered before the worker gives up on it, answers 504 and frees
	 * its slot; 0 = off (no bound, today's behavior). request_terminate_timeout
	 * cannot do this job here -- see fpm_http_direct_worker_rejects -- because
	 * the scoreboard never leaves the ACCEPTING stage for this executor
	 * (issue #331). */
	int worker_request_timeout;
	struct key_value_s *env;
	struct key_value_s *php_admin_values;
	struct key_value_s *php_values;
#ifdef HAVE_APPARMOR
	char *apparmor_hat;
#endif
#ifdef HAVE_FPM_ACL
	/* Using Posix ACL */
	char *listen_acl_users;
	char *listen_acl_groups;
#endif
#ifdef SO_SETFIB
	int listen_setfib;
#endif
};

struct ini_value_parser_s {
	char *name;
	char *(*parser)(zval *, void **, intptr_t);
	intptr_t offset;
};

enum {
	PM_STYLE_STATIC = 1,
	PM_STYLE_DYNAMIC = 2,
	PM_STYLE_ONDEMAND = 3
};

/* http.pool_full_policy, see fpm_http.c. Default is FPM_HTTP_POOL_FULL_REJECT
 * (0) so a zeroed config struct -- the state before validate() runs a
 * directive at all -- is the safe, existing behavior, not a silently-enabled
 * queue. */
enum {
	FPM_HTTP_POOL_FULL_REJECT = 0,
	FPM_HTTP_POOL_FULL_WAIT = 1
};

/* cron.jitter_mode, see fpm_pool_cron.c (issue #322). Default is
 * FPM_CRON_JITTER_RANDOM (0) -- naming follows systemd's
 * RandomizedDelaySec=/FixedRandomDelay=, where "random" (a fresh delay every
 * run) is the default and "stable" (one fixed per-pool delay) is the opt-in. */
enum {
	FPM_CRON_JITTER_RANDOM = 0,
	FPM_CRON_JITTER_STABLE = 1
};

/* fpm-ng: allocate a pool that fpm-ng creates for itself rather than one the
 * configuration asked for -- today only the operator endpoint's listener
 * (fpm_operator_endpoint.h). It is appended to fpm_worker_all_pools like any
 * other, so it is validated, gets a socket and a supervised child, and takes
 * part in reload without a second code path.
 *
 * `like` is the pool that needed it: the identity it runs under (user, group,
 * and the socket's ownership and mode) is copied from there, because a listener
 * created on a pool's behalf must not run with more privilege than the pool
 * that asked for it. Nothing else is copied -- in particular no php_value, no
 * chroot and no request settings, which would be meaningless on a process that
 * never starts PHP.
 *
 * Returns the new pool, or NULL. Must be called during configuration, before
 * fpm_conf_process_all_pools() has walked past the end of the list. */
struct fpm_worker_pool_s *fpm_conf_internal_pool_alloc(const char *name, const char *type,
	const char *listen_address, struct fpm_worker_pool_s *like);

int fpm_conf_init_main(int test_conf, int force_daemon);
int fpm_worker_pool_config_free(struct fpm_worker_pool_config_s *wpc);

/* fpm-ng: tracking pool directives that were actually set (see set_directives) */
int fpm_conf_note_directive(struct fpm_worker_pool_config_s *wpc, const char *name);
bool fpm_conf_directive_was_set(struct fpm_worker_pool_config_s *wpc, const char *name);
int fpm_conf_write_pid(void);
int fpm_conf_unlink_pid(void);

#endif

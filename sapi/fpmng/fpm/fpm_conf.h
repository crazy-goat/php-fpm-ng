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
};

extern struct fpm_global_config_s fpm_global_config;

/*
 * Please keep the same order as in fpm_conf.c and in php-fpm.conf.in
 */
struct fpm_worker_pool_config_s {
	char *name;
	char *type;			/* fpm-ng: pool.type, empty = fastcgi (see fpm_pool_type.h) */
	char *executor;			/* fpm-ng: pool.executor, empty = classic */
	char *set_directives;		/* fpm-ng: ";nazwa;nazwa;" faktycznie ustawionych dyrektyw,
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
	/* fpm-ng: pool.type = cron, see fpm_pool_cron.c */
	char *cron_schedule;
	char *cron_script;
	int cron_timeout;			/* seconds, 0 = no limit (default) */
	struct fpm_cron_schedule_s *cron_parsed_schedule;	/* filled in by validate() */
	char *cron_timezone;			/* IANA name, e.g. "Europe/Warsaw"; empty/NULL = UTC (default), see fpm_pool_cron.c */
	char *cron_log;			/* optional: one line per run (start, exit code, duration) appended here; see fpm_pool_cron.c */
	/* fpm-ng: pool.type = http, see fpm_http.c. The gateway starts ONLY when
	 * pool.type = http (see fpm_pool_type.c) — these directives merely tune it,
	 * they never enable it by themselves on another pool type. */
	char *http_listen;			/* empty = FastCGI port + 1 (or required when the pool listens on a UDS) */
	char *http_plain_listen;		/* optional redirect-only plain HTTP companion for a TLS listener */
	int http_gateways;			/* number of gateway processes, default 2 */
	/* Additional certificates selected by SNI (task 041), on top of the
	 * default http.tls_cert/http.tls_key pair above. Comma-separated list of
	 * "servername:cert_path:key_path" entries, e.g.
	 * "example.org:/certs/example.org/fullchain.pem:/certs/example.org/privkey.pem,
	 *  example.net:/certs/example.net/fullchain.pem:/certs/example.net/privkey.pem".
	 * Whitespace around commas/colons is trimmed. Empty/unset = no extra SNI
	 * certificates, exactly today's single-certificate behaviour. A
	 * connection with no SNI, or an unrecognized servername, falls back to
	 * the default http.tls_cert/http.tls_key pair -- see fpm_http_tls.c. */
	char *http_tls_sni_cert;
	int http_tls_reload_check;		/* seconds between cert/key mtime checks on disk, without restarting the gateway (task 040);
						 * unset -> FPM_HTTP_TLS_RELOAD_CHECK_DEFAULT (fpm_http_tls_reload.h), 0 = disabled */
	/* fpm-ng: pool.executor = fiber, see fpm_pool_coop_reval.c */
	int fiber_revalidate_freq;		/* seconds between mtime checks of loaded files; 0 = disabled (default) */
	/* fpm-ng: pool.executor = fiber, per-request isolation of listed class
	 * static properties; see fpm_pool_coop_statics.c. Comma-separated list of
	 * Class\Name::property (declaring class, PHP property syntax without the
	 * '$'); empty = mechanism off, see FPM_COOP_STATICS_MAX. */
	char *fiber_isolate_statics;
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

int fpm_conf_init_main(int test_conf, int force_daemon);
int fpm_worker_pool_config_free(struct fpm_worker_pool_config_s *wpc);

/* fpm-ng: tracking pool directives that were actually set (see set_directives) */
int fpm_conf_note_directive(struct fpm_worker_pool_config_s *wpc, const char *name);
bool fpm_conf_directive_was_set(struct fpm_worker_pool_config_s *wpc, const char *name);
int fpm_conf_write_pid(void);
int fpm_conf_unlink_pid(void);

#endif

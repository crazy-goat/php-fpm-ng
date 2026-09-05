	/* (c) 2007,2008 Andrei Nigmatulin */

#ifndef FPM_CONF_H
#define FPM_CONF_H 1

#include <stdint.h>
#include "php.h"

#define PM2STR(a) (a == PM_STYLE_STATIC ? "static" : (a == PM_STYLE_DYNAMIC ? "dynamic" : "ondemand"))

#define FPM_CONF_MAX_PONG_LENGTH 64

struct key_value_s;
struct fpm_cron_schedule_s;	/* fpm-ng: definicja w fpm_cron_schedule.h, tu tylko wskaznik */

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
	char *type;			/* fpm-ng: pool.type, pusty = fastcgi (patrz fpm_pool_type.h) */
	char *executor;			/* fpm-ng: pool.executor, pusty = classic */
	char *set_directives;		/* fpm-ng: ";nazwa;nazwa;" faktycznie ustawionych dyrektyw,
					 * zeby typ poola mogl odrzucic te, ktore go nie dotycza —
					 * z samej wartosci nie da sie odroznic "nieustawione"
					 * od "ustawione na domyslna" */
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
	int request_cpu_tracking;		/* fpm-ng: times() na start i koniec requestu; zasila "last request cpu" w statusie i %C w access.format */
	int rlimit_files;
	int rlimit_core;
	char *chroot;
	char *chdir;
	int catch_workers_output;
	int decorate_workers_output;
	int clear_env;
	char *security_limit_extensions;
	/* fpm-ng: pool.type = supervisor, patrz fpm_pool_supervisor.c */
	char *supervisor_script;
	int supervisor_processes;
	char *supervisor_restart;		/* "always" (domyslne) | "on-failure" | "never" */
	int supervisor_restart_delay;
	int supervisor_restart_delay_max;
	int supervisor_restart_max;		/* 0 = bez limitu, nigdy nie poddawaj sie */
	int supervisor_stop_timeout;
	int supervisor_fatal;			/* wyczerpanie restart_max ubija cala mastera */
	/* fpm-ng: pool.type = cron, patrz fpm_pool_cron.c */
	char *cron_schedule;
	char *cron_script;
	int cron_timeout;			/* sekundy, 0 = bez limitu (domyslne) */
	struct fpm_cron_schedule_s *cron_parsed_schedule;	/* wypelnia validate() */
	/* fpm-ng: pool.type = http, patrz fpm_http.c. Bramka startuje TYLKO gdy
	 * pool.type = http (patrz fpm_pool_type.c) — te dyrektywy ja jedynie
	 * dostrajaja, nigdy nie wlaczaja same z siebie na innym typie poola. */
	char *http_listen;			/* pusty = FastCGI port + 1 (albo wymagane, gdy pool sluchał na UDS) */
	int http_gateways;			/* liczba procesow bramki, domyslnie 2 */
	int http_reuseport;			/* kazda bramka wlasny SO_REUSEPORT socket */
	int http_static;			/* serwowanie plikow statycznych bez PHP, domyslnie wlaczone */
	int http_idle_timeout;			/* ms, zwalnia przypiete polaczenie po tylu ms bezczynnosci; 0 = nigdy */
	char *http_allowed_clients;		/* jak listen.allowed_clients, ale dla bramki HTTP; puste = brak ograniczenia */
	char *http_trusted_proxies;		/* adresy, z ktorych ufamy naglowkom X-Forwarded-*; puste = nikomu (bezpieczny domyslny), patrz fpm_http_forwarded.c */
	char *http_access_log;			/* sciezka do logu dostepu bramki HTTP; puste = wylaczony, patrz fpm_http_access_log.c */
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

/* fpm-ng: sledzenie faktycznie ustawionych dyrektyw poola (patrz set_directives) */
int fpm_conf_note_directive(struct fpm_worker_pool_config_s *wpc, const char *name);
bool fpm_conf_directive_was_set(struct fpm_worker_pool_config_s *wpc, const char *name);
int fpm_conf_write_pid(void);
int fpm_conf_unlink_pid(void);

#endif

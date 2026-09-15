	/* (c) 2007,2008 Andrei Nigmatulin */

#include "fpm_config.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>
#include <stdlib.h>
#include <stddef.h>
#include <inttypes.h>

#include <stdio.h>
#include <unistd.h>

#include "php.h"
#include "zend_ini_scanner.h"
#include "zend_globals.h"
#include "zend_stream.h"
#include "php_syslog.h"
#include "php_glob.h"

#include "fpm.h"
#include "fpm_conf.h"
#include "fpm_stdio.h"
#include "fpm_worker_pool.h"
#include "fpm_pool_type.h"
#include "fpm_operator_endpoint.h"
#include "fpm_cleanup.h"
#include "fpm_php.h"
#include "fpm_sockets.h"
#include "fpm_shm.h"
#include "fpm_status.h"
#include "fpm_log.h"
#include "fpm_events.h"
#include "fpm_unix.h"
#include "zlog.h"
#ifdef HAVE_SYSTEMD
#include "fpm_systemd.h"
#endif


#define STR2STR(a) ((a) ? (a) : "undefined")
#define BOOL2STR(a) ((a) ? "yes" : "no")
#define GO(field) offsetof(struct fpm_global_config_s, field)
#define WPO(field) offsetof(struct fpm_worker_pool_config_s, field)

static int fpm_conf_load_ini_file(char *filename);
static char *fpm_conf_set_integer(zval *value, void **config, intptr_t offset);
#if 0 /* not used for now */
static char *fpm_conf_set_long(zval *value, void **config, intptr_t offset);
#endif
static char *fpm_conf_set_time(zval *value, void **config, intptr_t offset);
static char *fpm_conf_set_bytes(zval *value, void **config, intptr_t offset);
static char *fpm_conf_set_boolean(zval *value, void **config, intptr_t offset);
static char *fpm_conf_set_string(zval *value, void **config, intptr_t offset);
static char *fpm_conf_set_log_level(zval *value, void **config, intptr_t offset);
static char *fpm_conf_set_rlimit_core(zval *value, void **config, intptr_t offset);
static char *fpm_conf_set_pm(zval *value, void **config, intptr_t offset);
static char *fpm_conf_set_pool_full_policy(zval *value, void **config, intptr_t offset);
static char *fpm_conf_set_cron_jitter_mode(zval *value, void **config, intptr_t offset);
#ifdef HAVE_SYSLOG_H
static char *fpm_conf_set_syslog_facility(zval *value, void **config, intptr_t offset);
#endif

struct fpm_global_config_s fpm_global_config = {
	.daemonize = 1,
#ifdef HAVE_SYSLOG_H
	.syslog_facility = -1,
#endif
	.process_max = 0,
	.process_priority = 64, /* 64 means unset */
#ifdef HAVE_SYSTEMD
	.systemd_watchdog = 0,
	.systemd_interval = -1, /* -1 means not set */
#endif
	.log_buffering = ZLOG_DEFAULT_BUFFERING,
	.log_limit = ZLOG_DEFAULT_LIMIT
};
static struct fpm_worker_pool_s *current_wp = NULL;
static int ini_recursion = 0;
static char *ini_filename = NULL;
static int ini_lineno = 0;
static char *ini_include = NULL;

/*
 * Please keep the same order as in fpm_conf.h and in php-fpm.conf.in
 */
static const struct ini_value_parser_s ini_fpm_global_options[] = {
	{ "pid",                         &fpm_conf_set_string,          GO(pid_file) },
	{ "error_log",                   &fpm_conf_set_string,          GO(error_log) },
#ifdef HAVE_SYSLOG_H
	{ "syslog.ident",                &fpm_conf_set_string,          GO(syslog_ident) },
	{ "syslog.facility",             &fpm_conf_set_syslog_facility, GO(syslog_facility) },
#endif
	{ "log_buffering",               &fpm_conf_set_boolean,         GO(log_buffering) },
	{ "log_level",                   &fpm_conf_set_log_level,       GO(log_level) },
	{ "log_limit",                   &fpm_conf_set_integer,         GO(log_limit) },
	{ "emergency_restart_threshold", &fpm_conf_set_integer,         GO(emergency_restart_threshold) },
	{ "emergency_restart_interval",  &fpm_conf_set_time,            GO(emergency_restart_interval) },
	{ "process_control_timeout",     &fpm_conf_set_time,            GO(process_control_timeout) },
	{ "process.max",                 &fpm_conf_set_integer,         GO(process_max) },
	{ "process.priority",            &fpm_conf_set_integer,         GO(process_priority) },
	{ "daemonize",                   &fpm_conf_set_boolean,         GO(daemonize) },
	{ "rlimit_files",                &fpm_conf_set_integer,         GO(rlimit_files) },
	{ "rlimit_core",                 &fpm_conf_set_rlimit_core,     GO(rlimit_core) },
	{ "events.mechanism",            &fpm_conf_set_string,          GO(events_mechanism) },
#ifdef HAVE_SYSTEMD
	{ "systemd_interval",            &fpm_conf_set_time,            GO(systemd_interval) },
#endif
	{ 0, 0, 0 }
};

/*
 * Please keep the same order as in fpm_conf.h and in php-fpm.conf.in
 */
static const struct ini_value_parser_s ini_fpm_pool_options[] = {
	{ "pool.type",                 &fpm_conf_set_string,      WPO(type) },
	{ "pool.executor",             &fpm_conf_set_string,      WPO(executor) },
	{ "prefix",                    &fpm_conf_set_string,      WPO(prefix) },
	{ "user",                      &fpm_conf_set_string,      WPO(user) },
	{ "group",                     &fpm_conf_set_string,      WPO(group) },
	{ "listen",                    &fpm_conf_set_string,      WPO(listen_address) },
	{ "listen.backlog",            &fpm_conf_set_integer,     WPO(listen_backlog) },
#ifdef HAVE_FPM_ACL
	{ "listen.acl_users",          &fpm_conf_set_string,      WPO(listen_acl_users) },
	{ "listen.acl_groups",         &fpm_conf_set_string,      WPO(listen_acl_groups) },
#endif
	{ "listen.owner",              &fpm_conf_set_string,      WPO(listen_owner) },
	{ "listen.group",              &fpm_conf_set_string,      WPO(listen_group) },
	{ "listen.mode",               &fpm_conf_set_string,      WPO(listen_mode) },
	{ "listen.allowed_clients",    &fpm_conf_set_string,      WPO(listen_allowed_clients) },
#ifdef SO_SETFIB
	{ "listen.setfib",             &fpm_conf_set_integer,     WPO(listen_setfib) },
#endif
	{ "process.priority",          &fpm_conf_set_integer,     WPO(process_priority) },
	{ "process.dumpable",          &fpm_conf_set_boolean,     WPO(process_dumpable) },
	{ "pm",                        &fpm_conf_set_pm,          WPO(pm) },
	{ "pm.max_children",           &fpm_conf_set_integer,     WPO(pm_max_children) },
	{ "pm.start_servers",          &fpm_conf_set_integer,     WPO(pm_start_servers) },
	{ "pm.min_spare_servers",      &fpm_conf_set_integer,     WPO(pm_min_spare_servers) },
	{ "pm.max_spare_servers",      &fpm_conf_set_integer,     WPO(pm_max_spare_servers) },
	{ "pm.max_spawn_rate",         &fpm_conf_set_integer,     WPO(pm_max_spawn_rate) },
	{ "pm.process_idle_timeout",   &fpm_conf_set_time,        WPO(pm_process_idle_timeout) },
	{ "pm.max_requests",           &fpm_conf_set_integer,     WPO(pm_max_requests) },
	{ "pm.status_path",            &fpm_conf_set_string,      WPO(pm_status_path) },
	{ "pm.status_listen",          &fpm_conf_set_string,      WPO(pm_status_listen) },
	{ "pm.metrics_path",           &fpm_conf_set_string,      WPO(pm_metrics_path) },
	{ "pm.metrics_listen",         &fpm_conf_set_string,      WPO(pm_metrics_listen) },
	{ "ping.path",                 &fpm_conf_set_string,      WPO(ping_path) },
	{ "ping.response",             &fpm_conf_set_string,      WPO(ping_response) },
	{ "access.log",                &fpm_conf_set_string,      WPO(access_log) },
	{ "access.format",             &fpm_conf_set_string,      WPO(access_format) },
	{ "slowlog",                   &fpm_conf_set_string,      WPO(slowlog) },
	{ "request_slowlog_timeout",   &fpm_conf_set_time,        WPO(request_slowlog_timeout) },
	{ "request_slowlog_trace_depth", &fpm_conf_set_integer,     WPO(request_slowlog_trace_depth) },
	{ "request_terminate_timeout", &fpm_conf_set_time,        WPO(request_terminate_timeout) },
	{ "request_terminate_timeout_track_finished", &fpm_conf_set_boolean, WPO(request_terminate_timeout_track_finished) },
	{ "request_cpu_tracking",      &fpm_conf_set_boolean,     WPO(request_cpu_tracking) },
	{ "rlimit_files",              &fpm_conf_set_integer,     WPO(rlimit_files) },
	{ "rlimit_core",               &fpm_conf_set_rlimit_core, WPO(rlimit_core) },
	{ "chroot",                    &fpm_conf_set_string,      WPO(chroot) },
	{ "chdir",                     &fpm_conf_set_string,      WPO(chdir) },
	{ "catch_workers_output",      &fpm_conf_set_boolean,     WPO(catch_workers_output) },
	{ "decorate_workers_output",   &fpm_conf_set_boolean,     WPO(decorate_workers_output) },
	{ "clear_env",                 &fpm_conf_set_boolean,     WPO(clear_env) },
	{ "security.limit_extensions", &fpm_conf_set_string,      WPO(security_limit_extensions) },
	{ "supervisor.script",         &fpm_conf_set_string,      WPO(supervisor_script) },
	{ "supervisor.processes",      &fpm_conf_set_integer,     WPO(supervisor_processes) },
	{ "supervisor.restart",        &fpm_conf_set_string,      WPO(supervisor_restart) },
	{ "supervisor.restart_delay",  &fpm_conf_set_time,        WPO(supervisor_restart_delay) },
	{ "supervisor.restart_delay_max", &fpm_conf_set_time,     WPO(supervisor_restart_delay_max) },
	{ "supervisor.restart_max",    &fpm_conf_set_integer,     WPO(supervisor_restart_max) },
	{ "supervisor.stop_timeout",   &fpm_conf_set_time,        WPO(supervisor_stop_timeout) },
	{ "supervisor.fatal",          &fpm_conf_set_boolean,     WPO(supervisor_fatal) },
	{ "cron.schedule",             &fpm_conf_set_string,      WPO(cron_schedule) },
	{ "cron.script",               &fpm_conf_set_string,      WPO(cron_script) },
	{ "cron.timeout",              &fpm_conf_set_time,        WPO(cron_timeout) },
	{ "cron.timezone",             &fpm_conf_set_string,      WPO(cron_timezone) },
	{ "cron.log",                  &fpm_conf_set_string,      WPO(cron_log) },
	{ "cron.jitter",               &fpm_conf_set_time,        WPO(cron_jitter) },
	{ "cron.jitter_mode",          &fpm_conf_set_cron_jitter_mode, WPO(cron_jitter_mode) },
	{ "http.listen",               &fpm_conf_set_string,      WPO(http_listen) },
	{ "http.plain_listen",         &fpm_conf_set_string,      WPO(http_plain_listen) },
	{ "http.gateways",             &fpm_conf_set_integer,     WPO(http_gateways) },
	{ "http.reuseport",            &fpm_conf_set_boolean,     WPO(http_reuseport) },
	{ "http.static",               &fpm_conf_set_boolean,     WPO(http_static) },
	{ "http.fault_upstream_write", &fpm_conf_set_integer,     WPO(http_fault_upstream_write) },
	{ "http.idle_timeout",         &fpm_conf_set_integer,     WPO(http_idle_timeout) },
	{ "http.read_timeout",         &fpm_conf_set_integer,     WPO(http_read_timeout) },
	{ "http.pool_full_policy",     &fpm_conf_set_pool_full_policy, WPO(http_pool_full_policy) },
	{ "http.pool_full_queue_max",  &fpm_conf_set_integer,     WPO(http_pool_full_queue_max) },
	{ "http.pool_full_wait_ms",    &fpm_conf_set_integer,     WPO(http_pool_full_wait_ms) },
	{ "http.max_body",             &fpm_conf_set_bytes,       WPO(http_max_body) },
	{ "http.max_connections",      &fpm_conf_set_integer,     WPO(http_max_connections) },
	{ "http.max_connections_per_client", &fpm_conf_set_integer, WPO(http_max_connections_per_client) },
	{ "http.allowed_clients",      &fpm_conf_set_string,      WPO(http_allowed_clients) },
	{ "http.trusted_proxies",      &fpm_conf_set_string,      WPO(http_trusted_proxies) },
	{ "http.access_log",           &fpm_conf_set_string,      WPO(http_access_log) },
	{ "http.front_controller",     &fpm_conf_set_string,      WPO(http_front_controller) },
	{ "http.tls_cert",             &fpm_conf_set_string,      WPO(http_tls_cert) },
	{ "http.tls_key",              &fpm_conf_set_string,      WPO(http_tls_key) },
	{ "http.tls_min_version",      &fpm_conf_set_string,      WPO(http_tls_min_version) },
	{ "http.tls_sni_cert",         &fpm_conf_set_string,      WPO(http_tls_sni_cert) },
	{ "http.tls_verify_client",    &fpm_conf_set_string,      WPO(http_tls_verify_client) },
	{ "http.tls_client_ca",        &fpm_conf_set_string,      WPO(http_tls_client_ca) },
	{ "http.tls_reload_check",     &fpm_conf_set_time,        WPO(http_tls_reload_check) },
	{ "http.tls_wait_for_cert",    &fpm_conf_set_boolean,     WPO(http_tls_wait_for_cert) },
	{ "http.stream",               &fpm_conf_set_boolean,     WPO(http_stream) },
	{ "http.stream_write_timeout", &fpm_conf_set_integer,     WPO(http_stream_write_timeout) },
	{ "fiber.revalidate_freq",     &fpm_conf_set_time,        WPO(fiber_revalidate_freq) },
	{ "fiber.isolate_statics",     &fpm_conf_set_string,      WPO(fiber_isolate_statics) },
#ifdef HAVE_APPARMOR
	{ "apparmor_hat",              &fpm_conf_set_string,      WPO(apparmor_hat) },
#endif
	{ 0, 0, 0 }
};

static int fpm_conf_is_dir(char *path) /* {{{ */
{
	struct stat sb;

	if (stat(path, &sb) != 0) {
		return 0;
	}

	return (sb.st_mode & S_IFMT) == S_IFDIR;
}
/* }}} */

/*
 * Expands the '$pool' token in a dynamically allocated string
 */
static int fpm_conf_expand_pool_name(char **value) {
	char *token;

	if (!value || !*value) {
		return 0;
	}

	while (*value && (token = strstr(*value, "$pool"))) {
		char *buf;
		char *p2 = token + strlen("$pool");

		/* If we are not in a pool, we cannot expand this name now */
		if (!current_wp || !current_wp->config  || !current_wp->config->name) {
			return -1;
		}

		/* "aaa$poolbbb" becomes "aaa\0oolbbb" */
		token[0] = '\0';

		/* Build a brand new string with the expanded token */
		spprintf(&buf, 0, "%s%s%s", *value, current_wp->config->name, p2);

		/* Free the previous value and save the new one */
		free(*value);
		*value = strdup(buf);
		efree(buf);
	}

	return 0;
}

static char *fpm_conf_set_boolean(zval *value, void **config, intptr_t offset) /* {{{ */
{
	zend_string *val = Z_STR_P(value);
	/* we need to check all allowed values to correctly set value from the environment variable */
	bool value_y = (
		zend_string_equals_literal(val, "1") ||
		zend_string_equals_literal(val, "yes") ||
		zend_string_equals_literal(val, "true") ||
		zend_string_equals_literal(val, "on")
	);
	bool value_n = (
		value_y || ZSTR_LEN(val) == 0 ||
		zend_string_equals_literal(val, "0") ||
		zend_string_equals_literal(val, "no") ||
		zend_string_equals_literal(val, "none") ||
		zend_string_equals_literal(val, "false") ||
		zend_string_equals_literal(val, "off")
	);


	if (!value_y && !value_n) {
		return "invalid boolean value";
	}

	* (int *) ((char *) *config + offset) = value_y ? 1 : 0;
	return NULL;
}
/* }}} */

static char *fpm_conf_set_string(zval *value, void **config, intptr_t offset) /* {{{ */
{
	char **config_val = (char **) ((char *) *config + offset);

	if (!config_val) {
		return "internal error: NULL value";
	}

	/* Check if there is a previous value to deallocate */
	if (*config_val) {
		free(*config_val);
	}

	*config_val = strdup(Z_STRVAL_P(value));
	if (!*config_val) {
		return "fpm_conf_set_string(): strdup() failed";
	}
	if (fpm_conf_expand_pool_name(config_val) == -1) {
		return "Can't use '$pool' when the pool is not defined";
	}

	return NULL;
}
/* }}} */

static char *fpm_conf_set_integer(zval *value, void **config, intptr_t offset) /* {{{ */
{
	char *val = Z_STRVAL_P(value);
	char *p;

	/* we don't use strtol because we don't want to allow negative values */
	for (p = val; *p; p++) {
		if (p == val && *p == '-') continue;
		if (*p < '0' || *p > '9') {
			return "is not a valid number (greater or equal than zero)";
		}
	}
	* (int *) ((char *) *config + offset) = atoi(val);
	return NULL;
}
/* }}} */

#if 0 /* not used for now */
static char *fpm_conf_set_long(zval *value, void **config, intptr_t offset) /* {{{ */
{
	char *val = Z_STRVAL_P(value);
	char *p;

	for (p = val; *p; p++) {
		if ( p == val && *p == '-' ) continue;
		if (*p < '0' || *p > '9') {
			return "is not a valid number (greater or equal than zero)";
		}
	}
	* (long int *) ((char *) *config + offset) = atol(val);
	return NULL;
}
/* }}} */
#endif

static char *fpm_conf_set_time(zval *value, void **config, intptr_t offset) /* {{{ */
{
	char *val = Z_STRVAL_P(value);
	int len = strlen(val);
	char suffix;
	int seconds;
	if (!len) {
		return "invalid time value";
	}

	suffix = val[len-1];
	switch (suffix) {
		case 'm' :
			val[len-1] = '\0';
			seconds = 60 * atoi(val);
			break;
		case 'h' :
			val[len-1] = '\0';
			seconds = 60 * 60 * atoi(val);
			break;
		case 'd' :
			val[len-1] = '\0';
			seconds = 24 * 60 * 60 * atoi(val);
			break;
		case 's' : /* s is the default suffix */
			val[len-1] = '\0';
			suffix = '0';
			ZEND_FALLTHROUGH;
		default :
			if (suffix < '0' || suffix > '9') {
				return "unknown suffix used in time value";
			}
			seconds = atoi(val);
			break;
	}

	* (int *) ((char *) *config + offset) = seconds;
	return NULL;
}
/* }}} */

/* Byte-size value with an optional K/M/G suffix (base 1024), e.g. "32m" or
 * "1048576". Lives next to fpm_conf_set_time() on purpose: same shape, but a
 * size_t target so the full 32 MiB default (and beyond) fits. */
static char *fpm_conf_set_bytes(zval *value, void **config, intptr_t offset) /* {{{ */
{
	char *val = Z_STRVAL_P(value);
	size_t multiplier = 1, bytes;
	int len = strlen(val);
	char suffix;
	const char *digits = val;
	char *end;

	if (!len) {
		return "invalid byte size value";
	}

	suffix = val[len-1];
	switch (suffix) {
		case 'k' : case 'K' :
			multiplier = 1024;
			break;
		case 'm' : case 'M' :
			multiplier = 1024 * 1024;
			break;
		case 'g' : case 'G' :
			multiplier = 1024 * 1024 * 1024;
			break;
		default :
			if (suffix < '0' || suffix > '9') {
				return "unknown suffix used in byte size value";
			}
			multiplier = 1;
			break;
	}

	bytes = strtoull(digits, &end, 10);
	if (end == digits || (multiplier > 1 && end != val + len - 1) || (multiplier == 1 && *end != '\0')) {
		return "is not a valid byte size (number with an optional K/M/G suffix)";
	}
	if (multiplier > 1 && bytes > (size_t) -1 / multiplier) {
		return "byte size value overflows";
	}

	* (size_t *) ((char *) *config + offset) = bytes * multiplier;
	return NULL;
}
/* }}} */

static char *fpm_conf_set_log_level(zval *value, void **config, intptr_t offset) /* {{{ */
{
	zend_string *val = Z_STR_P(value);
	int log_level;

	if (zend_string_equals_literal_ci(val, "debug")) {
		log_level = ZLOG_DEBUG;
	} else if (zend_string_equals_literal_ci(val, "notice")) {
		log_level = ZLOG_NOTICE;
	} else if (zend_string_equals_literal_ci(val, "warning") || zend_string_equals_literal_ci(val, "warn")) {
		log_level = ZLOG_WARNING;
	} else if (zend_string_equals_literal_ci(val, "error")) {
		log_level = ZLOG_ERROR;
	} else if (zend_string_equals_literal_ci(val, "alert")) {
		log_level = ZLOG_ALERT;
	} else {
		return "invalid value for 'log_level'";
	}

	* (int *) ((char *) *config + offset) = log_level;
	return NULL;
}
/* }}} */

#ifdef HAVE_SYSLOG_H
static char *fpm_conf_set_syslog_facility(zval *value, void **config, intptr_t offset) /* {{{ */
{
	zend_string *val = Z_STR_P(value);
	int *conf = (int *) ((char *) *config + offset);

#ifdef LOG_AUTH
	if (zend_string_equals_literal_ci(val, "AUTH")) {
		*conf = LOG_AUTH;
		return NULL;
	}
#endif

#ifdef LOG_AUTHPRIV
	if (zend_string_equals_literal_ci(val, "AUTHPRIV")) {
		*conf = LOG_AUTHPRIV;
		return NULL;
	}
#endif

#ifdef LOG_CRON
	if (zend_string_equals_literal_ci(val, "CRON")) {
		*conf = LOG_CRON;
		return NULL;
	}
#endif

#ifdef LOG_DAEMON
	if (zend_string_equals_literal_ci(val, "DAEMON")) {
		*conf = LOG_DAEMON;
		return NULL;
	}
#endif

#ifdef LOG_FTP
	if (zend_string_equals_literal_ci(val, "FTP")) {
		*conf = LOG_FTP;
		return NULL;
	}
#endif

#ifdef LOG_KERN
	if (zend_string_equals_literal_ci(val, "KERN")) {
		*conf = LOG_KERN;
		return NULL;
	}
#endif

#ifdef LOG_LPR
	if (zend_string_equals_literal_ci(val, "LPR")) {
		*conf = LOG_LPR;
		return NULL;
	}
#endif

#ifdef LOG_MAIL
	if (zend_string_equals_literal_ci(val, "MAIL")) {
		*conf = LOG_MAIL;
		return NULL;
	}
#endif

#ifdef LOG_NEWS
	if (zend_string_equals_literal_ci(val, "NEWS")) {
		*conf = LOG_NEWS;
		return NULL;
	}
#endif

#ifdef LOG_SYSLOG
	if (zend_string_equals_literal_ci(val, "SYSLOG")) {
		*conf = LOG_SYSLOG;
		return NULL;
	}
#endif

#ifdef LOG_USER
	if (zend_string_equals_literal_ci(val, "USER")) {
		*conf = LOG_USER;
		return NULL;
	}
#endif

#ifdef LOG_UUCP
	if (zend_string_equals_literal_ci(val, "UUCP")) {
		*conf = LOG_UUCP;
		return NULL;
	}
#endif

#ifdef LOG_LOCAL0
	if (zend_string_equals_literal_ci(val, "LOCAL0")) {
		*conf = LOG_LOCAL0;
		return NULL;
	}
#endif

#ifdef LOG_LOCAL1
	if (zend_string_equals_literal_ci(val, "LOCAL1")) {
		*conf = LOG_LOCAL1;
		return NULL;
	}
#endif

#ifdef LOG_LOCAL2
	if (zend_string_equals_literal_ci(val, "LOCAL2")) {
		*conf = LOG_LOCAL2;
		return NULL;
	}
#endif

#ifdef LOG_LOCAL3
	if (zend_string_equals_literal_ci(val, "LOCAL3")) {
		*conf = LOG_LOCAL3;
		return NULL;
	}
#endif

#ifdef LOG_LOCAL4
	if (zend_string_equals_literal_ci(val, "LOCAL4")) {
		*conf = LOG_LOCAL4;
		return NULL;
	}
#endif

#ifdef LOG_LOCAL5
	if (zend_string_equals_literal_ci(val, "LOCAL5")) {
		*conf = LOG_LOCAL5;
		return NULL;
	}
#endif

#ifdef LOG_LOCAL6
	if (zend_string_equals_literal_ci(val, "LOCAL6")) {
		*conf = LOG_LOCAL6;
		return NULL;
	}
#endif

#ifdef LOG_LOCAL7
	if (zend_string_equals_literal_ci(val, "LOCAL7")) {
		*conf = LOG_LOCAL7;
		return NULL;
	}
#endif

	return "invalid value";
}
/* }}} */
#endif

static char *fpm_conf_set_rlimit_core(zval *value, void **config, intptr_t offset) /* {{{ */
{
	zend_string *val = Z_STR_P(value);
	int *ptr = (int *) ((char *) *config + offset);

	if (zend_string_equals_literal_ci(val, "unlimited")) {
		*ptr = -1;
	} else {
		int int_value;
		void *subconf = &int_value;
		char *error;

		error = fpm_conf_set_integer(value, &subconf, 0);

		if (error) {
			return error;
		}

		if (int_value < 0) {
			return "must be greater than zero or 'unlimited'";
		}

		*ptr = int_value;
	}

	return NULL;
}
/* }}} */

static char *fpm_conf_set_pm(zval *value, void **config, intptr_t offset) /* {{{ */
{
	zend_string *val = Z_STR_P(value);
	struct fpm_worker_pool_config_s  *c = *config;
	if (zend_string_equals_ci(val, ZSTR_KNOWN(ZEND_STR_STATIC))) {
		c->pm = PM_STYLE_STATIC;
	} else if (zend_string_equals_literal_ci(val, "dynamic")) {
		c->pm = PM_STYLE_DYNAMIC;
	} else if (zend_string_equals_literal_ci(val, "ondemand")) {
		c->pm = PM_STYLE_ONDEMAND;
	} else {
		return "invalid process manager (static, dynamic or ondemand)";
	}
	return NULL;
}
/* }}} */

/* http.pool_full_policy: reject (default, the status quo fast-fail 503) or
 * wait (issue #309 -- opt-in per pool, only a good trade for IO-light work;
 * see docs/http-gateway-pool-full.md). Anything else is refused rather than
 * silently taken as "reject", the same way fpm_conf_set_pm() refuses an
 * unrecognized pm value instead of guessing. */
static char *fpm_conf_set_pool_full_policy(zval *value, void **config, intptr_t offset) /* {{{ */
{
	zend_string *val = Z_STR_P(value);
	struct fpm_worker_pool_config_s  *c = *config;

	if (zend_string_equals_literal_ci(val, "reject")) {
		c->http_pool_full_policy = FPM_HTTP_POOL_FULL_REJECT;
	} else if (zend_string_equals_literal_ci(val, "wait")) {
		c->http_pool_full_policy = FPM_HTTP_POOL_FULL_WAIT;
	} else {
		return "invalid http.pool_full_policy (reject or wait)";
	}
	return NULL;
}
/* }}} */

/* cron.jitter_mode (issue #322): random (default) picks a new delay each run;
 * stable derives one fixed per-pool delay from the pool name, so consecutive
 * runs of the SAME pool never show jitter between each other while DIFFERENT
 * pools sharing a cron.schedule still spread apart — see the naming rationale
 * next to FPM_CRON_JITTER_RANDOM in fpm_conf.h and the delay calculation in
 * fpm_pool_cron.c. An unrecognized value is refused rather than silently
 * taken as "random", the same way fpm_conf_set_pool_full_policy() above
 * refuses an unrecognized policy instead of guessing. */
static char *fpm_conf_set_cron_jitter_mode(zval *value, void **config, intptr_t offset) /* {{{ */
{
	zend_string *val = Z_STR_P(value);
	struct fpm_worker_pool_config_s *c = *config;

	if (zend_string_equals_literal_ci(val, "random")) {
		c->cron_jitter_mode = FPM_CRON_JITTER_RANDOM;
	} else if (zend_string_equals_literal_ci(val, "stable")) {
		c->cron_jitter_mode = FPM_CRON_JITTER_STABLE;
	} else {
		return "invalid cron.jitter_mode (random or stable)";
	}
	return NULL;
}
/* }}} */

static char *fpm_conf_set_array(zval *key, zval *value, void **config, int convert_to_bool) /* {{{ */
{
	struct key_value_s *kv;
	struct key_value_s ***parent = (struct key_value_s ***) config;
	int b;
	void *subconf = &b;

	kv = malloc(sizeof(*kv));

	if (!kv) {
		return "malloc() failed";
	}

	memset(kv, 0, sizeof(*kv));
	if (key) {
		kv->key = strdup(Z_STRVAL_P(key));

		if (!kv->key) {
			free(kv);
			return "fpm_conf_set_array: strdup(key) failed";
		}
	}

	if (convert_to_bool) {
		char *err = fpm_conf_set_boolean(value, &subconf, 0);
		if (err) {
			free(kv->key);
			free(kv);
			return err;
		}
		kv->value = strdup(b ? "1" : "0");
	} else {
		kv->value = strdup(Z_STRVAL_P(value));
		if (fpm_conf_expand_pool_name(&kv->value) == -1) {
			free(kv->key);
			free(kv);
			return "Can't use '$pool' when the pool is not defined";
		}
	}

	if (!kv->value) {
		free(kv->key);
		free(kv);
		return "fpm_conf_set_array: strdup(value) failed";
	}

	kv->next = **parent;
	**parent = kv;
	return NULL;
}
/* }}} */

static void *fpm_worker_pool_config_alloc(void)
{
	struct fpm_worker_pool_s *wp;

	wp = fpm_worker_pool_alloc();

	if (!wp) {
		return 0;
	}

	wp->config = malloc(sizeof(struct fpm_worker_pool_config_s));

	if (!wp->config) {
		fpm_worker_pool_free(wp);
		return 0;
	}

	memset(wp->config, 0, sizeof(struct fpm_worker_pool_config_s));
	wp->config->listen_backlog = FPM_BACKLOG_DEFAULT;
	wp->config->pm_max_spawn_rate = 32; /* 32 by default */
	wp->config->pm_process_idle_timeout = 10; /* 10s by default */
	wp->config->process_priority = 64; /* 64 means unset */
	wp->config->process_dumpable = 0;
	wp->config->clear_env = 1;
	wp->config->decorate_workers_output = 1;
	wp->config->request_cpu_tracking = 1;	/* fpm-ng: upstream default */
	wp->config->supervisor_processes = 1;
	wp->config->supervisor_restart_delay = 1;
	wp->config->supervisor_restart_delay_max = 60;
	wp->config->supervisor_stop_timeout = 10;
	wp->config->http_gateways = 2;		/* fpm-ng: FPM_HTTP_GATEWAYS_DEFAULT in fpm_http.c */
	wp->config->http_static = 1;
	wp->config->http_idle_timeout = 500;	/* fpm-ng: FPM_HTTP_IDLE_MS in fpm_http.c */
	wp->config->http_read_timeout = 5000;	/* fpm-ng: FPM_HTTP_READ_TIMEOUT_MS in fpm_http.c */
	wp->config->http_pool_full_policy = FPM_HTTP_POOL_FULL_REJECT;	/* fpm-ng: issue #309, off by default for every pool */
	wp->config->cron_jitter_mode = FPM_CRON_JITTER_RANDOM;	/* fpm-ng: issue #322, matters only once cron.jitter > 0 */
	wp->config->http_pool_full_queue_max = 32;	/* fpm-ng: issue #309, see docs/http-gateway-pool-full.md for the reasoning */
	wp->config->http_pool_full_wait_ms = 500;	/* fpm-ng: issue #309, see docs/http-gateway-pool-full.md for the reasoning */
	wp->config->http_max_body = 32 * 1024 * 1024;	/* fpm-ng: FPM_HTTP_MAX_BODY in fpm_http.c */
	wp->config->http_front_controller = strdup("/index.php");	/* fpm-ng: see the field comment in fpm_conf.h */
	wp->config->http_stream_write_timeout = 10000;	/* fpm-ng: ten seconds, see http.stream in docs/http-direct.md */
#ifdef SO_SETFIB
	wp->config->listen_setfib = -1;
#endif

	if (!fpm_worker_all_pools) {
		fpm_worker_all_pools = wp;
	} else {
		struct fpm_worker_pool_s *tmp = fpm_worker_all_pools;
		while (tmp) {
			if (!tmp->next) {
				tmp->next = wp;
				break;
			}
			tmp = tmp->next;
		}
	}

	current_wp = wp;
	return wp->config;
}

/* fpm-ng: append a directive name to the ";a;b;" list. Delimiters on both
 * sides prevent a prefix false positive ("pm" versus "pm.max_children"). */
int fpm_conf_note_directive(struct fpm_worker_pool_config_s *wpc, const char *name)
{
	size_t have = wpc->set_directives ? strlen(wpc->set_directives) : 0;
	size_t need = have + strlen(name) + 2 + 1;
	char *buf = realloc(wpc->set_directives, need);

	if (!buf) {
		return -1;
	}
	if (!have) {
		buf[0] = ';';
		buf[1] = '\0';
	}
	strcat(buf, name);
	strcat(buf, ";");
	wpc->set_directives = buf;

	return 0;
}

bool fpm_conf_directive_was_set(struct fpm_worker_pool_config_s *wpc, const char *name)
{
	char needle[128];

	if (!wpc->set_directives) {
		return false;
	}
	if ((size_t)snprintf(needle, sizeof(needle), ";%s;", name) >= sizeof(needle)) {
		return false;
	}

	return strstr(wpc->set_directives, needle) != NULL;
}

int fpm_worker_pool_config_free(struct fpm_worker_pool_config_s *wpc) /* {{{ */
{
	struct key_value_s *kv, *kv_next;

	free(wpc->name);
	free(wpc->type);
	free(wpc->executor);
	free(wpc->set_directives);
	free(wpc->prefix);
	free(wpc->user);
	free(wpc->group);
	free(wpc->listen_address);
	free(wpc->listen_owner);
	free(wpc->listen_group);
	free(wpc->listen_mode);
	free(wpc->listen_allowed_clients);
	free(wpc->pm_status_path);
	free(wpc->pm_status_listen);
	free(wpc->pm_metrics_path);
	free(wpc->pm_metrics_listen);
	free(wpc->ping_path);
	free(wpc->ping_response);
	free(wpc->access_log);
	free(wpc->access_format);
	free(wpc->slowlog);
	free(wpc->chroot);
	free(wpc->chdir);
	free(wpc->security_limit_extensions);
	free(wpc->supervisor_script);
	free(wpc->supervisor_restart);
	free(wpc->cron_schedule);
	free(wpc->cron_script);
	free(wpc->cron_parsed_schedule);
	free(wpc->cron_timezone);
	free(wpc->cron_log);
	free(wpc->http_listen);
	free(wpc->http_plain_listen);
	free(wpc->http_allowed_clients);
	free(wpc->http_trusted_proxies);
	free(wpc->http_access_log);
	free(wpc->http_front_controller);
	free(wpc->fiber_isolate_statics);
#ifdef HAVE_APPARMOR
	free(wpc->apparmor_hat);
#endif

	for (kv = wpc->access_suppress_paths; kv; kv = kv_next) {
		kv_next = kv->next;
		free(kv->value);
		free(kv);
	}
	for (kv = wpc->php_values; kv; kv = kv_next) {
		kv_next = kv->next;
		free(kv->key);
		free(kv->value);
		free(kv);
	}
	for (kv = wpc->php_admin_values; kv; kv = kv_next) {
		kv_next = kv->next;
		free(kv->key);
		free(kv->value);
		free(kv);
	}
	for (kv = wpc->env; kv; kv = kv_next) {
		kv_next = kv->next;
		free(kv->key);
		free(kv->value);
		free(kv);
	}

	return 0;
}
/* }}} */

struct fpm_worker_pool_s *fpm_conf_internal_pool_alloc(const char *name, const char *type,
	const char *listen_address, struct fpm_worker_pool_s *like) /* {{{ */
{
	struct fpm_worker_pool_config_s *config;
	struct fpm_worker_pool_s *wp, *saved = current_wp;
	char *owned_name;

	/* The name is taken before anything is created, because
	 * fpm_worker_pool_config_alloc() links the new pool into
	 * fpm_worker_all_pools immediately and both fpm_conf_process_all_pools()'s
	 * error path and fpm_conf_dump() print wp->config->name without checking
	 * it. Failing after the link, with the name still NULL, would leave them a
	 * pool to dereference; failing before it leaves the list untouched. */
	owned_name = strdup(name);
	if (!owned_name) {
		return NULL;
	}

	config = fpm_worker_pool_config_alloc();
	if (!config) {
		free(owned_name);
		return NULL;
	}
	wp = current_wp;
	/* fpm_worker_pool_config_alloc() moves current_wp to the pool it just
	 * created, which is how the config PARSER tracks the section it is inside.
	 * We are past parsing, so put it back rather than leave a pool nobody
	 * parsed looking like the current section. */
	current_wp = saved;

	config->name = owned_name;
	config->type = strdup(type);
	config->listen_address = strdup(listen_address);
	if (!config->type || !config->listen_address) {
		return NULL;
	}

	/* Identity only, and only from the pool that asked for this listener: a
	 * process created on a pool's behalf must not run with more privilege than
	 * the pool itself. Everything else a pool carries -- php_value, chroot,
	 * request timeouts -- describes running PHP, which this process never
	 * does. */
	if (like && like->config) {
		static const size_t identity[] = {
			WPO(user), WPO(group), WPO(listen_owner), WPO(listen_group), WPO(listen_mode)
		};
		size_t i;

		for (i = 0; i < sizeof(identity) / sizeof(identity[0]); i++) {
			char *const *from = (char *const *) ((char *) like->config + identity[i]);
			char **to = (char **) ((char *) config + identity[i]);

			if (*from && !(*to = strdup(*from))) {
				return NULL;
			}
		}
	}

	/* Deliberately NOT noted in set_directives: nothing here was set by a
	 * configuration, and a reject list must be free to refuse a directive on
	 * this type without that refusal being triggered by fpm-ng's own doing. */

	return wp;
}
/* }}} */

static int fpm_evaluate_full_path(char **path, struct fpm_worker_pool_s *wp, char *default_prefix, int expand) /* {{{ */
{
	char *prefix = NULL;
	char *full_path;

	if (!path || !*path || **path == '/') {
		return 0;
	}

	if (wp && wp->config) {
		prefix = wp->config->prefix;
	}

	/* if the wp prefix is not set */
	if (prefix == NULL) {
		prefix = fpm_globals.prefix;
	}

	/* if the global prefix is not set */
	if (prefix == NULL) {
		prefix = default_prefix ? default_prefix : PHP_PREFIX;
	}

	if (expand) {
		char *tmp;
		tmp = strstr(*path, "$prefix");
		if (tmp != NULL) {

			if (tmp != *path) {
				zlog(ZLOG_ERROR, "'$prefix' must be use at the beginning of the value");
				return -1;
			}

			if (strlen(*path) > strlen("$prefix")) {
				tmp = strdup((*path) + strlen("$prefix"));
				free(*path);
				*path = tmp;
			} else {
				free(*path);
				*path = NULL;
			}
		}
	}

	if (*path) {
		spprintf(&full_path, 0, "%s/%s", prefix, *path);
		free(*path);
		*path = strdup(full_path);
		efree(full_path);
	} else {
		*path = strdup(prefix);
	}

	if (**path != '/' && wp != NULL && wp->config) {
		return fpm_evaluate_full_path(path, NULL, default_prefix, expand);
	}
	return 0;
}
/* }}} */

static int fpm_conf_process_all_pools(void)
{
	struct fpm_worker_pool_s *wp, *wp2;

	if (!fpm_worker_all_pools) {
		zlog(ZLOG_ERROR, "No pool defined. at least one pool section must be specified in config file");
		return -1;
	}

	for (wp = fpm_worker_all_pools; wp; wp = wp->next) {
		const struct fpm_pool_type_s *type;

		/* pool.type + pool.executor — resolve once; from here on, use the
		 * requirements of the effective combination. */
		type = fpm_pool_type_get(wp->config->type);
		if (!type) {
			const char *retired = fpm_pool_type_retired(wp->config->type);
			char known[256];

			if (retired) {
				zlog(ZLOG_ALERT, "[pool %s] pool.type '%s' no longer exists: %s",
					wp->config->name, wp->config->type, retired);
				return -1;
			}

			fpm_pool_type_list(known, sizeof(known));
			zlog(ZLOG_ALERT, "[pool %s] unknown pool.type '%s'; known types: %s",
				wp->config->name, wp->config->type, known);
			return -1;
		}
		if (0 > fpm_pool_type_check_configurable(wp, type)) {
			return -1;
		}
		if (0 > fpm_pool_type_validate_executor(wp)) {
			return -1;
		}
		type = fpm_pool_type_resolve(wp);
		if (!type) {
			return -1;
		}

		/* prefix */
		if (wp->config->prefix && *wp->config->prefix) {
			fpm_evaluate_full_path(&wp->config->prefix, NULL, NULL, 0);

			if (!fpm_conf_is_dir(wp->config->prefix)) {
				zlog(ZLOG_ERROR, "[pool %s] the prefix '%s' does not exist or is not a directory", wp->config->name, wp->config->prefix);
				return -1;
			}
		}

		/* Directives unsupported by this type — reject, do not ignore. */
		if (0 > fpm_pool_type_check_directives(wp, type)) {
			return -1;
		}

		/* A type this binary cannot honour — reject before anything starts,
		 * rather than run it on top of the missing patch (issue #214). */
		if (0 > fpm_pool_type_check_build_support(wp, type)) {
			return -1;
		}

		/* listen — resolved before type->validate() (fpmng: task 015) so that
		 * wp->listen_address_domain is populated by the time a type's validate()
		 * reads it. fpm_http_validate_pool() needs this to tell a TCP listen
		 * (where http.listen may default to the FastCGI port + 1) from a unix
		 * socket (where it can't); with the original upstream ordering
		 * (validate() before listen) that check always saw the zero, unset
		 * value — matching neither FPM_AF_UNIX nor FPM_AF_INET — and treated
		 * every http pool as if it listened on a unix socket. This must still
		 * run before type->validate(), because fpm_pool_supervisor_validate()
		 * and its like set wp->config->pm/pm_max_children,
		 * which the "pm" checks further below depend on — moving listen later
		 * than those would work too, but moving validate() before listen would
		 * reintroduce this bug. */
		if (wp->config->listen_address && *wp->config->listen_address) {
			wp->listen_address_domain = fpm_sockets_domain_from_address(wp->config->listen_address);

			if (wp->listen_address_domain == FPM_AF_UNIX && *wp->config->listen_address != '/') {
				fpm_evaluate_full_path(&wp->config->listen_address, wp, NULL, 0);
			}
		} else if (type->requires_listen) {
			zlog(ZLOG_ALERT, "[pool %s] no listen address have been defined!", wp->config->name);
			return -1;
		}

		/* Type-specific checks. */
		if (type->validate && 0 > type->validate(wp)) {
			return -1;
		}

		/* alert if user is not set; only if we are root and fpm is not running with --allow-to-run-as-root */
		if (!wp->config->user && !geteuid() && !fpm_globals.run_as_root) {
			zlog(ZLOG_ALERT, "[pool %s] 'user' directive has not been specified when running as a root without --allow-to-run-as-root", wp->config->name);
			return -1;
		}

		if (wp->config->process_priority != 64 && (wp->config->process_priority < -19 || wp->config->process_priority > 20)) {
			zlog(ZLOG_ERROR, "[pool %s] process.priority must be included into [-19,20]", wp->config->name);
			return -1;
		}

		/* pm */
		if (type->requires_pm && wp->config->pm != PM_STYLE_STATIC && wp->config->pm != PM_STYLE_DYNAMIC && wp->config->pm != PM_STYLE_ONDEMAND) {
			zlog(ZLOG_ALERT, "[pool %s] the process manager is missing (static, dynamic or ondemand)", wp->config->name);
			return -1;
		}

		/* pm.max_children */
		if (type->requires_pm && wp->config->pm_max_children < 1) {
			zlog(ZLOG_ALERT, "[pool %s] pm.max_children must be a positive value", wp->config->name);
			return -1;
		}

		/* pm.start_servers, pm.min_spare_servers, pm.max_spare_servers, pm.max_spawn_rate */
		if (wp->config->pm == PM_STYLE_DYNAMIC) {
			struct fpm_worker_pool_config_s *config = wp->config;

			if (config->pm_min_spare_servers <= 0) {
				zlog(ZLOG_ALERT, "[pool %s] pm.min_spare_servers(%d) must be a positive value", wp->config->name, config->pm_min_spare_servers);
				return -1;
			}

			if (config->pm_max_spare_servers <= 0) {
				zlog(ZLOG_ALERT, "[pool %s] pm.max_spare_servers(%d) must be a positive value", wp->config->name, config->pm_max_spare_servers);
				return -1;
			}

			if (config->pm_min_spare_servers > config->pm_max_children ||
					config->pm_max_spare_servers > config->pm_max_children) {
				zlog(ZLOG_ALERT, "[pool %s] pm.min_spare_servers(%d) and pm.max_spare_servers(%d) cannot be greater than pm.max_children(%d)", wp->config->name, config->pm_min_spare_servers, config->pm_max_spare_servers, config->pm_max_children);
				return -1;
			}

			if (config->pm_max_spare_servers < config->pm_min_spare_servers) {
				zlog(ZLOG_ALERT, "[pool %s] pm.max_spare_servers(%d) must not be less than pm.min_spare_servers(%d)", wp->config->name, config->pm_max_spare_servers, config->pm_min_spare_servers);
				return -1;
			}

			if (config->pm_start_servers <= 0) {
				config->pm_start_servers = config->pm_min_spare_servers + ((config->pm_max_spare_servers - config->pm_min_spare_servers) / 2);
				zlog(ZLOG_NOTICE, "[pool %s] pm.start_servers is not set. It's been set to %d.", wp->config->name, config->pm_start_servers);

			} else if (config->pm_start_servers < config->pm_min_spare_servers || config->pm_start_servers > config->pm_max_spare_servers) {
				zlog(ZLOG_ALERT, "[pool %s] pm.start_servers(%d) must not be less than pm.min_spare_servers(%d) and not greater than pm.max_spare_servers(%d)", wp->config->name, config->pm_start_servers, config->pm_min_spare_servers, config->pm_max_spare_servers);
				return -1;
			}

			if (config->pm_max_spawn_rate < 1) {
				zlog(ZLOG_ALERT, "[pool %s] pm.max_spawn_rate must be a positive value", wp->config->name);
				return -1;
			}
		} else if (wp->config->pm == PM_STYLE_ONDEMAND) {
			struct fpm_worker_pool_config_s *config = wp->config;

			if (!fpm_event_support_edge_trigger()) {
				zlog(ZLOG_ALERT, "[pool %s] ondemand process manager can ONLY be used when events.mechanism is either epoll (Linux) or kqueue (*BSD).", wp->config->name);
				return -1;
			}

			if (config->pm_process_idle_timeout < 1) {
				zlog(ZLOG_ALERT, "[pool %s] pm.process_idle_timeout(%ds) must be greater than 0s", wp->config->name, config->pm_process_idle_timeout);
				return -1;
			}

			if (config->listen_backlog < FPM_BACKLOG_DEFAULT) {
				zlog(ZLOG_WARNING, "[pool %s] listen.backlog(%d) was too low for the ondemand process manager. I updated it for you to %d.", wp->config->name, config->listen_backlog, FPM_BACKLOG_DEFAULT);
				config->listen_backlog = FPM_BACKLOG_DEFAULT;
			}

			/* certainly useless but proper */
			config->pm_start_servers = 0;
			config->pm_min_spare_servers = 0;
			config->pm_max_spare_servers = 0;
		}

		/* status and metrics.
		 *
		 * On a type that carries its own operator endpoint (#273, #274) the two
		 * pm.*_listen directives say where THAT endpoint binds, and the pool
		 * they bind is created by fpm_operator_endpoint.c.
		 *
		 * On fastcgi and fastcgi-ng there is no such listener, on purpose:
		 * those types have a web server in front of them, which is where an
		 * operator already restricts who may reach a path. pm.status_path
		 * therefore keeps its upstream meaning there -- answered on the pool's
		 * own FastCGI socket -- and the two addresses have nothing to name.
		 * Whether a type has the listener is a flag on the type and never a
		 * name compared here; see fpm_pool_type_s.operator_endpoint. */
		if (type->operator_endpoint) {
			if (0 > fpm_operator_endpoint_configure(wp, type)) {
				return -1;
			}
		} else {
			/* Refused rather than ignored, and this is a behaviour change:
			 * until issue #278, pm.status_listen on these types auto-allocated
			 * a second pool named <pool>_status, pm = ondemand,
			 * pm.max_children = 2, speaking FastCGI and inheriting this pool's
			 * user, group, status path, ping path and allowed clients. That
			 * pool is gone -- #274 puts an operator endpoint on the types that
			 * need one without a second pool, and keeping both would be two
			 * answers to one question. A config that relied on it must now
			 * restrict pm.status_path at the web server that is already in
			 * front, so the error says so rather than starting a master that
			 * quietly no longer has the pool the operator is scraping. */
			static const char *const unsupported[] = { "pm.status_listen", "pm.metrics_listen", "pm.metrics_path" };
			const char *const values[] = {
				wp->config->pm_status_listen,
				wp->config->pm_metrics_listen,
				wp->config->pm_metrics_path
			};
			size_t i;

			for (i = 0; i < sizeof(unsupported) / sizeof(unsupported[0]); i++) {
				if (!values[i] || !*values[i]) {
					continue;
				}
				zlog(ZLOG_ALERT, "[pool %s] '%s' is not supported by pool.type = %s: that type has no "
					"operator listener of its own, because it has a web server in front of it -- "
					"keep 'pm.status_path' on the pool's own socket and restrict it there",
					wp->config->name, unsupported[i], type->name);
				return -1;
			}
		}

		if (wp->config->pm_status_path && *wp->config->pm_status_path) {
			size_t i;
			char *status = wp->config->pm_status_path;

			if (*status != '/') {
				zlog(ZLOG_ERROR, "[pool %s] the status path '%s' must start with a '/'", wp->config->name, status);
				return -1;
			}

			if (strlen(status) < 2) {
				zlog(ZLOG_ERROR, "[pool %s] the status path '%s' is not long enough", wp->config->name, status);
				return -1;
			}

			for (i = 0; i < strlen(status); i++) {
				if (!isalnum((unsigned char)status[i]) && status[i] != '/' && status[i] != '-' && status[i] != '_' && status[i] != '.' && status[i] != '~') {
					zlog(ZLOG_ERROR, "[pool %s] the status path '%s' must contain only the following characters '[alphanum]/_-.~'", wp->config->name, status);
					return -1;
				}
			}
		}

		/* ping */
		if (wp->config->ping_path && *wp->config->ping_path) {
			char *ping = wp->config->ping_path;
			size_t i;

			if (*ping != '/') {
				zlog(ZLOG_ERROR, "[pool %s] the ping path '%s' must start with a '/'", wp->config->name, ping);
				return -1;
			}

			if (strlen(ping) < 2) {
				zlog(ZLOG_ERROR, "[pool %s] the ping path '%s' is not long enough", wp->config->name, ping);
				return -1;
			}

			for (i = 0; i < strlen(ping); i++) {
				if (!isalnum((unsigned char)ping[i]) && ping[i] != '/' && ping[i] != '-' && ping[i] != '_' && ping[i] != '.' && ping[i] != '~') {
					zlog(ZLOG_ERROR, "[pool %s] the ping path '%s' must contain only the following characters '[alphanum]/_-.~'", wp->config->name, ping);
					return -1;
				}
			}

			if (!wp->config->ping_response) {
				wp->config->ping_response = strdup("pong");
			} else {
				if (strlen(wp->config->ping_response) < 1) {
					zlog(ZLOG_ERROR, "[pool %s] the ping response page '%s' is not long enough", wp->config->name, wp->config->ping_response);
					return -1;
				}
			}
		} else {
			if (wp->config->ping_response) {
				free(wp->config->ping_response);
				wp->config->ping_response = NULL;
			}
		}

		/* access.log, access.format */
		if (wp->config->access_log && *wp->config->access_log) {
			fpm_evaluate_full_path(&wp->config->access_log, wp, NULL, 0);
			if (!wp->config->access_format) {
				wp->config->access_format = strdup("%R - %u %t \"%m %r\" %s");
			}
		}

		if (wp->config->request_terminate_timeout) {
			fpm_globals.heartbeat = fpm_globals.heartbeat ? MIN(fpm_globals.heartbeat, (wp->config->request_terminate_timeout * 1000) / 3) : (wp->config->request_terminate_timeout * 1000) / 3;
		}

		/* slowlog */
		if (wp->config->slowlog && *wp->config->slowlog) {
			fpm_evaluate_full_path(&wp->config->slowlog, wp, NULL, 0);
		}

		/* request_slowlog_timeout */
		if (wp->config->request_slowlog_timeout) {
#if HAVE_FPM_TRACE
			if (! (wp->config->slowlog && *wp->config->slowlog)) {
				zlog(ZLOG_ERROR, "[pool %s] 'slowlog' must be specified for use with 'request_slowlog_timeout'", wp->config->name);
				return -1;
			}
#else
			static int warned = 0;

			if (!warned) {
				zlog(ZLOG_WARNING, "[pool %s] 'request_slowlog_timeout' is not supported on your system",	wp->config->name);
				warned = 1;
			}

			wp->config->request_slowlog_timeout = 0;
#endif

			if (wp->config->slowlog && *wp->config->slowlog) {
				int fd;

				fd = open(wp->config->slowlog, O_WRONLY | O_APPEND | O_CREAT, S_IRUSR | S_IWUSR);

				if (0 > fd) {
					zlog(ZLOG_SYSERROR, "Unable to create or open slowlog(%s)", wp->config->slowlog);
					return -1;
				}
				close(fd);
			}

			fpm_globals.heartbeat = fpm_globals.heartbeat ? MIN(fpm_globals.heartbeat, (wp->config->request_slowlog_timeout * 1000) / 3) : (wp->config->request_slowlog_timeout * 1000) / 3;

			if (wp->config->request_terminate_timeout && wp->config->request_slowlog_timeout > wp->config->request_terminate_timeout) {
				zlog(ZLOG_ERROR, "[pool %s] 'request_slowlog_timeout' (%d) can't be greater than 'request_terminate_timeout' (%d)", wp->config->name, wp->config->request_slowlog_timeout, wp->config->request_terminate_timeout);
				return -1;
			}
		}

		/* request_slowlog_trace_depth */
		if (wp->config->request_slowlog_trace_depth) {
#if HAVE_FPM_TRACE
			if (! (wp->config->slowlog && *wp->config->slowlog)) {
				zlog(ZLOG_ERROR, "[pool %s] 'slowlog' must be specified for use with 'request_slowlog_trace_depth'", wp->config->name);
				return -1;
			}
#else
			static int warned = 0;

			if (!warned) {
				zlog(ZLOG_WARNING, "[pool %s] 'request_slowlog_trace_depth' is not supported on your system", wp->config->name);
				warned = 1;
			}
#endif

			if (wp->config->request_slowlog_trace_depth <= 0) {
				zlog(ZLOG_ERROR, "[pool %s] 'request_slowlog_trace_depth' (%d) must be a positive value", wp->config->name, wp->config->request_slowlog_trace_depth);
				return -1;
			}
		} else {
			wp->config->request_slowlog_trace_depth = 20;
		}

		/* chroot */
		if (wp->config->chroot && *wp->config->chroot) {

			fpm_evaluate_full_path(&wp->config->chroot, wp, NULL, 1);

			if (*wp->config->chroot != '/') {
				zlog(ZLOG_ERROR, "[pool %s] the chroot path '%s' must start with a '/'", wp->config->name, wp->config->chroot);
				return -1;
			}

			if (!fpm_conf_is_dir(wp->config->chroot)) {
				zlog(ZLOG_ERROR, "[pool %s] the chroot path '%s' does not exist or is not a directory", wp->config->name, wp->config->chroot);
				return -1;
			}
		}

		/* chdir */
		if (wp->config->chdir && *wp->config->chdir) {

			fpm_evaluate_full_path(&wp->config->chdir, wp, NULL, 0);

			if (*wp->config->chdir != '/') {
				zlog(ZLOG_ERROR, "[pool %s] the chdir path '%s' must start with a '/'", wp->config->name, wp->config->chdir);
				return -1;
			}

			if (wp->config->chroot) {
				char *buf;

				spprintf(&buf, 0, "%s/%s", wp->config->chroot, wp->config->chdir);

				if (!fpm_conf_is_dir(buf)) {
					zlog(ZLOG_ERROR, "[pool %s] the chdir path '%s' within the chroot path '%s' ('%s') does not exist or is not a directory", wp->config->name, wp->config->chdir, wp->config->chroot, buf);
					efree(buf);
					return -1;
				}

				efree(buf);
			} else {
				if (!fpm_conf_is_dir(wp->config->chdir)) {
					zlog(ZLOG_ERROR, "[pool %s] the chdir path '%s' does not exist or is not a directory", wp->config->name, wp->config->chdir);
					return -1;
				}
			}
		}

		/* security.limit_extensions */
		if (!wp->config->security_limit_extensions) {
			wp->config->security_limit_extensions = strdup(".php .phar");
		}

		if (*wp->config->security_limit_extensions) {
			int nb_ext;
			char *ext;
			char *security_limit_extensions;
			char *limit_extensions;


			/* strdup because strtok(3) alters the string it parses */
			security_limit_extensions = strdup(wp->config->security_limit_extensions);
			limit_extensions = security_limit_extensions;
			nb_ext = 0;

			/* find the number of extensions */
			while (strtok(limit_extensions, " \t")) {
				limit_extensions = NULL;
				nb_ext++;
			}
			free(security_limit_extensions);

			/* if something found */
			if (nb_ext > 0) {

				/* malloc the extension array */
				wp->limit_extensions = malloc(sizeof(char *) * (nb_ext + 1));
				if (!wp->limit_extensions) {
					zlog(ZLOG_ERROR, "[pool %s] unable to malloc extensions array", wp->config->name);
					return -1;
				}

				/* strdup because strtok(3) alters the string it parses */
				security_limit_extensions = strdup(wp->config->security_limit_extensions);
				limit_extensions = security_limit_extensions;
				nb_ext = 0;

				/* parse the string and save the extension in the array */
				while ((ext = strtok(limit_extensions, " \t"))) {
					limit_extensions = NULL;
					wp->limit_extensions[nb_ext++] = strdup(ext);
				}

				/* end the array with NULL in order to parse it */
				wp->limit_extensions[nb_ext] = NULL;
				free(security_limit_extensions);
			}
		}

		/* env[], php_value[], php_admin_values[] */
		if (!wp->config->chroot) {
			struct key_value_s *kv;
			static const char *const options[] = FPM_PHP_INI_TO_EXPAND;

			for (kv = wp->config->php_values; kv; kv = kv->next) {
				for (const char *const*p = options; *p; p++) {
					if (!strcasecmp(kv->key, *p)) {
						fpm_evaluate_full_path(&kv->value, wp, NULL, 0);
					}
				}
			}
			for (kv = wp->config->php_admin_values; kv; kv = kv->next) {
				if (!strcasecmp(kv->key, "error_log") && !strcasecmp(kv->value, "syslog")) {
					continue;
				}
				for (const char *const*p = options; *p; p++) {
					if (!strcasecmp(kv->key, *p)) {
						fpm_evaluate_full_path(&kv->value, wp, NULL, 0);
					}
				}
			}
		}
	}

	/* ensure 2 pools do not use the same listening address */
	for (wp = fpm_worker_all_pools; wp; wp = wp->next) {
		for (wp2 = fpm_worker_all_pools; wp2; wp2 = wp2->next) {
			if (wp == wp2) {
				continue;
			}

			if (wp->config->listen_address && *wp->config->listen_address && wp2->config->listen_address && *wp2->config->listen_address && !strcmp(wp->config->listen_address, wp2->config->listen_address)) {
				zlog(ZLOG_ERROR, "[pool %s] unable to set listen address as it's already used in another pool '%s'", wp2->config->name, wp->config->name);
				return -1;
			}
		}
	}
	return 0;
}

int fpm_conf_unlink_pid(void)
{
	if (fpm_global_config.pid_file) {
		if (0 > unlink(fpm_global_config.pid_file)) {
			zlog(ZLOG_SYSERROR, "Unable to remove the PID file (%s).", fpm_global_config.pid_file);
			return -1;
		}
	}
	return 0;
}

int fpm_conf_write_pid(void)
{
	int fd;

	if (fpm_global_config.pid_file) {
		char buf[64];
		int len;

		unlink(fpm_global_config.pid_file);
		fd = creat(fpm_global_config.pid_file, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);

		if (fd < 0) {
			zlog(ZLOG_SYSERROR, "Unable to create the PID file (%s).", fpm_global_config.pid_file);
			return -1;
		}

		len = snprintf(buf, sizeof(buf), "%d", (int) fpm_globals.parent_pid);

		if (len != write(fd, buf, len)) {
			zlog(ZLOG_SYSERROR, "Unable to write to the PID file.");
			close(fd);
			return -1;
		}
		close(fd);
	}
	return 0;
}

static int fpm_conf_post_process(int force_daemon) /* {{{ */
{
	struct fpm_worker_pool_s *wp;

	if (fpm_global_config.pid_file) {
		fpm_evaluate_full_path(&fpm_global_config.pid_file, NULL, PHP_LOCALSTATEDIR, 0);
	}

	if (force_daemon >= 0) {
		/* forced from command line options */
		fpm_global_config.daemonize = force_daemon;
	}

	fpm_globals.log_level = fpm_global_config.log_level;
	zlog_set_level(fpm_globals.log_level);
	if (fpm_global_config.log_limit < ZLOG_MIN_LIMIT) {
		zlog(ZLOG_ERROR, "log_limit must be greater than %d", ZLOG_MIN_LIMIT);
		return -1;
	}
	zlog_set_limit(fpm_global_config.log_limit);
	zlog_set_buffering(fpm_global_config.log_buffering);

	if (fpm_global_config.process_max < 0) {
		zlog(ZLOG_ERROR, "process_max can't be negative");
		return -1;
	}

	if (fpm_global_config.process_priority != 64 && (fpm_global_config.process_priority < -19 || fpm_global_config.process_priority > 20)) {
		zlog(ZLOG_ERROR, "process.priority must be included into [-19,20]");
		return -1;
	}

	if (!fpm_global_config.error_log) {
		fpm_global_config.error_log = strdup("log/php-fpm.log");
	}

#ifdef HAVE_SYSTEMD
	if (0 > fpm_systemd_conf()) {
		return -1;
	}
#endif

#ifdef HAVE_SYSLOG_H
	if (!fpm_global_config.syslog_ident) {
		fpm_global_config.syslog_ident = strdup("php-fpm");
	}

	if (fpm_global_config.syslog_facility < 0) {
		fpm_global_config.syslog_facility = LOG_DAEMON;
	}

	if (strcasecmp(fpm_global_config.error_log, "syslog") != 0)
#endif
	{
		fpm_evaluate_full_path(&fpm_global_config.error_log, NULL, PHP_LOCALSTATEDIR, 0);
	}

	if (!fpm_global_config.daemonize && 0 > fpm_stdio_save_original_stderr()) {
		return -1;
	}

	if (0 > fpm_stdio_open_error_log(0)) {
		return -1;
	}

	if (0 > fpm_event_pre_init(fpm_global_config.events_mechanism)) {
		return -1;
	}

	if (0 > fpm_conf_process_all_pools()) {
		return -1;
	}

	if (0 > fpm_log_open(0)) {
		return -1;
	}

	for (wp = fpm_worker_all_pools; wp; wp = wp->next) {
		if (!wp->config->access_log || !*wp->config->access_log) {
			continue;
		}
		if (0 > fpm_log_write(wp->config->access_format)) {
			zlog(ZLOG_ERROR, "[pool %s] wrong format for access.format '%s'", wp->config->name, wp->config->access_format);
			return -1;
		}
	}

	return 0;
}
/* }}} */

static void fpm_conf_cleanup(int which, void *arg) /* {{{ */
{
	free(fpm_global_config.pid_file);
	free(fpm_global_config.error_log);
	free(fpm_global_config.events_mechanism);
	fpm_global_config.pid_file = 0;
	fpm_global_config.error_log = 0;
	fpm_global_config.log_limit = ZLOG_DEFAULT_LIMIT;
#ifdef HAVE_SYSLOG_H
	free(fpm_global_config.syslog_ident);
	fpm_global_config.syslog_ident = 0;
#endif
	free(fpm_globals.config);
}
/* }}} */

static void fpm_conf_ini_parser_include(char *inc, void *arg) /* {{{ */
{
	int *error = (int *)arg;
	php_glob_t g;
	size_t i;

	if (!inc || !arg) return;
	if (*error) return; /* We got already an error. Switch to the end. */

	const char *filename = ini_filename;

	{
		g.gl_offs = 0;
		if ((i = php_glob(inc, PHP_GLOB_ERR | PHP_GLOB_MARK, NULL, &g)) != 0) {
#ifdef PHP_GLOB_NOMATCH
			if (i == PHP_GLOB_NOMATCH) {
				zlog(ZLOG_WARNING, "Nothing matches the include pattern '%s' from %s at line %d.", inc, filename, ini_lineno);
				return;
			}
#endif /* PHP_GLOB_NOMATCH */
			zlog(ZLOG_ERROR, "Unable to globalize '%s' (ret=%zd) from %s at line %d.", inc, i, filename, ini_lineno);
			*error = 1;
			return;
		}

		for (i = 0; i < g.gl_pathc; i++) {
			size_t len = strlen(g.gl_pathv[i]);
			if (len < 1) continue;
			if (g.gl_pathv[i][len - 1] == '/') continue; /* don't parse directories */
			if (0 > fpm_conf_load_ini_file(g.gl_pathv[i])) {
				zlog(ZLOG_ERROR, "Unable to include %s from %s at line %d", g.gl_pathv[i], filename, ini_lineno);
				*error = 1;
				return;
			}
		}
		php_globfree(&g);
	}
}
/* }}} */

static void fpm_conf_ini_parser_section(zval *section, void *arg) /* {{{ */
{
	struct fpm_worker_pool_s *wp;
	struct fpm_worker_pool_config_s *config;
	int *error = (int *)arg;

	/* switch to global conf */
	if (zend_string_equals_literal_ci(Z_STR_P(section), "global")) {
		current_wp = NULL;
		return;
	}

	for (wp = fpm_worker_all_pools; wp; wp = wp->next) {
		if (!wp->config) continue;
		if (!wp->config->name) continue;
		if (!strcasecmp(wp->config->name, Z_STRVAL_P(section))) {
			/* Found a wp with the same name. Bring it back */
			current_wp = wp;
			return;
		}
	}

	/* it's a new pool */
	config = (struct fpm_worker_pool_config_s *)fpm_worker_pool_config_alloc();
	if (!current_wp || !config) {
		zlog(ZLOG_ERROR, "[%s:%d] Unable to alloc a new WorkerPool for worker '%s'", ini_filename, ini_lineno, Z_STRVAL_P(section));
		*error = 1;
		return;
	}
	config->name = strdup(Z_STRVAL_P(section));
	if (!config->name) {
		zlog(ZLOG_ERROR, "[%s:%d] Unable to alloc memory for configuration name for worker '%s'", ini_filename, ini_lineno, Z_STRVAL_P(section));
		*error = 1;
		return;
	}
}
/* }}} */

static void fpm_conf_ini_parser_entry(zval *name, zval *value, void *arg) /* {{{ */
{
	int in_pool = 0;
	const struct ini_value_parser_s *parser;
	void *config = NULL;

	int *error = (int *)arg;
	if (!value) {
		zlog(ZLOG_ERROR, "[%s:%d] value is NULL for a ZEND_INI_PARSER_ENTRY", ini_filename, ini_lineno);
		*error = 1;
		return;
	}

	if (zend_string_equals_literal(Z_STR_P(name), "include")) {
		if (ini_include) {
			zlog(ZLOG_ERROR, "[%s:%d] two includes at the same time !", ini_filename, ini_lineno);
			*error = 1;
			return;
		}
		ini_include = strdup(Z_STRVAL_P(value));
		return;
	}

	if (!current_wp) { /* we are in the global section */
		parser = ini_fpm_global_options;
		config = &fpm_global_config;
	} else {
		parser = ini_fpm_pool_options;
		config = current_wp->config;
		in_pool = 1;
	}

	for (; parser->name; parser++) {
		if (!strcasecmp(parser->name, Z_STRVAL_P(name))) {
			char *ret;
			if (!parser->parser) {
				zlog(ZLOG_ERROR, "[%s:%d] the parser for entry '%s' is not defined", ini_filename, ini_lineno, parser->name);
				*error = 1;
				return;
			}

			ret = parser->parser(value, &config, parser->offset);
			if (ret) {
				zlog(ZLOG_ERROR, "[%s:%d] unable to parse value for entry '%s': %s", ini_filename, ini_lineno, parser->name, ret);
				*error = 1;
				return;
			}

			/* fpm-ng: remember that this directive was actually set */
			if (in_pool && 0 > fpm_conf_note_directive(current_wp->config, parser->name)) {
				zlog(ZLOG_ERROR, "[%s:%d] out of memory noting entry '%s'", ini_filename, ini_lineno, parser->name);
				*error = 1;
			}

			/* all is good ! */
			return;
		}
	}

	/* nothing has been found if we got here */
	zlog(ZLOG_ERROR, "[%s:%d] unknown entry '%s'", ini_filename, ini_lineno, Z_STRVAL_P(name));
	*error = 1;
}
/* }}} */

static void fpm_conf_ini_parser_array(zval *name, zval *key, zval *value, void *arg) /* {{{ */
{
	int *error = (int *)arg;
	char *err = NULL;
	void *config;

	if (zend_string_equals_literal(Z_STR_P(name), "access.suppress_path")) {
		if (!(*Z_STRVAL_P(key) == '\0')) {
			zlog(ZLOG_ERROR, "[%s:%d] Keys provided to field 'access.suppress_path' are ignored", ini_filename, ini_lineno);
			*error = 1;
		}
		if (!(*Z_STRVAL_P(value)) || (*Z_STRVAL_P(value) != '/')) {
			zlog(ZLOG_ERROR, "[%s:%d] Values provided to field 'access.suppress_path' must begin with '/'", ini_filename, ini_lineno);
			*error = 1;
		}
		if (*error) {
			return;
		}
	} else if (!*Z_STRVAL_P(key)) {
		zlog(ZLOG_ERROR, "[%s:%d] You must provide a key for field '%s'", ini_filename, ini_lineno, Z_STRVAL_P(name));
		*error = 1;
		return;
	}

	if (!current_wp || !current_wp->config) {
		zlog(ZLOG_ERROR, "[%s:%d] Array are not allowed in the global section", ini_filename, ini_lineno);
		*error = 1;
		return;
	}

	if (zend_string_equals_literal(Z_STR_P(name), "env")) {
		if (!*Z_STRVAL_P(value)) {
			zlog(ZLOG_ERROR, "[%s:%d] empty value", ini_filename, ini_lineno);
			*error = 1;
			return;
		}
		config = (char *)current_wp->config + WPO(env);
		err = fpm_conf_set_array(key, value, &config, 0);

	} else if (zend_string_equals_literal(Z_STR_P(name), "php_value")) {
		config = (char *)current_wp->config + WPO(php_values);
		err = fpm_conf_set_array(key, value, &config, 0);

	} else if (zend_string_equals_literal(Z_STR_P(name), "php_admin_value")) {
		config = (char *)current_wp->config + WPO(php_admin_values);
		err = fpm_conf_set_array(key, value, &config, 0);

	} else if (zend_string_equals_literal(Z_STR_P(name), "php_flag")) {
		config = (char *)current_wp->config + WPO(php_values);
		err = fpm_conf_set_array(key, value, &config, 1);

	} else if (zend_string_equals_literal(Z_STR_P(name), "php_admin_flag")) {
		config = (char *)current_wp->config + WPO(php_admin_values);
		err = fpm_conf_set_array(key, value, &config, 1);

	} else if (zend_string_equals_literal(Z_STR_P(name), "access.suppress_path")) {
		config = (char *)current_wp->config + WPO(access_suppress_paths);
		err = fpm_conf_set_array(NULL, value, &config, 0);

	} else {
		zlog(ZLOG_ERROR, "[%s:%d] unknown directive '%s'", ini_filename, ini_lineno, Z_STRVAL_P(name));
		*error = 1;
		return;
	}

	if (err) {
		zlog(ZLOG_ERROR, "[%s:%d] error while parsing '%s[%s]' : %s", ini_filename, ini_lineno, Z_STRVAL_P(name), Z_STRVAL_P(key), err);
		*error = 1;
		return;
	}
}
/* }}} */

static void fpm_conf_ini_parser(zval *arg1, zval *arg2, zval *arg3, int callback_type, void *arg) /* {{{ */
{
	int *error;

	if (!arg1 || !arg) return;
	error = (int *)arg;
	if (*error) return; /* We got already an error. Switch to the end. */

	switch(callback_type) {
		case ZEND_INI_PARSER_ENTRY:
			fpm_conf_ini_parser_entry(arg1, arg2, error);
			break;
		case ZEND_INI_PARSER_SECTION:
			fpm_conf_ini_parser_section(arg1, error);
			break;
		case ZEND_INI_PARSER_POP_ENTRY:
			fpm_conf_ini_parser_array(arg1, arg3, arg2, error);
			break;
		default:
			zlog(ZLOG_ERROR, "[%s:%d] Unknown INI syntax", ini_filename, ini_lineno);
			*error = 1;
			break;
	}
}
/* }}} */

int fpm_conf_load_ini_file(char *filename) /* {{{ */
{
	int error = 0;
	char *buf = NULL, *newbuf = NULL;
	int bufsize = 0;
	int fd, n;
	int nb_read = 1;
	char c = '*';

	int ret = 1;

	if (!filename || !filename[0]) {
		zlog(ZLOG_ERROR, "configuration filename is empty");
		return -1;
	}

	fd = open(filename, O_RDONLY, 0);
	if (fd < 0) {
		zlog(ZLOG_SYSERROR, "failed to open configuration file '%s'", filename);
		return -1;
	}

	if (ini_recursion++ > 4) {
		zlog(ZLOG_ERROR, "failed to include more than 5 files recursively");
		close(fd);
		return -1;
	}

	ini_lineno = 0;
	while (nb_read > 0) {
		int tmp;
		ini_lineno++;
		ini_filename = filename;
		for (n = 0; (nb_read = read(fd, &c, sizeof(char))) == sizeof(char) && c != '\n'; n++) {
			if (n == bufsize) {
				bufsize += 1024;
				newbuf = (char*) realloc(buf, sizeof(char) * (bufsize + 2));
				if (newbuf == NULL) {
					ini_recursion--;
					close(fd);
					free(buf);
					return -1;
				}
				buf = newbuf;
			}

			buf[n] = c;
		}
		if (n == 0) {
			continue;
		}
		/* always append newline and null terminate */
		buf[n++] = '\n';
		buf[n] = '\0';
		tmp = zend_parse_ini_string(buf, 1, ZEND_INI_SCANNER_NORMAL, (zend_ini_parser_cb_t)fpm_conf_ini_parser, &error);
		ini_filename = filename;
		if (error || tmp == FAILURE) {
			if (ini_include) {
				free(ini_include);
				ini_include = NULL;
			}
			ini_recursion--;
			close(fd);
			free(buf);
			return -1;
		}
		if (ini_include) {
			char *tmp = ini_include;
			ini_include = NULL;
			fpm_evaluate_full_path(&tmp, NULL, NULL, 0);
			fpm_conf_ini_parser_include(tmp, &error);
			if (error) {
				free(tmp);
				ini_recursion--;
				close(fd);
				free(buf);
				return -1;
			}
			free(tmp);
		}
	}
	free(buf);

	ini_recursion--;
	close(fd);
	return ret;
}
/* }}} */

static void fpm_conf_dump(void)
{
	struct fpm_worker_pool_s *wp;

	/*
	 * Please keep the same order as in fpm_conf.h and in php-fpm.conf.in
	 */
	zlog(ZLOG_NOTICE, "[global]");
	zlog(ZLOG_NOTICE, "\tpid = %s",                         STR2STR(fpm_global_config.pid_file));
	zlog(ZLOG_NOTICE, "\terror_log = %s",                   STR2STR(fpm_global_config.error_log));
#ifdef HAVE_SYSLOG_H
	zlog(ZLOG_NOTICE, "\tsyslog.ident = %s",                STR2STR(fpm_global_config.syslog_ident));
	zlog(ZLOG_NOTICE, "\tsyslog.facility = %d",             fpm_global_config.syslog_facility); /* FIXME: convert to string */
#endif
	zlog(ZLOG_NOTICE, "\tlog_buffering = %s",               BOOL2STR(fpm_global_config.log_buffering));
	zlog(ZLOG_NOTICE, "\tlog_level = %s",                   zlog_get_level_name(fpm_globals.log_level));
	zlog(ZLOG_NOTICE, "\tlog_limit = %d",                   fpm_global_config.log_limit);
	zlog(ZLOG_NOTICE, "\temergency_restart_interval = %ds", fpm_global_config.emergency_restart_interval);
	zlog(ZLOG_NOTICE, "\temergency_restart_threshold = %d", fpm_global_config.emergency_restart_threshold);
	zlog(ZLOG_NOTICE, "\tprocess_control_timeout = %ds",    fpm_global_config.process_control_timeout);
	zlog(ZLOG_NOTICE, "\tprocess.max = %d",                 fpm_global_config.process_max);
	if (fpm_global_config.process_priority == 64) {
		zlog(ZLOG_NOTICE, "\tprocess.priority = undefined");
	} else {
		zlog(ZLOG_NOTICE, "\tprocess.priority = %d", fpm_global_config.process_priority);
	}
	zlog(ZLOG_NOTICE, "\tdaemonize = %s",                   BOOL2STR(fpm_global_config.daemonize));
	zlog(ZLOG_NOTICE, "\trlimit_files = %d",                fpm_global_config.rlimit_files);
	zlog(ZLOG_NOTICE, "\trlimit_core = %d",                 fpm_global_config.rlimit_core);
	zlog(ZLOG_NOTICE, "\tevents.mechanism = %s",            fpm_event_mechanism_name());
#ifdef HAVE_SYSTEMD
	zlog(ZLOG_NOTICE, "\tsystemd_interval = %ds",           fpm_global_config.systemd_interval/1000);
#endif
	zlog(ZLOG_NOTICE, " ");

	for (wp = fpm_worker_all_pools; wp; wp = wp->next) {
		struct key_value_s *kv;

		if (!wp->config) {
			continue;
		}

		zlog(ZLOG_NOTICE, "[%s]",                              STR2STR(wp->config->name));
		zlog(ZLOG_NOTICE, "\tprefix = %s",                     STR2STR(wp->config->prefix));
		zlog(ZLOG_NOTICE, "\tuser = %s",                       STR2STR(wp->config->user));
		zlog(ZLOG_NOTICE, "\tgroup = %s",                      STR2STR(wp->config->group));
		zlog(ZLOG_NOTICE, "\tpool.type = %s",                  STR2STR(wp->config->type ? wp->config->type : "fastcgi"));
		zlog(ZLOG_NOTICE, "\tpool.executor = %s",              STR2STR(wp->config->executor ? wp->config->executor : "classic"));
		zlog(ZLOG_NOTICE, "\tlisten = %s",                     STR2STR(wp->config->listen_address));
		zlog(ZLOG_NOTICE, "\tlisten.backlog = %d",             wp->config->listen_backlog);
#ifdef HAVE_FPM_ACL
		zlog(ZLOG_NOTICE, "\tlisten.acl_users = %s",           STR2STR(wp->config->listen_acl_users));
		zlog(ZLOG_NOTICE, "\tlisten.acl_groups = %s",          STR2STR(wp->config->listen_acl_groups));
#endif
		zlog(ZLOG_NOTICE, "\tlisten.owner = %s",               STR2STR(wp->config->listen_owner));
		zlog(ZLOG_NOTICE, "\tlisten.group = %s",               STR2STR(wp->config->listen_group));
		zlog(ZLOG_NOTICE, "\tlisten.mode = %s",                STR2STR(wp->config->listen_mode));
		zlog(ZLOG_NOTICE, "\tlisten.allowed_clients = %s",     STR2STR(wp->config->listen_allowed_clients));
#ifdef SO_SETFIB
		zlog(ZLOG_NOTICE, "\tlisten.setfib = %d",              wp->config->listen_setfib);
#endif
		if (wp->config->process_priority == 64) {
			zlog(ZLOG_NOTICE, "\tprocess.priority = undefined");
		} else {
			zlog(ZLOG_NOTICE, "\tprocess.priority = %d", wp->config->process_priority);
		}
		zlog(ZLOG_NOTICE, "\tprocess.dumpable = %s",           BOOL2STR(wp->config->process_dumpable));
		zlog(ZLOG_NOTICE, "\tpm = %s",                         PM2STR(wp->config->pm));
		zlog(ZLOG_NOTICE, "\tpm.max_children = %d",            wp->config->pm_max_children);
		zlog(ZLOG_NOTICE, "\tpm.start_servers = %d",           wp->config->pm_start_servers);
		zlog(ZLOG_NOTICE, "\tpm.min_spare_servers = %d",       wp->config->pm_min_spare_servers);
		zlog(ZLOG_NOTICE, "\tpm.max_spare_servers = %d",       wp->config->pm_max_spare_servers);
		zlog(ZLOG_NOTICE, "\tpm.max_spawn_rate = %d",          wp->config->pm_max_spawn_rate);
		zlog(ZLOG_NOTICE, "\tpm.process_idle_timeout = %d",    wp->config->pm_process_idle_timeout);
		zlog(ZLOG_NOTICE, "\tpm.max_requests = %d",            wp->config->pm_max_requests);
		zlog(ZLOG_NOTICE, "\tpm.status_path = %s",             STR2STR(wp->config->pm_status_path));
		zlog(ZLOG_NOTICE, "\tpm.metrics_path = %s",            STR2STR(wp->config->pm_metrics_path));
		zlog(ZLOG_NOTICE, "\tpm.metrics_listen = %s",          STR2STR(wp->config->pm_metrics_listen));
		zlog(ZLOG_NOTICE, "\tpm.status_listen = %s",           STR2STR(wp->config->pm_status_listen));
		zlog(ZLOG_NOTICE, "\tping.path = %s",                  STR2STR(wp->config->ping_path));
		zlog(ZLOG_NOTICE, "\tping.response = %s",              STR2STR(wp->config->ping_response));
		zlog(ZLOG_NOTICE, "\taccess.log = %s",                 STR2STR(wp->config->access_log));
		zlog(ZLOG_NOTICE, "\taccess.format = %s",              STR2STR(wp->config->access_format));
		for (kv = wp->config->access_suppress_paths; kv; kv = kv->next) {
			zlog(ZLOG_NOTICE, "\taccess.suppress_path[] = %s", kv->value);
		}
		zlog(ZLOG_NOTICE, "\tslowlog = %s",                    STR2STR(wp->config->slowlog));
		zlog(ZLOG_NOTICE, "\trequest_slowlog_timeout = %ds",   wp->config->request_slowlog_timeout);
		zlog(ZLOG_NOTICE, "\trequest_slowlog_trace_depth = %d", wp->config->request_slowlog_trace_depth);
		zlog(ZLOG_NOTICE, "\trequest_terminate_timeout = %ds", wp->config->request_terminate_timeout);
		zlog(ZLOG_NOTICE, "\trequest_terminate_timeout_track_finished = %s", BOOL2STR(wp->config->request_terminate_timeout_track_finished));
		zlog(ZLOG_NOTICE, "\trequest_cpu_tracking = %s",       BOOL2STR(wp->config->request_cpu_tracking));
		zlog(ZLOG_NOTICE, "\trlimit_files = %d",               wp->config->rlimit_files);
		zlog(ZLOG_NOTICE, "\trlimit_core = %d",                wp->config->rlimit_core);
		zlog(ZLOG_NOTICE, "\tchroot = %s",                     STR2STR(wp->config->chroot));
		zlog(ZLOG_NOTICE, "\tchdir = %s",                      STR2STR(wp->config->chdir));
		zlog(ZLOG_NOTICE, "\tcatch_workers_output = %s",       BOOL2STR(wp->config->catch_workers_output));
		zlog(ZLOG_NOTICE, "\tdecorate_workers_output = %s",    BOOL2STR(wp->config->decorate_workers_output));
		zlog(ZLOG_NOTICE, "\tclear_env = %s",                  BOOL2STR(wp->config->clear_env));
		zlog(ZLOG_NOTICE, "\tsecurity.limit_extensions = %s",  wp->config->security_limit_extensions);

		for (kv = wp->config->env; kv; kv = kv->next) {
			zlog(ZLOG_NOTICE, "\tenv[%s] = %s", kv->key, kv->value);
		}

		for (kv = wp->config->php_values; kv; kv = kv->next) {
			zlog(ZLOG_NOTICE, "\tphp_value[%s] = %s", kv->key, kv->value);
		}

		for (kv = wp->config->php_admin_values; kv; kv = kv->next) {
			zlog(ZLOG_NOTICE, "\tphp_admin_value[%s] = %s", kv->key, kv->value);
		}
		zlog(ZLOG_NOTICE, " ");
	}
}

int fpm_conf_init_main(int test_conf, int force_daemon) /* {{{ */
{
	int ret;

	if (fpm_globals.prefix && *fpm_globals.prefix) {
		if (!fpm_conf_is_dir(fpm_globals.prefix)) {
			zlog(ZLOG_ERROR, "the global prefix '%s' does not exist or is not a directory", fpm_globals.prefix);
			return -1;
		}
	}

	if (fpm_globals.pid && *fpm_globals.pid) {
		fpm_global_config.pid_file = strdup(fpm_globals.pid);
	}

	if (fpm_globals.config == NULL) {
		char *tmp;

		if (fpm_globals.prefix == NULL) {
			spprintf(&tmp, 0, "%s/php-fpm.conf", PHP_SYSCONFDIR);
		} else {
			spprintf(&tmp, 0, "%s/etc/php-fpm.conf", fpm_globals.prefix);
		}

		if (!tmp) {
			zlog(ZLOG_SYSERROR, "spprintf() failed (tmp for fpm_globals.config)");
			return -1;
		}

		fpm_globals.config = strdup(tmp);
		efree(tmp);

		if (!fpm_globals.config) {
			zlog(ZLOG_SYSERROR, "spprintf() failed (fpm_globals.config)");
			return -1;
		}
	}

	ret = fpm_conf_load_ini_file(fpm_globals.config);

	if (0 > ret) {
		zlog(ZLOG_ERROR, "failed to load configuration file '%s'", fpm_globals.config);
		return -1;
	}

	if (0 > fpm_conf_post_process(force_daemon)) {
		zlog(ZLOG_ERROR, "failed to post process the configuration");
		return -1;
	}

	if (test_conf) {
		for (struct fpm_worker_pool_s *wp = fpm_worker_all_pools; wp; wp = wp->next) {
			if (!fpm_unix_test_config(wp)) {
				return -1;
			}
		}

		if (test_conf > 1) {
			fpm_conf_dump();
		}
		zlog(ZLOG_NOTICE, "configuration file %s test is successful", fpm_globals.config);
		fpm_globals.test_successful = 1;
		return -1;
	}

	if (0 > fpm_cleanup_add(FPM_CLEANUP_ALL, fpm_conf_cleanup, 0)) {
		return -1;
	}

	return 0;
}
/* }}} */

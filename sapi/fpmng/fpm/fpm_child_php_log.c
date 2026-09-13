/* fpm-ng: PHP diagnostics into error_log for child-driven pool types.
 * See fpm_child_php_log.h for the why. */

#include "fpm_config.h"

#include "php.h"
#include "php_syslog.h"
#include "SAPI.h"
#include "zend_ini.h"

#include "fpm_conf.h"
#include "fpm_php.h"
#include "fpm_pool_type.h"
#include "fpm_worker_pool.h"
#include "fpm_child_php_log.h"
#include "zlog.h"

/* The pool this child belongs to, for the "[pool %s]" prefix every message in
 * error_log carries. Held as a pointer because a child never changes pools and
 * the configuration outlives it. */
static struct fpm_worker_pool_s *fpm_child_php_log_wp = NULL;

/* syslog severity (what php_error_cb() computed for the error type) -> zlog
 * level. FPM has no INFO level, and the only thing that arrives as LOG_INFO is
 * E_DEPRECATED, which an operator of a supervised script should see with the
 * default log_level = notice — hence NOTICE rather than DEBUG. */
static int fpm_child_php_log_level(int syslog_type_int) /* {{{ */
{
	switch (syslog_type_int) {
		case LOG_EMERG:
		case LOG_ALERT:
		case LOG_CRIT:
			return ZLOG_ALERT;
		case LOG_ERR:
			return ZLOG_ERROR;
		case LOG_WARNING:
			return ZLOG_WARNING;
		case LOG_DEBUG:
			return ZLOG_DEBUG;
		default:
			/* LOG_NOTICE, LOG_INFO, and whatever a caller of php_log_err()
			 * outside php_error_cb() passes. */
			return ZLOG_NOTICE;
	}
}
/* }}} */

static void fpm_child_php_log_message(const char *message, int syslog_type_int) /* {{{ */
{
	const char *pool = fpm_child_php_log_wp && fpm_child_php_log_wp->config
		? fpm_child_php_log_wp->config->name : "-";
	int level = fpm_child_php_log_level(syslog_type_int);

	if (!message) {
		return;
	}

	/* The one filter this path applies itself. zlog() calls the external
	 * logger BEFORE comparing the level against log_level (zlog.c, vzlog()),
	 * so without this a script emitting E_DEPRECATED in a loop would format
	 * and send one datagram per notice for the master to drop — and how many
	 * of those there are is up to the script, not to the pool type (see the
	 * traffic note in fpm_child_log.c). Same log_level the master will apply,
	 * inherited from the same global configuration across the fork, so nothing
	 * that would have been logged is lost. */
	if (level < fpm_global_config.log_level) {
		return;
	}

	/* "PHP message: " kept from upstream's sapi_cgi_log_message(): it is what
	 * an existing log grep looks for, and the text after it is PHP's own
	 * ("PHP Fatal error:  ..."), not ours. One zlog() call, so a multi-line
	 * message (a stack trace) stays one entry.
	 *
	 * TRAP: zlog() formats into a 2048-byte buffer (MAX_BUF_LENGTH, zlog.c)
	 * and the master re-emits under log_limit (default 1024), so a very deep
	 * stack trace is cut twice. Upstream's sapi_cgi_log_message() avoids the
	 * first cut by using zlog_msg(), which streams — at the price of the
	 * message arriving as SEVERAL relayed records, hence several error_log
	 * entries. One entry per error is what issue #124 asked for, so the cap is
	 * accepted deliberately; raise log_limit and revisit this if a truncated
	 * trace ever costs a real diagnosis. */
	zlog(level, "[pool %s] PHP message: %s", pool, message);
}
/* }}} */

/* ZEND_INI_USER, not ZEND_INI_SYSTEM: fpm_php_zend_ini_alter_master() only
 * narrows an entry's modifiable mask for ZEND_INI_SYSTEM, and narrowing it here
 * would stop the script itself from calling ini_set('display_errors', ...) —
 * which is not what a default is for. */
static void fpm_child_php_log_ini_default(struct fpm_worker_pool_s *wp,
	const char *name, const char *value) /* {{{ */
{
	struct key_value_s kv;

	kv.next = NULL;
	kv.key = (char *) name;
	kv.value = (char *) value;

	if (0 > fpm_php_apply_defines_ex(&kv, ZEND_INI_USER)) {
		zlog(ZLOG_ERROR, "[pool %s] unable to set the default '%s = %s' for this "
			"pool type; PHP errors may not reach error_log",
			wp->config->name, name, value);
	}
}
/* }}} */

void fpm_child_php_log_init_child(struct fpm_worker_pool_s *wp) /* {{{ */
{
	/* Issue #260 split this from child_logs_via_master: a type can want the
	 * log channel for its own lifecycle lines and still have a response to put
	 * PHP's errors in, which is exactly http-direct. See fpm_pool_type.h. */
	if (!fpm_pool_type_of(wp)->child_php_log_via_master) {
		return;
	}

	fpm_child_php_log_wp = wp;

	/* Order matters only against fpm_php_init_child(), which runs after this
	 * and applies the pool's php_value/php_admin_value on top — see the header. */
	fpm_child_php_log_ini_default(wp, "log_errors", "1");
	fpm_child_php_log_ini_default(wp, "display_errors", "0");
	fpm_child_php_log_ini_default(wp, "html_errors", "0");

	sapi_module.log_message = fpm_child_php_log_message;
}
/* }}} */

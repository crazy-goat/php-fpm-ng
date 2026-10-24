/*
 * Weak stand-ins for the fpm_* and fpmng_* functions that the fuzz harnesses
 * in this directory do not link for real (issue #995).
 *
 * Each harness #includes the one production .c file it fuzzes, so that file's
 * static functions are visible to the harness and its non-static functions
 * are compiled in. Every other function that file calls comes from here. A
 * harness that needs a capture or a counter defines a strong function of the
 * same name, and the linker keeps the strong one over the weak one.
 *
 * The stubs are untyped on purpose: they ignore their arguments and return 0,
 * which reads as NULL, false or success to the caller. A stub that fuzz input
 * can reach must do nothing harmful. Triage treats a crash whose top frame is
 * a stub here as a harness problem, not as a parser bug.
 */
#include <stdarg.h>
#include <stdio.h>

#define FUZZ_STUB(name)              \
	__attribute__((weak)) int name() \
	{                                \
		return 0;                    \
	}

/* fpm_http_fcgi.c, fpm_http_client.c and fpm_http_upstream_timeout.c */
FUZZ_STUB(fpm_http_finish)
FUZZ_STUB(fpm_http_finish_truncated)
FUZZ_STUB(fpm_http_start_reply)
FUZZ_STUB(fpm_http_stdout)
FUZZ_STUB(fpm_http_upstream_deadline_arm)
FUZZ_STUB(fpm_http_upstream_deadline_free)
FUZZ_STUB(fpm_http_upstream_deadline_new)
FUZZ_STUB(fpm_http_upstream_deadline_stop)
FUZZ_STUB(fpm_http_read_deadline_disarm)
FUZZ_STUB(fpm_http_pump)
FUZZ_STUB(fpm_http_request_done)
FUZZ_STUB(fpm_http_upstream_drop)
FUZZ_STUB(fpm_http_upstream_fail)
FUZZ_STUB(fpm_http_upstream_free)
FUZZ_STUB(fpm_http_conn_free)
FUZZ_STUB(fpm_http_response_resume)
FUZZ_STUB(fpm_http_transport_connect)
FUZZ_STUB(fpm_http_access_log_write)
FUZZ_STUB(fpm_http_method_name)
FUZZ_STUB(fpm_http_build_request)

/* fpm_http.c and fpm_http_static.c and the rest of the gateway */
FUZZ_STUB(fpm_http_absolute_authority)
FUZZ_STUB(fpm_http_acl_check)
FUZZ_STUB(fpm_http_acl_free)
FUZZ_STUB(fpm_http_acl_parse)
FUZZ_STUB(fpm_http_direct_conns_request)
FUZZ_STUB(fpm_http_direct_header_dropped)
FUZZ_STUB(fpm_http_direct_header_name_ok)
FUZZ_STUB(fpm_http_direct_tls_shutdown_step)
FUZZ_STUB(fpm_http_direct_tls_validate)
FUZZ_STUB(fpm_http_forwarded_resolve)
FUZZ_STUB(fpm_http_normalize_target)
FUZZ_STUB(fpm_http_operator_request)
FUZZ_STUB(fpm_http_origin_form)
FUZZ_STUB(fpm_http_raw_path)
FUZZ_STUB(fpm_http_request_id_generate)
FUZZ_STUB(fpm_http_request_id_valid)
FUZZ_STUB(fpm_http_request_path)
FUZZ_STUB(fpm_http_static_serve)
FUZZ_STUB(fpm_http_static_decode_path)
FUZZ_STUB(fpm_http_header_cgi_key)
FUZZ_STUB(fpm_http_log_escape)
FUZZ_STUB(fpm_clock_get)
FUZZ_STUB(fpm_pack_http_script_name)
FUZZ_STUB(fpm_pack_is_app_path)
FUZZ_STUB(fpm_pack_is_unresolved_path)
FUZZ_STUB(fpm_pack_validate_path)
FUZZ_STUB(fpm_worker_account_answered_request)
FUZZ_STUB(fpm_worker_pending_get)
FUZZ_STUB(fpm_worker_reap)
FUZZ_STUB(fpm_worker_send_error)
FUZZ_STUB(fpm_worker_send_reply)
FUZZ_STUB(fpm_worker_stopping)

/* Zlog keeps its format string live, so the STDERR branch of the FastCGI
 * record parser hits the same %.*s read that production does. ASan checks
 * that read against the exact input buffer. Nothing is written anywhere. */
__attribute__((weak)) void fpmng_zlog_ex(const char *function, int line, int flags, const char *fmt, ...)
{
	char buf[1024];
	va_list ap;

	(void) function;
	(void) line;
	(void) flags;
	va_start(ap, fmt);
	(void) vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
}

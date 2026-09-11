/* fpm-ng: see fpm_http_direct_access_log.h. */

#include "fpm_config.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include <sys/time.h>

#include <event2/http.h>
#include <event2/keyvalq_struct.h>

#include "fpm.h"
#include "fpm_conf.h"
#include "fpm_worker_pool.h"
#include "fpm_http_direct_access_log.h"
#include "zlog.h"

/* One line is built whole and written with one write(), which is what makes it
 * atomic against the pool's other children. The cap is the reason the buffer
 * is a fixed size: request_uri and query_string are already bounded by the
 * scoreboard's 512/2048 (fpm_scoreboard.h), a header value by
 * FPM_HTTP_HEADERS_MAX for the whole block, so 8 KiB leaves room for a format
 * that prints several of them. A line that still does not fit is truncated
 * rather than split: half a line in the middle of the file is worse than a
 * short one, and the alternative -- a heap allocation per request -- buys
 * nothing for a case no real format reaches. */
#define FPM_HTTP_DIRECT_ACCESS_LINE_MAX 8192

struct fpm_http_direct_access_log_s {
	int fd;
	char *format;
	/* access.suppress_path values, copied so the child does not depend on the
	 * config list surviving. */
	char **suppress;
	unsigned suppress_count;
	char pool[32];
};

struct fpm_http_direct_access_buf {
	char data[FPM_HTTP_DIRECT_ACCESS_LINE_MAX];
	size_t len;
	int truncated;
};

static void fpm_http_direct_access_puts(struct fpm_http_direct_access_buf *b, const char *s)
{
	size_t len;

	if (!s) {
		s = "-";
	}
	len = strlen(s);
	if (b->len + len >= sizeof(b->data)) {
		len = sizeof(b->data) - 1 - b->len;
		b->truncated = 1;
	}
	memcpy(b->data + b->len, s, len);
	b->len += len;
}

static void fpm_http_direct_access_putc(struct fpm_http_direct_access_buf *b, char c)
{
	if (b->len + 1 >= sizeof(b->data)) {
		b->truncated = 1;
		return;
	}
	b->data[b->len++] = c;
}

static void fpm_http_direct_access_printf(struct fpm_http_direct_access_buf *b, const char *fmt, ...)
{
	va_list ap;
	int n;

	if (b->len >= sizeof(b->data) - 1) {
		b->truncated = 1;
		return;
	}
	va_start(ap, fmt);
	n = vsnprintf(b->data + b->len, sizeof(b->data) - b->len, fmt, ap);
	va_end(ap);
	if (n < 0) {
		return;
	}
	if ((size_t) n >= sizeof(b->data) - b->len) {
		b->len = sizeof(b->data) - 1;
		b->truncated = 1;
	} else {
		b->len += (size_t) n;
	}
}

/* "-" for an empty value, the same substitution upstream fpm_log.c makes, so a
 * field that is absent is still one token and a log parser does not have to
 * special-case this transport. */
static const char *fpm_http_direct_access_or_dash(const char *value)
{
	return value && *value ? value : "-";
}

static void fpm_http_direct_access_time(struct fpm_http_direct_access_buf *b, time_t when, const char *fmt)
{
	char tmp[129];
	struct tm tm;

	if (!localtime_r(&when, &tm)) {
		fpm_http_direct_access_putc(b, '-');
		return;
	}
	if (!strftime(tmp, sizeof(tmp) - 1, fmt && *fmt ? fmt : "%d/%b/%Y:%H:%M:%S %z", &tm)) {
		fpm_http_direct_access_putc(b, '-');
		return;
	}
	fpm_http_direct_access_puts(b, tmp);
}

/* The value of a response header, for %o{...}. Read from the request the
 * caller is about to answer (or is answering), which is why the header
 * documents that logging happens while it is still alive. */
static const char *fpm_http_direct_access_out_header(struct evhttp_request *http, const char *name)
{
	if (!http) {
		return NULL;
	}
	return evhttp_find_header(evhttp_request_get_output_headers(http), name);
}

static const char *fpm_http_direct_access_env(const struct evkeyvalq *env, const char *name)
{
	const struct evkeyval *kv;

	if (!env) {
		return NULL;
	}
	for (kv = env->tqh_first; kv; kv = kv->next.tqe_next) {
		if (!strcmp(kv->key, name)) {
			return kv->value;
		}
	}
	return NULL;
}

static void fpm_http_direct_access_render(struct fpm_http_direct_access_log_s *log,
	const struct fpm_http_direct_access_entry *e, struct fpm_http_direct_access_buf *b)
{
	const char *s = log->format;
	char modifier[129];

	while (*s) {
		if (*s != '%') {
			fpm_http_direct_access_putc(b, *s++);
			continue;
		}
		s++;
		modifier[0] = '\0';
		/* %x{modifier}: fpm_conf.c has already accepted the format through
		 * upstream's parser, so an unterminated brace cannot reach here. */
		if (*s == '{') {
			const char *end = strchr(s + 1, '}');
			size_t len = end ? (size_t) (end - s - 1) : 0;

			if (!end || len >= sizeof(modifier)) {
				fpm_http_direct_access_putc(b, '%');
				continue;
			}
			memcpy(modifier, s + 1, len);
			modifier[len] = '\0';
			s = end + 1;
		}
		if (!*s) {
			fpm_http_direct_access_putc(b, '%');
			break;
		}
		switch (*s) {
			case '%':
				fpm_http_direct_access_putc(b, '%');
				break;
			/* The caller has already turned the scoreboard's tms deltas into a
			 * percentage, because only it knows whether a PHP request ran at
			 * all: a static file, a ping or a refusal never touches the proc
			 * slot, and reading the slot here would report the CPU of whatever
			 * PHP request this child served last. */
			case 'C':
				fpm_http_direct_access_printf(b, "%.2f", e->cpu_percent);
				break;
			case 'd': {
				double elapsed;

				if (e->duration.tv_sec || e->duration.tv_usec) {
					elapsed = (double) e->duration.tv_sec + (double) e->duration.tv_usec / 1000000.;
				} else {
					struct timeval now;

					gettimeofday(&now, NULL);
					elapsed = (double) (now.tv_sec - e->started.tv_sec)
						+ (double) (now.tv_usec - e->started.tv_usec) / 1000000.;
				}
				if (elapsed < 0) {
					elapsed = 0;
				}
				if (!modifier[0] || !strcasecmp(modifier, "seconds")) {
					fpm_http_direct_access_printf(b, "%.3f", elapsed);
				} else if (!strcasecmp(modifier, "microseconds") || !strcasecmp(modifier, "micro")) {
					fpm_http_direct_access_printf(b, "%lu", (unsigned long) (elapsed * 1000000.));
				} else {
					/* milli/mili, both spellings, as upstream accepts them */
					fpm_http_direct_access_printf(b, "%.3f", elapsed * 1000.);
				}
				break;
			}
			case 'e':
				fpm_http_direct_access_puts(b,
					fpm_http_direct_access_or_dash(fpm_http_direct_access_env(e->env, modifier)));
				break;
			case 'f':
				fpm_http_direct_access_puts(b, fpm_http_direct_access_or_dash(e->script_filename));
				break;
			case 'l':
				fpm_http_direct_access_printf(b, "%zu", e->content_length);
				break;
			case 'm':
				fpm_http_direct_access_puts(b, fpm_http_direct_access_or_dash(e->method));
				break;
			case 'M':
				if (!modifier[0] || !strcasecmp(modifier, "bytes")) {
					fpm_http_direct_access_printf(b, "%zu", e->memory);
				} else if (!strcasecmp(modifier, "kilobytes") || !strcasecmp(modifier, "kilo")) {
					fpm_http_direct_access_printf(b, "%zu", e->memory / 1024);
				} else {
					fpm_http_direct_access_printf(b, "%zu", e->memory / 1024 / 1024);
				}
				break;
			case 'n':
				fpm_http_direct_access_puts(b, log->pool);
				break;
			case 'o':
				fpm_http_direct_access_puts(b, fpm_http_direct_access_or_dash(
					fpm_http_direct_access_out_header(e->http, modifier)));
				break;
			case 'p':
				fpm_http_direct_access_printf(b, "%ld", (long) getpid());
				break;
			case 'P':
				fpm_http_direct_access_printf(b, "%ld", (long) getppid());
				break;
			case 'q':
				fpm_http_direct_access_puts(b, e->query_string ? e->query_string : "");
				break;
			case 'Q':
				fpm_http_direct_access_puts(b, e->query_string && *e->query_string ? "?" : "");
				break;
			case 'r':
				fpm_http_direct_access_puts(b, e->uri ? e->uri : "");
				break;
			case 'R':
				fpm_http_direct_access_puts(b, fpm_http_direct_access_or_dash(e->remote_addr));
				break;
			case 's':
				fpm_http_direct_access_printf(b, "%d", e->status);
				break;
			case 't':
				fpm_http_direct_access_time(b, e->started_epoch, modifier);
				break;
			case 'T':
				fpm_http_direct_access_time(b, time(NULL), modifier);
				break;
			case 'u':
				fpm_http_direct_access_puts(b, fpm_http_direct_access_or_dash(e->remote_user));
				break;
			default:
				/* Unreachable: fpm_conf.c refused to start with anything
				 * upstream's parser does not know. Printed verbatim rather
				 * than dropped, so a divergence between the two shows up in
				 * the log instead of disappearing. */
				fpm_http_direct_access_putc(b, '%');
				fpm_http_direct_access_putc(b, *s);
				break;
		}
		s++;
	}
}

struct fpm_http_direct_access_log_s *fpm_http_direct_access_log_init_child(struct fpm_worker_pool_s *wp)
{
	struct fpm_http_direct_access_log_s *log;
	struct key_value_s *kv;
	unsigned n = 0;

	if (!wp->config->access_log || !*wp->config->access_log) {
		return NULL;
	}
	if (wp->log_fd < 0) {
		/* fpm_log_open() failed for this pool and FPM refused to start, so
		 * reaching here means the descriptor was closed under us. Say so once
		 * rather than writing to whatever fd 0 happens to be. */
		zlog(ZLOG_ERROR, "[pool %s] access.log: no descriptor from the master, logging disabled",
			wp->config->name);
		return NULL;
	}
	log = calloc(1, sizeof(*log));
	if (!log) {
		return NULL;
	}
	log->fd = wp->log_fd;
	log->format = strdup(wp->config->access_format ? wp->config->access_format : "%R - %u %t \"%m %r\" %s");
	if (!log->format) {
		free(log);
		return NULL;
	}
	snprintf(log->pool, sizeof(log->pool), "%s", wp->config->name);

	for (kv = wp->config->access_suppress_paths; kv; kv = kv->next) {
		n++;
	}
	if (n) {
		log->suppress = calloc(n, sizeof(*log->suppress));
		if (!log->suppress) {
			free(log->format);
			free(log);
			return NULL;
		}
		for (kv = wp->config->access_suppress_paths; kv; kv = kv->next) {
			log->suppress[log->suppress_count] = strdup(kv->value);
			if (!log->suppress[log->suppress_count]) {
				break;
			}
			log->suppress_count++;
		}
	}

	return log;
}

void fpm_http_direct_access_log_free(struct fpm_http_direct_access_log_s *log)
{
	unsigned i;

	if (!log) {
		return;
	}
	for (i = 0; i < log->suppress_count; i++) {
		free(log->suppress[i]);
	}
	free(log->suppress);
	free(log->format);
	free(log);
}

void fpm_http_direct_access_log_write(struct fpm_http_direct_access_log_s *log,
	const struct fpm_http_direct_access_entry *entry)
{
	struct fpm_http_direct_access_buf b;
	unsigned i;
	ssize_t written;

	if (!log) {
		return;
	}
	for (i = 0; i < log->suppress_count; i++) {
		if (entry->uri && !strcmp(entry->uri, log->suppress[i])) {
			return;
		}
	}

	b.len = 0;
	b.truncated = 0;
	fpm_http_direct_access_render(log, entry, &b);
	fpm_http_direct_access_putc(&b, '\n');
	/* The newline is the one byte that must survive truncation: without it two
	 * truncated lines merge into one and every line after them is misparsed. */
	if (b.truncated) {
		b.data[sizeof(b.data) - 2] = '\n';
		b.len = sizeof(b.data) - 1;
	}

	do {
		written = write(log->fd, b.data, b.len);
	} while (written < 0 && errno == EINTR);
	if (written < 0) {
		zlog(ZLOG_SYSERROR, "[pool %s] access.log: write failed", log->pool);
	}
}

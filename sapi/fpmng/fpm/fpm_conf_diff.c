/* fpm-ng: issue #330 -- see fpm_conf_diff.h for the design rationale (raw
 * per-[section] text comparison instead of a field-by-field struct diff). */

#include "fpm_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "php_glob.h"

#include "fpm_conf_diff.h"
#include "zlog.h"

struct fpm_conf_diff_section_s {
	char *name;			/* NULL for the preamble before the first [section] -- kept
					 * out of the list entirely, see append_line() below */
	char *body;
	size_t body_len, body_cap;
	struct fpm_conf_diff_section_s *next;
};

static struct fpm_conf_diff_section_s *current_snapshot = NULL;
static struct fpm_conf_diff_section_s *pass_snapshot = NULL;
static int pass_valid = 0;

/* Mirrors fpm_conf.c's own ini_recursion > 4 cap on include= depth (issue
 * #330 comment there too) -- not sharing the static counter itself, since
 * this reader never runs interleaved with the real parser, but bounded the
 * same way for the same reason: a config that somehow includes itself must
 * not hang or blow the stack here either. */
#define FPM_CONF_DIFF_MAX_INCLUDE_DEPTH 5

static void fpm_conf_diff_sections_free(struct fpm_conf_diff_section_s *head) /* {{{ */
{
	while (head) {
		struct fpm_conf_diff_section_s *next = head->next;

		free(head->name);
		free(head->body);
		free(head);
		head = next;
	}
}
/* }}} */

static struct fpm_conf_diff_section_s *fpm_conf_diff_section_find(
		struct fpm_conf_diff_section_s *head, const char *name) /* {{{ */
{
	for (; head; head = head->next) {
		if (head->name && strcmp(head->name, name) == 0) {
			return head;
		}
	}
	return NULL;
}
/* }}} */

static int fpm_conf_diff_body_append(struct fpm_conf_diff_section_s *section, const char *line, size_t len) /* {{{ */
{
	if (section->body_len + len + 2 > section->body_cap) {
		size_t new_cap = (section->body_cap ? section->body_cap * 2 : 256);
		char *next;

		while (new_cap < section->body_len + len + 2) {
			new_cap *= 2;
		}
		next = realloc(section->body, new_cap);
		if (!next) {
			return -1;
		}
		section->body = next;
		section->body_cap = new_cap;
	}
	memcpy(section->body + section->body_len, line, len);
	section->body_len += len;
	section->body[section->body_len++] = '\n';
	section->body[section->body_len] = '\0';
	return 0;
}
/* }}} */

struct fpm_conf_diff_scan_state {
	struct fpm_conf_diff_section_s **head;
	struct fpm_conf_diff_section_s *cur;	/* NULL while still in the preamble */
};

static int fpm_conf_diff_scan_file(const char *path, struct fpm_conf_diff_scan_state *st, int depth); /* forward */

/* One physical line, already read and NUL-terminated (no trailing '\n').
 * Recognizes a `[name]` section header and switches `st->cur`; recognizes an
 * `include=...` directive and recurses into fpm_conf_diff_scan_file() for
 * whatever it globs to (inheriting the CURRENT section, exactly like
 * fpm_conf.c's own ini parser keeps parsing into whichever section was open
 * when it hit the include -- see fpm_conf_ini_parser_include()); every other
 * line is appended verbatim to the currently open section's body, or dropped
 * if there is no open section yet (global-scope preamble lines are not a
 * pool, and [global] itself is diffed the same way as any other section name
 * if a config happens to write one -- either way, not this issue's concern:
 * reload.selective only ever asks about POOL sections). Returns -1 only for
 * an include this reader could not resolve (propagates as a snapshot
 * failure -- see the header's safety-bias note). */
static int fpm_conf_diff_scan_line(char *line, struct fpm_conf_diff_scan_state *st, int depth) /* {{{ */
{
	char *trimmed = line;

	while (*trimmed == ' ' || *trimmed == '\t' || *trimmed == '\r') {
		trimmed++;
	}

	if (*trimmed == '[') {
		char *close = strchr(trimmed, ']');
		char *name;
		struct fpm_conf_diff_section_s *section;

		if (!close) {
			return 0; /* malformed line -- fpm_conf.c's real parser will reject the
			           * whole file later; nothing useful to diff here either way */
		}
		name = strndup(trimmed + 1, (size_t) (close - trimmed - 1));
		if (!name) {
			return -1;
		}
		section = fpm_conf_diff_section_find(*st->head, name);
		if (section) {
			free(name);
		} else {
			section = calloc(1, sizeof(*section));
			if (!section) {
				free(name);
				return -1;
			}
			section->name = name;
			section->next = *st->head;
			*st->head = section;
		}
		st->cur = section;
		return 0;
	}

	if (strncmp(trimmed, "include", 7) == 0) {
		char *rest = trimmed + 7;

		while (*rest == ' ' || *rest == '\t') {
			rest++;
		}
		if (*rest == '=') {
			char *pattern, *end;
			php_glob_t g;
			size_t i;
			int ret = 0;

			rest++;
			while (*rest == ' ' || *rest == '\t') {
				rest++;
			}
			end = rest + strlen(rest);
			while (end > rest && (end[-1] == ' ' || end[-1] == '\t')) {
				end--;
			}
			*end = '\0';

			if (depth >= FPM_CONF_DIFF_MAX_INCLUDE_DEPTH) {
				zlog(ZLOG_WARNING, "issue #330: include= nesting too deep while diffing "
					"config for selective reload, treating every pool as changed this time");
				return -1;
			}

			pattern = rest;
			g.gl_offs = 0;
			if (php_glob(pattern, PHP_GLOB_ERR | PHP_GLOB_MARK, NULL, &g) != 0) {
				/* Same as fpm_conf.c's own PHP_GLOB_NOMATCH handling: an
				 * include with nothing to expand to is not an error there,
				 * so it is not one here either -- just nothing more to scan. */
				return 0;
			}
			for (i = 0; i < g.gl_pathc && ret == 0; i++) {
				size_t len = strlen(g.gl_pathv[i]);

				if (len < 1 || g.gl_pathv[i][len - 1] == '/') {
					continue;
				}
				ret = fpm_conf_diff_scan_file(g.gl_pathv[i], st, depth + 1);
			}
			php_globfree(&g);
			return ret;
		}
	}

	if (st->cur) {
		return fpm_conf_diff_body_append(st->cur, trimmed, strlen(trimmed));
	}
	return 0;
}
/* }}} */

static int fpm_conf_diff_scan_file(const char *path, struct fpm_conf_diff_scan_state *st, int depth) /* {{{ */
{
	FILE *fp = fopen(path, "r");
	char *line = NULL;
	size_t cap = 0;
	ssize_t n;
	int ret = 0;

	if (!fp) {
		return -1;
	}

	while (ret == 0 && (n = getline(&line, &cap, fp)) != -1) {
		if (n > 0 && line[n - 1] == '\n') {
			line[n - 1] = '\0';
		}
		ret = fpm_conf_diff_scan_line(line, st, depth);
	}

	free(line);
	fclose(fp);
	return ret;
}
/* }}} */

/* Shared by both public entry points below: reads `config_file` (plus
 * include=) into a fresh section list, or returns NULL on any failure --
 * see the header's safety-bias note; the caller decides what "no snapshot"
 * means for it (fpm_conf_diff_snapshot_current(): next reload cannot prove
 * anything unchanged; fpm_conf_diff_begin_reload_pass(): this reload cannot
 * prove anything unchanged). */
static struct fpm_conf_diff_section_s *fpm_conf_diff_read(const char *config_file) /* {{{ */
{
	struct fpm_conf_diff_scan_state st = { 0 };
	struct fpm_conf_diff_section_s *head = NULL;

	if (!config_file) {
		return NULL;
	}

	st.head = &head;
	if (0 > fpm_conf_diff_scan_file(config_file, &st, 0)) {
		fpm_conf_diff_sections_free(head);
		return NULL;
	}
	return head;
}
/* }}} */

void fpm_conf_diff_snapshot_current(const char *config_file) /* {{{ */
{
	struct fpm_conf_diff_section_s *snap = fpm_conf_diff_read(config_file);

	fpm_conf_diff_sections_free(current_snapshot);
	current_snapshot = snap; /* NULL is a valid "no snapshot" state -- fine */

	if (!snap) {
		zlog(ZLOG_DEBUG, "issue #330: could not snapshot '%s' for selective reload; "
			"the next reload will restart every pool regardless of reload.selective",
			config_file ? config_file : "(null)");
	}
}
/* }}} */

int fpm_conf_diff_begin_reload_pass(const char *config_file) /* {{{ */
{
	fpm_conf_diff_sections_free(pass_snapshot);
	pass_snapshot = fpm_conf_diff_read(config_file);
	pass_valid = (pass_snapshot != NULL) && (current_snapshot != NULL);

	if (!pass_valid) {
		zlog(ZLOG_NOTICE, "issue #330: reload.selective could not diff '%s'; "
			"reloading every pool this time (same as reload.selective = no)",
			config_file ? config_file : "(null)");
	}
	return pass_valid;
}
/* }}} */

int fpm_conf_diff_pool_unchanged(const char *pool_name) /* {{{ */
{
	struct fpm_conf_diff_section_s *old_section, *new_section;

	if (!pass_valid || !pool_name) {
		return 0;
	}

	old_section = fpm_conf_diff_section_find(current_snapshot, pool_name);
	new_section = fpm_conf_diff_section_find(pass_snapshot, pool_name);

	if (!old_section || !new_section) {
		return 0; /* new or removed pool -- never "unchanged" */
	}

	if (old_section->body_len != new_section->body_len) {
		return 0;
	}
	return memcmp(old_section->body, new_section->body, old_section->body_len) == 0;
}
/* }}} */

void fpm_conf_diff_shutdown(void) /* {{{ */
{
	fpm_conf_diff_sections_free(current_snapshot);
	fpm_conf_diff_sections_free(pass_snapshot);
	current_snapshot = NULL;
	pass_snapshot = NULL;
	pass_valid = 0;
}
/* }}} */

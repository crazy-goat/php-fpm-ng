/* fpm-ng: .user.ini activation for HTTP-direct pools (issue #60).
 *
 * ---------------------------------------------------------------- the problem
 *
 * The CGI SAPI derives the per-directory ini set from PATH_TRANSLATED: it
 * takes dirname(PATH_TRANSLATED) and walks it from DOCUMENT_ROOT down,
 * reading PG(user_ini_filename) in each directory it passes. In a FastCGI
 * deployment PATH_TRANSLATED comes from the web server, which derived it from
 * the request URI -- so the ini set is chosen, indirectly, by the client.
 * That is tolerable there because every one of those directories is also a
 * directory whose scripts the server was willing to execute.
 *
 * A direct pool has no such mapping. It executes exactly one script, the
 * front controller, for every request; the request URI is data the script
 * routes on, never a path this SAPI opens. Reusing the CGI derivation here
 * would read ini files out of directories chosen by the client and apply them
 * to a script that lives somewhere else entirely. That is why task 054 left
 * the hook unwired and docs/http-direct.md said so.
 *
 * ------------------------------------------------------------- what this does
 *
 * The directory that governs is the front controller's, and nothing else.
 * Both ends of the walk are pool configuration resolved once per child by
 * fpm_http_direct_resolve_script():
 *
 *   root   = realpath(chdir)                 -- the DOCUMENT_ROOT the pool reports
 *   script = root + http.front_controller    -- validated to live under root
 *
 * so the scan is root -> dirname(script), which is the same *shape* as the CGI
 * walk with the client removed from it. A request cannot move it, lengthen it
 * or shorten it: the candidate list is computed once, at child start, from two
 * strings the client never touches. A traversal-style URI, a URI naming a
 * directory with its own .user.ini, and a URI naming no path at all all
 * produce the same list, because none of them is an input here.
 *
 * ---------------------------------------------------------------- the default
 *
 * On by default, governed by the php.ini settings that govern it everywhere
 * else: user_ini.filename (empty disables it, as in every other SAPI) and
 * user_ini.cache_ttl. Issue #60 asked for this to be decided and recorded, and
 * the reason for default-on is that the surprising pool is the other one: an
 * operator who moves a pool from FastCGI to http-direct and keeps the same
 * php.ini has every reason to expect the same .user.ini to still apply. A
 * pool-level opt-in would mean a direct pool silently ignoring a file every
 * other pool type honours, which is the failure mode that is hard to notice.
 *
 * Turning it on does not widen what a request can reach: the directories
 * scanned are pool configuration either way.
 *
 * ---------------------------------------------------------------- the caching
 *
 * The CGI cache is keyed by path with a TTL, and on expiry it re-reads
 * unconditionally. Here the key is redundant -- one child serves one path --
 * so what is left is the TTL, plus a stat() of every candidate before a
 * re-parse. An unchanged deployment therefore costs user_ini.cache_ttl-spaced
 * stat() calls and never a re-parse, and a changed .user.ini is picked up on
 * the first request after the TTL expires, exactly as under CGI. Changing the
 * file and waiting out the TTL is how issue #60's mtime criterion is met;
 * cache_ttl = 0 makes every request re-stat, which is what the .phpt uses.
 *
 * What is deliberately NOT done here: php_ini_activate_per_host_config(). It
 * is keyed on SERVER_NAME, which in a direct pool is the client's Host header
 * -- precisely the client-chosen input this file exists to keep out of the ini
 * set. Issue #60 scopes this to .user.ini; per-host configuration for direct
 * pools would need its own decision about where the name may come from.
 */

#include "fpm_config.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "php.h"
#include "php_ini.h"
#include "SAPI.h"
#include "zend_hash.h"

#include "fpm_http_direct_user_ini.h"
#include "zlog.h"

/* One entry per directory between root and dirname(script), inclusive. A
 * front controller is not nested a hundred levels below the document root,
 * and a fixed cap keeps the per-child state one allocation-free struct; a
 * deeper tree is served with the levels that fit and a loud log line, never
 * silently. */
#define FPM_DIRECT_USER_INI_MAX_DEPTH 32

struct fpm_direct_user_ini_file {
	char dir[PATH_MAX];		/* directory to hand to php_parse_user_ini_file() */
	/* Identity of the ini file last seen in that directory. st_ino and st_dev
	 * are in here because a deploy that swaps a symlinked release directory
	 * changes neither mtime nor size of the file the old path resolved to. */
	int present;
	time_t mtime;
	off_t size;
	ino_t ino;
	dev_t dev;
};

static struct {
	int enabled;			/* 0 after a failure, or when the feature is off */
	int initialised;
	const char *pool;		/* pool name, for logs; owned by the config */
	char path[PATH_MAX];		/* dirname(script) with a trailing slash, for php_ini_activate_per_dir_config() */
	size_t path_len;
	struct fpm_direct_user_ini_file file[FPM_DIRECT_USER_INI_MAX_DEPTH];
	unsigned files;
	HashTable config;
	int config_ready;
	time_t expires;
	int scanned_once;
} fpm_direct_user_ini;

/* Records the current identity of <dir>/<user_ini_filename> and reports
 * whether it differs from what the cache was built on. */
static int fpm_direct_user_ini_file_changed(struct fpm_direct_user_ini_file *f)
{
	char candidate[PATH_MAX];
	struct stat st;
	int present;

	if ((size_t) snprintf(candidate, sizeof(candidate), "%s/%s", f->dir, PG(user_ini_filename))
		>= sizeof(candidate)) {
		/* Too long to name is the same as not there: it cannot be the file
		 * php_parse_user_ini_file() will open either. */
		present = 0;
		memset(&st, 0, sizeof(st));
	} else {
		present = stat(candidate, &st) == 0 && S_ISREG(st.st_mode);
	}
	if (present == f->present && (!present ||
		(st.st_mtime == f->mtime && st.st_size == f->size &&
		 st.st_ino == f->ino && st.st_dev == f->dev))) {
		return 0;
	}
	f->present = present;
	f->mtime = present ? st.st_mtime : 0;
	f->size = present ? st.st_size : 0;
	f->ino = present ? st.st_ino : 0;
	f->dev = present ? st.st_dev : 0;
	return 1;
}

/* Builds the ordered list of directories to scan, root first. Returns -1 only
 * when the two paths do not stand in the relation the caller promised, which
 * would mean fpm_http_direct_resolve_script() changed under this file. */
static int fpm_direct_user_ini_plan(const char *root, const char *script)
{
	size_t root_len = strlen(root);
	const char *p;
	char dir[PATH_MAX];
	size_t dir_len;

	/* dirname(script), which is at least root. */
	p = strrchr(script, '/');
	if (!p || (size_t) (p - script) < root_len) {
		return -1;
	}
	dir_len = (size_t) (p - script);
	if (dir_len == 0) {			/* the script is directly under "/" */
		dir_len = 1;
	}
	if (dir_len >= sizeof(dir)) {
		return -1;
	}
	memcpy(dir, script, dir_len);
	dir[dir_len] = '\0';

	/* php_ini_activate_per_dir_config() matches [PATH=...] sections against a
	 * path with a trailing slash, the way the CGI SAPI hands it one. */
	if (dir_len + 2 > sizeof(fpm_direct_user_ini.path)) {
		return -1;
	}
	memcpy(fpm_direct_user_ini.path, dir, dir_len);
	if (fpm_direct_user_ini.path[dir_len - 1] != '/') {
		fpm_direct_user_ini.path[dir_len++] = '/';
	}
	fpm_direct_user_ini.path[dir_len] = '\0';
	fpm_direct_user_ini.path_len = dir_len;

	/* root first, then each directory below it, down to and including dir.
	 * The separator walk is the CGI one; what differs is that it runs once
	 * instead of per request, over a path no request can influence.
	 *
	 * `p` is the separator that ends the prefix being added, or NULL once the
	 * whole of `dir` has been added. root_len > 1 starts the walk at root;
	 * root == "/" starts it at 0 so that "/" itself is the first entry. */
	fpm_direct_user_ini.files = 0;
	p = dir + (root_len > 1 ? root_len : 0);
	while (1) {
		struct fpm_direct_user_ini_file *f;
		size_t len = p ? (size_t) (p - dir) : dir_len;

		if (len == 0) {
			len = 1;		/* "/" itself */
		}
		if (fpm_direct_user_ini.files >= FPM_DIRECT_USER_INI_MAX_DEPTH) {
			zlog(ZLOG_WARNING, "[pool %s] http-direct: the front controller is more than %d directories "
				"below the document root; %s files above that depth are not read",
				fpm_direct_user_ini.pool, FPM_DIRECT_USER_INI_MAX_DEPTH, PG(user_ini_filename));
			break;
		}
		f = &fpm_direct_user_ini.file[fpm_direct_user_ini.files++];
		memset(f, 0, sizeof(*f));
		memcpy(f->dir, dir, len);
		f->dir[len] = '\0';
		/* Done when the prefix just added is the whole path -- which is the
		 * case both when there is no further separator and when the walk
		 * started at the end of the string, i.e. root and dir are the same
		 * directory. Advancing past that NUL is what a `p + 1` without this
		 * test would do. */
		if (!p || len == dir_len) {
			break;
		}
		p = strchr(p + 1, '/');
	}
	return 0;
}

int fpm_http_direct_user_ini_init_child(const char *pool, const char *root, const char *script)
{
	memset(&fpm_direct_user_ini, 0, sizeof(fpm_direct_user_ini));
	fpm_direct_user_ini.pool = pool;
	fpm_direct_user_ini.initialised = 1;

	/* The empty filename is how php.ini turns .user.ini off, here as
	 * everywhere else. php_ini_has_per_dir_config() is checked per request
	 * rather than here: it is a startup property, but reading it at startup
	 * would make this file depend on module init order for no gain. */
	if (!PG(user_ini_filename) || !*PG(user_ini_filename)) {
		return 0;
	}
	/* Refused, not worked around. php_parse_user_ini_file() joins the
	 * directory and this name with a slash and opens the result, so a name
	 * carrying a separator turns the walk below -- whose whole purpose is that
	 * every directory it reads is one the pool owns -- into a walk over
	 * directories the name chose. Upstream CGI tolerates it because its walk
	 * is already driven by the request; here it would quietly undo the
	 * containment this file exists to provide, so the pool does not start. */
	if (strchr(PG(user_ini_filename), '/')) {
		zlog(ZLOG_ALERT, "[pool %s] http-direct: user_ini.filename must be a bare file name, not '%s': "
			"a separator in it would let the per-directory ini scan leave the document root",
			pool, PG(user_ini_filename));
		return -1;
	}
	if (fpm_direct_user_ini_plan(root, script) < 0) {
		zlog(ZLOG_WARNING, "[pool %s] http-direct: cannot derive the %s search path from '%s' under '%s'; "
			"this pool serves with php.ini alone", pool, PG(user_ini_filename), script, root);
		return 0;
	}
	fpm_direct_user_ini.enabled = 1;
	return 0;
}

int fpm_http_direct_user_ini_pre_request(void)
{
	int per_dir = php_ini_has_per_dir_config();
	unsigned i;
	int changed;

	if (!fpm_direct_user_ini.enabled) {
		/* [PATH=...] sections from php.ini still apply: they are keyed on the
		 * same client-free path and do not depend on user_ini.filename. */
		if (per_dir && fpm_direct_user_ini.path_len) {
			php_ini_activate_per_dir_config(fpm_direct_user_ini.path, fpm_direct_user_ini.path_len);
		}
		return SUCCESS;
	}
	if (per_dir) {
		php_ini_activate_per_dir_config(fpm_direct_user_ini.path, fpm_direct_user_ini.path_len);
	}
	if (!fpm_direct_user_ini.config_ready) {
		zend_hash_init(&fpm_direct_user_ini.config, 0, NULL, config_zval_dtor, 1);
		fpm_direct_user_ini.config_ready = 1;
	}
	/* sapi_get_request_time() is what the CGI SAPI compares against, so a
	 * pool serving both transports ages its cache the same way. */
	if (!fpm_direct_user_ini.scanned_once || sapi_get_request_time() > fpm_direct_user_ini.expires) {
		changed = !fpm_direct_user_ini.scanned_once;
		/* Every candidate is stat'ed, not just until the first difference:
		 * the loop doubles as the refresh of the recorded identities, and
		 * stopping early would leave the rest stale and re-trigger on the
		 * next TTL. */
		for (i = 0; i < fpm_direct_user_ini.files; i++) {
			if (fpm_direct_user_ini_file_changed(&fpm_direct_user_ini.file[i])) {
				changed = 1;
			}
		}
		if (changed) {
			zend_hash_clean(&fpm_direct_user_ini.config);
			for (i = 0; i < fpm_direct_user_ini.files; i++) {
				if (fpm_direct_user_ini.file[i].present) {
					php_parse_user_ini_file(fpm_direct_user_ini.file[i].dir, PG(user_ini_filename),
						&fpm_direct_user_ini.config);
				}
			}
		}
		fpm_direct_user_ini.scanned_once = 1;
		fpm_direct_user_ini.expires = sapi_get_request_time() + PG(user_ini_cache_ttl);
	}
	/* PHP_INI_STAGE_HTACCESS is the stage that makes these values revert at
	 * request shutdown, which is what keeps one request's .user.ini out of
	 * the next one. */
	php_ini_activate_config(&fpm_direct_user_ini.config, PHP_INI_PERDIR, PHP_INI_STAGE_HTACCESS);
	return SUCCESS;
}

void fpm_http_direct_user_ini_shutdown_child(void)
{
	if (fpm_direct_user_ini.config_ready) {
		zend_hash_destroy(&fpm_direct_user_ini.config);
		fpm_direct_user_ini.config_ready = 0;
	}
	fpm_direct_user_ini.enabled = 0;
	fpm_direct_user_ini.initialised = 0;
}

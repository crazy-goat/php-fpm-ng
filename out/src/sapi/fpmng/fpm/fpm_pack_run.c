/* fpm-ng: running a packed executable (issue #430).
 *
 * `php-fpm-ng pack` (fpm_pack.c, #429) appends fpm.conf, php.ini and app.phar to
 * the binary as one application payload. This file is the other half: when the
 * running binary carries one, main() calls fpm_pack_activate() before the engine
 * starts, and the process then behaves as the application it holds.
 *
 * THE CONTRACT (docs/NOTES.md 3a, decision D1 on #430)
 *
 *  - The embedded php.ini is the only php.ini. The host's php.ini, the scan
 *    directory and PHPRC are not read, so the same executable behaves the same
 *    on every host. This is done with `-c <embedded php.ini>` plus an EMPTY
 *    PHP_INI_SCAN_DIR rather than with `-n`: `-n` also drops the file we want
 *    (PHP gives the ini of -c no chance once php_ini_ignore is set), and
 *    fpm_main.c is vendored and cannot take a hook (#428 measured both).
 *  - The embedded fpm.conf is the only configuration, read through `-y`.
 *  - The PHAR is a container, never run as a program: nothing executes its stub.
 *    A script path in fpm.conf names its own entry with `fpmng-app://<entry>`.
 *  - An argument the operator adds (`-c`, `-y`, `-d`, `-t`) comes AFTER the
 *    injected ones and wins, exactly as it would on a plain binary. That is the
 *    explicit override #427 asks for; nothing is merged silently.
 *
 * WHY THE FILES ARE WRITTEN OUT
 *
 * #426 measured that `phar://<ELF>/...` cannot open an archive appended to an
 * executable, so the PHAR has to be a real file before PHP can run an entry of
 * it. php.ini and fpm.conf are written next to it because both are read by
 * path (-c, -y) and because a reload re-reads fpm.conf: a descriptor or a memory
 * buffer cannot survive the master's re-exec (fpm_conf.c, "-y fd:N").
 *
 * All three go to <base>/php-fpm-ng-app-<euid>/<payload sha256>/, where <base> is
 * $FPMNG_APP_DIR, else $TMPDIR, else /tmp. Content-addressed means two different
 * applications never share a path, so OPcache (which keys a PHAR by its archive
 * path, #426) cannot hand one application the other's cached scripts (OPcache does cache
 * PHAR entries that carry a non-zero mtime; one with mtime 0 is silently skipped), and a
 * repack under the same name gets a new directory. The directories are owned by
 * the effective uid and not writable by anyone else; nothing is followed through
 * a symlink; files are written under a temporary name and renamed into place.
 * An existing file is reused only if it is byte-for-byte what this run would
 * write, otherwise it is replaced.
 *
 * Old directories are never removed. A master that was started before an
 * upgrade, or a worker still finishing a request, may still be reading the old
 * archive, and a safe "nobody uses it" test does not exist for files that
 * processes have open; the cost is one directory per distinct payload ever run.
 * The operator can delete the whole php-fpm-ng-app-<euid> tree while the
 * service is stopped.
 *
 * RELOAD AND UPGRADE
 *
 * The master re-execs itself with its saved argument vector on SIGUSR2. The
 * injected arguments (-c <state>/php.ini -y <state>/fpm.conf) are therefore
 * recognised by their shape and stripped again on the next start, so the
 * vector is rebuilt from whatever payload the binary on disk carries NOW. Replace
 * the executable (rename over it), send SIGUSR2, and the new master runs the new
 * application from a new directory. A binary without a payload strips the
 * injected arguments and runs as a plain php-fpm-ng.
 */

#include "fpm_config.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "php.h"
#include "zend_API.h"

#include "fpm_pack.h"
#include "fpm_pack_run.h"
#include "fpm_payload.h"
#include "zlog.h"

#define FPM_PACK_ARCHIVE_MAGIC "FPMNGAR1"
#define FPM_PACK_ENV_BASE "FPMNG_APP_DIR"
#define FPM_PACK_INJECTED_ARGS 4 /* -c <php.ini> -y <fpm.conf> */
#define FPM_PACK_MAX_NAMES 1000000

static struct {
	int active;
	char prefix[PATH_MAX + 16]; /* "phar://<archive>/" */
	size_t prefix_len;
	char **names; /* the PHAR's manifest, entry names */
	size_t n_names;
	char *ini; /* the embedded php.ini after substitution */
	size_t ini_len;
} fpm_pack_state;

static uint32_t fpm_pack_u32(const unsigned char *p)
{
	return (uint32_t) p[0] | ((uint32_t) p[1] << 8) | ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24);
}

static uint64_t fpm_pack_u64(const unsigned char *p)
{
	return (uint64_t) fpm_pack_u32(p) | ((uint64_t) fpm_pack_u32(p + 4) << 32);
}

static int fpm_pack_fail(const char *what, const char *detail)
{
	fprintf(stderr, "php-fpm-ng: cannot run the packed application: %s%s%s\n", what, detail ? ": " : "", detail ? detail : "");
	return -1;
}

/* One entry of the application archive (the FPMNGAR1 layout fpm_pack.c writes). */
static int fpm_pack_archive_find(const unsigned char *a, size_t len, const char *name, const unsigned char **data, size_t *size)
{
	uint32_t count, i;
	size_t pos = 12, name_len = strlen(name);

	if (len < 12 || memcmp(a, FPM_PACK_ARCHIVE_MAGIC, 8) != 0) {
		return -1;
	}
	count = fpm_pack_u32(a + 8);
	for (i = 0; i < count; i++) {
		uint32_t nl;
		uint64_t sz, off;

		if (len - pos < 20) {
			return -1;
		}
		nl = fpm_pack_u32(a + pos);
		sz = fpm_pack_u64(a + pos + 4);
		off = fpm_pack_u64(a + pos + 12);
		pos += 20;
		if (nl > len - pos) {
			return -1;
		}
		if (sz > len || off > len - sz) {
			return -1;
		}
		if (nl == name_len && memcmp(a + pos, name, nl) == 0) {
			*data = a + off;
			*size = (size_t) sz;
			return 0;
		}
		pos += nl;
	}
	return -1;
}

/* Reads the manifest of a phar-format archive and collects the entry names.
 * Only the layout `Phar::buildFromDirectory()` and Box write is understood; a
 * tar- or zip-based PHAR has no such manifest and is refused by name rather than
 * guessed at. This is the "structural validity before activation" of #427, and
 * it is what lets a typo in `fpmng-app://` be reported at startup instead of at
 * the first request. The signature is not checked here (ext/phar does it when
 * the archive is opened, and a hash is not publisher authenticity anyway). */
static int fpm_pack_phar_manifest(const unsigned char *d, size_t len, const char **why)
{
	static const char halt[] = "__HALT_COMPILER();";
	const unsigned char *p = NULL, *end = d + len, *m;
	uint32_t mlen, count, alen, mdlen, i;
	size_t remain;

	m = memmem(d, len, halt, sizeof(halt) - 1);
	if (m != NULL) {
		p = m + sizeof(halt) - 1;
	}
	if (p == NULL) {
		*why = "the PHAR has no __HALT_COMPILER(); marker (a tar- or zip-based PHAR is not supported)";
		return -1;
	}
	if (end - p >= 3 && memcmp(p, " ?>", 3) == 0) {
		p += 3;
	}
	if (end - p >= 2 && p[0] == '\r' && p[1] == '\n') {
		p += 2;
	} else if (end - p >= 1 && p[0] == '\n') {
		p += 1;
	}
	if (end - p < 4) {
		*why = "the PHAR manifest is truncated";
		return -1;
	}
	mlen = fpm_pack_u32(p);
	p += 4;
	if (mlen < 14 || (size_t) (end - p) < mlen) {
		*why = "the PHAR manifest is truncated";
		return -1;
	}
	end = p + mlen;
	count = fpm_pack_u32(p);
	p += 4 + 2 + 4; /* count, API version, global flags */
	if (count > FPM_PACK_MAX_NAMES) {
		*why = "the PHAR manifest lists an implausible number of files";
		return -1;
	}
	if (end - p < 4) {
		*why = "the PHAR manifest is truncated";
		return -1;
	}
	alen = fpm_pack_u32(p);
	p += 4;
	if (alen > (size_t) (end - p) || (size_t) (end - p) - alen < 4) {
		*why = "the PHAR manifest is truncated";
		return -1;
	}
	p += alen;
	mdlen = fpm_pack_u32(p);
	p += 4;
	if (mdlen > (size_t) (end - p)) {
		*why = "the PHAR manifest is truncated";
		return -1;
	}
	p += mdlen;

	fpm_pack_state.names = calloc(count ? count : 1, sizeof(char *));
	if (fpm_pack_state.names == NULL) {
		*why = "out of memory";
		return -1;
	}
	for (i = 0; i < count; i++) {
		uint32_t nl, ml;

		remain = (size_t) (end - p);
		if (remain < 4) {
			*why = "the PHAR manifest is truncated";
			return -1;
		}
		nl = fpm_pack_u32(p);
		p += 4;
		remain -= 4;
		if (nl > remain || remain - nl < 24) {
			*why = "the PHAR manifest is truncated";
			return -1;
		}
		fpm_pack_state.names[i] = malloc((size_t) nl + 1);
		if (fpm_pack_state.names[i] == NULL) {
			*why = "out of memory";
			return -1;
		}
		memcpy(fpm_pack_state.names[i], p, nl);
		fpm_pack_state.names[i][nl] = '\0';
		fpm_pack_state.n_names = i + 1;
		p += nl;
		/* size, timestamp, compressed size, crc32, flags, metadata length */
		ml = fpm_pack_u32(p + 20);
		p += 24;
		if (ml > (size_t) (end - p)) {
			*why = "the PHAR manifest is truncated";
			return -1;
		}
		p += ml;
	}
	return 0;
}

/* Replaces every FPM_PACK_APP_SCHEME in `in` by "phar://<archive>/". */
static char *fpm_pack_substitute(const unsigned char *in, size_t len, const char *prefix, size_t *out_len)
{
	static const char scheme[] = FPM_PACK_APP_SCHEME;
	size_t slen = sizeof(scheme) - 1, plen = strlen(prefix), hits = 0, i, o = 0;
	char *out;

	for (i = 0; i + slen <= len; i++) {
		if (in[i] == 'f' && memcmp(in + i, scheme, slen) == 0) {
			hits++;
			i += slen - 1;
		}
	}
	out = malloc(len + hits * plen + 1);
	if (out == NULL) {
		return NULL;
	}
	for (i = 0; i < len;) {
		if (i + slen <= len && in[i] == 'f' && memcmp(in + i, scheme, slen) == 0) {
			memcpy(out + o, prefix, plen);
			o += plen;
			i += slen;
		} else {
			out[o++] = (char) in[i++];
		}
	}
	out[o] = '\0';
	*out_len = o;
	return out;
}

/* Opens (creating it when `create` allows) the directory `name` under `parent`
 * without following a symlink, and insists that it belongs to this process's
 * effective uid and that nobody else can write into it. */
static int fpm_pack_open_private_dir(int parent, const char *name, const char *shown, mode_t mode, int *out)
{
	struct stat st;
	int fd, created = 0;

	if (mkdirat(parent, name, mode) == 0) {
		created = 1;
	} else if (errno != EEXIST) {
		fprintf(stderr, "php-fpm-ng: cannot create %s: %s\n", shown, strerror(errno));
		return -1;
	}
	fd = openat(parent, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	if (fd < 0) {
		fprintf(stderr, "php-fpm-ng: cannot open %s (not a directory, or a symlink): %s\n", shown, strerror(errno));
		return -1;
	}
	if (created && fchmod(fd, mode) != 0) {
		fprintf(stderr, "php-fpm-ng: cannot set the mode of %s: %s\n", shown, strerror(errno));
		close(fd);
		return -1;
	}
	if (fstat(fd, &st) != 0 || st.st_uid != geteuid() || (st.st_mode & (S_IWGRP | S_IWOTH)) != 0) {
		fprintf(stderr, "php-fpm-ng: %s is not private to this user (owner uid %ld, mode %o); remove it or set FPMNG_APP_DIR to another directory\n",
				shown, (long) st.st_uid, (unsigned) (st.st_mode & 07777));
		close(fd);
		return -1;
	}
	*out = fd;
	return 0;
}

/* Makes `dir/name` hold exactly `data`. Returns 0, or -1 after printing why. */
static int fpm_pack_put_file(int dir, const char *shown_dir, const char *name, const char *data, size_t len, mode_t mode)
{
	char tmp[64];
	struct stat st;
	int fd;
	size_t done = 0;

	fd = openat(dir, name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
	if (fd >= 0) {
		int same = 0;

		if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_uid == geteuid() && (st.st_mode & (S_IWGRP | S_IWOTH)) == 0 && (uint64_t) st.st_size == len) {
			char *back = malloc(len ? len : 1);
			size_t got = 0;

			while (back != NULL && got < len) {
				ssize_t r = read(fd, back + got, len - got);

				if (r < 0 && errno == EINTR) {
					continue;
				}
				if (r <= 0) {
					break;
				}
				got += (size_t) r;
			}
			same = back != NULL && got == len && (len == 0 || memcmp(back, data, len) == 0);
			free(back);
		}
		close(fd);
		if (same) {
			return 0;
		}
	}

	snprintf(tmp, sizeof(tmp), ".%s.%ld.tmp", name, (long) getpid());
	/* The directory is private to this euid, so a leftover from a start that was
	 * killed mid-write (a container master is always PID 1, so the name repeats)
	 * is ours: remove it instead of failing on O_EXCL forever. */
	unlinkat(dir, tmp, 0);
	fd = openat(dir, tmp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, mode);
	if (fd < 0) {
		fprintf(stderr, "php-fpm-ng: cannot create %s/%s: %s\n", shown_dir, tmp, strerror(errno));
		return -1;
	}
	while (done < len) {
		ssize_t w = write(fd, data + done, len - done);

		if (w < 0 && errno == EINTR) {
			continue;
		}
		if (w <= 0) {
			fprintf(stderr, "php-fpm-ng: cannot write %s/%s: %s\n", shown_dir, tmp, strerror(errno));
			close(fd);
			unlinkat(dir, tmp, 0);
			return -1;
		}
		done += (size_t) w;
	}
	if (fchmod(fd, mode) != 0 || fsync(fd) != 0 || close(fd) != 0 || renameat(dir, tmp, dir, name) != 0) {
		fprintf(stderr, "php-fpm-ng: cannot finish %s/%s: %s\n", shown_dir, name, strerror(errno));
		unlinkat(dir, tmp, 0);
		return -1;
	}
	return 0;
}

/* True when argv[1..4] is exactly what fpm_pack_activate() injects:
 * -c <base>/php-fpm-ng-app-<euid>/<64 hex>/php.ini -y <same dir>/fpm.conf.
 * Anything else is the operator's own arguments and is never touched. */
static int fpm_pack_has_injected(int argc, char **argv)
{
	const char *ini, *conf, *slash, *dir;
	size_t dir_len, i;

	if (argc < 1 + FPM_PACK_INJECTED_ARGS || strcmp(argv[1], "-c") != 0 || strcmp(argv[3], "-y") != 0) {
		return 0;
	}
	ini = argv[2];
	conf = argv[4];
	slash = strrchr(ini, '/');
	if (slash == NULL || strcmp(slash, "/" FPM_PACK_ENTRY_INI) != 0) {
		return 0;
	}
	dir_len = (size_t) (slash - ini);
	if (strlen(conf) != dir_len + 1 + sizeof(FPM_PACK_ENTRY_CONF) - 1 || strncmp(conf, ini, dir_len) != 0 ||
			strcmp(conf + dir_len, "/" FPM_PACK_ENTRY_CONF) != 0) {
		return 0;
	}
	/* The last component of the directory is the 64 hex digit digest, its parent php-fpm-ng-app-<euid>. */
	if (dir_len < 2 * FPM_PAYLOAD_DIGEST_SIZE + 1 || ini[dir_len - 2 * FPM_PAYLOAD_DIGEST_SIZE - 1] != '/') {
		return 0;
	}
	for (dir = ini + dir_len - 2 * FPM_PAYLOAD_DIGEST_SIZE, i = 0; i < 2 * FPM_PAYLOAD_DIGEST_SIZE; i++) {
		if (!isxdigit((unsigned char) dir[i])) {
			return 0;
		}
	}
	return strstr(ini, "/php-fpm-ng-app-") != NULL;
}

static int fpm_pack_strip_injected(int *argc, char ***argv)
{
	if (!fpm_pack_has_injected(*argc, *argv)) {
		return 0;
	}
	memmove(*argv + 1, *argv + 1 + FPM_PACK_INJECTED_ARGS, (size_t) (*argc - FPM_PACK_INJECTED_ARGS) * sizeof(char *));
	*argc -= FPM_PACK_INJECTED_ARGS;
	return 1;
}

int fpm_pack_activate(int *argc, char ***argv)
{
	struct fpm_payload_entry entry;
	const char *why = NULL, *self, *base;
	char *blob = NULL, *conf = NULL, *ini = NULL, hex[2 * FPM_PAYLOAD_DIGEST_SIZE + 1];
	char realbase[PATH_MAX], top[PATH_MAX + 64], sub[PATH_MAX + 128], archive[PATH_MAX + 256];
	const unsigned char *conf_in, *ini_in, *phar_in;
	size_t blob_len = 0, conf_len, ini_len, phar_len, conf_out_len = 0, ini_out_len = 0, i;
	char **nv;
	int rc, basefd = -1, topfd = -1, subfd = -1, ret = -1;
	int was_injected;

	was_injected = fpm_pack_strip_injected(argc, argv);

	fpm_payload_set_argv0((*argv)[0]);
	self = fpm_payload_self_path();
	if (self == NULL && access("/proc/self/exe", R_OK) == 0) {
		/* Deleted-but-running binary: the kernel still opens its inode. */
		self = "/proc/self/exe";
	}
	if (self == NULL) {
		if (was_injected) {
			/* This is a re-exec of a packed master; running on as a plain binary
			 * would use the host's php.ini and fpm.conf. */
			return fpm_pack_fail("cannot locate the running executable to read its payload",
					"no /proc/self/exe and argv[0] is not a path");
		}
		/* No path, no way to tell whether a payload exists. */
		return 0;
	}
	rc = fpm_payload_find(self, FPM_PAYLOAD_KIND_APPLICATION, &entry, &why);
	if (rc < 0) {
		return fpm_pack_fail("the application payload is damaged", why);
	}
	if (rc == 0) {
		/* "No record at the end" is also what a cut-off packed executable looks
		 * like; running that as a plain php-fpm-ng would silently use whatever
		 * the host has. */
		if (fpm_payload_check_tail(self, &why) != 0) {
			return fpm_pack_fail("the executable ends with data that is not an intact payload", why);
		}
		return 0;
	}
	if (fpm_payload_read(self, &entry, &blob, &blob_len, &why) < 0) {
		return fpm_pack_fail("the application payload is damaged", why);
	}

	if (fpm_pack_archive_find((unsigned char *) blob, blob_len, FPM_PACK_ENTRY_CONF, &conf_in, &conf_len) != 0 ||
			fpm_pack_archive_find((unsigned char *) blob, blob_len, FPM_PACK_ENTRY_INI, &ini_in, &ini_len) != 0 ||
			fpm_pack_archive_find((unsigned char *) blob, blob_len, FPM_PACK_ENTRY_PHAR, &phar_in, &phar_len) != 0) {
		ret = fpm_pack_fail("the application payload does not hold " FPM_PACK_ENTRY_CONF ", " FPM_PACK_ENTRY_INI " and " FPM_PACK_ENTRY_PHAR, NULL);
		goto out;
	}
	if (memchr(conf_in, '\0', conf_len) != NULL || memchr(ini_in, '\0', ini_len) != NULL) {
		ret = fpm_pack_fail("the embedded " FPM_PACK_ENTRY_CONF " or " FPM_PACK_ENTRY_INI " contains a NUL byte", NULL);
		goto out;
	}
	if (fpm_pack_phar_manifest(phar_in, phar_len, &why) != 0) {
		ret = fpm_pack_fail("the embedded " FPM_PACK_ENTRY_PHAR " is not usable", why);
		goto out;
	}

	for (i = 0; i < FPM_PAYLOAD_DIGEST_SIZE; i++) {
		snprintf(hex + 2 * i, 3, "%02x", entry.digest[i]);
	}

	base = getenv(FPM_PACK_ENV_BASE);
	if (base == NULL || *base == '\0') {
		base = getenv("TMPDIR");
	}
	if (base == NULL || *base == '\0') {
		base = "/tmp";
	}
	if (base[0] != '/' || realpath(base, realbase) == NULL) {
		ret = fpm_pack_fail("the directory for the unpacked application is not an existing absolute path", base);
		goto out;
	}
	snprintf(top, sizeof(top), "%s/php-fpm-ng-app-%ld", realbase, (long) geteuid());
	snprintf(sub, sizeof(sub), "%s/%s", top, hex);
	snprintf(archive, sizeof(archive), "%s/" FPM_PACK_ENTRY_PHAR, sub);
	if (strlen(archive) >= PATH_MAX) {
		ret = fpm_pack_fail("the directory for the unpacked application has too long a path", sub);
		goto out;
	}
	snprintf(fpm_pack_state.prefix, sizeof(fpm_pack_state.prefix), "phar://%s/", archive);
	fpm_pack_state.prefix_len = strlen(fpm_pack_state.prefix);

	conf = fpm_pack_substitute(conf_in, conf_len, fpm_pack_state.prefix, &conf_out_len);
	ini = fpm_pack_substitute(ini_in, ini_len, fpm_pack_state.prefix, &ini_out_len);
	if (conf == NULL || ini == NULL) {
		ret = fpm_pack_fail("out of memory", NULL);
		goto out;
	}

	/* A root-run master drops to each pool's user, who must be able to reach
	 * the archive; everyone else gets a directory only they can enter. */
	{
		mode_t dir_mode = geteuid() == 0 ? 0755 : 0700;

		basefd = open(realbase, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
		if (basefd < 0) {
			ret = fpm_pack_fail("cannot open the directory for the unpacked application", realbase);
			goto out;
		}
		if (fpm_pack_open_private_dir(basefd, strrchr(top, '/') + 1, top, dir_mode, &topfd) != 0 ||
				fpm_pack_open_private_dir(topfd, hex, sub, dir_mode, &subfd) != 0) {
			goto out;
		}
	}
	if (fpm_pack_put_file(subfd, sub, FPM_PACK_ENTRY_PHAR, (const char *) phar_in, phar_len, 0444) != 0 ||
			fpm_pack_put_file(subfd, sub, FPM_PACK_ENTRY_INI, ini, ini_out_len, 0444) != 0 ||
			fpm_pack_put_file(subfd, sub, FPM_PACK_ENTRY_CONF, conf, conf_out_len, 0400) != 0) {
		goto out;
	}

	nv = malloc((size_t) (*argc + FPM_PACK_INJECTED_ARGS + 1) * sizeof(char *));
	if (nv == NULL) {
		ret = fpm_pack_fail("out of memory", NULL);
		goto out;
	}
	{
		char *ini_path = malloc(strlen(sub) + sizeof("/" FPM_PACK_ENTRY_INI));
		char *conf_path = malloc(strlen(sub) + sizeof("/" FPM_PACK_ENTRY_CONF));

		if (ini_path == NULL || conf_path == NULL) {
			free(nv);
			free(ini_path);
			free(conf_path);
			ret = fpm_pack_fail("out of memory", NULL);
			goto out;
		}
		sprintf(ini_path, "%s/" FPM_PACK_ENTRY_INI, sub);
		sprintf(conf_path, "%s/" FPM_PACK_ENTRY_CONF, sub);
		nv[0] = (*argv)[0];
		nv[1] = (char *) "-c";
		nv[2] = ini_path;
		nv[3] = (char *) "-y";
		nv[4] = conf_path;
		memcpy(nv + 1 + FPM_PACK_INJECTED_ARGS, *argv + 1, (size_t) (*argc - 1 + 1) * sizeof(char *));
		*argc += FPM_PACK_INJECTED_ARGS;
		*argv = nv;
	}
	/* Empty, not unset: unset falls back to the directory PHP was compiled with. */
	setenv("PHP_INI_SCAN_DIR", "", 1);

	fpm_pack_state.ini = ini;
	fpm_pack_state.ini_len = ini_out_len;
	ini = NULL;
	fpm_pack_state.active = 1;
	ret = 1;

out:
	if (subfd >= 0) {
		close(subfd);
	}
	if (topfd >= 0) {
		close(topfd);
	}
	if (basefd >= 0) {
		close(basefd);
	}
	free(blob);
	free(conf);
	free(ini);
	return ret;
}

bool fpm_pack_is_app_path(const char *path)
{
	return fpm_pack_state.active && path != NULL && strncmp(path, fpm_pack_state.prefix, fpm_pack_state.prefix_len) == 0;
}

bool fpm_pack_is_unresolved_path(const char *path)
{
	return path != NULL && strncmp(path, FPM_PACK_APP_SCHEME, sizeof(FPM_PACK_APP_SCHEME) - 1) == 0;
}

int fpm_pack_validate_path(const char *path, const char **why)
{
	const char *entry;
	size_t i;

	if (!fpm_pack_is_app_path(path)) {
		*why = "not a path into the application PHAR";
		return -1;
	}
	entry = path + fpm_pack_state.prefix_len;
	if (*entry == '\0' || *entry == '/' || strstr(entry, "..") != NULL || strchr(entry, '\\') != NULL || strstr(entry, "//") != NULL) {
		*why = "the entry name is empty, absolute, or contains '..', '//' or a backslash";
		return -1;
	}
	for (i = 0; i < fpm_pack_state.n_names; i++) {
		if (strcmp(fpm_pack_state.names[i], entry) == 0) {
			return 0;
		}
	}
	*why = "no such file in the application PHAR";
	return -1;
}

const char *fpm_pack_http_script_name(const char *front_controller)
{
	if (fpm_pack_is_app_path(front_controller)) {
		return front_controller + fpm_pack_state.prefix_len - 1;
	}
	return front_controller;
}

/* The module name PHP registers for an `extension=` value: the file name
 * without directory, ".so" and a leading "php_", lower-cased. */
static bool fpm_pack_extension_loaded(const char *value, size_t len)
{
	char name[128];
	const char *slash;
	size_t n, i;

	while (len > 0 && (isspace((unsigned char) *value) || *value == '"' || *value == '\'')) {
		value++;
		len--;
	}
	while (len > 0 && (isspace((unsigned char) value[len - 1]) || value[len - 1] == '"' || value[len - 1] == '\'')) {
		len--;
	}
	for (slash = value + len; slash > value && slash[-1] != '/'; slash--) {
	}
	len -= (size_t) (slash - value);
	value = slash;
	if (len > 3 && memcmp(value + len - 3, ".so", 3) == 0) {
		len -= 3;
	}
	if (len > 4 && memcmp(value, "php_", 4) == 0) {
		value += 4;
		len -= 4;
	}
	if (len == 0 || len >= sizeof(name)) {
		return true; /* nothing sensible to look up: leave it to PHP */
	}
	n = len;
	for (i = 0; i < n; i++) {
		name[i] = (char) tolower((unsigned char) value[i]);
	}
	name[n] = '\0';
	return zend_hash_str_exists(&module_registry, name, n);
}

int fpm_pack_check_runtime(void)
{
	const char *p, *end;

	if (!fpm_pack_state.active) {
		return 0;
	}
	/* PHP read PHP_INI_SCAN_DIR at startup, which is over; a child process the
	 * application starts must not inherit an empty one. The re-exec path sets
	 * it again. */
	unsetenv("PHP_INI_SCAN_DIR");

	if (!zend_hash_str_exists(&module_registry, "phar", sizeof("phar") - 1)) {
		zlog(ZLOG_ALERT, "this executable carries an application PHAR, but PHP's phar extension is not loaded; "
						 "the embedded php.ini must contain 'extension=phar' (the host's php.ini is not read)");
		return -1;
	}
	for (p = fpm_pack_state.ini, end = p + fpm_pack_state.ini_len; p < end;) {
		const char *nl = memchr(p, '\n', (size_t) (end - p)), *eol = nl ? nl : end, *q = p;

		while (q < eol && (*q == ' ' || *q == '\t')) {
			q++;
		}
		if ((size_t) (eol - q) > 10 && memcmp(q, "extension", 9) == 0) {
			const char *r = q + 9;

			while (r < eol && (*r == ' ' || *r == '\t')) {
				r++;
			}
			if (r < eol && *r == '=') {
				const char *v = r + 1, *semi = memchr(v, ';', (size_t) (eol - v));
				size_t vlen = (size_t) ((semi ? semi : eol) - v);

				if (!fpm_pack_extension_loaded(v, vlen)) {
					zlog(ZLOG_ALERT, "the embedded php.ini asks for '%.*s', which PHP did not load (see the startup warning above); "
									 "the application is not started",
							(int) vlen, v);
					return -1;
				}
			}
		}
		p = nl ? nl + 1 : end;
	}
	return 0;
}

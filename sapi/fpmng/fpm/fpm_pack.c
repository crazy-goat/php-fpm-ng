/* fpm-ng: `php-fpm-ng pack <app.phar> <php.ini> <fpm.conf> -o <output>` (issue #429).
 *
 * WHAT THIS DOES
 *
 * Copies the running binary to <output> and appends one application payload
 * (kind 2, fpm_payload.h) holding the three inputs. All three are mandatory and
 * none is interpreted: the PHAR is stored byte for byte, and neither it nor its
 * stub is opened by PHP, so no application code runs while packing (acceptance
 * criterion 2). The runtime that consumes the payload is #428/#430; this only
 * writes it.
 *
 * WHY A SUBCOMMAND OF THE BINARY, NOT payload-pack.php
 *
 * Decision D2 on #429: packing needs no PHP CLI and no toolchain on the machine
 * that packs, only a php-fpm-ng. build/payload-pack.php stays the build-time
 * tool and reads what this writes (`list`).
 *
 * THE ARCHIVE
 *
 * The same FPMNGAR1 layout build/payload-pack.php writes for the distribution
 * payload, with entries sorted by name so the same inputs give the same bytes:
 * fpm.conf, php.ini, app.phar (FPM_PACK_ENTRY_*). The record is the one
 * fpm_payload.h describes and chains to the record that was last in the copied
 * binary, so the distribution entry stays byte-identical.
 *
 * WHAT IT REFUSES, AND WHY
 *
 *  - an input that is missing, unreadable, not a regular file or empty;
 *  - a PHAR with no `__HALT_COMPILER` (a file that is plainly not a PHAR), and a
 *    php.ini or fpm.conf containing a NUL byte;
 *  - a running binary that already carries an application payload: the newest
 *    kind-2 entry would win at run time and the old one would stay as dead
 *    weight, so repacking starts from an unpacked php-fpm-ng (docs/payload.md);
 *  - an <output> that already exists. Overwriting is the one thing a pack run
 *    could do to a file the user cares about, and removing it first is cheap.
 *
 * <output> is written under a temporary name next to it, read back through the
 * same reader the runtime uses (fpm_payload_find() and fpm_payload_read(), which
 * check the SHA-256), and only then renamed into place, so a failed pack leaves
 * nothing at <output>.
 */

#include "fpm_config.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "php.h"
#include "ext/hash/php_hash.h" /* PHP_HASH_API, which php_hash_sha.h uses and does not include */
#include "ext/hash/php_hash_sha.h"

#include "fpm_pack.h"
#include "fpm_payload.h"

#define FPM_PACK_USAGE_EXIT 64 /* EX_USAGE, as in fpm_serve.c */
#define FPM_PACK_ARCHIVE_MAGIC "FPMNGAR1"
#define FPM_PACK_ARCHIVE_MAGIC_SIZE 8

struct fpm_pack_input {
	const char *name; /* entry name in the archive */
	const char *what; /* for messages */
	const char *path;
	char *data;
	size_t size;
};

static void fpm_pack_usage(FILE *out)
{
	fputs("Usage: php-fpm-ng pack <app.phar> <php.ini> <fpm.conf> -o <output>\n"
		  "\n"
		  "Write <output>: a copy of this php-fpm-ng with the three files embedded.\n"
		  "All three are mandatory and are stored unchanged; nothing in them is run or\n"
		  "parsed while packing. <output> must not exist yet.\n"
		  "\n"
		  "  -o, --output <file>   the executable to write\n"
		  "  -h, --help            this text\n",
			out);
}

static int fpm_pack_fail(const char *fmt, const char *a, const char *b)
{
	fputs("php-fpm-ng pack: ", stderr);
	fprintf(stderr, fmt, a, b);
	fputc('\n', stderr);
	return 1;
}

static void fpm_pack_put32(unsigned char *p, uint32_t v)
{
	int i;

	for (i = 0; i < 4; i++) {
		p[i] = (unsigned char) (v >> (8 * i));
	}
}

static void fpm_pack_put64(unsigned char *p, uint64_t v)
{
	int i;

	for (i = 0; i < 8; i++) {
		p[i] = (unsigned char) (v >> (8 * i));
	}
}

static int fpm_pack_read_input(struct fpm_pack_input *in)
{
	struct stat st;
	FILE *fp;

	if (stat(in->path, &st) != 0) {
		fprintf(stderr, "php-fpm-ng pack: cannot read the %s '%s': %s\n", in->what, in->path, strerror(errno));
		return 1;
	}
	if (!S_ISREG(st.st_mode)) {
		return fpm_pack_fail("the %s '%s' is not a regular file", in->what, in->path);
	}
	if (st.st_size <= 0) {
		return fpm_pack_fail("the %s '%s' is empty", in->what, in->path);
	}
	if ((uint64_t) st.st_size > (uint64_t) SIZE_MAX - 1) {
		return fpm_pack_fail("the %s '%s' is too large", in->what, in->path);
	}
	in->size = (size_t) st.st_size;
	in->data = malloc(in->size + 1);
	fp = in->data ? fopen(in->path, "rb") : NULL;
	if (fp == NULL || fread(in->data, 1, in->size, fp) != in->size) {
		if (fp != NULL) {
			fclose(fp);
		}
		return fpm_pack_fail("cannot read the %s '%s' in full", in->what, in->path);
	}
	fclose(fp);
	in->data[in->size] = '\0';
	return 0;
}

static int fpm_pack_contains(const char *hay, size_t n, const char *needle)
{
	size_t len = strlen(needle);
	const char *p = hay, *end = hay + n;

	while (n >= len && (p = memchr(p, needle[0], (size_t) (end - p) - len + 1)) != NULL) {
		if (memcmp(p, needle, len) == 0) {
			return 1;
		}
		p++;
	}
	return 0;
}

static int fpm_pack_check_content(const struct fpm_pack_input *phar, const struct fpm_pack_input *ini, const struct fpm_pack_input *conf)
{
	if (!fpm_pack_contains(phar->data, phar->size, "__HALT_COMPILER")) {
		return fpm_pack_fail("the %s '%s' is not a PHAR (no __HALT_COMPILER marker)", phar->what, phar->path);
	}
	if (memchr(ini->data, '\0', ini->size) != NULL) {
		return fpm_pack_fail("the %s '%s' contains a NUL byte", ini->what, ini->path);
	}
	if (memchr(conf->data, '\0', conf->size) != NULL) {
		return fpm_pack_fail("the %s '%s' contains a NUL byte", conf->what, conf->path);
	}
	return 0;
}

/* The archive for `files` (already in name order). Returns a malloc()ed buffer. */
static unsigned char *fpm_pack_build_archive(struct fpm_pack_input **files, size_t count, size_t *len)
{
	size_t header = FPM_PACK_ARCHIVE_MAGIC_SIZE + 4, index = 0, total, at, i;
	unsigned char *buf, *p;

	for (i = 0; i < count; i++) {
		index += 4 + 8 + 8 + strlen(files[i]->name);
	}
	total = header + index;
	for (i = 0; i < count; i++) {
		total += files[i]->size;
	}
	buf = malloc(total);
	if (buf == NULL) {
		return NULL;
	}
	memcpy(buf, FPM_PACK_ARCHIVE_MAGIC, FPM_PACK_ARCHIVE_MAGIC_SIZE);
	fpm_pack_put32(buf + FPM_PACK_ARCHIVE_MAGIC_SIZE, (uint32_t) count);
	p = buf + header;
	at = header + index;
	for (i = 0; i < count; i++) {
		size_t n = strlen(files[i]->name);

		fpm_pack_put32(p, (uint32_t) n);
		fpm_pack_put64(p + 4, files[i]->size);
		fpm_pack_put64(p + 12, at);
		memcpy(p + 20, files[i]->name, n);
		p += 20 + n;
		memcpy(buf + at, files[i]->data, files[i]->size);
		at += files[i]->size;
	}
	*len = total;
	return buf;
}

static int fpm_pack_write_all(int fd, const void *buf, size_t n)
{
	const char *p = buf;

	while (n > 0) {
		ssize_t w = write(fd, p, n);

		if (w < 0) {
			if (errno == EINTR) {
				continue;
			}
			return -1;
		}
		p += w;
		n -= (size_t) w;
	}
	return 0;
}

/* Copies `self` to `fd`; stores its size in *size and whether it ends in a
 * payload record in *chained. */
static int fpm_pack_copy_self(const char *self, int fd, uint64_t *size, int *chained)
{
	unsigned char block[65536], tail[FPM_PAYLOAD_RECORD_SIZE];
	size_t tail_n = 0;
	uint64_t total = 0;
	ssize_t n;
	int in = open(self, O_RDONLY);

	if (in < 0) {
		return -1;
	}
	while ((n = read(in, block, sizeof(block))) != 0) {
		if (n < 0) {
			if (errno == EINTR) {
				continue;
			}
			close(in);
			return -1;
		}
		if (fpm_pack_write_all(fd, block, (size_t) n) != 0) {
			close(in);
			return -1;
		}
		total += (uint64_t) n;
		if ((size_t) n >= sizeof(tail)) {
			memcpy(tail, block + n - sizeof(tail), sizeof(tail));
			tail_n = sizeof(tail);
		} else {
			size_t keep = tail_n + (size_t) n > sizeof(tail) ? sizeof(tail) - (size_t) n : tail_n;

			memmove(tail, tail + tail_n - keep, keep);
			memcpy(tail + keep, block, (size_t) n);
			tail_n = keep + (size_t) n;
		}
	}
	close(in);
	*size = total;
	*chained = tail_n == sizeof(tail) && memcmp(tail, FPM_PAYLOAD_MAGIC, FPM_PAYLOAD_MAGIC_SIZE) == 0;
	return 0;
}

static int fpm_pack_verify(const char *path, const unsigned char *archive, size_t archive_len)
{
	struct fpm_payload_entry entry;
	const char *why = NULL;
	char *data;
	size_t size;
	int rc, same;

	rc = fpm_payload_find(path, FPM_PAYLOAD_KIND_APPLICATION, &entry, &why);
	if (rc != 1) {
		return fpm_pack_fail("the packed file carries no readable application payload: %s", why ? why : "not found", NULL);
	}
	if (fpm_payload_read(path, &entry, &data, &size, &why) != 0) {
		return fpm_pack_fail("the packed file's application payload is broken: %s", why ? why : "?", NULL);
	}
	same = size == archive_len && memcmp(data, archive, size) == 0;
	free(data);
	if (!same) {
		return fpm_pack_fail("the packed file's application payload differs from what was written", NULL, NULL);
	}
	return 0;
}

static int fpm_pack_write_output(const char *out, const char *self, const unsigned char *archive, size_t archive_len)
{
	unsigned char record[FPM_PAYLOAD_RECORD_SIZE];
	PHP_SHA256_CTX ctx;
	char tmp[PATH_MAX + 32];
	uint64_t self_size = 0;
	mode_t mask;
	int fd, chained = 0, rc;

	if (snprintf(tmp, sizeof(tmp), "%s.tmp.XXXXXX", out) >= (int) sizeof(tmp)) {
		return fpm_pack_fail("the output path is too long: %s", out, NULL);
	}
	/* Like cp and install: executable for whoever the umask lets read it. */
	mask = umask(0);
	umask(mask);
	fd = mkstemp(tmp);
	if (fd < 0) {
		return fpm_pack_fail("cannot create a file next to '%s': %s", out, strerror(errno));
	}
	if (fpm_pack_copy_self(self, fd, &self_size, &chained) != 0) {
		rc = fpm_pack_fail("cannot copy the running binary '%s': %s", self, strerror(errno));
		goto fail;
	}

	memset(record, 0, sizeof(record));
	memcpy(record, FPM_PAYLOAD_MAGIC, FPM_PAYLOAD_MAGIC_SIZE);
	fpm_pack_put32(record + 8, FPM_PAYLOAD_KIND_APPLICATION);
	fpm_pack_put64(record + 16, self_size);
	fpm_pack_put64(record + 24, archive_len);
	PHP_SHA256Init(&ctx);
	PHP_SHA256Update(&ctx, archive, archive_len);
	PHP_SHA256Final(record + 32, &ctx);
	fpm_pack_put64(record + 64, chained ? self_size - FPM_PAYLOAD_RECORD_SIZE : 0);

	if (fpm_pack_write_all(fd, archive, archive_len) != 0 || fpm_pack_write_all(fd, record, sizeof(record)) != 0 || fchmod(fd, 0777 & ~mask) != 0 || fsync(fd) != 0) {
		rc = fpm_pack_fail("cannot write '%s': %s", tmp, strerror(errno));
		goto fail;
	}
	if (close(fd) != 0) {
		fd = -1;
		rc = fpm_pack_fail("cannot write '%s': %s", tmp, strerror(errno));
		goto fail;
	}
	fd = -1;
	if ((rc = fpm_pack_verify(tmp, archive, archive_len)) != 0) {
		goto fail;
	}
	/* link() refuses to replace, which closes the window between the existence check and here. */
	if (link(tmp, out) != 0) {
		rc = fpm_pack_fail("cannot create '%s': %s", out, strerror(errno));
		goto fail;
	}
	unlink(tmp);
	return 0;

fail:
	if (fd >= 0) {
		close(fd);
	}
	unlink(tmp);
	return rc ? rc : 1;
}

int fpm_pack_main(int argc, char **argv)
{
	struct fpm_pack_input phar = { FPM_PACK_ENTRY_PHAR, "PHAR" }, ini = { FPM_PACK_ENTRY_INI, "php.ini" }, conf = { FPM_PACK_ENTRY_CONF, "fpm.conf" };
	struct fpm_pack_input *sorted[3] = { &conf, &ini, &phar }, *positional[3] = { &phar, &ini, &conf };
	struct fpm_payload_entry existing;
	struct stat st;
	const char *out = NULL, *self, *why = NULL;
	unsigned char *archive;
	size_t archive_len, i, npos = 0;
	int rc = 0;

	for (i = 2; i < (size_t) argc; i++) {
		const char *arg = argv[i];

		if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0) {
			fpm_pack_usage(stdout);
			return 0;
		}
		if (strcmp(arg, "-o") == 0 || strcmp(arg, "--output") == 0) {
			if (i + 1 >= (size_t) argc) {
				fprintf(stderr, "php-fpm-ng pack: %s needs a value\nTry 'php-fpm-ng pack --help'.\n", arg);
				return FPM_PACK_USAGE_EXIT;
			}
			out = argv[++i];
		} else if (strncmp(arg, "--output=", 9) == 0) {
			out = arg + 9;
		} else if (arg[0] == '-' && arg[1] != '\0') {
			fprintf(stderr, "php-fpm-ng pack: unknown option: %s\nTry 'php-fpm-ng pack --help'.\n", arg);
			return FPM_PACK_USAGE_EXIT;
		} else if (npos < 3) {
			positional[npos++]->path = arg;
		} else {
			fprintf(stderr, "php-fpm-ng pack: too many arguments: %s\nTry 'php-fpm-ng pack --help'.\n", arg);
			return FPM_PACK_USAGE_EXIT;
		}
	}
	if (npos != 3 || out == NULL || *out == '\0') {
		fprintf(stderr, "php-fpm-ng pack: wants <app.phar> <php.ini> <fpm.conf> and -o <output>; all three files are mandatory\nTry 'php-fpm-ng pack --help'.\n");
		return FPM_PACK_USAGE_EXIT;
	}

	for (i = 0; i < 3; i++) {
		if (positional[i]->path[0] == '\0') {
			return fpm_pack_fail("the path of the %s is empty", positional[i]->what, NULL);
		}
		if (fpm_pack_read_input(positional[i]) != 0) {
			return 1;
		}
	}
	if (fpm_pack_check_content(&phar, &ini, &conf) != 0) {
		return 1;
	}

	if (lstat(out, &st) == 0) {
		return fpm_pack_fail("'%s' already exists; remove it or choose another output", out, NULL);
	}
	if (errno != ENOENT) {
		return fpm_pack_fail("cannot use '%s' as the output: %s", out, strerror(errno));
	}

	self = fpm_payload_self_path();
	if (self == NULL) {
		return fpm_pack_fail("cannot find the running php-fpm-ng binary to copy", NULL, NULL);
	}
	rc = fpm_payload_find(self, FPM_PAYLOAD_KIND_APPLICATION, &existing, &why);
	if (rc == 1) {
		return fpm_pack_fail("'%s' already carries an application; pack from an unpacked php-fpm-ng", self, NULL);
	}
	if (rc < 0) {
		return fpm_pack_fail("the payload of '%s' is broken: %s", self, why ? why : "?");
	}

	archive = fpm_pack_build_archive(sorted, 3, &archive_len);
	if (archive == NULL) {
		return fpm_pack_fail("out of memory", NULL, NULL);
	}
	rc = fpm_pack_write_output(out, self, archive, archive_len);
	free(archive);
	if (rc == 0) {
		fprintf(stderr, "php-fpm-ng pack: wrote %s (%s %zu bytes, %s %zu bytes, %s %zu bytes)\n", out, phar.what, phar.size, ini.what, ini.size, conf.what, conf.size);
	}
	return rc;
}

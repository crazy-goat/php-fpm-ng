/* fpm-ng: reading typed append-only payloads out of the running binary
 * (issue #171). The format and the reasons for it are in fpm_payload.h.
 */
#include "fpm_config.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "php.h"
/* After php.h, not before: the header uses PHP_HASH_API and ZEND_ATTRIBUTE_UNUSED,
 * which php.h defines. */
#include "ext/hash/php_hash.h"	/* PHP_HASH_API, which php_hash_sha.h uses and does not include */
#include "ext/hash/php_hash_sha.h"

#include "fpm.h"
#include "fpm_payload.h"

/* A chain longer than this in a file we are about to trust is not a payload, it
 * is a loop or a fabrication. Nothing legitimate appends anywhere near this
 * many entries: the build writes one, the pack command writes one more. */
#define FPM_PAYLOAD_MAX_CHAIN 64

static uint32_t fpm_payload_u32(const unsigned char *p)
{
	return (uint32_t) p[0] | ((uint32_t) p[1] << 8) |
		((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24);
}

static uint64_t fpm_payload_u64(const unsigned char *p)
{
	return (uint64_t) fpm_payload_u32(p) | ((uint64_t) fpm_payload_u32(p + 4) << 32);
}

const char *fpm_payload_self_path(void)
{
	static char resolved[4096];
	static int tried = 0;
	ssize_t n;

	if (tried) {
		return resolved[0] ? resolved : NULL;
	}
	tried = 1;

	/* readlink(), not realpath(): on a deleted-and-replaced binary /proc/self/exe
	 * still points at the inode this process is running, which is the file whose
	 * payload belongs to this process. A path resolved through the file system
	 * would find the NEW binary after a package upgrade. */
	n = readlink("/proc/self/exe", resolved, sizeof(resolved) - 1);
	if (n > 0) {
		resolved[n] = '\0';
		if (access(resolved, R_OK) == 0) {
			return resolved;
		}
	}
	/* No /proc (NOTES.md:157-201 asks for this fallback: /proc is mounted even
	 * in a scratch container, but a binary run outside one may have none). An
	 * argv[0] without a slash would have to be searched for in PATH, which is
	 * guessing which of several binaries is running -- refused rather than
	 * guessed. */
	resolved[0] = '\0';
	if (fpm_globals.argv && fpm_globals.argv[0] && strchr(fpm_globals.argv[0], '/')) {
		if (access(fpm_globals.argv[0], R_OK) == 0) {
			snprintf(resolved, sizeof(resolved), "%s", fpm_globals.argv[0]);
		}
	}
	return resolved[0] ? resolved : NULL;
}

int fpm_payload_find(const char *path, uint32_t kind, struct fpm_payload_entry *entry,
	const char **why)
{
	unsigned char record[FPM_PAYLOAD_RECORD_SIZE];
	uint64_t at;
	long file_size;
	FILE *fp;
	int steps;

	*why = NULL;
	if (!path) {
		*why = "the path of the running binary is not known";
		return -1;
	}
	fp = fopen(path, "rb");
	if (!fp) {
		*why = strerror(errno);
		return -1;
	}
	if (fseek(fp, 0, SEEK_END) != 0 || (file_size = ftell(fp)) < 0) {
		*why = strerror(errno);
		fclose(fp);
		return -1;
	}
	if ((uint64_t) file_size < FPM_PAYLOAD_RECORD_SIZE) {
		fclose(fp);
		return 0;
	}
	at = (uint64_t) file_size - FPM_PAYLOAD_RECORD_SIZE;

	for (steps = 0; steps < FPM_PAYLOAD_MAX_CHAIN; steps++) {
		uint64_t offset, size, prev;
		uint32_t this_kind, flags;

		if (fseek(fp, (long) at, SEEK_SET) != 0 ||
				fread(record, 1, sizeof(record), fp) != sizeof(record)) {
			*why = "a payload record could not be read";
			fclose(fp);
			return -1;
		}
		if (memcmp(record, FPM_PAYLOAD_MAGIC, FPM_PAYLOAD_MAGIC_SIZE) != 0) {
			/* Only the LAST record is reached without following a pointer, and
			 * an ordinary binary ends in whatever the linker put there. So a
			 * mismatch on the first step means "no payload" (the normal case,
			 * not an error), while a mismatch after following prev_offset means
			 * the chain pointed at something that is not a record. */
			fclose(fp);
			if (steps == 0) {
				return 0;
			}
			*why = "a payload record points at data that is not a payload record";
			return -1;
		}
		this_kind = fpm_payload_u32(record + 8);
		flags = fpm_payload_u32(record + 12);
		offset = fpm_payload_u64(record + 16);
		size = fpm_payload_u64(record + 24);
		prev = fpm_payload_u64(record + 64);

		if (flags != 0) {
			*why = "a payload record sets flags this build does not know";
			fclose(fp);
			return -1;
		}
		/* The data must lie fully before its own record. Checked before the
		 * kind is even looked at: a record of a kind we do not want still gets
		 * to place the next seek through prev, so a nonsense one must not be
		 * walked past as if it were fine. */
		if (size > at || offset > at - size) {
			*why = "a payload record describes data outside the binary";
			fclose(fp);
			return -1;
		}
		if (this_kind == kind) {
			entry->kind = this_kind;
			entry->offset = offset;
			entry->size = size;
			memcpy(entry->digest, record + 32, FPM_PAYLOAD_DIGEST_SIZE);
			fclose(fp);
			return 1;
		}
		if (prev == 0) {
			fclose(fp);
			return 0;
		}
		/* Strictly backwards, or the chain could point at itself or forward and
		 * be walked forever. FPM_PAYLOAD_MAX_CHAIN bounds it anyway; this makes
		 * the malformed case a message instead of 64 wasted seeks. */
		if (prev >= at || prev > (uint64_t) file_size - FPM_PAYLOAD_RECORD_SIZE) {
			*why = "a payload record does not point backwards";
			fclose(fp);
			return -1;
		}
		at = prev;
	}
	*why = "the payload chain is longer than this build accepts";
	fclose(fp);
	return -1;
}

int fpm_payload_read(const char *path, const struct fpm_payload_entry *entry,
	char **data, size_t *size, const char **why)
{
	unsigned char digest[FPM_PAYLOAD_DIGEST_SIZE];
	PHP_SHA256_CTX ctx;
	char *buf;
	FILE *fp;

	*why = NULL;
	*data = NULL;
	*size = 0;
	/* (size_t) on a 32-bit build would truncate a >4 GiB entry into a small
	 * allocation and a huge read. Nothing legitimate is that big; refuse it
	 * rather than trust the arithmetic. */
	if (entry->size > (uint64_t) SIZE_MAX - 1) {
		*why = "the payload entry does not fit in memory";
		return -1;
	}
	fp = fopen(path, "rb");
	if (!fp) {
		*why = strerror(errno);
		return -1;
	}
	buf = malloc((size_t) entry->size + 1);
	if (!buf) {
		*why = "out of memory";
		fclose(fp);
		return -1;
	}
	if (fseek(fp, (long) entry->offset, SEEK_SET) != 0 ||
			fread(buf, 1, (size_t) entry->size, fp) != (size_t) entry->size) {
		*why = "the payload entry could not be read in full";
		free(buf);
		fclose(fp);
		return -1;
	}
	fclose(fp);
	buf[entry->size] = '\0';

	/* ext/hash's SHA-256 rather than OpenSSL's: this has to work in a build
	 * configured without OpenSSL, and ext/hash is always compiled in (it has
	 * had no --disable since PHP 7.4). */
	PHP_SHA256Init(&ctx);
	PHP_SHA256Update(&ctx, (const unsigned char *) buf, (size_t) entry->size);
	PHP_SHA256Final(digest, &ctx);
	if (memcmp(digest, entry->digest, sizeof(digest)) != 0) {
		/* Corruption or truncation, not tampering: see the digest paragraph in
		 * fpm_payload.h for what this check does not promise. */
		*why = "the payload entry does not match its digest";
		free(buf);
		return -1;
	}

	*data = buf;
	*size = (size_t) entry->size;
	return 0;
}

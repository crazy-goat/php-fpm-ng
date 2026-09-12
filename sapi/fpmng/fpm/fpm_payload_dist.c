/* fpm-ng: the distribution payload and its stream wrapper (issue #171).
 * Format and spelling rationale: fpm_payload_dist.h.
 */
#include "fpm_config.h"

#include <stdlib.h>
#include <string.h>

#include "php.h"
#include "php_streams.h"
#include "main/streams/php_stream_plain_wrapper.h"

#include "fpm_payload.h"
#include "fpm_payload_dist.h"

struct fpm_payload_dist_member {
	const char *name;		/* into `archive`, NOT NUL-terminated */
	size_t name_len;
	const char *data;		/* into `archive` */
	size_t size;
};

/* Loaded once per process and kept for its lifetime. malloc(), not emalloc():
 * this outlives every request, and the wrapper reads it from whatever request
 * happens to open a file. */
static char *archive = NULL;
static size_t archive_size = 0;
static struct fpm_payload_dist_member *members = NULL;
static uint32_t member_count = 0;
static int registered = 0;

#define FPM_PAYLOAD_DIST_MAGIC "FPMNGAR1"
#define FPM_PAYLOAD_DIST_MAGIC_SIZE 8

static uint32_t fpm_payload_dist_u32(const unsigned char *p)
{
	return (uint32_t) p[0] | ((uint32_t) p[1] << 8) |
		((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24);
}

static uint64_t fpm_payload_dist_u64(const unsigned char *p)
{
	return (uint64_t) fpm_payload_dist_u32(p) |
		((uint64_t) fpm_payload_dist_u32(p + 4) << 32);
}

bool fpm_payload_dist_is_path(const char *path)
{
	return path && strncmp(path, FPM_PAYLOAD_DIST_SCHEME,
		sizeof(FPM_PAYLOAD_DIST_SCHEME) - 1) == 0;
}

static const char *fpm_payload_dist_member_name(const char *path)
{
	return path + sizeof(FPM_PAYLOAD_DIST_SCHEME) - 1;
}

/* Walks the index once and records where each member's name and data are. Every
 * bound is checked against the archive's own length rather than trusted: this
 * data comes from the end of a file anyone can append to, and the digest in the
 * payload record says it arrived intact, not that it was written by us. */
static int fpm_payload_dist_index(const char **why)
{
	const unsigned char *p = (const unsigned char *) archive;
	size_t at;
	uint32_t i;

	if (archive_size < FPM_PAYLOAD_DIST_MAGIC_SIZE + 4 ||
			memcmp(p, FPM_PAYLOAD_DIST_MAGIC, FPM_PAYLOAD_DIST_MAGIC_SIZE) != 0) {
		*why = "the distribution payload is not an archive this build understands";
		return -1;
	}
	member_count = fpm_payload_dist_u32(p + FPM_PAYLOAD_DIST_MAGIC_SIZE);
	if (member_count == 0) {
		*why = "the distribution payload contains no files";
		return -1;
	}
	/* 20 bytes is the smallest possible index record (name length, size,
	 * offset) with a one-byte name; the multiplication cannot overflow for any
	 * count that passes this. */
	if (member_count > (archive_size - FPM_PAYLOAD_DIST_MAGIC_SIZE - 4) / 21) {
		*why = "the distribution payload claims more files than it can hold";
		return -1;
	}
	members = calloc(member_count, sizeof(*members));
	if (!members) {
		*why = "out of memory";
		return -1;
	}
	at = FPM_PAYLOAD_DIST_MAGIC_SIZE + 4;
	for (i = 0; i < member_count; i++) {
		uint64_t size, offset;
		uint32_t name_len;

		if (archive_size - at < 20) {
			*why = "the distribution payload index is truncated";
			return -1;
		}
		name_len = fpm_payload_dist_u32(p + at);
		size = fpm_payload_dist_u64(p + at + 4);
		offset = fpm_payload_dist_u64(p + at + 12);
		at += 20;
		if (name_len == 0 || name_len > archive_size - at) {
			*why = "a file name in the distribution payload is out of range";
			return -1;
		}
		members[i].name = archive + at;
		members[i].name_len = name_len;
		at += name_len;
		if (size > archive_size || offset > archive_size - size) {
			*why = "a file in the distribution payload lies outside it";
			return -1;
		}
		members[i].data = archive + offset;
		members[i].size = (size_t) size;
	}
	return 0;
}

static int fpm_payload_dist_load(const char **why)
{
	struct fpm_payload_entry entry;
	const char *self;
	int found;

	if (archive) {
		return 0;
	}
	self = fpm_payload_self_path();
	found = fpm_payload_find(self, FPM_PAYLOAD_KIND_DISTRIBUTION, &entry, why);
	if (found < 0) {
		return -1;
	}
	if (found == 0) {
		/* Acceptance criterion 3: a build that embedded nothing has to say so
		 * here, at startup, rather than at the first ACME order. */
		*why = "this build carries no embedded distribution payload";
		return -1;
	}
	if (fpm_payload_read(self, &entry, &archive, &archive_size, why) < 0) {
		return -1;
	}
	if (fpm_payload_dist_index(why) < 0) {
		free(members);
		members = NULL;
		member_count = 0;
		free(archive);
		archive = NULL;
		archive_size = 0;
		return -1;
	}
	return 0;
}

static const struct fpm_payload_dist_member *fpm_payload_dist_member(const char *name)
{
	size_t len = strlen(name);
	uint32_t i;

	for (i = 0; i < member_count; i++) {
		if (members[i].name_len == len && memcmp(members[i].name, name, len) == 0) {
			return &members[i];
		}
	}
	return NULL;
}

int fpm_payload_dist_validate(const char *path, const char **why)
{
	*why = NULL;
	if (!fpm_payload_dist_is_path(path)) {
		*why = "not an embedded path";
		return -1;
	}
	if (fpm_payload_dist_load(why) < 0) {
		return -1;
	}
	if (!fpm_payload_dist_member(fpm_payload_dist_member_name(path))) {
		*why = "no such file in the embedded distribution payload";
		return -1;
	}
	return 0;
}

static php_stream *fpm_payload_dist_opener(php_stream_wrapper *wrapper, const char *path,
	const char *mode, int options, zend_string **opened_path,
	php_stream_context *context STREAMS_DC)
{
	const struct fpm_payload_dist_member *member;
	zend_string *buf;
	php_stream *stream;

	(void) context;
	if (strpbrk(mode, "wa+")) {
		/* NOTES.md:157-201, "Writes -- the biggest problem": embedded code is
		 * immutable, and state belongs on a volume. Refused with a message
		 * rather than silently opened read-only. */
		php_stream_wrapper_log_error(wrapper, options,
			"fpmng-dist:// is read-only: embedded files cannot be written to");
		return NULL;
	}
	if (!fpm_payload_dist_is_path(path)) {
		return NULL;
	}
	member = fpm_payload_dist_member(fpm_payload_dist_member_name(path));
	if (!member) {
		php_stream_wrapper_log_error(wrapper, options,
			"no such file in the embedded distribution payload");
		return NULL;
	}
	/* A copy per open, not a stream over the shared buffer: php_stream_memory
	 * owns the zend_string it is given and frees it on close, and the archive
	 * has to survive every open this process ever does. The embedded files are
	 * a few kilobytes of PHP each. */
	buf = zend_string_init(member->data, member->size, 0);
	stream = php_stream_memory_open(TEMP_STREAM_READONLY, buf);
	zend_string_release(buf);
	if (stream && opened_path) {
		*opened_path = zend_string_init(path, strlen(path), 0);
	}
	return stream;
}

static int fpm_payload_dist_url_stat(php_stream_wrapper *wrapper, const char *path, int flags,
	php_stream_statbuf *ssb, php_stream_context *context)
{
	const struct fpm_payload_dist_member *member;

	(void) wrapper;
	(void) flags;
	(void) context;
	if (!fpm_payload_dist_is_path(path)) {
		return -1;
	}
	member = fpm_payload_dist_member(fpm_payload_dist_member_name(path));
	if (!member) {
		return -1;
	}
	/* Enough for the two things PHP asks this wrapper: is_file()/file_exists()
	 * on an include path, and the size. A regular file, readable by everyone,
	 * with no times -- there are none: the archive stores no timestamps, and
	 * inventing one would make include caches believe something changed. */
	memset(ssb, 0, sizeof(*ssb));
	ssb->sb.st_mode = 0100444;
	ssb->sb.st_size = (zend_off_t) member->size;
	ssb->sb.st_nlink = 1;
	return 0;
}

static const php_stream_wrapper_ops fpm_payload_dist_wrapper_ops = {
	fpm_payload_dist_opener,
	NULL,	/* stream_closer: the memory stream closes itself */
	NULL,	/* stream_stat */
	fpm_payload_dist_url_stat,
	NULL,	/* dir_opener: the archive has no directories */
	"fpmng-dist",
	NULL,	/* unlink */
	NULL,	/* rename */
	NULL,	/* mkdir */
	NULL,	/* rmdir */
	NULL,	/* metadata */
};

static const php_stream_wrapper fpm_payload_dist_wrapper = {
	&fpm_payload_dist_wrapper_ops,
	NULL,
	0		/* is_url = 0: this is not the network, and allow_url_fopen /
			 * allow_url_include must not decide whether the project's own code
			 * can be loaded. */
};

int fpm_payload_dist_register(const char **why)
{
	*why = NULL;
	if (registered) {
		return 0;
	}
	if (fpm_payload_dist_load(why) < 0) {
		return -1;
	}
	if (php_register_url_stream_wrapper("fpmng-dist",
			(php_stream_wrapper *) &fpm_payload_dist_wrapper) == FAILURE) {
		*why = "the fpmng-dist:// stream wrapper could not be registered";
		return -1;
	}
	registered = 1;
	return 0;
}

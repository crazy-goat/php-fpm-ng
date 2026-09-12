/* fpm-ng: typed, append-only payloads carried by the binary itself (issue #171).
 *
 * WHAT THIS IS FOR
 *
 * docs/NOTES.md:157-201 (the self-runner) wants one file that contains the
 * application; docs/NOTES.md:721-728 wants the project-owned ACME client
 * shipped inside the binary rather than as "a file the operator must place
 * somewhere" -- a file that can be edited, replaced, or left at an old version
 * by a package upgrade. Both need the same thing: data appended after the end
 * of a finished ELF binary, found again at run time. This module is that
 * mechanism, and #171 builds it for the ACME case only; the application side
 * (the `pack` command) is a separate task.
 *
 * THE FORMAT, AND WHY IT IS A BACKWARDS-LINKED LIST
 *
 * Every entry is appended as its data followed immediately by a fixed-size
 * record, so the last record ends at the very end of the file:
 *
 *     [ELF binary][data A][record A][data B][record B]<EOF>
 *
 * A record names its own data and the offset of the record before it, which is
 * how the reader walks the chain: read the last FPM_PAYLOAD_RECORD_SIZE bytes,
 * check the magic, then follow prev_offset backwards. Lookup is BY KIND, not by
 * position (acceptance criterion 2), and the first match walking backwards --
 * the most recently appended one -- wins.
 *
 * A directory at the end, rewritten on every append, was the obvious
 * alternative and is the one this format rejects: it would mean rewriting bytes
 * that an earlier append already wrote, and criterion 4 asks for the opposite
 * guarantee -- appending an application payload leaves the distribution entry
 * byte-identical, digest included. A linked list appends and never rewrites.
 *
 * Little-endian on the wire, fixed widths, no padding read from the file: the
 * fields are parsed byte by byte, so the same binary payload is readable by a
 * build on any endianness rather than only on the machine that wrote it.
 *
 * WHAT THE DIGEST IS AND IS NOT (acceptance criterion 6)
 *
 * The SHA-256 in the record is checked against the data before anything is
 * handed to PHP, so a truncated download, a partial write, or a byte flipped by
 * bad storage is caught with a message that names the entry instead of a PHP
 * parse error. It is NOT a signature and makes nothing tamper-proof: anyone who
 * can rewrite the binary can rewrite the digest next to the data. Protecting
 * against that needs a key, which is deliberately out of scope for #171.
 *
 * strip: appended data is not part of any ELF section, so strip(1) drops it.
 * Embed AFTER stripping. build/package-apk.sh already builds with `!strip`
 * (options at :70) and build/package-deb.sh never strips.
 */

#ifndef FPM_PAYLOAD_H
#define FPM_PAYLOAD_H 1

#include "fpm_config.h"

#include <stddef.h>
#include <stdint.h>

/* Kinds. Values are on-disk format: never renumber, only append.
 *
 * DISTRIBUTION is the project's own code, put there by our build. APPLICATION
 * is the user's, put there by the future `pack` command. They are separate
 * kinds rather than two files in one archive precisely so that repacking an
 * application cannot replace ACME code. */
#define FPM_PAYLOAD_KIND_DISTRIBUTION 1u
#define FPM_PAYLOAD_KIND_APPLICATION  2u

/* "FPMNGPY" plus a format version digit. The version is in the magic, not in a
 * field, so a reader from a future format sees a mismatched magic and reports
 * "no payload" rather than parsing a record it does not understand. */
#define FPM_PAYLOAD_MAGIC "FPMNGPY1"
#define FPM_PAYLOAD_MAGIC_SIZE 8

/* magic 8 + kind 4 + flags 4 + data offset 8 + data size 8 + digest 32
 * + prev record offset 8. Flags is reserved, written as zero and required to be
 * zero: a reader that ignored unknown flags could not be told later that an
 * entry is, say, compressed. */
#define FPM_PAYLOAD_RECORD_SIZE 72
#define FPM_PAYLOAD_DIGEST_SIZE 32

struct fpm_payload_entry {
	uint32_t kind;
	uint64_t offset;			/* of the data, from the start of the file */
	uint64_t size;
	unsigned char digest[FPM_PAYLOAD_DIGEST_SIZE];
};

/* Finds the newest entry of `kind` in `path`. Returns 1 and fills `entry` when
 * there is one, 0 when the file carries no payload of that kind at all
 * (including a file with no payload, which is the normal unpacked binary), and
 * -1 on a payload that is present but broken -- a record pointing outside the
 * file, a chain that loops, an unreadable file. Only -1 is an error worth
 * reporting; `why` then points at a static description. */
int fpm_payload_find(const char *path, uint32_t kind, struct fpm_payload_entry *entry,
	const char **why);

/* Reads an entry found by fpm_payload_find() into a malloc()ed buffer and
 * verifies its SHA-256 before returning it. Returns 0 and sets `*data`/`*size`
 * (the buffer is NUL-terminated one byte past `*size`, so it can be used as a
 * C string when the entry holds text), or -1 with `*why` set. The caller
 * free()s. */
int fpm_payload_read(const char *path, const struct fpm_payload_entry *entry,
	char **data, size_t *size, const char **why);

/* The path of the running binary: /proc/self/exe where it exists, else the
 * argv[0] fpm-ng was started with (docs/NOTES.md:157-201 asks for both -- /proc
 * is mounted even in a scratch container, but a binary run outside one may have
 * no /proc at all). Returns a pointer into a static buffer, or NULL if neither
 * source produced a path that can be opened. */
const char *fpm_payload_self_path(void);

#endif

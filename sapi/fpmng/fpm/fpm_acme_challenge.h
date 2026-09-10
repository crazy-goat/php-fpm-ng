/* fpm-ng: the shared HTTP-01 challenge state.
 *
 * docs/NOTES.md section 3l settles who does what: the ACME client is
 * project-owned PHP run by a dedicated pool.type = cron process, and
 * "ISSUING adds exactly one token to the shared challenge state". This file
 * is that state, and nothing more -- it knows no ACME protocol, performs no
 * I/O and has no timer. One process publishes tokens; every gateway process
 * of every pool can look one up.
 *
 * Why shared memory and not a file under the document root: with
 * http.gateways > 1 the process that answers the CA's single challenge
 * request is not the process that created the token (SO_REUSEPORT decides,
 * and the CA gives no retry guarantee), and a key authorization must never
 * become a static file -- see issue #48, acceptance criteria 6 and 7.
 * A file would also make the answer depend on http.static, which criterion 4
 * forbids.
 *
 * Layout follows the one existing cross-process publication idiom in this
 * SAPI, fpm_http_tls_reload.c: two fixed-size slots plus one generation
 * counter, the writer always filling the slot that is NOT published and
 * bumping the counter afterwards. There is exactly one writer by
 * construction (the dedicated ACME process; issue #47 is what keeps it
 * exactly one), so the counter only has to be visible across processes, not
 * arbitrated between concurrent writers.
 *
 * The region is allocated once in the master before the first fork, whether
 * or not anything in the configuration uses ACME: it costs a couple of
 * kilobytes, and allocating it conditionally would mean fpm.c or a pool type
 * deciding on behalf of a facility that is global to the process tree.
 */

#ifndef FPM_ACME_CHALLENGE_H
#define FPM_ACME_CHALLENGE_H 1

#include <stddef.h>

/* An HTTP-01 token is base64url of at least 128 bits of entropy (RFC 8555
 * section 8.3); a key authorization is that token, a dot, and the base64url
 * SHA-256 of the account key thumbprint. Both are comfortably inside these
 * bounds, which are checked -- a value that does not fit is refused with an
 * error rather than silently truncated into an answer no CA would accept. */
#define FPM_ACME_CHALLENGE_TOKEN_MAX   128
#define FPM_ACME_CHALLENGE_KEYAUTH_MAX 512

/* More than one name may be validated in one order, and an order may be
 * retried while a previous authorization is still valid, so this is not 1. */
#define FPM_ACME_CHALLENGE_MAX 8

/* Master, before the first fork. Idempotent. 0 or -1. */
int fpm_acme_challenge_init_main(void);

/* Reader, any process. Copies the key authorization for `token` into `out`
 * and returns its length, or -1 when there is no such token (including when
 * the store was never initialised). `out` is NUL-terminated on success.
 *
 * `token` is compared as an opaque flat string: this function never treats
 * it as a path and never touches the filesystem. */
ssize_t fpm_acme_challenge_lookup(const char *token, char *out, size_t out_len);

/* Writer, the dedicated ACME process. Both return 0 or -1; -1 is already
 * logged. Setting a token that is already present replaces its key
 * authorization; clearing one that is absent succeeds. */
int fpm_acme_challenge_set(const char *token, const char *keyauth);
int fpm_acme_challenge_clear(const char *token);

/* Writer, the dedicated ACME process. Copies the currently published tokens
 * (not the key authorizations) into `out`, at most `max` of them, and
 * returns how many were copied. Each entry points into caller-owned storage
 * of FPM_ACME_CHALLENGE_TOKEN_MAX bytes at out[i]. */
size_t fpm_acme_challenge_tokens(char (*out)[FPM_ACME_CHALLENGE_TOKEN_MAX], size_t max);

/* Registers fpmng_acme_challenge_set/clear/list in `function_table`, for a
 * pool type whose publishes_acme_challenges flag is set. Call once per
 * process. 0 or -1. */
int fpm_acme_challenge_register_functions(void);

#endif

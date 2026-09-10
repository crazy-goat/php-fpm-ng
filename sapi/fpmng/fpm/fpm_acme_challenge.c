/* fpm-ng: the shared HTTP-01 challenge state, see fpm_acme_challenge.h. */

#include "fpm_config.h"

#include <php.h>
#include <zend_API.h>

#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#include "fpm_acme_challenge.h"
#include "fpm_shm.h"
#include "fpm_atomic.h"
#include "zlog.h"

struct fpm_acme_challenge_entry_s {
	size_t keyauth_len;
	char token[FPM_ACME_CHALLENGE_TOKEN_MAX];
	char keyauth[FPM_ACME_CHALLENGE_KEYAUTH_MAX];
};

struct fpm_acme_challenge_slot_s {
	size_t count;
	struct fpm_acme_challenge_entry_s entry[FPM_ACME_CHALLENGE_MAX];
};

struct fpm_acme_challenge_shared_s {
	/* index = generation % 2 is the slot currently in effect; see the header. */
	atomic_t generation;
	struct fpm_acme_challenge_slot_s slot[2];
};

static struct fpm_acme_challenge_shared_s *fpm_acme_challenge_shared = NULL;

int fpm_acme_challenge_init_main(void)
{
	if (fpm_acme_challenge_shared) {
		return 0;
	}
	/* fpm_shm_alloc() is anonymous mmap(MAP_SHARED), zero-filled by the
	 * kernel: generation 0, slot 0 published and empty, which is exactly
	 * "no challenge is active". */
	fpm_acme_challenge_shared = fpm_shm_alloc(sizeof(*fpm_acme_challenge_shared));
	if (!fpm_acme_challenge_shared) {
		zlog(ZLOG_ERROR, "acme: cannot allocate the shared challenge state");
		return -1;
	}
	return 0;
}

/* Reader ------------------------------------------------------------------ */

ssize_t fpm_acme_challenge_lookup(const char *token, char *out, size_t out_len)
{
	struct fpm_acme_challenge_shared_s *sh = fpm_acme_challenge_shared;
	int attempt;

	if (!sh || !token || !*token || !out || !out_len) {
		return -1;
	}

	/* The writer fills the unpublished slot and only then bumps the counter,
	 * so a reader that sees the same generation before and after its copy
	 * read a slot nobody was writing. Two publications inside one read is the
	 * only way to lose the race, and the ACME process publishes at most a
	 * handful of times per certificate lifetime -- a few attempts is
	 * generous, and giving up looks to the CA like a token that is not
	 * provisioned yet, which is a state it already has to tolerate. */
	for (attempt = 0; attempt < 4; attempt++) {
		unsigned long gen = (unsigned long) sh->generation;
		const struct fpm_acme_challenge_slot_s *slot = &sh->slot[gen % 2];
		size_t i, count = slot->count;

		if (count > FPM_ACME_CHALLENGE_MAX) {
			continue;			/* torn read of count itself */
		}
		for (i = 0; i < count; i++) {
			const struct fpm_acme_challenge_entry_s *e = &slot->entry[i];
			size_t len = e->keyauth_len;

			if (len == 0 || len >= FPM_ACME_CHALLENGE_KEYAUTH_MAX || len >= out_len) {
				continue;
			}
			/* strncmp against a fixed-size buffer that the writer always
			 * NUL-terminates; the token is public (it travels in the URL),
			 * so a timing-safe comparison would protect nothing. */
			if (strncmp(e->token, token, FPM_ACME_CHALLENGE_TOKEN_MAX) != 0) {
				continue;
			}
			memcpy(out, e->keyauth, len);
			out[len] = '\0';
			if ((unsigned long) sh->generation == gen) {
				return (ssize_t) len;
			}
			break;			/* republished under us: read the new slot */
		}
		if ((unsigned long) sh->generation == gen && i == count) {
			return -1;		/* a stable slot that does not have the token */
		}
	}
	return -1;
}

/* Writer ------------------------------------------------------------------ */

/* Copies the published slot into the unpublished one and hands it to the
 * caller to edit. The caller publishes with fpm_acme_challenge_publish(). */
static struct fpm_acme_challenge_slot_s *fpm_acme_challenge_draft(unsigned long *gen)
{
	struct fpm_acme_challenge_shared_s *sh = fpm_acme_challenge_shared;
	struct fpm_acme_challenge_slot_s *draft;

	*gen = (unsigned long) sh->generation;
	draft = &sh->slot[(*gen + 1) % 2];
	memcpy(draft, &sh->slot[*gen % 2], sizeof(*draft));
	return draft;
}

static void fpm_acme_challenge_publish(unsigned long gen)
{
	/* One writer by construction, so this cannot fail for contention; the
	 * atomic form is used to match the one other cross-process publication
	 * in this SAPI rather than to invent a second idiom. */
	atomic_cmp_set(&fpm_acme_challenge_shared->generation, gen, gen + 1);
}

int fpm_acme_challenge_set(const char *token, const char *keyauth)
{
	struct fpm_acme_challenge_slot_s *draft;
	unsigned long gen;
	size_t token_len, keyauth_len, i;

	if (!fpm_acme_challenge_shared) {
		zlog(ZLOG_ERROR, "acme: the shared challenge state is not available");
		return -1;
	}
	token_len = token ? strlen(token) : 0;
	keyauth_len = keyauth ? strlen(keyauth) : 0;
	if (!token_len || token_len >= FPM_ACME_CHALLENGE_TOKEN_MAX) {
		zlog(ZLOG_ERROR, "acme: a challenge token must be 1..%d bytes",
			FPM_ACME_CHALLENGE_TOKEN_MAX - 1);
		return -1;
	}
	/* A token reaches the gateway as one path segment. Refusing anything
	 * that is not flat here, at the only place a token enters the store,
	 * means the answering side never has to reason about it: see criterion 5
	 * of issue #48. */
	if (strchr(token, '/') || strchr(token, '%') || strchr(token, '?') || strchr(token, '#')) {
		zlog(ZLOG_ERROR, "acme: a challenge token is a flat opaque string, never a path");
		return -1;
	}
	/* Deliberately not logging keyauth_len's contents, here or anywhere:
	 * a key authorization is a secret for as long as the challenge is
	 * active. */
	if (!keyauth_len || keyauth_len >= FPM_ACME_CHALLENGE_KEYAUTH_MAX) {
		zlog(ZLOG_ERROR, "acme: the key authorization for token %s must be 1..%d bytes",
			token, FPM_ACME_CHALLENGE_KEYAUTH_MAX - 1);
		return -1;
	}

	draft = fpm_acme_challenge_draft(&gen);
	for (i = 0; i < draft->count; i++) {
		if (!strcmp(draft->entry[i].token, token)) {
			break;
		}
	}
	if (i == draft->count) {
		if (draft->count == FPM_ACME_CHALLENGE_MAX) {
			zlog(ZLOG_ERROR, "acme: at most %d challenges may be active at once",
				FPM_ACME_CHALLENGE_MAX);
			return -1;
		}
		draft->count++;
	}
	memset(&draft->entry[i], 0, sizeof(draft->entry[i]));
	memcpy(draft->entry[i].token, token, token_len + 1);
	memcpy(draft->entry[i].keyauth, keyauth, keyauth_len + 1);
	draft->entry[i].keyauth_len = keyauth_len;

	fpm_acme_challenge_publish(gen);
	return 0;
}

int fpm_acme_challenge_clear(const char *token)
{
	struct fpm_acme_challenge_slot_s *draft;
	unsigned long gen;
	size_t i;

	if (!fpm_acme_challenge_shared) {
		zlog(ZLOG_ERROR, "acme: the shared challenge state is not available");
		return -1;
	}
	if (!token || !*token) {
		return -1;
	}
	draft = fpm_acme_challenge_draft(&gen);
	for (i = 0; i < draft->count; i++) {
		if (strcmp(draft->entry[i].token, token)) {
			continue;
		}
		/* Move the last entry into the hole and shrink; order carries no
		 * meaning. The vacated tail is zeroed so the key authorization does
		 * not stay readable in shared memory after the challenge is over. */
		draft->entry[i] = draft->entry[draft->count - 1];
		memset(&draft->entry[draft->count - 1], 0, sizeof(draft->entry[0]));
		draft->count--;
		break;
	}
	fpm_acme_challenge_publish(gen);
	return 0;
}

size_t fpm_acme_challenge_tokens(char (*out)[FPM_ACME_CHALLENGE_TOKEN_MAX], size_t max)
{
	struct fpm_acme_challenge_shared_s *sh = fpm_acme_challenge_shared;
	const struct fpm_acme_challenge_slot_s *slot;
	unsigned long gen;
	size_t i, count;

	if (!sh || !out || !max) {
		return 0;
	}
	gen = (unsigned long) sh->generation;
	slot = &sh->slot[gen % 2];
	count = slot->count > max ? max : slot->count;
	for (i = 0; i < count; i++) {
		memcpy(out[i], slot->entry[i].token, FPM_ACME_CHALLENGE_TOKEN_MAX);
		out[i][FPM_ACME_CHALLENGE_TOKEN_MAX - 1] = '\0';
	}
	return count;
}

/* PHP-callable surface ---------------------------------------------------- */

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_fpmng_acme_challenge_set, 0, 2, _IS_BOOL, 0)
	ZEND_ARG_TYPE_INFO(0, token, IS_STRING, 0)
	ZEND_ARG_TYPE_INFO(0, key_authorization, IS_STRING, 0)
ZEND_END_ARG_INFO()

/* Makes one HTTP-01 token answerable by every gateway process of every pool.
 * Returns false, with the reason in error_log, for a token or key
 * authorization this store cannot hold. */
static ZEND_FUNCTION(fpmng_acme_challenge_set)
{
	zend_string *token, *keyauth;

	ZEND_PARSE_PARAMETERS_START(2, 2)
		Z_PARAM_STR(token)
		Z_PARAM_STR(keyauth)
	ZEND_PARSE_PARAMETERS_END();

	/* ZEND_STRVAL is NUL-terminated, but a PHP string may also contain an
	 * embedded NUL; such a value cannot be what the CA will ask for, and
	 * silently storing its prefix would answer the challenge wrongly. */
	if (ZSTR_LEN(token) != strlen(ZSTR_VAL(token)) ||
	    ZSTR_LEN(keyauth) != strlen(ZSTR_VAL(keyauth))) {
		zlog(ZLOG_ERROR, "acme: a challenge token or key authorization contains a NUL byte");
		RETURN_FALSE;
	}
	RETURN_BOOL(fpm_acme_challenge_set(ZSTR_VAL(token), ZSTR_VAL(keyauth)) == 0);
}

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_fpmng_acme_challenge_clear, 0, 1, _IS_BOOL, 0)
	ZEND_ARG_TYPE_INFO(0, token, IS_STRING, 0)
ZEND_END_ARG_INFO()

/* Removes a token. Succeeds whether or not it was present, so the ACME
 * client can clear unconditionally on both the success and the failure path. */
static ZEND_FUNCTION(fpmng_acme_challenge_clear)
{
	zend_string *token;

	ZEND_PARSE_PARAMETERS_START(1, 1)
		Z_PARAM_STR(token)
	ZEND_PARSE_PARAMETERS_END();

	RETURN_BOOL(fpm_acme_challenge_clear(ZSTR_VAL(token)) == 0);
}

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_fpmng_acme_challenge_list, 0, 0, IS_ARRAY, 0)
ZEND_END_ARG_INFO()

/* The currently published tokens. Key authorizations are deliberately not
 * returned: the publisher already has them, and nothing else needs them in
 * userland, where they would end up in a var_dump or a stack trace. */
static ZEND_FUNCTION(fpmng_acme_challenge_list)
{
	char tokens[FPM_ACME_CHALLENGE_MAX][FPM_ACME_CHALLENGE_TOKEN_MAX];
	size_t i, count;

	ZEND_PARSE_PARAMETERS_NONE();

	count = fpm_acme_challenge_tokens(tokens, FPM_ACME_CHALLENGE_MAX);
	array_init(return_value);
	for (i = 0; i < count; i++) {
		add_next_index_string(return_value, tokens[i]);
	}
}

static const zend_function_entry fpm_acme_challenge_functions[] = {
	ZEND_FE(fpmng_acme_challenge_set, arginfo_fpmng_acme_challenge_set)
	ZEND_FE(fpmng_acme_challenge_clear, arginfo_fpmng_acme_challenge_clear)
	ZEND_FE(fpmng_acme_challenge_list, arginfo_fpmng_acme_challenge_list)
	ZEND_FE_END
};

/* MODULE_TEMPORARY for exactly the reason fpm_http_direct_worker.c documents
 * at length for its own anchor: zend_register_functions() stores
 * EG(current_module) in every entry, a NULL there crashes opcache's
 * function_exists() folding, and claiming MODULE_PERSISTENT instead would let
 * that folding bake "this function exists" into an SHM cache entry keyed only
 * on script path -- which is wrong for a function set registered per fork, in
 * one pool type only. */
static zend_module_entry fpm_acme_challenge_module_entry = {
	.size = sizeof(zend_module_entry),
	.zend_api = ZEND_MODULE_API_NO,
	.zend_debug = ZEND_DEBUG,
	.zts = USING_ZTS,
	.name = "fpmng_acme_builtins",
	.type = MODULE_TEMPORARY,
	.build_id = ZEND_MODULE_BUILD_ID,
};

int fpm_acme_challenge_register_functions(void)
{
	zend_module_entry *saved_module = EG(current_module);
	zend_result result;

	EG(current_module) = &fpm_acme_challenge_module_entry;
	result = zend_register_functions(NULL, fpm_acme_challenge_functions,
		CG(function_table), MODULE_PERSISTENT);
	EG(current_module) = saved_module;
	return result == SUCCESS ? 0 : -1;
}

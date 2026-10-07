/* getnameinfo() for AF_UNIX peers on musl (issue #467).
 *
 * libevent's evhttp calls getnameinfo(NI_NUMERICHOST|NI_NUMERICSERV) on every
 * accepted connection (name_from_addr() in http.c), AF_UNIX peers included,
 * and on any failure it calls event_errx(1, "getnameinfo failed: ..."), which
 * exits the process. glibc answers for AF_UNIX; musl (Alpine) returns
 * EAI_FAMILY, so every evhttp pool listening on a unix socket -- http-direct
 * children and the gateway -- died on its first connection there. strace of
 * the child showed accept4() of an AF_UNIX peer, getnameinfo() failing, then
 * exit_group(1).
 *
 * This definition interposes the libc one inside libevent's shared object:
 * the binary is linked with -Wl,-E (build/libphp-build.sh), so its own
 * getnameinfo wins symbol resolution for every DSO loaded after it. The real
 * function is called first through dlsym(RTLD_NEXT), so glibc behaviour is
 * unchanged byte for byte; only EAI_FAMILY for an AF_UNIX address is turned
 * into what glibc itself answers with NI_NUMERICHOST: host "localhost", service
 * the socket path (empty for an unnamed peer).
 *
 * Rejected alternatives (see the issue): refusing unix listeners on musl,
 * and patching libevent, which we link from the distribution. Static musl
 * builds, where interposing cannot work, are retired (#424).
 *
 * Linux only. macOS has the same libc defect, but its two-level namespace
 * makes an interposed definition in the executable invisible to libevent.
 */

#if defined(__linux__)

#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif

#include <dlfcn.h>
#include <netdb.h>
#include <stddef.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>

typedef int (*fpmng_getnameinfo_fn)(const struct sockaddr *, socklen_t, char *, socklen_t,
		char *, socklen_t, int);

/* Copies src to dst, or reports that it does not fit, as glibc does (EAI_OVERFLOW). */
static int fpmng_gni_put(char *dst, socklen_t dstlen, const char *src, size_t len)
{
	if (len + 1 > dstlen) {
		return EAI_OVERFLOW;
	}
	memcpy(dst, src, len);
	dst[len] = '\0';
	return 0;
}

int getnameinfo(const struct sockaddr *sa, socklen_t salen, char *host, socklen_t hostlen,
		char *serv, socklen_t servlen, int flags)
{
	static fpmng_getnameinfo_fn real;
	int rc;

	if (real == NULL) {
		real = (fpmng_getnameinfo_fn) dlsym(RTLD_NEXT, "getnameinfo");
	}
	if (real == NULL) {
		return EAI_SYSTEM;
	}

	rc = real(sa, salen, host, hostlen, serv, servlen, flags);
	if (rc != EAI_FAMILY || sa == NULL || salen < sizeof(sa_family_t) || sa->sa_family != AF_UNIX) {
		return rc;
	}

	/* A peer that never bound has no path: salen covers the family only. */
	const size_t off = offsetof(struct sockaddr_un, sun_path);
	const char *path = "";
	size_t pathlen = 0;
	if (salen > off) {
		size_t room = salen - off;
		if (room > sizeof(((struct sockaddr_un *) 0)->sun_path)) {
			room = sizeof(((struct sockaddr_un *) 0)->sun_path);
		}
		path = ((const struct sockaddr_un *) sa)->sun_path;
		pathlen = strnlen(path, room);
	}

	if (host != NULL && hostlen > 0) {
		rc = fpmng_gni_put(host, hostlen, "localhost", sizeof("localhost") - 1);
		if (rc != 0) {
			return rc;
		}
	}
	if (serv != NULL && servlen > 0) {
		rc = fpmng_gni_put(serv, servlen, path, pathlen);
		if (rc != 0) {
			return rc;
		}
	}
	return 0;
}

#endif /* __linux__ */

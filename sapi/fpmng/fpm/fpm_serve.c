/* fpm-ng: `php-fpm-ng serve`, a zero-config dev server (issue #728).
 *
 * WHAT THIS DOES
 *
 * `php-fpm-ng serve [options]` builds a configuration in memory, writes it to a
 * private temporary directory and starts the ordinary master on it with
 * `-F -y <dir>/serve.conf`. It is a front end to the existing configuration
 * parser, not a second code path: everything the master does with a file it does
 * here, including SIGUSR2 reload (which re-reads the generated file) and the
 * `-t` checks. `--print-config` prints the same text and exits, so a user can
 * move to a real file later; the output passes `php-fpm-ng -t`.
 *
 * WHY IT WRAPS main()
 *
 * main() lives in the vendored sapi/fpm/fpm/fpm_main.c, which is pristine and
 * hash-checked (build/vendor-php-src.sh check), and its getopt loop refuses both
 * the word `serve` and the long options. build/libphp-build.sh therefore
 * compiles that file with -Dmain=fpmng_fpm_main and -DFPMNG_SERVE_WRAP is set
 * for this one, which defines the real main() below: it either hands the
 * argument vector on untouched or replaces it with `-F -y <generated file>`.
 * The from-source flow (build/prepare.sh) does not set the define, so this file
 * compiles to nothing there and `serve` is a Linux package feature.
 *
 * `pack` (fpm_pack.c, issue #429) is dispatched from the same main(): it is a
 * subcommand of the binary and needs the same hook ahead of the getopt loop.
 *
 * THE TEMPORARY DIRECTORY
 *
 * It holds serve.conf and, in the default mode, the private unix socket between
 * the gateway and the fastcgi pool. A reload execvp()s the same binary with the
 * rewritten argument vector, so the directory has to outlive it; the path is kept
 * in the environment (FPMNG_SERVE_DIR, which survives execvp()), and every
 * generation of the master arms an atexit() handler for it. The handler only
 * acts in the process that armed it: the pool children are forks and inherit the
 * handler, and must not remove the directory when they exit.
 */

#include "fpm_config.h"

#ifdef FPMNG_SERVE_WRAP

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "fpm_pack.h"

#define FPM_SERVE_DIR_ENV "FPMNG_SERVE_DIR"
#define FPM_SERVE_USAGE_EXIT 64 /* EX_USAGE, the same code upstream's usage exit uses */
/* sun_path is 108 bytes, and the socket name is appended to the directory. */
#define FPM_SERVE_DIR_MAX 80

extern int fpmng_fpm_main(int argc, char *argv[]);

enum fpm_serve_mode {
	FPM_SERVE_GATEWAY,
	FPM_SERVE_DIRECT,
	FPM_SERVE_WORKER,
};

struct fpm_serve_opts {
	const char *root;
	const char *listen;
	const char *front_controller;
	const char *worker_script;
	long workers;
	enum fpm_serve_mode mode;
	int print_config;
	/* what is handed on to the master unchanged: -n, -c <file>, -d <x=y>, -R */
	const char *passthrough[16];
	int passthrough_n;
};

static char *fpm_serve_dir;
static pid_t fpm_serve_dir_owner;

static void fpm_serve_usage(FILE *out)
{
	fputs("Usage: php-fpm-ng serve [options]\n"
		  "\n"
		  "Run a PHP application without a configuration file (development).\n"
		  "\n"
		  "  --root <dir>              document root (default: public/ if it exists, else .)\n"
		  "  --listen <addr>           host:port or port (default: 127.0.0.1:8080)\n"
		  "  --front-controller <file> script for paths that are not files (default: index.php)\n"
		  "  --workers <N>             PHP processes (default: the number of CPUs)\n"
		  "  --direct                  http-direct pool, classic executor, no FastCGI hop\n"
		  "  --worker <file>           http-direct pool, worker executor running <file> (implies --direct)\n"
		  "  --print-config            print the generated configuration and exit\n"
		  "  -n, -c <file>, -d <x=y>, -R   passed on to the master (PHP ini handling, run as root)\n"
		  "  -h, --help                this text\n"
		  "\n"
		  "Default: a gateway in front of a FastCGI pool on a private unix socket, like\n"
		  "nginx + php-fpm. Runs in the foreground; Ctrl-C stops it. It cannot be combined\n"
		  "with a configuration file (-y).\n",
			out);
}

static int fpm_serve_fail(const char *what, const char *arg)
{
	fprintf(stderr, "php-fpm-ng serve: %s%s%s\n", what, arg ? ": " : "", arg ? arg : "");
	fprintf(stderr, "Try 'php-fpm-ng serve --help'.\n");
	return FPM_SERVE_USAGE_EXIT;
}

/* "--name value" and "--name=value". Returns the value or NULL when argv[*i] is
 * not that option; *missing is set when it is but has no value. */
static const char *fpm_serve_value(int argc, char **argv, int *i, const char *name, int *missing)
{
	size_t n = strlen(name);
	const char *arg = argv[*i];

	if (strncmp(arg, name, n) != 0) {
		return NULL;
	}
	if (arg[n] == '=') {
		return arg + n + 1;
	}
	if (arg[n] != '\0') {
		return NULL;
	}
	if (*i + 1 >= argc) {
		*missing = 1;
		return NULL;
	}
	return argv[++*i];
}

static int fpm_serve_parse(int argc, char **argv, struct fpm_serve_opts *o)
{
	int i;

	for (i = 2; i < argc; i++) {
		const char *arg = argv[i], *v;
		int missing = 0;
		char *end;

		if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0) {
			fpm_serve_usage(stdout);
			exit(0);
		}
		if (strcmp(arg, "-y") == 0 || strncmp(arg, "--fpm-config", 12) == 0 || strncmp(arg, "--config", 8) == 0) {
			return fpm_serve_fail("serve builds its own configuration and cannot be combined with a configuration file", arg);
		}
		if (strcmp(arg, "--direct") == 0) {
			if (o->mode == FPM_SERVE_GATEWAY) {
				o->mode = FPM_SERVE_DIRECT;
			}
			continue;
		}
		if (strcmp(arg, "--print-config") == 0) {
			o->print_config = 1;
			continue;
		}
		if ((strcmp(arg, "-n") == 0 || strcmp(arg, "-R") == 0) && o->passthrough_n < 15) {
			o->passthrough[o->passthrough_n++] = arg;
			continue;
		}
		if ((strcmp(arg, "-c") == 0 || strcmp(arg, "-d") == 0) && o->passthrough_n < 14) {
			if (i + 1 >= argc) {
				return fpm_serve_fail("option needs a value", arg);
			}
			o->passthrough[o->passthrough_n++] = arg;
			o->passthrough[o->passthrough_n++] = argv[++i];
			continue;
		}

		if ((v = fpm_serve_value(argc, argv, &i, "--root", &missing)) != NULL) {
			o->root = v;
		} else if (!missing && (v = fpm_serve_value(argc, argv, &i, "--listen", &missing)) != NULL) {
			o->listen = v;
		} else if (!missing && (v = fpm_serve_value(argc, argv, &i, "--front-controller", &missing)) != NULL) {
			o->front_controller = v;
		} else if (!missing && (v = fpm_serve_value(argc, argv, &i, "--workers", &missing)) != NULL) {
			errno = 0;
			o->workers = strtol(v, &end, 10);
			if (errno || *end || end == v || o->workers < 1 || o->workers > 1024) {
				return fpm_serve_fail("--workers wants a number from 1 to 1024", v);
			}
		} else if (!missing && (v = fpm_serve_value(argc, argv, &i, "--worker", &missing)) != NULL) {
			o->worker_script = v;
			o->mode = FPM_SERVE_WORKER;
		} else if (missing) {
			return fpm_serve_fail("option needs a value", arg);
		} else {
			return fpm_serve_fail("unknown option", arg);
		}
		if (v != NULL && *v == '\0') {
			return fpm_serve_fail("empty value for", arg);
		}
	}
	return 0;
}

static int fpm_serve_is_dir(const char *path)
{
	struct stat st;

	return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static int fpm_serve_is_file(const char *path)
{
	struct stat st;

	return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

/* A bare port number means the loopback interface: the dev server is never
 * public unless the user writes an address. */
static void fpm_serve_listen_addr(const char *in, char *out, size_t out_size)
{
	const char *p;

	for (p = in; *p && isdigit((unsigned char) *p); p++) {
	}
	if (*in && *p == '\0') {
		snprintf(out, out_size, "127.0.0.1:%s", in);
	} else if (in[0] == ':') {
		snprintf(out, out_size, "127.0.0.1%s", in);
	} else {
		snprintf(out, out_size, "%s", in);
	}
}

/* The master replaces its standard output with /dev/null before the pools open
 * their files (fpm_stdio_init_main()), so /dev/stdout would be a black hole:
 * the error log and the access logs both go to /dev/stderr. */

/* The values go into the generated file inside double quotes. The ini scanner
 * cannot represent a quote, a backslash, a newline or a ${ there, and the master
 * expands $pool in string settings after parsing, so those are
 * refused instead of being written wrongly. */
static int fpm_serve_check_value(const char *what, const char *v)
{
	if (strpbrk(v, "\"\\\n\r") != NULL || strstr(v, "${") != NULL || strstr(v, "$pool") != NULL) {
		return fpm_serve_fail(what, v);
	}
	return 0;
}

/* The gateway only logs a warning when its port is taken and keeps running
 * without a listener, which is useless for a dev server: try the bind first. */
static int fpm_serve_probe_listen(const char *listen)
{
	char host[256], *colon;
	const char *port, *h;
	struct addrinfo hints = { .ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM, .ai_flags = AI_PASSIVE }, *res, *ai;
	int fd, err = 0, one = 1, rc;

	if (strchr(listen, '/') != NULL || strlen(listen) >= sizeof(host)) {
		return 0;
	}
	snprintf(host, sizeof(host), "%s", listen);
	colon = strrchr(host, ':');
	if (colon == NULL) {
		return 0;
	}
	*colon = '\0';
	port = colon + 1;
	h = host;
	if (h[0] == '[') {
		size_t l = strlen(h);

		if (l > 1 && h[l - 1] == ']') {
			host[l - 1] = '\0';
			h++;
		}
	}
	if (strcmp(h, "*") == 0 || *h == '\0') {
		h = NULL;
	}
	if ((rc = getaddrinfo(h, port, &hints, &res)) != 0) {
		fprintf(stderr, "php-fpm-ng serve: cannot resolve the listen address %s: %s\n", listen, gai_strerror(rc));
		return 1;
	}
	for (ai = res; ai != NULL; ai = ai->ai_next) {
		fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
		if (fd < 0) {
			continue;
		}
		setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
		if (bind(fd, ai->ai_addr, ai->ai_addrlen) == 0) {
			close(fd);
			freeaddrinfo(res);
			return 0;
		}
		err = errno;
		close(fd);
	}
	freeaddrinfo(res);
	if (err != 0) {
		fprintf(stderr, "php-fpm-ng serve: cannot listen on %s: %s\n", listen, strerror(err));
		return 1;
	}
	return 0;
}

/* The text of serve.conf. `socket_path` is the fastcgi pool's unix socket. */
static char *fpm_serve_build_config(const struct fpm_serve_opts *o, const char *root, const char *listen, const char *front_controller,
		long workers, const char *socket_path, const char *pid_path, size_t *len)
{
	char *buf = NULL;
	FILE *f = open_memstream(&buf, len);
	long start, max_spare;

	if (f == NULL) {
		return NULL;
	}

	fprintf(f,
			"; Generated by `php-fpm-ng serve`. Save it as a file and run it with\n"
			"; `php-fpm-ng -y <file>` to leave the dev server behind.\n"
			"[global]\n"
			"daemonize = no\n"
			"pid = \"%s\"\n"
			"error_log = /dev/stderr\n"
			"\n",
			pid_path);

	if (o->mode == FPM_SERVE_GATEWAY) {
		start = workers < 2 ? workers : 2;
		max_spare = workers < 3 ? workers : 3;
		fprintf(f,
				"; The public port: static files from the root, everything else to the pool below.\n"
				"; One gateway process: each gateway holds its own connections to the pool, so two of them would starve a one-worker pool.\n"
				"[gateway]\n"
				"pool.type = gateway\n"
				"listen = \"%s\"\n"
				"chdir = \"%s\"\n"
				"http.gateways = 1\n"
				"http.static = yes\n"
				"http.front_controller = \"%s\"\n"
				"http.route[app] = /\n"
				"http.access_log = /dev/stderr\n"
				"operator.status = off\n"
				"operator.metrics = off\n"
				"\n"
				"; The PHP workers: an ordinary FastCGI pool on a private unix socket.\n"
				"[app]\n"
				"pool.type = fastcgi\n"
				"listen = \"%s\"\n"
				"listen.mode = 0600\n"
				"chdir = \"%s\"\n"
				"catch_workers_output = yes\n"
				"pm = dynamic\n"
				"pm.max_children = %ld\n"
				"pm.start_servers = %ld\n"
				"pm.min_spare_servers = 1\n"
				"pm.max_spare_servers = %ld\n",
				listen, root, front_controller, socket_path, root, workers, start, max_spare);
	} else {
		fprintf(f,
				"; One pool that speaks HTTP itself (no gateway, no FastCGI hop).\n"
				"[app]\n"
				"pool.type = http-direct\n"
				"pool.executor = %s\n"
				"listen = \"%s\"\n"
				"chdir = \"%s\"\n"
				"http.front_controller = \"%s\"\n"
				"catch_workers_output = yes\n"
				"pm = static\n"
				"pm.max_children = %ld\n",
				o->mode == FPM_SERVE_WORKER ? "worker" : "classic", listen, root, front_controller, workers);
		if (o->mode == FPM_SERVE_WORKER) {
			/* The worker script runs for the life of the process; the master refuses a pool that leaves the default 30 s limit on it. */
			fputs("php_admin_value[max_execution_time] = 0\n", f);
		}
		if (o->mode == FPM_SERVE_DIRECT) {
			/* The worker executor serves no static files and refuses access.* */
			fputs("http.static = yes\naccess.log = /dev/stderr\n", f);
		}
	}

	if (fclose(f) != 0) {
		free(buf);
		return NULL;
	}
	return buf;
}

static void fpm_serve_cleanup(void)
{
	char path[PATH_MAX];

	if (fpm_serve_dir == NULL || getpid() != fpm_serve_dir_owner) {
		return;
	}
	snprintf(path, sizeof(path), "%s/serve.conf", fpm_serve_dir);
	unlink(path);
	snprintf(path, sizeof(path), "%s/serve.pid", fpm_serve_dir);
	unlink(path);
	snprintf(path, sizeof(path), "%s/php.sock", fpm_serve_dir);
	unlink(path);
	rmdir(fpm_serve_dir);
}

static void fpm_serve_arm_cleanup(const char *dir)
{
	if (fpm_serve_dir != NULL) {
		return;
	}
	fpm_serve_dir = strdup(dir);
	fpm_serve_dir_owner = getpid();
	if (fpm_serve_dir != NULL) {
		atexit(fpm_serve_cleanup);
	}
}

static int fpm_serve_main(int argc, char **argv)
{
	struct fpm_serve_opts o = { .listen = "127.0.0.1:8080", .front_controller = "index.php", .mode = FPM_SERVE_GATEWAY };
	char root[PATH_MAX], listen[256], fc[PATH_MAX], dir[PATH_MAX + 32], conf_path[PATH_MAX + 64], sock_path[PATH_MAX + 64], tmpreal[PATH_MAX], pid_path[PATH_MAX + 64];
	char *text, *new_argv[24];
	const char *given_root, *tmp;
	size_t len;
	int rc, n, i;
	FILE *f;

	if ((rc = fpm_serve_parse(argc, argv, &o)) != 0) {
		return rc;
	}

	given_root = o.root ? o.root : (fpm_serve_is_dir("public") ? "public" : ".");
	if (!fpm_serve_is_dir(given_root) || realpath(given_root, root) == NULL) {
		return fpm_serve_fail("the document root is not a directory", given_root);
	}

	fpm_serve_listen_addr(o.listen, listen, sizeof(listen));

	/* http.front_controller is relative to chdir, spelled with a leading slash. */
	{
		const char *name = o.mode == FPM_SERVE_WORKER ? o.worker_script : o.front_controller;
		char file[2 * PATH_MAX];

		snprintf(fc, sizeof(fc), "%s%s", name[0] == '/' ? "" : "/", name);
		snprintf(file, sizeof(file), "%s%s", root, fc);
		if (!fpm_serve_is_file(file)) {
			if (o.mode == FPM_SERVE_WORKER) {
				return fpm_serve_fail("the worker script is not a file under the document root", file);
			}
			if (o.mode == FPM_SERVE_DIRECT) {
				return fpm_serve_fail("the front controller is not a file under the document root", file);
			}
			fprintf(stderr, "php-fpm-ng serve: warning: the front controller %s does not exist\n", file);
		}
	}

	if ((rc = fpm_serve_check_value("the document root cannot be written into a configuration file", root)) != 0 || (rc = fpm_serve_check_value("the listen address cannot be written into a configuration file", listen)) != 0 || (rc = fpm_serve_check_value("the front controller cannot be written into a configuration file", fc)) != 0) {
		return rc;
	}

	if (o.workers == 0) {
		long cpus = sysconf(_SC_NPROCESSORS_ONLN);

		o.workers = cpus > 0 ? cpus : 1;
	}

	if (o.print_config) {
		/* No directory is created for a print: the socket path is a stand-in. */
		text = fpm_serve_build_config(&o, root, listen, fc, o.workers, "/tmp/php-fpm-ng-serve.sock", "/tmp/php-fpm-ng-serve.pid", &len);
		if (text == NULL) {
			return fpm_serve_fail("out of memory", NULL);
		}
		fwrite(text, 1, len, stdout);
		free(text);
		return 0;
	}

	if (fpm_serve_probe_listen(listen) != 0) {
		return 1;
	}

	/* The master resolves a relative path against its prefix, so the directory must be absolute. */
	tmp = getenv("TMPDIR");
	if (tmp == NULL || *tmp == '\0' || realpath(tmp, tmpreal) == NULL || strlen(tmpreal) > FPM_SERVE_DIR_MAX - 30) {
		snprintf(tmpreal, sizeof(tmpreal), "/tmp");
	}
	tmp = tmpreal;
	snprintf(dir, sizeof(dir), "%s/php-fpm-ng-serve-XXXXXX", tmp);
	if (mkdtemp(dir) == NULL) {
		fprintf(stderr, "php-fpm-ng serve: cannot create a directory under %s: %s\n", tmp, strerror(errno));
		return 1;
	}
	fpm_serve_arm_cleanup(dir);
	if ((rc = fpm_serve_check_value("the temporary directory cannot be written into a configuration file", dir)) != 0) {
		return rc;
	}
	setenv(FPM_SERVE_DIR_ENV, dir, 1);

	snprintf(conf_path, sizeof(conf_path), "%s/serve.conf", dir);
	snprintf(sock_path, sizeof(sock_path), "%s/php.sock", dir);
	snprintf(pid_path, sizeof(pid_path), "%s/serve.pid", dir);
	text = fpm_serve_build_config(&o, root, listen, fc, o.workers, sock_path, pid_path, &len);
	f = text ? fopen(conf_path, "w") : NULL;
	if (f == NULL || fwrite(text, 1, len, f) != len || fclose(f) != 0) {
		fprintf(stderr, "php-fpm-ng serve: cannot write %s: %s\n", conf_path, strerror(errno));
		free(text);
		return 1;
	}
	free(text);

	fprintf(stderr, "php-fpm-ng serve: %s on http://%s, root %s, %ld worker%s (Ctrl-C to stop)\n"
					"php-fpm-ng serve: master pid %ld, pid file %s (kill -USR2 reloads, kill -TERM stops)\n",
			o.mode == FPM_SERVE_GATEWAY ? "gateway + fastcgi" : (o.mode == FPM_SERVE_WORKER ? "http-direct, worker executor" : "http-direct, classic executor"), listen,
			root, o.workers, o.workers == 1 ? "" : "s", (long) getpid(), pid_path);

	n = 0;
	new_argv[n++] = argv[0];
	for (i = 0; i < o.passthrough_n; i++) {
		new_argv[n++] = (char *) o.passthrough[i];
	}
	new_argv[n++] = (char *) "-F";
	new_argv[n++] = (char *) "-y";
	new_argv[n++] = conf_path;
	new_argv[n] = NULL;
	return fpmng_fpm_main(n, new_argv);
}

int main(int argc, char *argv[])
{
	const char *dir;

	if (argc >= 2 && strcmp(argv[1], "serve") == 0) {
		return fpm_serve_main(argc, argv);
	}
	if (argc >= 2 && strcmp(argv[1], "pack") == 0) {
		return fpm_pack_main(argc, argv);
	}
	/* A master that reloaded itself: its argument vector is already the
	 * generated one, and the directory is still ours to remove. */
	dir = getenv(FPM_SERVE_DIR_ENV);
	if (dir != NULL && *dir != '\0' && fpm_serve_is_dir(dir)) {
		fpm_serve_arm_cleanup(dir);
	}
	return fpmng_fpm_main(argc, argv);
}

#else

typedef int fpm_serve_disabled_in_this_build;

#endif /* FPMNG_SERVE_WRAP */

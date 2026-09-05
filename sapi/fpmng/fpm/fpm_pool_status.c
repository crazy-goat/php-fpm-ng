/* fpm-ng: pool.type = status.
 *
 * Patrz fpm_pool_status.h i docs/NOTES.md sekcja 3u dla uzasadnienia
 * projektowego. W skrocie: to jedyny typ poola, ktory nie odpala PHP w
 * ogole — dziecko robi wlasna petle accept na wlasnym gnieznie (ten sam
 * mechanizm co listen dla fcgi/http, ale nasluchujacy BEZPOSREDNIO, bez
 * fcgi+1 jak bramka http) i na kazde polaczenie odpowiada surowym HTTP,
 * bez przechodzenia przez PHP ani FastCGI.
 *
 * KSZTALT DANYCH JEST INNY dla roznych typow poola (to jest sedno zadania,
 * nie szczegol) — rozgalezienie po fpm_pool_type_s.serves_requests:
 *   - serves_requests = 1 (fcgi, http): idle/active workers, requests —
 *     czytane bezposrednio ze scoreboardu tamtego poola (fpm_scoreboard_copy(),
 *     ten sam mechanizm co istniejacy fpm_status.c uzywa dla WLASNEGO poola —
 *     tu czytamy scoreboard CUDZEGO poola, z INNEGO procesu, stad kopia
 *     zamiast bezposredniego czytania pod lockiem).
 *   - serves_requests = 0 (supervisor, cron): stan/last_start/exit_code/
 *     consecutive_failures(/next_run dla crona) przez fpm_pool_type_s.status(),
 *     ktore kazdy taki typ implementuje we WLASNYM pliku, czytajac WLASNA
 *     pamiec dzielona (fpm_pool_supervisor.c, fpm_pool_cron.c). Ten plik nie
 *     zna wewnetrznej struktury tamtych stanow — dokladnie tak, jak wymaga
 *     kontrakt z docs/NOTES.md 3h.
 *   - typy bez .status i serves_requests = 0 (czyli "status" samo siebie) —
 *     pomijane w wyjsciu w ogole, bez specjalnego traktowania po nazwie.
 *
 * Etykieta metryk to nazwa poola — kardynalnosc ograniczona z natury (liczba
 * poolow w configu). Zadnych etykiet o nieograniczonej kardynalnosci (bez
 * request path, bez timestampu jako label, itp).
 */

#include "fpm_config.h"

#include <sys/socket.h>
#include <sys/types.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>

#include "fpm.h"
#include "fpm_conf.h"
#include "fpm_worker_pool.h"
#include "fpm_pool_status.h"
#include "fpm_pool_type.h"
#include "fpm_scoreboard.h"
#include "zlog.h"

/* Lista ODRZUCEN, nie dopuszczen — patrz fpm_pool_type_check_directives()
 * w fpm_pool_type.c. W odroznieniu od supervisor/cron, "listen"/"listen."
 * NIE sa tu odrzucane: status faktycznie nasluchuje, na wlasnym porcie.
 * Reszta to ten sam zestaw powodow co supervisor/cron: brak requestow
 * FastCGI (wiec brak sensu dla dyrektyw requestowych, ping., access.), brak
 * PHP (wiec pm.* generowane programowo, zawsze static+1 — nie ma tu
 * dyrektywy "ile procesow", jeden proces w zupelnosci wystarcza do obslugi
 * scrapow monitoringu), oraz dyrektywy DRUGICH typow poola. */
/* Limit na recv()/send() dla pojedynczego polaczenia — patrz uzasadnienie
 * przy setsockopt() w fpm_pool_status_child_main(). */
#define FPM_POOL_STATUS_IO_TIMEOUT_SEC 5

const char *const fpm_pool_status_rejects[] = {
	"pm",
	"pm.",
	"request_terminate_timeout",
	"request_terminate_timeout_track_finished",
	"request_slowlog_timeout",
	"request_slowlog_trace_depth",
	"slowlog",
	"ping.",
	"access.",
	"security.limit_extensions",
	"supervisor.",
	"cron.",
	NULL
};

int fpm_pool_status_validate(struct fpm_worker_pool_s *wp) /* {{{ */
{
	/* Jeden proces w zupelnosci wystarcza: to lekki, sekwencyjny serwer HTTP
	 * dla scrapow monitoringu (Prometheus typowo co 15-60s), nie ruch
	 * publiczny. Brak wlasnej dyrektywy "ile procesow" jest swiadome — gdyby
	 * ktos kiedys potrzebowal wiecej, to bedzie decyzja projektowa (nowa
	 * dyrektywa status.processes, wzorem supervisor.processes), nie domysl. */
	wp->config->pm = PM_STYLE_STATIC;
	wp->config->pm_max_children = 1;

	return 0;
}
/* }}} */

/* Bufor rosnacy — liczba poolow jest z natury mala (rozmiar configu), wiec
 * prostota (realloc x2) jest tu wazniejsza niz unikanie paru alokacji. */
struct fpm_status_buf_s {
	char *data;
	size_t len;
	size_t cap;
};

static void fpm_status_buf_free(struct fpm_status_buf_s *b) /* {{{ */
{
	free(b->data);
	b->data = NULL;
	b->len = b->cap = 0;
}
/* }}} */

static void fpm_status_buf_appendf(struct fpm_status_buf_s *b, const char *fmt, ...) /* {{{ */
{
	for (;;) {
		size_t avail = b->cap - b->len;
		va_list ap;
		int n;

		va_start(ap, fmt);
		n = vsnprintf(b->data ? b->data + b->len : NULL, avail, fmt, ap);
		va_end(ap);

		if (n < 0) {
			return;
		}
		if ((size_t) n < avail) {
			b->len += (size_t) n;
			return;
		}

		{
			size_t new_cap = b->cap ? b->cap * 2 : 256;
			char *new_data;

			while (new_cap < b->len + (size_t) n + 1) {
				new_cap *= 2;
			}
			new_data = realloc(b->data, new_cap);
			if (!new_data) {
				return;
			}
			b->data = new_data;
			b->cap = new_cap;
		}
	}
}
/* }}} */

static const char *fpm_pool_status_state_name(enum fpm_pool_state_e state) /* {{{ */
{
	switch (state) {
		case FPM_POOL_STATE_RUNNING:  return "running";
		case FPM_POOL_STATE_BACKOFF:  return "backoff";
		case FPM_POOL_STATE_GAVE_UP:  return "gave_up";
		case FPM_POOL_STATE_FINISHED: return "finished";
		case FPM_POOL_STATE_IDLE:     return "idle";
	}
	return "unknown";
}
/* }}} */

/* Wspolne dla obu formatow: dla kazdego poola w mastrze, w kolejnosci
 * configu, zawola callback z gotowymi danymi — albo scoreboardowymi
 * (serves_requests), albo z fpm_pool_type_s.status() (reszta). Pool bez
 * .status i serves_requests = 0 (czyli "status" samo siebie) jest pomijany —
 * nie ma czego pokazac, i nie ma potrzeby specjalnego wykrywania po nazwie. */
struct fpm_pool_status_row_s {
	const char *name;
	int serves_requests;

	/* serves_requests = 1 */
	int idle, active;
	unsigned long requests;

	/* serves_requests = 0 */
	struct fpm_pool_status_s st;
};

typedef void (*fpm_pool_status_row_cb)(struct fpm_status_buf_s *b, const struct fpm_pool_status_row_s *row, int first);

static void fpm_pool_status_collect_and_render(struct fpm_status_buf_s *b, fpm_pool_status_row_cb cb) /* {{{ */
{
	struct fpm_worker_pool_s *wp;
	int first = 1;

	for (wp = fpm_worker_all_pools; wp; wp = wp->next) {
		const struct fpm_pool_type_s *type = fpm_pool_type_of(wp);
		struct fpm_pool_status_row_s row;

		memset(&row, 0, sizeof(row));
		row.name = wp->config->name;
		row.serves_requests = type->serves_requests;

		if (type->serves_requests) {
			struct fpm_scoreboard_s *copy = fpm_scoreboard_copy(wp->scoreboard, 0);

			if (!copy) {
				continue;
			}
			row.idle = copy->idle;
			row.active = copy->active;
			row.requests = copy->requests;
			fpm_scoreboard_free_copy(copy);
		} else if (type->status) {
			type->status(wp, &row.st);
		} else {
			/* Typ bez sensownego stanu do pokazania (dzisiaj: "status" samo
			 * siebie) — pomijamy, zamiast zgadywac. */
			continue;
		}

		cb(b, &row, first);
		first = 0;
	}
}
/* }}} */

static void fpm_pool_status_row_prometheus(struct fpm_status_buf_s *b, const struct fpm_pool_status_row_s *row, int first) /* {{{ */
{
	(void) first;

	if (row->serves_requests) {
		fpm_status_buf_appendf(b, "fpmng_pool_workers_idle{pool=\"%s\"} %d\n", row->name, row->idle);
		fpm_status_buf_appendf(b, "fpmng_pool_workers_active{pool=\"%s\"} %d\n", row->name, row->active);
		fpm_status_buf_appendf(b, "fpmng_pool_requests_total{pool=\"%s\"} %lu\n", row->name, row->requests);
	} else {
		fpm_status_buf_appendf(b, "fpmng_pool_state{pool=\"%s\"} %d\n", row->name, (int) row->st.state);
		fpm_status_buf_appendf(b, "fpmng_pool_last_start_seconds{pool=\"%s\"} %ld\n", row->name, (long) row->st.last_start);
		fpm_status_buf_appendf(b, "fpmng_pool_last_exit_code{pool=\"%s\"} %d\n", row->name, row->st.last_exit_code);
		fpm_status_buf_appendf(b, "fpmng_pool_consecutive_failures{pool=\"%s\"} %u\n", row->name, row->st.consecutive_failures);
		if (row->st.next_run) {
			fpm_status_buf_appendf(b, "fpmng_pool_next_run_seconds{pool=\"%s\"} %ld\n", row->name, (long) row->st.next_run);
		}
	}
}
/* }}} */

static void fpm_pool_status_render_prometheus(struct fpm_status_buf_s *b) /* {{{ */
{
	fpm_status_buf_appendf(b,
		"# HELP fpmng_pool_workers_idle Idle worker processes (pools that serve requests).\n"
		"# TYPE fpmng_pool_workers_idle gauge\n"
		"# HELP fpmng_pool_workers_active Active worker processes (pools that serve requests).\n"
		"# TYPE fpmng_pool_workers_active gauge\n"
		"# HELP fpmng_pool_requests_total Requests served since start (pools that serve requests).\n"
		"# TYPE fpmng_pool_requests_total counter\n"
		"# HELP fpmng_pool_state Pool state: 0=running 1=backoff 2=gave_up 3=finished 4=idle (pools that do not serve requests).\n"
		"# TYPE fpmng_pool_state gauge\n"
		"# HELP fpmng_pool_last_start_seconds Unix time of the last start, 0 = never (pools that do not serve requests).\n"
		"# TYPE fpmng_pool_last_start_seconds gauge\n"
		"# HELP fpmng_pool_last_exit_code Exit code of the last finished run (pools that do not serve requests).\n"
		"# TYPE fpmng_pool_last_exit_code gauge\n"
		"# HELP fpmng_pool_consecutive_failures Consecutive failed runs (pools that do not serve requests).\n"
		"# TYPE fpmng_pool_consecutive_failures gauge\n"
		"# HELP fpmng_pool_next_run_seconds Unix time of the next scheduled run, cron only (0 = not applicable).\n"
		"# TYPE fpmng_pool_next_run_seconds gauge\n");
	fpm_pool_status_collect_and_render(b, fpm_pool_status_row_prometheus);
}
/* }}} */

static void fpm_pool_status_row_json(struct fpm_status_buf_s *b, const struct fpm_pool_status_row_s *row, int first) /* {{{ */
{
	if (!first) {
		fpm_status_buf_appendf(b, ",");
	}

	if (row->serves_requests) {
		fpm_status_buf_appendf(b,
			"{\"name\":\"%s\",\"serves_requests\":true,"
			"\"idle\":%d,\"active\":%d,\"requests\":%lu}",
			row->name, row->idle, row->active, row->requests);
	} else {
		fpm_status_buf_appendf(b,
			"{\"name\":\"%s\",\"serves_requests\":false,"
			"\"state\":\"%s\",\"last_start\":%ld,\"last_exit_code\":%d,"
			"\"consecutive_failures\":%u,\"next_run\":%ld}",
			row->name, fpm_pool_status_state_name(row->st.state),
			(long) row->st.last_start, row->st.last_exit_code,
			row->st.consecutive_failures, (long) row->st.next_run);
	}
}
/* }}} */

static void fpm_pool_status_render_json(struct fpm_status_buf_s *b) /* {{{ */
{
	fpm_status_buf_appendf(b, "{\"pools\":[");
	fpm_pool_status_collect_and_render(b, fpm_pool_status_row_json);
	fpm_status_buf_appendf(b, "]}\n");
}
/* }}} */

/* Surowy, minimalny serwer HTTP — celowo bez keep-alive, bez chunked, bez
 * parsowania naglowkow. To endpoint monitoringu (Prometheus scrape co
 * 15-60s), nie serwer WWW; kazde polaczenie to jeden request, jedna
 * odpowiedz, zamkniecie. Interesuje nas WYLACZNIE pierwsza linia
 * ("GET <path> HTTP/1.x"). */
static void fpm_pool_status_handle_conn(int fd) /* {{{ */
{
	char req[4096];
	ssize_t n;
	size_t total = 0;
	char path[256] = "";
	struct fpm_status_buf_s body = {0};
	const char *content_type = "text/plain; charset=utf-8";
	int status_code = 200;
	const char *status_text = "OK";
	char header[256];
	int header_len;

	/* Jedno albo kilka recv(), az zobaczymy koniec pierwszej linii albo
	 * zapelnimy bufor — wystarczy, "GET /metrics HTTP/1.1\r\n" miesci sie
	 * z ogromnym zapasem. Reszta requestu (naglowki, ewentualne cialo) jest
	 * ignorowana — i tak jej nie parsujemy. */
	while (total < sizeof(req) - 1) {
		n = recv(fd, req + total, sizeof(req) - 1 - total, 0);
		if (n <= 0) {
			if (n < 0 && errno == EINTR) {
				continue;
			}
			break;
		}
		total += (size_t) n;
		req[total] = '\0';
		if (strstr(req, "\r\n") || strstr(req, "\n")) {
			break;
		}
	}

	if (total == 0) {
		return;
	}
	req[total] = '\0';

	{
		char method[16];
		if (sscanf(req, "%15s %255s", method, path) != 2) {
			path[0] = '\0';
		}
	}

	if (!strcmp(path, "/metrics")) {
		fpm_pool_status_render_prometheus(&body);
	} else if (!strcmp(path, "/status")) {
		content_type = "application/json";
		fpm_pool_status_render_json(&body);
	} else {
		status_code = 404;
		status_text = "Not Found";
		fpm_status_buf_appendf(&body, "not found: %s\nknown paths: /metrics /status\n",
			path[0] ? path : "(unparseable request)");
	}

	header_len = snprintf(header, sizeof(header),
		"HTTP/1.1 %d %s\r\n"
		"Content-Type: %s\r\n"
		"Content-Length: %zu\r\n"
		"Connection: close\r\n"
		"\r\n",
		status_code, status_text, content_type, body.len);

	if (header_len > 0) {
		(void) write(fd, header, (size_t) header_len);
	}
	if (body.data && body.len) {
		(void) write(fd, body.data, body.len);
	}

	fpm_status_buf_free(&body);
}
/* }}} */

void fpm_pool_status_child_main(struct fpm_worker_pool_s *wp) /* {{{ */
{
	int listen_fd = wp->listening_socket;

	/* Brak wlasnej obslugi sygnalow, celowo: SIGTERM ma tu domyslna
	 * dyspozycje (fpm_signals_child_init() ustawia ja dla dzieci przed
	 * run_child:) — proces po prostu konczy sie natychmiast, co jest
	 * poprawne, bo status nie ma zadnej "biezacej pracy" do dokonczenia
	 * (kazde polaczenie jest obslugiwane w pelni w jednym accept()-cyklu,
	 * nigdy nie trwa dluzej niz pojedynczy recv/send). Ewentualne opoznienie
	 * przy SIGQUIT (graceful) do eskalacji SIGTERM przez mastera to znane,
	 * zaakceptowane zachowanie tego samego rodzaju co dla supervisor/cron
	 * bez wlasnego uchwytu na SIGQUIT — patrz docs/NOTES.md 3p, scenariusz 2. */

	for (;;) {
		int fd = accept(listen_fd, NULL, NULL);
		struct timeval tv;

		if (fd < 0) {
			if (errno == EINTR) {
				continue;
			}
			/* Bledy accept() na gniazdach TCP/UDS sa zwykle przejsciowe
			 * (np. ECONNABORTED) — nie ma sensu konczyc caly proces
			 * z powodu jednego nieudanego polaczenia. */
			continue;
		}

		/* Klient, ktory otworzy polaczenie i nigdy nic nie wysle (albo nie
		 * czyta odpowiedzi), zawiesilby JEDYNY proces tego poola na zawsze —
		 * pm.max_children jest tu zawsze 1 (patrz validate()), wiec nie ma
		 * innego workera, ktory by przejal ruch w tym czasie. Timeout na
		 * recv() I send(), nie tylko na accept(). */
		tv.tv_sec = FPM_POOL_STATUS_IO_TIMEOUT_SEC;
		tv.tv_usec = 0;
		setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
		setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

		fpm_pool_status_handle_conn(fd);
		close(fd);
	}
}
/* }}} */

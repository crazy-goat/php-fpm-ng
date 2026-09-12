#!/usr/bin/env python3
"""Process-manager measurement harness for http-direct pools (issue #163, spike #66).

#66 asks two questions that `build/benchmark-http-direct.py` cannot answer: what
one idle keep-alive connection costs a direct worker in RSS, and what happens to
the connections a worker holds when it is retired. That harness is parameterised
over transports and uses `wrk`, which cannot hold idle keep-alive connections
beside a separate request stream, cannot say which connections were closed with
no reply, and merges every outcome into one latency histogram.

This one is parameterised over (executor, pm, spare/children settings, TLS, idle
connection count N, workload) and speaks HTTP itself, so every metric #163 lists
is measured rather than derived:

  rss_child_bytes                  per child, sampled at the configured N
  t_exit / t_last_response_byte /  all from one t0, the harness's own timestamp
  t_last_conn_closed /             for the SIGQUIT that starts the retirement
  t_replacement_accepting
  n_conn_closed_without_response   requests in flight when the socket died
  n_client_visible_failures        5xx, resets and timeouts during the retirement
                                   window only, never mixed with steady state
  latency                          successful-only and non-2xx, reported apart

The decision rule these numbers are judged against is in
`build/benchmark-http-direct-pm.md` and was written before the first run.

Shared box rules, same as the other harnesses here: own scratch directory, own
port range, teardown by the pid file this process wrote. Nothing is ever matched
by binary name.
"""
import argparse
import collections
import errno
import hashlib
import json
import os
from pathlib import Path
import resource
import selectors
import signal
import socket
import ssl
import statistics
import subprocess
import threading
import time

# The response body is the worker pid and nothing else, so every record can say
# which child served it. That is what makes "the connections this child holds"
# a measurable set rather than an assumption.
FRONT_CONTROLLER = """<?php
$sleep = (int) ($_GET['sleep'] ?? 0);
if ($sleep > 0) {
    usleep($sleep * 1000);
}
header('Content-Type: text/plain');
header('Content-Length: ' . (strlen((string) getmypid()) + 1));
echo getmypid(), "\\n";
"""

# Deliberately free of Composer, Revolt and amphp: examples/http-direct-worker/
# needs all three, and a harness that has to `composer install` on the poligon
# before it can measure anything is a harness that will not be run. The calls
# below are the same primitive set sapi/fpmng/tests/fpmng-http-direct-worker.phpt
# pins, so this script measures the executor rather than a driver on top of it.
WORKER_SCRIPT = """<?php
$notify = fpmng_worker_notify_stream();

function handle(int $id): void
{
    $env = fpmng_worker_request_env($id);
    $query = [];
    parse_str((string) ($env['QUERY_STRING'] ?? ''), $query);
    $sleep = (int) ($query['sleep'] ?? 0);
    if ($sleep > 0) {
        usleep($sleep * 1000);
    }
    fpmng_worker_respond($id, 200, ['Content-Type' => 'text/plain'], getmypid() . "\\n");
}

$watcher = fpmng_worker_event_create(FPMNG_WORKER_READ, $notify, function () use ($notify): void {
    /* Level-triggered: the pipe carries no payload, only "look at the queue",
     * and an undrained pipe fires this watcher on every loop iteration. */
    fread($notify, 65536);
    while (($id = fpmng_worker_next_request()) !== null) {
        handle($id);
    }
});
fpmng_worker_event_enable($watcher);

while (!fpmng_worker_may_exit()) {
    fpmng_worker_loop(true);
}
"""

# fpm_process_ctl.c:536 -- the master's idle-server maintenance pass runs once a
# second, and fpm_pctl_kill_idle_child() (:342-350) escalates SIGQUIT to SIGKILL
# on the pass after the one that sent it. A master-driven retirement whose
# t_exit lands at or past this bound measured the SIGKILL, not a drain, and the
# drain time it appears to report is the escalation delay instead.
SIGKILL_FLOOR_MS = 1000.0

# The literal as it appears in .rodata: fpm_http_direct_request.c:105 logs
# "[pool %s] %s requires pm = static", so the subject is a format argument and
# the rendered sentence is never in the binary.
DIRECT_MARKER = "requires pm = static"


def verify_binary(binary):
    """Refuse anything that is not an fpm-ng binary with the direct transport.

    A stock php-fpm would answer every request in this harness perfectly well
    over its own listener and the numbers would look plausible, which is the
    failure mode worth an explicit check.
    """
    data = binary.read_bytes()
    strings = subprocess.check_output(["strings", str(binary)], text=True)
    if DIRECT_MARKER not in strings:
        raise SystemExit(f"refusing {binary}: no http-direct marker ({DIRECT_MARKER!r}) in the binary")
    return hashlib.sha256(data).hexdigest()


class PortRange:
    """Holds every port of the range bound until the pool that wants it starts.

    The box is shared; discovering the collision from a half-started pool means
    reading someone else's error log to find out. Binding and immediately
    closing -- the obvious version -- only checks the range at t=0, and the last
    scenario of a twelve-row run starts minutes later: a port taken in between
    comes back as "pool never listened", which reads like a broken binary.
    SO_REUSEADDR lets our own pool bind the port we are still holding, so
    release() right before start() has no window at all.
    """

    def __init__(self, ports):
        self.held = {}
        for port in ports:
            s = socket.socket()
            s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            try:
                s.bind(("127.0.0.1", port))
            except OSError as exc:
                self.close()
                raise SystemExit(f"port {port} is taken ({exc}); pick another --base-port")
            self.held[port] = s

    def release(self, port):
        s = self.held.pop(port, None)
        if s is not None:
            s.close()

    def close(self):
        for s in self.held.values():
            s.close()
        self.held.clear()


def raise_nofile(wanted):
    """N idle connections need N descriptors on this side too.

    Only the soft limit is touched, and only upwards to the hard limit: a run
    that silently measured 1024 connections as 200 would be worse than one that
    refused to start.
    """
    soft, hard = resource.getrlimit(resource.RLIMIT_NOFILE)
    if soft >= wanted:
        return soft
    target = min(hard, max(wanted, soft))
    resource.setrlimit(resource.RLIMIT_NOFILE, (target, hard))
    if target < wanted:
        raise SystemExit(f"need {wanted} descriptors, the hard limit is {hard}")
    return target


def pss_bytes(pid):
    """Proportional set size: the figure the pm = dynamic decision rests on.

    VmRSS counts libphp's text and every mapped extension once per child, so
    summing it over a pool credits a retired child with tens of MB of shared
    pages that were never duplicated and are not returned when it dies. Pss
    divides each shared page by its sharer count, which is what actually comes
    back. VmRSS is still recorded next to it: it is the number an operator sees
    in top, and the gap between the two is itself worth reading.
    """
    try:
        for line in open(f"/proc/{pid}/smaps_rollup"):
            if line.startswith("Pss:"):
                return int(line.split()[1]) * 1024
    except OSError:
        pass
    return None


def children_of(pid):
    """Direct children of the master, from /proc.

    Walking /proc/<pid>/task/<pid>/children rather than scanning every process
    for a parent: the box is shared and the scan would see work that is not
    ours.
    """
    try:
        raw = Path(f"/proc/{pid}/task/{pid}/children").read_text()
    except OSError:
        return []
    return [int(p) for p in raw.split()]


def rss_bytes(pid):
    try:
        for line in Path(f"/proc/{pid}/status").read_text().splitlines():
            if line.startswith("VmRSS:"):
                return int(line.split()[1]) * 1024
    except OSError:
        return None
    return None


def alive(pid):
    return Path(f"/proc/{pid}").exists()


class Client:
    """One HTTP/1.1 keep-alive connection, spoken by hand over a raw socket.

    http.client cannot do what this harness needs: a connection that stays open
    with nothing in flight, is watched for EOF by a selector together with a
    thousand others, and reports the exact monotonic timestamp of its last
    received byte. All three are load-bearing for the retirement metrics.
    """

    def __init__(self, port, tls_context=None, timeout=30.0):
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=timeout)
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        if tls_context is not None:
            self.sock = tls_context.wrap_socket(self.sock, server_hostname="localhost")
        self.buffer = b""
        self.pid = None
        self.in_flight = False
        self.responses = 0
        self.last_byte = None
        self.closed_at = None
        self.closed_without_response = False

    def send(self, sleep_ms=0):
        path = f"/?sleep={sleep_ms}" if sleep_ms else "/"
        request = (f"GET {path} HTTP/1.1\r\nHost: localhost\r\n"
                   "Connection: keep-alive\r\nAccept: */*\r\n\r\n").encode()
        self.in_flight = True
        self.started = time.monotonic()
        self.sock.sendall(request)

    def _parse(self):
        """Return one complete response from the buffer, or None.

        Only what this harness's own front controller can produce is handled:
        a status line, headers, and either Content-Length or a connection
        close. Chunked is not parsed because nothing here emits it -- a body
        that arrived chunked would be a finding, so it is reported as one
        rather than quietly decoded.
        """
        head, sep, rest = self.buffer.partition(b"\r\n\r\n")
        if not sep:
            return None
        lines = head.split(b"\r\n")
        status = int(lines[0].split()[1])
        headers = {}
        for line in lines[1:]:
            name, _, value = line.partition(b":")
            headers[name.strip().lower()] = value.strip()
        if b"transfer-encoding" in headers:
            raise RuntimeError("chunked response from a harness front controller")
        length = int(headers.get(b"content-length", b"0"))
        if len(rest) < length:
            return None
        body, self.buffer = rest[:length], rest[length:]
        return status, headers, body

    def recv(self):
        """Feed whatever is readable into the parser. Returns a record or None.

        A record is also produced for a peer close with a request in flight --
        that is `n_conn_closed_without_response`, the number the spike most
        needs, and it exists only as the absence of a reply.
        """
        try:
            chunk = self.sock.recv(65536)
        except (ssl.SSLWantReadError, TimeoutError, socket.timeout):
            # Nothing to read yet. A timeout is how the caller keeps its own
            # deadline: the socket's timeout is always the shorter of the two,
            # so control comes back here rather than parking past t0 + limit.
            return None
        except OSError as exc:
            chunk = b""
            if exc.errno not in (errno.ECONNRESET, errno.ETIMEDOUT, errno.EPIPE):
                raise
        now = time.monotonic()
        if not chunk:
            self.closed_at = now
            if self.in_flight:
                self.closed_without_response = True
                self.in_flight = False
                return {"status": None, "outcome": "closed_without_response",
                        "ms": (now - self.started) * 1000, "at": now, "pid": None}
            return None
        self.buffer += chunk
        self.last_byte = now
        parsed = self._parse()
        if parsed is None:
            return None
        status, _, body = parsed
        self.in_flight = False
        self.responses += 1
        pid = None
        if status == 200 and body.split():
            try:
                pid = int(body.split()[0])
            except ValueError:
                pid = None
        if self.pid is None:
            self.pid = pid
        return {"status": status, "outcome": "ok" if status == 200 else "non2xx",
                "ms": (now - self.started) * 1000, "at": now, "pid": pid}

    def exchange(self, sleep_ms=0, deadline=20.0):
        self.send(sleep_ms)
        end = time.monotonic() + deadline
        while True:
            # Re-armed every iteration so a silent peer cannot hold this call
            # past `deadline`; the connection's own timeout is only an upper
            # bound and is typically much longer.
            self.sock.settimeout(max(0.05, min(0.5, end - time.monotonic())))
            record = self.recv()
            if record is not None:
                return record
            if self.closed_at is not None:
                return {"status": None, "outcome": "closed_without_response",
                        "ms": (time.monotonic() - self.started) * 1000,
                        "at": time.monotonic(), "pid": None}
            if time.monotonic() > end:
                self.in_flight = False
                return {"status": None, "outcome": "timeout",
                        "ms": deadline * 1000, "at": time.monotonic(), "pid": None}

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass


class Pool:
    """One configured fpm-ng pool in its own directory, on its own port."""

    def __init__(self, binary, root, name, port, config_text):
        self.binary = binary
        self.directory = root / name
        self.directory.mkdir(parents=True, exist_ok=True)
        self.port = port
        self.config = self.directory / "fpm.conf"
        self.config.write_text(config_text)
        self.pid_file = self.directory / "fpm.pid"
        self.log = self.directory / "fpm.log"
        self.stderr = self.directory / "stderr.log"
        self.process = None

    def start(self, tls_context=None, seconds=20.0):
        """Start, and report a configuration refusal as a result, not a crash.

        Today's binary refuses `pm = dynamic` and `pm = ondemand` for an
        http-direct pool (fpm_http_direct_request.c:105). That refusal is one of
        the answers this spike is collecting, so it has to come back as data.
        """
        handle = open(self.stderr, "w")
        self.process = subprocess.Popen(
            [str(self.binary), "-n", "-F", "-y", str(self.config)],
            stdout=handle, stderr=handle,
            # Own session, so teardown can reach the workers too: SIGKILL to a
            # wedged master leaves its children alive, still holding the
            # inherited listening socket, and the port stays bound on a shared
            # box long after the harness exits.
            start_new_session=True)
        self._log_handle = handle
        deadline = time.monotonic() + seconds
        while True:
            if self.process.poll() is not None:
                return {"started": False, "refused": True,
                        "exit_code": self.process.returncode,
                        "error": self._refusal_reason()}
            try:
                client = Client(self.port, tls_context, timeout=2.0)
            except OSError:
                if time.monotonic() > deadline:
                    return {"started": False, "refused": False,
                            "error": f"pool on port {self.port} never listened"}
                time.sleep(0.05)
                continue
            try:
                record = client.exchange(deadline=5.0)
            finally:
                client.close()
            if record["status"] == 200:
                return {"started": True, "refused": False}
            if time.monotonic() > deadline:
                return {"started": False, "refused": False,
                        "error": f"first request answered {record['outcome']}"}
            time.sleep(0.05)

    def _refusal_reason(self):
        lines = []
        for path in (self.log, self.stderr):
            if path.exists():
                lines += [l for l in path.read_text().splitlines()
                          if "ERROR" in l or "ALERT" in l]
        return lines[-3:] if lines else ["(no ERROR/ALERT line in the pool log)"]

    def master_pid(self):
        if self.pid_file.exists():
            try:
                return int(self.pid_file.read_text().strip())
            except ValueError:
                pass
        return self.process.pid if self.process else None

    def children(self):
        pid = self.master_pid()
        return sorted(children_of(pid)) if pid else []

    def stop(self):
        """Teardown by our own pid file. Never by binary name: the box is shared."""
        pid = self.master_pid()
        if pid:
            try:
                os.kill(pid, signal.SIGTERM)
            except OSError:
                pass
        if self.process:
            try:
                self.process.wait(timeout=15)
            except subprocess.TimeoutExpired:
                # A master wedged in shutdown is exactly the state a retirement
                # bug produces, and it is the state that orphans workers, so the
                # escalation goes to the whole process group -- never to a
                # pattern matching the binary name, which would hit other users
                # of this box.
                try:
                    os.killpg(os.getpgid(self.process.pid), signal.SIGKILL)
                except OSError:
                    self.process.kill()
                self.process.wait(timeout=5)
        if getattr(self, "_log_handle", None):
            self._log_handle.close()


def percentile(values, fraction):
    if not values:
        return None
    ordered = sorted(values)
    index = min(len(ordered) - 1, int(len(ordered) * fraction))
    return round(ordered[index], 3)


def latency_split(records):
    """Successful-only and non-2xx, never merged.

    docs/http-direct.md calls out `wrk`'s mixed-outcome percentile as a limit of
    the old harness: a run that answers half its requests with an instant 503
    looks *faster* than one that serves them all.
    """
    ok = [r["ms"] for r in records if r["outcome"] == "ok"]
    bad = [r["ms"] for r in records if r["outcome"] == "non2xx"]
    return {
        "n_ok": len(ok),
        "ok_p50_ms": percentile(ok, 0.50),
        "ok_p99_ms": percentile(ok, 0.99),
        "n_non2xx": len(bad),
        "non2xx_p50_ms": percentile(bad, 0.50),
        "non2xx_p99_ms": percentile(bad, 0.99),
    }


class IdleSet:
    """N keep-alive connections, each one request old and then silent.

    One request first, because an accepted-but-never-used connection says
    nothing about which child holds it -- and "which child holds it" is what
    turns the retirement measurement from an average into an observation.
    """

    def __init__(self, port, count, tls_context):
        self.clients = []
        self.by_pid = collections.defaultdict(list)
        for _ in range(count):
            client = Client(port, tls_context)
            record = client.exchange()
            if record["status"] != 200:
                self.close()
                raise RuntimeError(f"idle connection got {record['outcome']} instead of a pid")
            self.clients.append(client)
            self.by_pid[client.pid].append(client)

    def close(self):
        for client in self.clients:
            client.close()


class Stream:
    """A small pool of threads issuing requests back to back on its own connections.

    Separate from the idle set on purpose: the whole point of the harness is
    that traffic keeps flowing while N connections sit idle, which is exactly
    what a single `wrk` run cannot express.

    `mode` is not a detail. Under keep-alive every thread stays pinned to the
    child that first answered it, because all children accept on the same
    inherited listening socket and a newly spawned one gets no share of
    connections that are already open (#53). A scale-up measured that way looks
    useless by construction -- an artefact of the client, not a result -- so
    `new-connection` closes after each request and makes the load a stream of
    accepts, which is the only shape in which a new child can take work.
    """

    def __init__(self, port, connections, tls_context, sleep_ms, mode="keepalive"):
        self.port = port
        self.tls = tls_context
        self.sleep_ms = sleep_ms
        self.mode = mode
        self.records = []
        self._lock = threading.Lock()
        self._stop = threading.Event()
        self._threads = [threading.Thread(target=self._run, daemon=True)
                         for _ in range(connections)]

    def _run(self):
        client = None
        while not self._stop.is_set():
            try:
                if client is None:
                    client = Client(self.port, self.tls, timeout=10.0)
                record = client.exchange(self.sleep_ms, deadline=10.0)
            except OSError as exc:
                record = {"status": None, "outcome": "connect_error", "ms": 0.0,
                          "at": time.monotonic(), "pid": None, "error": repr(exc)}
                client = None
                # Without this a dead pool has every thread spinning at memory
                # speed: n_client_visible_failures reaches six figures for a
                # single retirement and records grows without bound, so the
                # rule-3 counter would report the harness, not the server.
                self._stop.wait(0.01)
            if self.mode == "new-connection" and client is not None:
                client.close()
                client = None
            if record["outcome"] != "ok":
                # A connection that produced anything but a 200 is not reused:
                # its framing state is unknown, and reusing it would attribute
                # the next failure to the wrong cause.
                if client is not None:
                    client.close()
                    client = None
            with self._lock:
                self.records.append(record)
        if client is not None:
            client.close()

    def start(self):
        for t in self._threads:
            t.start()

    def stop(self):
        self._stop.set()
        for t in self._threads:
            t.join(timeout=15)

    def snapshot(self):
        with self._lock:
            return list(self.records)


def failures_between(records, t0, t1):
    """Client-visible failures inside one window.

    Counted over a window rather than a whole run because #163 asks for the
    failures a scale-down causes to be separable from whatever the pool does in
    steady state; a single total cannot answer that.
    """
    window = [r for r in records if t0 <= r["at"] <= t1]
    return {
        "n_client_visible_failures": sum(1 for r in window if r["outcome"] != "ok"),
        "n_window_requests": len(window),
        "failures_by_outcome": dict(collections.Counter(
            r["outcome"] for r in window if r["outcome"] != "ok")),
    }


def retire(pool, idle_set, stream, inflight_ms, grace_ms, timeout, sig):
    """Retire one child and time everything that follows from a single t0.

    The signal is sent by the harness, not waited for from the master, for one
    reason: t0 has to be exact, and the master's SIGQUIT has no observable
    timestamp from outside. It is the same signal the master sends
    (fpm_pctl_kill_idle_child -> FPM_PCTL_QUIT, fpm_process_ctl.c:342-350), so
    the child-side path under measurement is identical. SIGUSR1 is the other
    retirement worth measuring -- the operator-driven one from issue #65, which
    keeps serving the connections it holds instead of refusing what arrives --
    and `--retire-signal` selects it; the row records which one produced it. What is NOT reproduced
    is the master's escalation to SIGKILL one maintenance pass later: that only
    follows a kill the master itself started. A t_exit past SIGKILL_FLOOR_MS is
    therefore a slow drain here and would be a SIGKILL under a real scale-down;
    both are flagged, and the caller is told which mode produced the row.
    """
    before = set(pool.children())
    target = max(idle_set.by_pid, key=lambda p: len(idle_set.by_pid[p]),
                 default=None) if idle_set.by_pid else None
    if target is None or target not in before:
        # N = 0 is a scenario, not a mistake: with no idle set there is still a
        # child to retire, it just has no held connections, so the connection
        # counters are trivially zero and t_exit is the drain of the request
        # stream alone. Picked from the stream's own records so it is a child
        # that is demonstrably serving.
        serving = [r["pid"] for r in stream.snapshot()[-200:] if r["pid"] in before]
        target = serving[-1] if serving else (sorted(before)[0] if before else None)
        if target is None:
            return {"error": "the pool has no children to retire"}
    held = idle_set.by_pid.get(target, [])

    # A few of the held connections get a slow request just before t0, so the
    # child is retiring with work in flight. Without this the drain path is
    # never exercised and t_last_response_byte would only ever be "never".
    inflight = held[: min(4, len(held))]
    for client in inflight:
        client.send(inflight_ms)

    t0 = time.monotonic()
    os.kill(target, sig)

    selector = selectors.DefaultSelector()
    for client in held:
        # Readable at the TCP level is not the same as "has application data"
        # on a TLS connection, where a readable socket may carry only a
        # handshake record. A short timeout keeps that case from blocking the
        # one loop that times the whole retirement.
        client.sock.settimeout(0.2)
        selector.register(client.sock, selectors.EVENT_READ, client)

    t_exit = None
    t_last_response_byte = None
    t_last_conn_closed = None
    open_conns = set(held)
    deadline = t0 + timeout
    while time.monotonic() < deadline and (open_conns or t_exit is None):
        events = selector.select(timeout=0.01)
        if not events:
            # select() over an empty registration set returns at once, so
            # without this the loop stats /proc in a tight spin for the whole
            # timeout -- and it does so while Stream latency is being recorded
            # into this very window, which would measure the harness.
            time.sleep(0.005)
        for key, _ in events:
            client = key.data
            record = None
            try:
                # One readiness event can carry several TLS records: recv()
                # returns the plaintext of one, and the rest sit in OpenSSL's
                # BIO with the fd no longer readable. Stopping after one read
                # loses a response tail or a close_notify on exactly the TLS
                # rows this spike needs.
                while True:
                    got = client.recv()
                    if got is not None:
                        record = got
                    sock = client.sock
                    if not (isinstance(sock, ssl.SSLSocket) and sock.pending()):
                        break
            except OSError:
                record = None
                client.closed_at = time.monotonic()
            if record and record["outcome"] in ("ok", "non2xx"):
                t_last_response_byte = record["at"]
            if client.closed_at is not None and client in open_conns:
                open_conns.discard(client)
                t_last_conn_closed = client.closed_at
                try:
                    selector.unregister(client.sock)
                except (KeyError, ValueError):
                    pass
        if t_exit is None and not alive(target):
            t_exit = time.monotonic()
        if t_exit is not None and not open_conns:
            break
    selector.close()

    # The replacement is "accepting" the moment a request is answered by a pid
    # the pool did not have before, which is the only definition a client can
    # verify. A pid appearing in /proc proves a fork, not a listener.
    t_replacement_accepting = None
    replacement = None
    # Its own deadline: the drain loop may have used the whole grace, and the
    # master forks the replacement only after it reaps the child.
    replacement_deadline = time.monotonic() + 10.0
    # Probed with a fresh connection each pass rather than read off the stream:
    # a stream thread only reconnects when its own connection failed, and the
    # retired child need not have held any stream connection at all, so a pool
    # whose replacement was accepting in 20 ms would still report null here.
    # The stream is kept as a corroborating source, not the only one.
    while time.monotonic() < replacement_deadline and t_replacement_accepting is None:
        for record in stream.snapshot():
            if record["at"] > t0 and record["pid"] and record["pid"] not in before:
                t_replacement_accepting = record["at"]
                replacement = record["pid"]
                break
        if t_replacement_accepting is not None:
            break
        probe = None
        try:
            probe = Client(stream.port, stream.tls, timeout=1.0)
            record = probe.exchange(deadline=1.0)
            if record["pid"] and record["pid"] not in before:
                t_replacement_accepting = record["at"]
                replacement = record["pid"]
        except OSError:
            pass
        finally:
            if probe is not None:
                probe.close()
        if t_replacement_accepting is None:
            time.sleep(0.02)

    t_end = time.monotonic()
    result = {
        "retired_pid": target,
        "signal": signal.Signals(sig).name,
        "drain_grace_ms": grace_ms,
        "replacement_pid": replacement,
        "idle_conns_on_target": len(held),
        "inflight_requests_at_t0": len(inflight),
        "t_exit_ms": round((t_exit - t0) * 1000, 3) if t_exit else None,
        "t_last_response_byte_ms": (round((t_last_response_byte - t0) * 1000, 3)
                                    if t_last_response_byte else None),
        "t_last_conn_closed_ms": (round((t_last_conn_closed - t0) * 1000, 3)
                                  if t_last_conn_closed else None),
        "t_replacement_accepting_ms": (round((t_replacement_accepting - t0) * 1000, 3)
                                       if t_replacement_accepting else None),
        "n_conn_closed_without_response": sum(1 for c in held if c.closed_without_response),
        "n_conn_closed_while_idle": sum(1 for c in held
                                        if c.closed_at is not None and not c.closed_without_response),
        "n_conn_still_open": len(open_conns),
    }
    result |= failures_between(stream.snapshot(), t0, t_end)
    result["sigkill_floor_ms"] = SIGKILL_FLOOR_MS
    # A child that left exactly at the grace did not drain: it gave up waiting
    # for the connections it still held (fpm_http_direct.c:279-291). Reported
    # as its own flag because the number looks like a drain time and is not.
    result["exited_at_grace"] = bool(result["t_exit_ms"] is not None
                                     and result["t_exit_ms"] >= grace_ms)
    result["sigkill_floor_hit"] = bool(result["t_exit_ms"] is not None
                                       and result["t_exit_ms"] >= SIGKILL_FLOOR_MS)
    return result


def make_tls(root):
    """Self-signed material for the TLS dimension, generated once per run.

    Verification is off on the client side and that is deliberate: what is
    being measured is the per-connection cost of an `SSL` object in the worker,
    not the trust chain.
    """
    cert = root / "tls.crt"
    key = root / "tls.key"
    subprocess.run(
        ["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "2",
         "-subj", "/CN=localhost", "-keyout", str(key), "-out", str(cert)],
        check=True, capture_output=True)
    context = ssl.create_default_context()
    context.check_hostname = False
    context.verify_mode = ssl.CERT_NONE
    return cert, key, context


def config_text(scenario, directory, root, port):
    tls = ""
    if scenario["tls"]:
        tls = (f"http.tls_cert = {scenario['tls_cert']}\n"
               f"http.tls_key = {scenario['tls_key']}\n")
    front = "/worker.php" if scenario["executor"] == "worker" else "/index.php"
    spare = ""
    if scenario["pm"] in ("dynamic", "ondemand"):
        spare = (f"pm.start_servers = {scenario['start_servers']}\n"
                 f"pm.min_spare_servers = {scenario['min_spare']}\n"
                 f"pm.max_spare_servers = {scenario['max_spare']}\n"
                 f"pm.process_idle_timeout = {scenario['idle_timeout']}s\n")
    return f"""[global]
error_log = {directory}/fpm.log
pid = {directory}/fpm.pid
daemonize = no
[pm{scenario['index']}]
listen = 127.0.0.1:{port}
pool.type = http-direct
pool.executor = {scenario['executor']}
http.front_controller = {front}
http.read_timeout = {scenario['read_timeout_ms']}
{tls}pm = {scenario['pm']}
pm.max_children = {scenario['max_children']}
pm.max_requests = {scenario['max_requests']}
{spare}chdir = {root}
catch_workers_output = no
request_cpu_tracking = no
php_admin_value[max_execution_time] = 0
php_admin_value[opcache.enable] = 0
"""


def run_scenario(args, root, scenario, tls_context, port, ports=None):
    directory_name = (f"{scenario['executor']}-{scenario['pm']}"
                      f"-{'tls' if scenario['tls'] else 'plain'}-n{scenario['idle']}")
    directory = root / f"{directory_name}-{scenario['index']}"
    pool = Pool(args.binary, root, directory.name, port,
                config_text(scenario, directory, root, port))
    row_base = {k: scenario[k] for k in
                ("executor", "pm", "tls", "idle", "max_children", "max_requests",
                 "min_spare", "max_spare", "workload", "read_timeout_ms")}
    row_base["stream_mode"] = args.stream_mode
    rows = []
    idle_set = None
    stream = None
    try:
        if ports is not None:
            ports.release(port)
        # Inside the try, because start() parses a response and can raise on a
        # malformed one (RuntimeError, ValueError) -- none of which is OSError.
        # Raised outside, it would leave a live master bound to this port on a
        # shared box, reachable only through the pid file this path never wrote.
        status = pool.start(tls_context if scenario["tls"] else None)
        if not status["started"]:
            return [row_base | {"round": 0,
                                "result": "refused" if status["refused"] else "failed",
                                "error": status.get("error")}]
        for round_index in range(1, args.rounds + 1):
            idle_set = IdleSet(port, scenario["idle"], tls_context if scenario["tls"] else None)
            stream = Stream(port, args.stream_connections,
                            tls_context if scenario["tls"] else None, args.request_sleep_ms,
                            args.stream_mode)
            stream.start()
            # Let the pool settle before RSS is read: a child that is still
            # accepting its share of the idle set has not paid for it yet.
            time.sleep(args.settle_seconds)
            children = pool.children()
            row = row_base | {
                "round": round_index,
                "result": "ok",
                "children": len(children),
                "rss_child_bytes": {str(pid): rss_bytes(pid) for pid in children},
                "pss_child_bytes": {str(pid): pss_bytes(pid) for pid in children},
                "idle_conns_per_child": {str(pid): len(cs) for pid, cs in idle_set.by_pid.items()},
            }
            steady_from = time.monotonic()
            time.sleep(args.seconds)
            steady_to = time.monotonic()
            steady = [r for r in stream.snapshot() if steady_from <= r["at"] <= steady_to]
            row["steady"] = latency_split(steady) | failures_between(steady, steady_from, steady_to)
            if scenario["workload"] == "retire":
                row["retirement"] = retire(pool, idle_set, stream, args.inflight_ms,
                                           scenario["read_timeout_ms"],
                                           args.retire_timeout_seconds,
                                           args.retire_signal)
                row["retirement_trigger"] = f"harness {args.retire_signal.name} to one child"
            stream.stop()
            idle_set.close()
            stream, idle_set = None, None
            rows.append(row)
            print(json.dumps({k: v for k, v in row.items() if k != "rss_child_bytes"}
                             | {"rss_child_max": max(
                                 (v for v in row["rss_child_bytes"].values() if v), default=None)}))
    finally:
        if stream is not None:
            stream.stop()
        if idle_set is not None:
            idle_set.close()
        pool.stop()
    return rows


def main():
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("binary", type=Path)
    parser.add_argument("scratch", type=Path)
    parser.add_argument("--base-port", type=int, default=28300)
    parser.add_argument("--executors", nargs="+", default=["classic"],
                        choices=["classic", "worker"])
    parser.add_argument("--pms", nargs="+", default=["static"],
                        choices=["static", "dynamic", "ondemand"])
    parser.add_argument("--tls-modes", nargs="+", default=["plain"],
                        choices=["plain", "tls"])
    parser.add_argument("--idle", type=int, nargs="+", default=[0, 1024],
                        help="idle keep-alive connection counts to measure")
    parser.add_argument("--workloads", nargs="+", default=["steady"],
                        choices=["steady", "retire"])
    parser.add_argument("--max-children", type=int, default=4)
    parser.add_argument("--max-requests", type=int, default=0)
    parser.add_argument("--start-servers", type=int, default=2)
    parser.add_argument("--min-spare", type=int, default=1)
    parser.add_argument("--max-spare", type=int, default=3)
    parser.add_argument("--idle-timeout", type=int, default=10)
    parser.add_argument("--stream-connections", type=int, default=8)
    parser.add_argument("--request-sleep-ms", type=int, default=0)
    parser.add_argument("--stream-mode", default="keepalive",
                        choices=["keepalive", "new-connection"],
                        help="new-connection drives the load by accepts rather than by "
                             "requests on connections already open; a scale-up cannot "
                             "help the latter at all, see issue #53")
    parser.add_argument("--read-timeout-ms", type=int, default=5000,
                        help="http.read_timeout; also the grace a retiring child "
                             "gives the connections it holds (fpm_http_direct.c:245-253)")
    parser.add_argument("--retire-timeout-seconds", type=float, default=None,
                        help="how long to watch one retirement; defaults to the "
                             "grace above plus 10s, since a child may use all of it")
    parser.add_argument("--retire-signal", default="SIGQUIT",
                        choices=["SIGQUIT", "SIGUSR1"],
                        help="SIGQUIT is what the master sends when it retires an idle "
                             "child; SIGUSR1 is the operator retirement of issue #65")
    parser.add_argument("--inflight-ms", type=int, default=200,
                        help="how long the requests in flight at t0 hold the retiring child")
    parser.add_argument("--seconds", type=float, default=5.0)
    parser.add_argument("--settle-seconds", type=float, default=1.0)
    parser.add_argument("--rounds", type=int, default=3)
    args = parser.parse_args()
    args.retire_signal = getattr(signal, args.retire_signal)
    if args.retire_timeout_seconds is None:
        args.retire_timeout_seconds = args.read_timeout_ms / 1000.0 + 10.0

    args.binary = args.binary.resolve()
    # Before the scratch directory is created, so a refused binary leaves
    # nothing behind to clean up.
    sha256 = verify_binary(args.binary)
    # Same reason: a descriptor limit too low to hold the idle set is a refusal
    # before anything exists, not a half-made directory the rerun trips over.
    raise_nofile(max(args.idle) + args.stream_connections + 64)
    root = args.scratch.resolve()
    root.mkdir(parents=True, exist_ok=False)

    (root / "index.php").write_text(FRONT_CONTROLLER)
    (root / "worker.php").write_text(WORKER_SCRIPT)
    tls_cert = tls_key = None
    tls_context = None
    if "tls" in args.tls_modes:
        tls_cert, tls_key, tls_context = make_tls(root)

    scenarios = []
    index = 0
    for executor in args.executors:
        for pm in args.pms:
            for tls in args.tls_modes:
                for idle in args.idle:
                    for workload in args.workloads:
                        scenarios.append({
                            "index": index, "executor": executor, "pm": pm,
                            "tls": tls == "tls", "tls_cert": tls_cert, "tls_key": tls_key,
                            "idle": idle, "workload": workload,
                            "max_children": args.max_children,
                            "max_requests": args.max_requests,
                            "start_servers": args.start_servers,
                            "min_spare": args.min_spare, "max_spare": args.max_spare,
                            "idle_timeout": args.idle_timeout,
                            "read_timeout_ms": args.read_timeout_ms,
                        })
                        index += 1

    ports = PortRange(range(args.base_port, args.base_port + len(scenarios)))

    metadata = {
        "binary": str(args.binary),
        "sha256": sha256,
        "version": subprocess.check_output([str(args.binary), "-v"], text=True).strip(),
        # signal.Signals stringifies to its number under str(); record the name,
        # because a metadata.json saying "3" is not a reproducible run record.
        "settings": {k: (v.name if isinstance(v, signal.Signals)
                         else str(v) if isinstance(v, Path) else v)
                     for k, v in vars(args).items()},
        "platform": subprocess.check_output(["uname", "-a"], text=True).strip(),
        "load_before": os.getloadavg(),
        "sigkill_floor_ms": SIGKILL_FLOOR_MS,
        "decision_rule": "build/benchmark-http-direct-pm.md",
    }
    (root / "metadata.json").write_text(json.dumps(metadata, indent=2, default=str))

    results = []
    try:
        for scenario in scenarios:
            port = args.base_port + scenario["index"]
            results += run_scenario(args, root, scenario, tls_context, port, ports)
            # Incremental, so a run interrupted at scenario 9 of 12 still leaves
            # nine usable scenarios behind.
            (root / "results.json").write_text(json.dumps(results, indent=2, default=str))
    finally:
        ports.close()

    metadata["load_after"] = os.getloadavg()
    (root / "metadata.json").write_text(json.dumps(metadata, indent=2, default=str))
    print(f"\nwrote {root / 'results.json'} ({len(results)} rows)")


if __name__ == "__main__":
    main()

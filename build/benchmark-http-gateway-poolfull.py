#!/usr/bin/env python3
"""Pool-full measurement harness for the HTTP gateway (issue #154, spike #54).

#54 asks for "p50/p99 **of successful responses**" while a `pool.type = http`
pool is overloaded. The harness that produced `docs/http-direct.md` cannot
answer that. It drives `wrk`, which emits one latency histogram over every
response it received, and that file says so in as many words -- "Latency is
wrk's mixed-outcome percentile, not successful-only". At overload the two are
different questions: the measured `http` row for tiny-c32 reports p50 = 1.03 ms
beside 714 509 non-2xx responses, and that p50 is mostly the cost of generating
a 503.

It is also parameterised over three *transports*, and this spike compares
several *policies of one transport*, possibly with a second binary in play
(#155 and #156 each build a throwaway). So the thing that varies has to be a
matrix dimension rather than a hardcoded triple.

This harness measures one cell of

    (arm) x (http.gateways) x (concurrency) x (workload)

where an arm is a (label, binary, extra pool directives) triple. That is what
lets "reject", "wait" and "idle_timeout tuning" be three arms of one run: the
first two are different binaries, the third is the status-quo binary with
different directives, and nothing about the harness has to know which is which.

What it reports per cell, which is #54's list:

  rps_2xx                    2xx responses per second of the timed window
  n_non2xx                   and what they were, split by status
  latency.successful_only    p50/p99 over 2xx responses alone
  latency.mixed              p50/p99 over every response, side by side with it,
                             because the gap is the finding this harness exists
                             to make visible
  cpu_us_per_2xx             utime+stime over the master's whole descendant
                             tree, differenced across the window, divided by
                             the 2xx count -- so a policy that answers fewer
                             requests more cheaply cannot look efficient
  rss_end / pss_end          the same tree at the end of the window
  queue_wait_ms              percentiles of the `X-Fpmng-Queue-Wait` header if
                             the arm's binary emits one (#155's throwaway), and
                             absent rather than zero if it does not
  goodput                    the same window counted the client's way (issue
                             #158): successes per second per *logical* request,
                             attempts each one took, and end-to-end latency
                             timed from the client's first attempt rather than
                             from the attempt that happened to work

The goodput block exists because a 503 is the fastest thing this gateway can
produce. A client that fires the next request the instant a response arrives
is rewarded for being rejected, which flatters the status quo by construction
-- so `--retry-attempts N` turns the client into one that honours the
rejection, waits (Retry-After first, capped, exponential backoff otherwise) and
tries again, and counts a client that runs out of attempts as a failure rather
than dropping it from the denominator. The policy is recorded in
`metadata.json` and in every row, because the number depends on it entirely.
The default is one attempt, which is exactly the closed-loop client task 054
measured with -- a harness whose default client differed from the one behind
the published numbers would make every comparison with them wrong.

What is kept from `build/benchmark-http-direct.py`, because it was right there:
binary marker and sha256 identity check, a response-body check before anything
is timed, /proc-walked CPU and RSS over the whole descendant tree so gateway
children are counted, warmup plus rotated rounds, port pre-reservation, its own
scratch directory, an incrementally written `results.json`, and teardown by the
pids this process forked itself.

What is deliberately NOT kept is that harness's `threads = min(2, concurrency)`.
At concurrency 128 on the same box as the server, two threads measure the load
generator. Here the generator is `--generator-threads`, it is recorded in every
row, and `--thread-sweep` runs each cell at T and 2T so that "the generator was
not the bottleneck" is a measured claim in the artifact rather than an
assumption in a commit message.

Shared box rules, the same as every other harness here: its own scratch
directory under `~/rd/`, its own port range (21000-21099 by default), and
teardown by the pid file this process wrote. Nothing is ever matched by binary
name.

The decision rule these numbers are judged against belongs to #159/#160 and is
not in this file; this file only has to produce numbers that can be judged.
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
import shutil
import signal
import socket
import statistics
import subprocess
import sys
import threading
import time

# The literal as it appears in .rodata: fpm_http.c:3114 logs
# "[pool %s] http.gateways must be at least 1", so the pool name is a format
# argument and this substring is the part that is actually in the binary. It is
# specific to the gateway -- a stock php-fpm would serve every request in this
# harness over its own listener and produce plausible numbers, which is the
# failure mode worth an explicit check.
GATEWAY_MARKER = "http.gateways must be at least 1"

# One front controller for every workload, so a cell differs from its
# neighbour by a query string and not by a file on disk. The body is the pid
# and nothing else: that is what makes "which worker served this" readable from
# the client side, and it keeps the response small enough that the measurement
# is of the pool rather than of the loopback.
#
# `sleep` pins a worker without spending CPU, which is how a pool is made full
# on purpose. `cpu` spends CPU without releasing the worker either, which is
# the other half of #54's "light and CPU-bound scripts" -- a policy that looks
# good when the workers are idle-blocked may not when they are saturated.
FRONT_CONTROLLER = """<?php
$sleep = (int) ($_GET['sleep'] ?? 0);
$cpu   = (int) ($_GET['cpu'] ?? 0);
if ($sleep > 0) {
    usleep($sleep * 1000);
}
if ($cpu > 0) {
    $deadline = microtime(true) + $cpu / 1000;
    $sink = 0;
    while (microtime(true) < $deadline) {
        for ($i = 0; $i < 1000; $i++) {
            $sink += $i * $i;
        }
    }
}
header('Content-Type: text/plain');
header('Content-Length: ' . (strlen((string) getmypid()) + 1));
echo getmypid(), "\\n";
"""

# The workloads, as query strings. A workload is a name in the row and a shape
# of request, nothing else -- adding one does not touch any other part of this
# file.
WORKLOADS = {
    # Nothing but the dispatch. The pool is full only because concurrency
    # exceeds pm.max_children, which is the cheapest way to reach the state
    # under study and the one that isolates the policy from the script.
    "light": "",
    # A worker held for 50 ms without using the CPU: the shape of a request
    # waiting on a database. This is where a queueing policy should win, if it
    # wins anywhere.
    "blocking": "sleep=50",
    # A worker using the CPU for 10 ms. Here the box itself is the bound, and a
    # queue can only move latency around rather than remove it.
    "cpu": "cpu=10",
}


def verify_binary(binary):
    """Refuse anything that is not an fpm-ng binary with the HTTP gateway in it.

    Acceptance criterion of #154, and checked before a directory is created or a
    port is bound: pointing this at /bin/true has to abort with a sentence, not
    start a run that measures nothing.
    """
    binary = Path(binary)
    if not binary.is_file():
        raise SystemExit(f"refusing {binary}: not a file")
    try:
        strings = subprocess.check_output(["strings", str(binary)], text=True)
    except (OSError, subprocess.CalledProcessError) as exc:
        raise SystemExit(f"refusing {binary}: cannot read its strings ({exc})")
    if GATEWAY_MARKER not in strings:
        raise SystemExit(
            f"refusing {binary}: no HTTP gateway marker ({GATEWAY_MARKER!r}) in the binary.\n"
            "  This harness measures pool.type = http. A binary without that pool type would\n"
            "  answer every request in this run over its own listener and the numbers would\n"
            "  look entirely plausible, which is why this check exists.")
    return hashlib.sha256(binary.read_bytes()).hexdigest()


def binary_version(binary):
    try:
        out = subprocess.run([str(binary), "-n", "-v"], capture_output=True, text=True,
                             timeout=20)
        return out.stdout.strip() or out.stderr.strip()
    except (OSError, subprocess.SubprocessError) as exc:
        return f"(could not be asked: {exc})"


class PortRange:
    """Holds every port of the range bound until the pool that wants it starts.

    Copied in behaviour from `build/benchmark-http-direct-pm.py`, and for the
    same reason: the box is shared, and binding-then-closing only checks the
    range at t=0. The last cell of a long run starts many minutes later, and a
    port taken in between comes back as "the pool never listened", which reads
    like a broken binary. SO_REUSEADDR lets our own pool take the port we are
    still holding, so release() immediately before start() has no window.
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
    """Concurrency N needs N descriptors on this side too.

    Only the soft limit is touched, and only upwards towards the hard limit. A
    run that silently measured concurrency 128 as concurrency 40 would be worse
    than one that refused to start.
    """
    soft, hard = resource.getrlimit(resource.RLIMIT_NOFILE)
    if soft >= wanted:
        return soft
    target = min(hard, max(wanted, soft))
    resource.setrlimit(resource.RLIMIT_NOFILE, (target, hard))
    if target < wanted:
        raise SystemExit(f"need {wanted} descriptors, the hard limit is {hard}")
    return target


CLOCK_TICKS = os.sysconf("SC_CLK_TCK")


def children_of(pid):
    """Direct children of one process, from /proc.

    Walking /proc/<pid>/task/<pid>/children rather than scanning every process
    for a parent: the box is shared and a scan would see work that is not ours.
    """
    try:
        raw = Path(f"/proc/{pid}/task/{pid}/children").read_text()
    except OSError:
        return []
    return [int(p) for p in raw.split()]


def descendants_of(pid):
    """The master and everything under it.

    Not just the direct children: a gateway child is a direct child of the
    master today, and so is a worker, but the CPU and RSS figures in this file
    are about the whole stack and must not silently stop being true if that
    changes.
    """
    seen, stack = [], [pid]
    while stack:
        current = stack.pop()
        if current in seen:
            continue
        seen.append(current)
        stack.extend(children_of(current))
    return seen


def rss_bytes(pid):
    try:
        for line in Path(f"/proc/{pid}/status").read_text().splitlines():
            if line.startswith("VmRSS:"):
                return int(line.split()[1]) * 1024
    except OSError:
        return None
    return None


def pss_bytes(pid):
    """Proportional set size.

    VmRSS counts libphp's text and every mapped extension once per child, so
    summing it over a pool credits each child with tens of MB of pages that
    were never duplicated. Pss divides each shared page by its sharer count.
    Both are recorded: VmRSS is the number an operator sees in top, and the gap
    between the two is itself worth reading.
    """
    try:
        for line in open(f"/proc/{pid}/smaps_rollup"):
            if line.startswith("Pss:"):
                return int(line.split()[1]) * 1024
    except OSError:
        pass
    return None


def cpu_seconds(pid):
    """utime+stime of one process, in seconds."""
    try:
        # Everything after the last ')' -- the comm field is parenthesised and
        # may itself contain spaces and parentheses, so splitting the whole
        # line is wrong. state is [0] here, so utime/stime are [11] and [12].
        fields = Path(f"/proc/{pid}/stat").read_text().rpartition(")")[2].split()
        return (int(fields[11]) + int(fields[12])) / CLOCK_TICKS
    except (OSError, IndexError, ValueError):
        return None


def tree_cpu_seconds(pid):
    total = 0.0
    for p in descendants_of(pid):
        value = cpu_seconds(p)
        if value is not None:
            total += value
    return total


def tree_memory(pid):
    rss = pss = 0
    counted = 0
    for p in descendants_of(pid):
        r, s = rss_bytes(p), pss_bytes(p)
        if r is not None:
            rss += r
            counted += 1
        if s is not None:
            pss += s
    return {"rss_bytes": rss, "pss_bytes": pss or None, "processes": counted}


class Client:
    """One HTTP/1.1 keep-alive connection, spoken by hand over a raw socket.

    http.client will not do: this harness needs a connection whose every
    response carries the monotonic timestamp of its last byte, whose peer close
    with a request in flight is a *record* rather than an exception, and which
    can be driven from a selector alongside a hundred others in one thread. All
    three are load-bearing.
    """

    def __init__(self, port, timeout=30.0):
        self.port = port
        self.timeout = timeout
        self.sock = None
        self.buffer = b""
        self.started = None
        self.in_flight = False
        self.reconnects = 0
        self.connect()

    def connect(self):
        self.sock = socket.create_connection(("127.0.0.1", self.port), timeout=self.timeout)
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.sock.setblocking(False)
        self.buffer = b""
        self.in_flight = False

    def fileno(self):
        return self.sock.fileno()

    def send(self, query):
        path = f"/?{query}" if query else "/"
        request = (f"GET {path} HTTP/1.1\r\nHost: localhost\r\n"
                   "Connection: keep-alive\r\nAccept: */*\r\n\r\n").encode()
        self.started = time.monotonic()
        self.in_flight = True
        self.sock.sendall(request)

    def _parse(self):
        """Return one complete response from the buffer, or None.

        Only what this harness's own front controller and the gateway's own
        error pages can produce is handled: a status line, headers, and a body
        delimited by Content-Length or by the connection closing. A chunked
        body would be a finding about the gateway, so it is raised rather than
        quietly decoded.
        """
        head, sep, rest = self.buffer.partition(b"\r\n\r\n")
        if not sep:
            return None
        lines = head.split(b"\r\n")
        try:
            status = int(lines[0].split()[1])
        except (IndexError, ValueError):
            raise RuntimeError(f"unparseable status line from the gateway: {lines[0]!r}")
        headers = {}
        for line in lines[1:]:
            name, _, value = line.partition(b":")
            headers[name.strip().lower()] = value.strip()
        if b"transfer-encoding" in headers:
            raise RuntimeError("chunked response from the gateway; this harness does not decode it")
        length = int(headers.get(b"content-length", b"0"))
        if len(rest) < length:
            return None
        body, self.buffer = rest[:length], rest[length:]
        return status, headers, body

    def recv(self):
        """Feed whatever is readable into the parser. Returns a record or None.

        A record is also produced for a peer close with a request in flight.
        That outcome exists only as the absence of a reply, and a policy that
        produces it is doing something materially different from one that
        answers 503 -- so it gets its own outcome name and is never folded into
        `non2xx`.
        """
        try:
            chunk = self.sock.recv(65536)
        except (BlockingIOError, TimeoutError, socket.timeout):
            return None
        except OSError as exc:
            if exc.errno not in (errno.ECONNRESET, errno.ETIMEDOUT, errno.EPIPE):
                raise
            chunk = b""
        now = time.monotonic()
        if not chunk:
            if self.in_flight:
                self.in_flight = False
                return {"status": None, "outcome": "closed_without_response",
                        "ms": (now - self.started) * 1000, "at": now,
                        "retry_after": None, "queue_wait_ms": None}
            return {"status": None, "outcome": "closed_idle", "ms": None, "at": now,
                    "retry_after": None, "queue_wait_ms": None}
        self.buffer += chunk
        parsed = self._parse()
        if parsed is None:
            return None
        status, headers, _ = parsed
        self.in_flight = False
        queue_wait = headers.get(b"x-fpmng-queue-wait")
        try:
            queue_wait = float(queue_wait) if queue_wait is not None else None
        except ValueError:
            queue_wait = None
        return {
            "status": status,
            "outcome": "ok" if 200 <= status < 300 else "non2xx",
            "ms": (now - self.started) * 1000,
            "at": now,
            "retry_after": (headers.get(b"retry-after") or b"").decode() or None,
            "queue_wait_ms": queue_wait,
            # A 503 that closes the connection costs the client a new handshake
            # on every rejected request, which is a real difference between
            # policies and invisible in a status-code count.
            "closing": headers.get(b"connection", b"").lower() == b"close",
        }

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass


def probe_once(port, query, deadline=20.0):
    """One blocking request, used for readiness and for the body check.

    Separate from Client on purpose: Client is non-blocking because the load
    loop drives it from a selector, and a readiness check that had to share
    that machinery would be the most complicated part of this file for no
    reason.
    """
    end = time.monotonic() + deadline
    try:
        sock = socket.create_connection(("127.0.0.1", port), timeout=max(0.2, deadline))
    except OSError as exc:
        return {"ok": False, "error": str(exc)}
    try:
        path = f"/?{query}" if query else "/"
        sock.sendall((f"GET {path} HTTP/1.1\r\nHost: localhost\r\n"
                      "Connection: close\r\nAccept: */*\r\n\r\n").encode())
        data = b""
        while time.monotonic() < end:
            sock.settimeout(max(0.1, end - time.monotonic()))
            try:
                chunk = sock.recv(65536)
            except (TimeoutError, socket.timeout):
                break
            if not chunk:
                break
            data += chunk
        if not data:
            return {"ok": False, "error": "the gateway sent nothing"}
        head, _, body = data.partition(b"\r\n\r\n")
        status = int(head.split(b"\r\n")[0].split()[1])
        return {"ok": status == 200, "status": status, "body": body.decode(errors="replace")}
    except OSError as exc:
        return {"ok": False, "error": str(exc)}
    finally:
        sock.close()


class Pool:
    """One configured fpm-ng master with one `pool.type = http` pool."""

    def __init__(self, binary, directory, config_text):
        self.binary = Path(binary)
        self.directory = Path(directory)
        self.directory.mkdir(parents=True, exist_ok=True)
        self.config = self.directory / "fpm.conf"
        self.config.write_text(config_text)
        self.pid_file = self.directory / "fpm.pid"
        self.log = self.directory / "fpm.log"
        self.stderr = self.directory / "stderr.log"
        self.process = None
        self._log_handle = None

    def start(self, port, seconds=25.0):
        """Start, and report a refusal as a result rather than as a crash.

        A binary linked against a distribution libphp refuses `pool.type = http`
        outright (issue #214). That refusal is a legitimate outcome for an arm
        of this run -- it means "this binary cannot answer the question" -- and
        it belongs in the artifact next to the arms that could.
        """
        handle = open(self.stderr, "w")
        self.process = subprocess.Popen(
            [str(self.binary), "-n", "-F", "-y", str(self.config)],
            stdout=handle, stderr=handle,
            # Own session, so teardown can reach the gateway children too:
            # SIGKILL to a wedged master leaves them alive, still holding the
            # inherited listening socket, and on a shared box the port stays
            # bound long after this harness has exited.
            start_new_session=True)
        self._log_handle = handle
        deadline = time.monotonic() + seconds
        while True:
            if self.process.poll() is not None:
                return {"started": False, "refused": True,
                        "exit_code": self.process.returncode,
                        "error": self.error_lines()}
            probe = probe_once(port, "", deadline=2.0)
            if probe.get("ok"):
                return {"started": True, "refused": False, "probe_body": probe["body"]}
            if time.monotonic() > deadline:
                return {"started": False, "refused": False,
                        "error": probe.get("error") or f"first request answered {probe.get('status')}",
                        "log": self.error_lines()}
            time.sleep(0.05)

    def error_lines(self):
        lines = []
        for path in (self.log, self.stderr):
            if path.exists():
                lines += [l for l in path.read_text().splitlines()
                          if "ERROR" in l or "ALERT" in l or "WARNING" in l]
        return lines[-5:] if lines else ["(no ERROR/ALERT/WARNING line in the pool log)"]

    def master_pid(self):
        if self.pid_file.exists():
            try:
                return int(self.pid_file.read_text().strip())
            except ValueError:
                pass
        return self.process.pid if self.process else None

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
                # A master wedged in shutdown is exactly the state a queueing
                # policy can produce, and it is the state that orphans gateway
                # children. The escalation goes to our own process group --
                # never to a pattern matching the binary name, which would hit
                # other users of this box.
                try:
                    os.killpg(os.getpgid(self.process.pid), signal.SIGKILL)
                except OSError:
                    self.process.kill()
                try:
                    self.process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    pass
        if self._log_handle:
            self._log_handle.close()
            self._log_handle = None


def retry_policy(args):
    """The retry policy as it goes into the results file, or None.

    Issue #158 leaves the shape of the retry to the implementer and requires
    that it be recorded, because the goodput number depends on it entirely.
    This dict IS the record: it is written into metadata.json and into every
    row, and it is the only thing run_cell() passes to the load generator.
    """
    if args.retry_attempts <= 1:
        return None
    return {
        "max_attempts": args.retry_attempts,
        "honour_retry_after": not args.retry_ignore_retry_after,
        "base_ms": args.retry_base_ms,
        "cap_ms": args.retry_cap_ms,
        "backoff": "exponential, doubling per attempt, capped at cap_ms",
        "retry_on": ["non2xx", "closed_without_response"],
        "giveup_is": "a failure, counted in the denominator",
    }


class Load:
    """The load generator: `threads` threads, `concurrency` keep-alive clients.

    One thread owns a share of the connections and drives them from its own
    selector, so the concurrency the server sees is the connection count and
    not the thread count. `build/benchmark-http-direct.py` hardcodes
    `min(2, concurrency)` threads, and at concurrency 128 on the same box as
    the server that measures the generator; here the number is an argument, it
    is recorded in the row, and `--thread-sweep` exists to show what doubling
    it does.

    Closed loop per connection: one request outstanding at a time, the next one
    sent the moment the previous response is complete. That is the shape #54's
    question is about -- a client that keeps N requests in flight regardless of
    what the server does measures the server's queue, not its policy.
    """

    def __init__(self, port, concurrency, threads, query, timeout, retry=None):
        self.port = port
        self.concurrency = concurrency
        self.threads = max(1, min(threads, concurrency))
        self.query = query
        self.timeout = timeout
        # Issue #158. None keeps the closed loop above exactly as it was: every
        # response, whatever it says, is followed at once by the next request.
        # A dict turns the client into one that honours a rejection -- see
        # _plan() for what "honours" means and RETRY_POLICY_HELP for why the
        # exact shape of it is written into the results file.
        self.retry = retry
        self.records = []
        self.logical = []
        self.in_flight_at_end = 0
        self._lock = threading.Lock()
        self.errors = []

    def _retry_delay(self, record, attempt):
        """How long this client waits before attempt number `attempt`.

        Retry-After first, because that is the gateway telling the client what
        it thinks -- and the whole point of issue #158 is that ignoring it is
        what made rejection look free. It is capped: the gateway says 1 second
        (FPM_HTTP_RETRY_AFTER), and a 10-second window spent mostly asleep
        would measure the cap and nothing else. The cap is a measurement
        decision and it is recorded in the row.
        """
        cap = self.retry["cap_ms"] / 1000.0
        wait = None
        if self.retry["honour_retry_after"] and record.get("retry_after"):
            try:
                wait = float(record["retry_after"])
            except ValueError:
                # An HTTP-date rather than a delta. Not produced by this
                # gateway; if it ever is, the backoff below covers it.
                wait = None
        if wait is None:
            wait = (self.retry["base_ms"] / 1000.0) * (2 ** max(0, attempt - 2))
        return min(wait, cap)

    def _finish(self, state, logical, outcome, now, collect_from):
        """Close one logical request -- the thing the client actually wanted."""
        if now >= collect_from:
            logical.append({
                "outcome": outcome,
                "attempts": state["attempts"],
                "e2e_ms": (now - state["start"]) * 1000,
                "at": now,
            })
        state["start"] = now
        state["attempts"] = 1

    def _plan(self, state, record, logical, collect_from):
        """Returns how long to wait before this client's next send.

        Without a retry policy the answer is always 0 -- the closed loop this
        harness had before issue #158 -- but the logical-request bookkeeping
        still runs, so the goodput columns exist for both clients and can be
        compared.
        """
        now = record["at"]
        if record["outcome"] == "closed_idle":
            # Nothing was answered: the connection was idle when it went away.
            # The attempt in progress, if any, is still the same one.
            return 0.0
        if record["outcome"] == "ok":
            self._finish(state, logical, "ok", now, collect_from)
            return 0.0
        if record["outcome"] == "error":
            # A fault on this side of the socket. Counted, but not charged to
            # the policy under test as a rejection.
            self._finish(state, logical, "error", now, collect_from)
            return 0.0
        # A rejection: a 503, any other non-2xx, or a close with the request in
        # flight. All three cost the client another attempt, which is the cost
        # issue #158 exists to put on the clock.
        # No retry policy is the same statement with max_attempts = 1: the
        # closed-loop client never comes back for the request it was refused,
        # so that request is a give-up on its first and only attempt. Saying it
        # that way is what lets one goodput column describe both clients.
        max_attempts = self.retry["max_attempts"] if self.retry else 1
        if state["attempts"] >= max_attempts:
            self._finish(state, logical, "giveup", now, collect_from)
            return 0.0
        state["attempts"] += 1
        return self._retry_delay(record, state["attempts"])

    def _worker(self, count, stop_at, collect_from):
        local = []
        logical = []
        clients = []
        state = {}
        in_flight_at_end = 0
        try:
            for _ in range(count):
                try:
                    clients.append(Client(self.port, timeout=self.timeout))
                except OSError as exc:
                    with self._lock:
                        self.errors.append(f"connect: {exc}")
            if not clients:
                return
            selector = selectors.DefaultSelector()
            now = time.monotonic()
            for client in clients:
                state[client] = {"start": now, "attempts": 1, "due": None}
                client.send(self.query)
                selector.register(client.sock, selectors.EVENT_READ, client)

            while time.monotonic() < stop_at:
                # Shorter than the 50 ms this used to be: with a retry policy
                # the loop also has to wake up to send requests whose wait has
                # expired, and a 50 ms floor under a 5 ms backoff would be the
                # backoff.
                for key, _ in selector.select(timeout=0.005 if self.retry else 0.05):
                    client = key.data
                    try:
                        record = client.recv()
                    except (OSError, RuntimeError) as exc:
                        with self._lock:
                            self.errors.append(str(exc))
                        record = {"status": None, "outcome": "error", "ms": None,
                                  "at": time.monotonic(), "retry_after": None,
                                  "queue_wait_ms": None, "closing": True}
                    if record is None:
                        continue
                    if record["outcome"] != "closed_idle" and record["at"] >= collect_from:
                        local.append(record)
                    # The connection is gone -- either the peer closed it or the
                    # response asked us to. Reconnecting rather than dropping the
                    # client keeps the concurrency the server sees constant for
                    # the whole window, which is the only way the rate at the end
                    # is comparable with the rate at the start.
                    if record["outcome"] in ("closed_without_response", "closed_idle", "error") \
                            or record.get("closing"):
                        selector.unregister(client.sock)
                        client.close()
                        try:
                            client.connect()
                            client.reconnects += 1
                            selector.register(client.sock, selectors.EVENT_READ, client)
                        except OSError as exc:
                            with self._lock:
                                self.errors.append(f"reconnect: {exc}")
                            continue
                    delay = self._plan(state[client], record, logical, collect_from)
                    if time.monotonic() >= stop_at:
                        continue
                    if delay > 0:
                        # The client is honouring the rejection. Its next
                        # attempt is sent by the sweep below, and the wait is
                        # part of the end-to-end time of the request it is
                        # still trying to get answered.
                        state[client]["due"] = time.monotonic() + delay
                        continue
                    state[client]["due"] = None
                    try:
                        client.send(self.query)
                    except OSError as exc:
                        with self._lock:
                            self.errors.append(f"send: {exc}")

                if self.retry:
                    now = time.monotonic()
                    if now >= stop_at:
                        break
                    for client in clients:
                        due = state[client]["due"]
                        if due is None or due > now or client.in_flight:
                            continue
                        state[client]["due"] = None
                        try:
                            client.send(self.query)
                        except OSError as exc:
                            with self._lock:
                                self.errors.append(f"send: {exc}")
            # Logical requests still unanswered when the window closed are
            # neither successes nor give-ups; they are reported separately so
            # that successes + give-ups + errors is exactly the number of
            # logical requests that finished inside the window.
            in_flight_at_end = sum(1 for c in clients if c.in_flight or state[c]["due"] is not None)
            reconnects = sum(c.reconnects for c in clients)
        finally:
            for client in clients:
                client.close()
        with self._lock:
            self.records.extend(local)
            self.logical.extend(logical)
            self.in_flight_at_end += in_flight_at_end
            self.reconnects = getattr(self, "reconnects", 0) + reconnects

    def run(self, warmup_seconds, window_seconds):
        """Warm up and measure in one continuous drive.

        Not two runs: stopping and restarting the load between warmup and window
        would give the pool a chance to go idle and scale back, and the first
        second of the timed window would then measure a cold start rather than
        the steady state the warmup was supposed to establish.
        """
        start = time.monotonic()
        collect_from = start + warmup_seconds
        stop_at = collect_from + window_seconds
        share = [self.concurrency // self.threads] * self.threads
        for i in range(self.concurrency % self.threads):
            share[i] += 1
        workers = [threading.Thread(target=self._worker, args=(n, stop_at, collect_from),
                                    daemon=True)
                   for n in share if n > 0]
        for w in workers:
            w.start()
        for w in workers:
            w.join(timeout=warmup_seconds + window_seconds + 60)
        return {"collect_from": collect_from, "stop_at": stop_at,
                "window_seconds": window_seconds}


def percentile(values, fraction):
    if not values:
        return None
    ordered = sorted(values)
    index = min(len(ordered) - 1, max(0, int(round(fraction * (len(ordered) - 1)))))
    return ordered[index]


def summarize(values):
    if not values:
        return None
    return {
        "n": len(values),
        "p50": percentile(values, 0.50),
        "p90": percentile(values, 0.90),
        "p99": percentile(values, 0.99),
        "mean": statistics.fmean(values),
        "max": max(values),
    }


def latency_report(records):
    """Successful-only and mixed, side by side. The point of this harness.

    #54 asks for the successful-only figure; the mixed one is reported next to
    it because the two being far apart is the evidence that the mixed one was
    never an answer to the question, and a table that shows only the number the
    issue asked for cannot make that argument.
    """
    ok = [r["ms"] for r in records if r["outcome"] == "ok" and r["ms"] is not None]
    every = [r["ms"] for r in records if r["ms"] is not None]
    non2xx = [r["ms"] for r in records if r["outcome"] == "non2xx" and r["ms"] is not None]
    report = {
        "successful_only_ms": summarize(ok),
        "mixed_ms": summarize(every),
        "non2xx_ms": summarize(non2xx),
    }
    if report["successful_only_ms"] and report["mixed_ms"]:
        mixed_p50 = report["mixed_ms"]["p50"]
        report["p50_successful_over_mixed"] = (
            report["successful_only_ms"]["p50"] / mixed_p50 if mixed_p50 else None)
    return report


def goodput_report(logical, window, in_flight_at_end):
    """What the client got, counted the client's way (issue #158).

    A 503 is the fastest thing this gateway can produce, so a client that fires
    the next request the instant one arrives rewards a rejection. Everything
    here is per *logical* request instead: one thing the client wanted, however
    many attempts it took, timed from the first attempt.

    A client that ran out of attempts is a `giveup` and stays in the
    denominator. Dropping it would be the same mistake one level up: the run
    would report the latency of the requests that happened to get through and
    call it the service's latency.
    """
    ok = [r for r in logical if r["outcome"] == "ok"]
    giveup = [r for r in logical if r["outcome"] == "giveup"]
    error = [r for r in logical if r["outcome"] == "error"]
    attempts_ok = sum(r["attempts"] for r in ok)
    return {
        "n_logical_completed": len(logical),
        "n_success": len(ok),
        "n_giveup": len(giveup),
        "n_error": len(error),
        # Criterion 4 of issue #158, stated in the file rather than trusted:
        # nothing the client started and finished inside the window is missing
        # from the counts above.
        "counts_add_up": len(ok) + len(giveup) + len(error) == len(logical),
        "n_in_flight_at_window_end": in_flight_at_end,
        "attempts_total": sum(r["attempts"] for r in logical),
        "attempts_per_success": (attempts_ok / len(ok)) if ok else None,
        "success_per_sec": (len(ok) / window) if window else None,
        "giveup_per_sec": (len(giveup) / window) if window else None,
        # From the client's FIRST attempt. Next to the per-attempt latency in
        # the same row, these two being far apart is the finding.
        "end_to_end_ms": summarize([r["e2e_ms"] for r in ok]),
        "end_to_end_including_giveups_ms": summarize([r["e2e_ms"] for r in ok + giveup]),
        "giveup_ms": summarize([r["e2e_ms"] for r in giveup]),
    }


def config_text(arm, gateways, directory, port, listen_path, max_children, extra):
    """The pool under measurement, written out in full.

    `pm = static` and an explicit `pm.max_children`: the budget the gateway
    shares is `pm.max_children` (fpm_http.c:13-19), so the whole question this
    spike asks is about a number that must not be allowed to move on its own
    while a cell is running.

    The FastCGI listener is a unix socket inside the cell's own directory, so a
    cell costs exactly one TCP port -- the gateway's -- out of the range this
    harness confines itself to.
    """
    lines = [
        "[global]",
        f"error_log = {directory / 'fpm.log'}",
        f"pid = {directory / 'fpm.pid'}",
        "daemonize = no",
        "process_control_timeout = 5",
        "",
        "[gw]",
        f"listen = {listen_path}",
        "pool.type = http",
        "pm = static",
        f"pm.max_children = {max_children}",
        f"chdir = {directory}",
        f"http.gateways = {gateways}",
        f"http.listen = 127.0.0.1:{port}",
        "php_admin_value[opcache.enable] = 0",
        "php_admin_value[max_execution_time] = 0",
    ]
    # The arm's own directives, last, so an arm can override anything above it
    # -- variant 3 of #54 is "the status quo with a different http.idle_timeout"
    # and it has to be expressible without a second binary or a second template.
    lines += list(extra)
    return "\n".join(lines) + "\n"


def run_cell(args, root, arm, gateways, concurrency, workload, threads, port, ports):
    """One cell: start a pool, warm it, measure a window, tear it down."""
    name = f"{arm['label']}-gw{gateways}-c{concurrency}-{workload}-t{threads}"
    directory = root / name
    if directory.exists():
        shutil.rmtree(directory)
    directory.mkdir(parents=True)
    (directory / "index.php").write_text(FRONT_CONTROLLER)
    listen_path = directory / "fastcgi.sock"

    row = {
        "cell": name,
        "arm": arm["label"],
        "binary": str(arm["binary"]),
        "binary_sha256": arm["sha256"],
        "extra_directives": list(arm["extra"]),
        "gateways": gateways,
        "concurrency": concurrency,
        "workload": workload,
        "query": WORKLOADS[workload],
        "generator_threads": threads,
        "max_children": args.max_children,
        "port": port,
        # Recorded in every row, not only in the metadata: the goodput numbers
        # below mean nothing without it, and a row is what gets pasted into a
        # comment. None means the closed loop -- the client of task 054.
        "retry_policy": retry_policy(args),
    }

    pool = Pool(arm["binary"], directory,
                config_text(arm, gateways, directory, port, listen_path,
                            args.max_children, arm["extra"]))
    ports.release(port)
    started = pool.start(port)
    row["start"] = started
    if not started["started"]:
        pool.stop()
        row["status"] = "refused" if started.get("refused") else "did_not_start"
        return row

    # Body check before anything is timed: a run whose "2xx" responses are the
    # gateway's own fallback page would report a rate and a latency that mean
    # nothing. The body is the worker pid, so a body that does not parse as an
    # integer fails the cell here rather than becoming a number in the table.
    probe = probe_once(port, WORKLOADS[workload], deadline=30.0)
    body = (probe.get("body") or "").strip()
    if not probe.get("ok") or not body.isdigit():
        pool.stop()
        row["status"] = "body_check_failed"
        row["body_check"] = probe
        return row
    row["body_check"] = {"ok": True, "worker_pid": int(body)}

    master = pool.master_pid()
    load = Load(port, concurrency, threads, WORKLOADS[workload], args.request_timeout,
                retry=retry_policy(args))

    cpu_before = tree_cpu_seconds(master)
    timing = load.run(args.warmup_seconds, args.seconds)
    cpu_after = tree_cpu_seconds(master)
    memory = tree_memory(master)

    records = load.records
    counts = collections.Counter(r["outcome"] for r in records)
    statuses = collections.Counter(r["status"] for r in records if r["status"] is not None)
    n_ok = counts.get("ok", 0)
    window = timing["window_seconds"]

    queue_waits = [r["queue_wait_ms"] for r in records if r.get("queue_wait_ms") is not None]

    row.update({
        "status": "measured",
        "window_seconds": window,
        "n_requests": len(records),
        "outcomes": dict(counts),
        "status_codes": {str(k): v for k, v in sorted(statuses.items())},
        "rps_2xx": n_ok / window if window else None,
        "rps_all": len(records) / window if window else None,
        "n_non2xx": counts.get("non2xx", 0),
        "n_closed_without_response": counts.get("closed_without_response", 0),
        "retry_after_seen": sorted({r["retry_after"] for r in records
                                    if r.get("retry_after")}),
        "reconnects": getattr(load, "reconnects", 0),
        "latency": latency_report(records),
        # Absent rather than zero when the binary emits no header: a policy that
        # does not queue has no queue wait, and a 0.0 in that column would read
        # as "it queued instantly".
        "queue_wait_ms": summarize(queue_waits),
        # Issue #158. Present for every run; without a retry policy it counts
        # one attempt per logical request, which makes the two latency columns
        # agree and is itself worth being able to show.
        "goodput": goodput_report(load.logical, window, load.in_flight_at_end),
        "cpu_seconds_window": cpu_after - cpu_before,
        "cpu_us_per_2xx": ((cpu_after - cpu_before) * 1e6 / n_ok) if n_ok else None,
        "memory_end": memory,
        "generator_errors": load.errors[:10],
        "n_generator_errors": len(load.errors),
        "pool_log_tail": pool.error_lines(),
    })

    pool.stop()
    return row


def main():
    parser = argparse.ArgumentParser(
        description="Pool-full measurement harness for the HTTP gateway (issue #154, spike #54)")
    parser.add_argument("scratch", help="scratch directory; the run refuses to reuse one")
    parser.add_argument("--arm", action="append", required=True, metavar="LABEL=BINARY[:DIRECTIVE,...]",
                        help="an arm of the comparison: a label, the binary to run it with, and "
                             "optional extra pool directives. Repeatable. Example: "
                             "'reject=/home/x/bin/php-fpm-ng' or "
                             "'idle500=/home/x/bin/php-fpm-ng:http.idle_timeout = 500'")
    parser.add_argument("--gateways", type=int, nargs="+", default=[1, 2])
    parser.add_argument("--concurrency", type=int, nargs="+", default=[4, 32, 128])
    parser.add_argument("--workloads", nargs="+", default=["light", "blocking"],
                        choices=sorted(WORKLOADS))
    parser.add_argument("--max-children", type=int, default=4)
    parser.add_argument("--generator-threads", type=int, default=8)
    parser.add_argument("--thread-sweep", action="store_true",
                        help="run every cell a second time at twice the generator threads, so "
                             "'the generator was not the bottleneck' is measured rather than assumed")
    parser.add_argument("--seconds", type=float, default=10.0)
    parser.add_argument("--warmup-seconds", type=float, default=3.0)
    parser.add_argument("--rounds", type=int, default=3)
    parser.add_argument("--request-timeout", type=float, default=30.0)
    # Issue #158: the retrying client. Off by default -- one attempt per
    # request is the closed loop task 054 measured, and a harness whose default
    # client differs from the one the published numbers came from would make
    # every comparison with them wrong.
    parser.add_argument("--retry-attempts", type=int, default=1, metavar="N",
                        help="attempts a client makes before it gives up on a request (issue "
                             "#158). 1, the default, is the closed-loop client: a rejection is "
                             "simply the end of that request. Anything above 1 makes the client "
                             "honour the rejection and pay for it")
    parser.add_argument("--retry-base-ms", type=float, default=5.0,
                        help="first backoff when the rejection carried no Retry-After; doubles "
                             "per attempt")
    parser.add_argument("--retry-cap-ms", type=float, default=250.0,
                        help="ceiling on any single wait, Retry-After included. The gateway says "
                             "1 second and a measurement window is ten, so the cap is what keeps "
                             "the window measuring the server rather than the sleep; it is "
                             "recorded with the results")
    parser.add_argument("--retry-ignore-retry-after", action="store_true",
                        help="back off on the schedule above even when the response carried a "
                             "Retry-After, for showing what honouring it costs")
    parser.add_argument("--base-port", type=int, default=21000)
    parser.add_argument("--port-span", type=int, default=100,
                        help="ports reserved from --base-port; the default keeps this harness "
                             "inside 21000-21099, which is the range #154 gives it")
    args = parser.parse_args()

    arms = []
    for spec in args.arm:
        label, _, rest = spec.partition("=")
        if not rest:
            raise SystemExit(f"--arm {spec!r}: expected LABEL=BINARY[:DIRECTIVE,...]")
        binary, _, directives = rest.partition(":")
        extra = [d.strip() for d in directives.split(",") if d.strip()]
        arms.append({"label": label, "binary": Path(binary).resolve(),
                     "extra": extra, "sha256": verify_binary(binary),
                     "version": binary_version(binary)})

    root = Path(args.scratch).expanduser().resolve()
    if root.exists() and any(root.iterdir()):
        raise SystemExit(f"{root} exists and is not empty; pick a fresh scratch directory")
    root.mkdir(parents=True, exist_ok=True)

    raise_nofile(max(args.concurrency) * 2 + 256)

    if args.retry_attempts < 1:
        raise SystemExit("--retry-attempts is a number of attempts, so it cannot be below 1")

    thread_counts = [args.generator_threads]
    if args.thread_sweep:
        thread_counts.append(args.generator_threads * 2)

    cells = [(arm, gw, c, w, t)
             for arm in arms
             for gw in args.gateways
             for c in args.concurrency
             for w in args.workloads
             for t in thread_counts]

    ports_needed = len(cells) * args.rounds
    if ports_needed > args.port_span:
        # One port per cell per round, never reused: a pool that has not quite
        # finished closing its listener would otherwise make the next cell's
        # start look like a refusal.
        raise SystemExit(
            f"{ports_needed} cells x rounds need {ports_needed} ports but only {args.port_span} "
            f"are reserved from {args.base_port}. Reduce the matrix or raise --port-span, "
            "having checked that the wider range is yours to use on this box.")

    metadata = {
        "harness": "build/benchmark-http-gateway-poolfull.py",
        "issue": 154,
        "spike": 54,
        "started_at": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
        "argv": sys.argv,
        "settings": {k: (str(v) if isinstance(v, Path) else v) for k, v in vars(args).items()},
        "arms": [{**a, "binary": str(a["binary"])} for a in arms],
        "retry_policy": retry_policy(args),
        "platform": subprocess.run(["uname", "-a"], capture_output=True, text=True).stdout.strip(),
        "load_before": os.getloadavg(),
    }
    (root / "metadata.json").write_text(json.dumps(metadata, indent=2) + "\n")

    ports = PortRange(range(args.base_port, args.base_port + args.port_span))
    results = []
    results_path = root / "results.json"
    next_port = args.base_port
    try:
        for round_index in range(args.rounds):
            # Rotated, so that a cell is not always measured at the same point
            # in the run: the box warms up, and a fixed order would give the
            # first arm every cold cache in every round.
            order = cells[round_index % len(cells):] + cells[:round_index % len(cells)]
            for arm, gw, concurrency, workload, threads in order:
                port = next_port
                next_port += 1
                row = run_cell(args, root, arm, gw, concurrency, workload, threads, port, ports)
                row["round"] = round_index
                results.append(row)
                # Written after every cell: a run that dies in hour two still
                # leaves behind everything it measured in hour one.
                results_path.write_text(json.dumps(results, indent=2) + "\n")
                goodput = row.get("goodput") or {}
                print(f"{row['cell']} round={round_index} status={row['status']} "
                      f"rps_2xx={row.get('rps_2xx')} "
                      f"goodput={goodput.get('success_per_sec')} "
                      f"attempts_per_success={goodput.get('attempts_per_success')} "
                      f"p99_ok={(row.get('latency') or {}).get('successful_only_ms', {}) or {}}",
                      flush=True)
    finally:
        ports.close()
        metadata["load_after"] = os.getloadavg()
        metadata["finished_at"] = time.strftime("%Y-%m-%dT%H:%M:%S%z")
        (root / "metadata.json").write_text(json.dumps(metadata, indent=2) + "\n")

    print(f"{len(results)} rows -> {results_path}")


if __name__ == "__main__":
    main()

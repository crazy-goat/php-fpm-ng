#!/usr/bin/env python3
"""How an http-direct pool spreads connections across its workers (issue #53).

The question this answers is not throughput. It is whether a connection that
arrives while one worker is busy can be served by a worker that is not. An
http-direct pool has every child accepting from the one listening socket the
master opened, and a child's event loop cannot run while PHP is executing -- so
a connection the kernel handed to a busy child waits for that child's current
request, however idle its peers are.

Three workloads, each run against pm.max_children of 1, 2 and 4:

  burst      N connections opened before any request is sent, then one request
             each. This is the shape the original probe in docs/http-direct.md
             failed on: both connections were accepted by one worker while
             another sat idle.
  keepalive  N persistent connections, each issuing requests back to back for
             the duration. Once a connection is accepted it is pinned to that
             worker for its whole life, so this measures what the pinning costs
             when the per-connection load is uneven.
  slow-peer  one connection asks for a request that sleeps, the others ask for
             fast ones. The answer to the issue's "can queued work progress on
             idle peers" is read off the fast requests' own latency: one that
             ran on an idle peer costs a millisecond, one that was queued behind
             the sleeping worker costs the whole sleep. Counting how many
             finished before the slow one does NOT answer it -- when they are
             all queued behind the same worker that count is still everything
             but the slow request itself.

Every request's response carries the worker pid that served it, so the
distribution is measured rather than inferred. Raw per-request records are kept
next to the summary so a rerun can be compared against this one.

Uses only its own scratch directory and its own port range; it starts one FPM
per pool size and stops it by the pid file it wrote. Nothing is matched by
process name, because the poligon box is shared.
"""
import argparse
import collections
import hashlib
import http.client
import json
import os
from pathlib import Path
import socket
import statistics
import subprocess
import time
import urllib.request

FRONT_CONTROLLER = """<?php
/* Reports which worker answered, so the harness measures the distribution
 * instead of inferring it. `sleep` is in milliseconds and uses usleep, which
 * holds the worker exactly the way a blocking call in an application does --
 * the event loop of this child cannot run meanwhile, which is the property
 * under test. */
$sleep = (int) ($_GET['sleep'] ?? 0);
if ($sleep > 0) {
    usleep($sleep * 1000);
}
header('Content-Type: text/plain');
echo getmypid(), "\\n";
"""


def wait_until_serving(port, process, seconds=15):
    deadline = time.monotonic() + seconds
    while True:
        try:
            with urllib.request.urlopen(f"http://127.0.0.1:{port}/", timeout=1) as r:
                return int(r.read().split()[0])
        except OSError:
            if time.monotonic() > deadline or process.poll() is not None:
                raise RuntimeError(f"the pool on port {port} never answered")
            time.sleep(0.05)


class Connection:
    """One client connection, kept open so the worker it lands on stays fixed.

    http.client is enough here and a real HTTP client is the point: the
    fairness question is about what libevent's evhttp does with connections,
    so the harness must speak the protocol it parses rather than write bytes
    at a socket.
    """

    def __init__(self, port):
        self.conn = http.client.HTTPConnection("127.0.0.1", port, timeout=30)
        self.conn.connect()

    def request(self, sleep_ms=0):
        path = f"/?sleep={sleep_ms}" if sleep_ms else "/"
        started = time.monotonic()
        self.conn.request("GET", path)
        response = self.conn.getresponse()
        body = response.read()
        return {
            "pid": int(body.split()[0]) if response.status == 200 and body.split() else None,
            "status": response.status,
            "ms": (time.monotonic() - started) * 1000,
            "started": started,
            "finished": time.monotonic(),
        }

    def close(self):
        try:
            self.conn.close()
        except OSError:
            pass


def run_in_threads(jobs):
    """Run one callable per thread and collect the results in order.

    Threads rather than asyncio because each job is a blocking HTTP exchange
    and the count is small; what matters is that the connections are opened
    before any request is sent, which a thread pool would not guarantee.
    """
    import threading

    results = [None] * len(jobs)

    def target(i, job):
        try:
            results[i] = job()
        except Exception as exc:  # recorded, not raised: a failed connection is a result
            results[i] = {"error": repr(exc)}

    threads = [threading.Thread(target=target, args=(i, j)) for i, j in enumerate(jobs)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    return results


def burst(port, connections):
    """Open every connection first, then let them all request at once.

    Opening first is what the probe in docs/http-direct.md did NOT do, and the
    reason it was documented as a limit: if the first request is already
    running when the second connection opens, the busy worker is not in accept
    and the result says nothing about fairness.
    """
    import threading

    conns = [Connection(port) for _ in range(connections)]
    gate = threading.Barrier(connections)

    def job(c):
        def run():
            gate.wait()
            return c.request()
        return run

    try:
        return run_in_threads([job(c) for c in conns])
    finally:
        for c in conns:
            c.close()


def keepalive(port, connections, seconds):
    import threading

    conns = [Connection(port) for _ in range(connections)]
    gate = threading.Barrier(connections)
    deadline = None

    def job(c):
        def run():
            nonlocal deadline
            gate.wait()
            if deadline is None:
                deadline = time.monotonic() + seconds
            records = []
            while time.monotonic() < deadline:
                records.append(c.request())
            return records
        return run

    try:
        nested = run_in_threads([job(c) for c in conns])
    finally:
        for c in conns:
            c.close()
    flat = []
    for item in nested:
        flat.extend(item if isinstance(item, list) else [item])
    return flat


def slow_peer(port, connections, sleep_ms):
    """One slow request, the rest fast, all starting together.

    The number that matters is how many fast requests finished before the slow
    one did. With perfectly idle peers that is all of them; with the fast
    requests queued behind the slow worker it is none.
    """
    import threading

    conns = [Connection(port) for _ in range(connections)]
    gate = threading.Barrier(connections)

    def job(c, ms):
        def run():
            gate.wait()
            return c.request(ms)
        return run

    jobs = [job(conns[0], sleep_ms)] + [job(c, 0) for c in conns[1:]]
    try:
        return run_in_threads(jobs)
    finally:
        for c in conns:
            c.close()


def slow_peer_verdict(records, slow_ms):
    """Did the fast requests get served, or did they wait for the sleeper?

    A fast request costs about a millisecond. One that was accepted by the
    worker currently inside usleep() cannot be looked at until that worker
    returns to its event loop, so it costs the whole sleep. Half the sleep is
    the threshold because nothing lands in between: there is no partial
    progress to be had on a worker that is not in its event loop.
    """
    ok = [r for r in records if isinstance(r, dict) and r.get("status") == 200]
    fast = sorted(ok, key=lambda r: r["ms"])[:-1] if ok else []
    blocked = [r for r in fast if r["ms"] >= slow_ms / 2]
    return {
        "fast_total": len(fast),
        "fast_served_on_idle_peer": len(fast) - len(blocked),
        "fast_queued_behind_sleeper": len(blocked),
        "fast_ms_min": round(fast[0]["ms"], 2) if fast else None,
        "fast_ms_max": round(fast[-1]["ms"], 2) if fast else None,
        "slow_ms": round(max((r["ms"] for r in ok), default=0), 2),
    }


def summarise(records):
    ok = [r for r in records if isinstance(r, dict) and r.get("status") == 200]
    errors = [r for r in records if not (isinstance(r, dict) and r.get("status") == 200)]
    per_worker = collections.Counter(r["pid"] for r in ok)
    latencies = sorted(r["ms"] for r in ok)
    summary = {
        "requests": len(records),
        "ok": len(ok),
        "errors": len(errors),
        "workers_used": len(per_worker),
        "per_worker": dict(sorted(per_worker.items())),
    }
    if latencies:
        summary["latency_ms"] = {
            "min": round(latencies[0], 2),
            "median": round(statistics.median(latencies), 2),
            "p95": round(latencies[min(len(latencies) - 1, int(len(latencies) * 0.95))], 2),
            "max": round(latencies[-1], 2),
        }
    if per_worker:
        counts = list(per_worker.values())
        # Share of the busiest worker. 1/workers is perfect, 1.0 is one worker
        # doing everything -- the single number the issue asks to be reported.
        summary["busiest_share"] = round(max(counts) / sum(counts), 3)
    return summary


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("binary", type=Path)
    parser.add_argument("scratch", type=Path)
    parser.add_argument("--base-port", type=int, default=28210)
    parser.add_argument("--workers", type=int, nargs="+", default=[1, 2, 4])
    parser.add_argument("--connections", type=int, default=8)
    parser.add_argument("--seconds", type=int, default=5)
    parser.add_argument("--slow-ms", type=int, default=1000)
    # Each workload is stochastic -- which child the kernel hands a connection
    # to is not ours to choose -- so a single run cannot distinguish a policy
    # from a coin toss. Every number below is reported per repeat.
    parser.add_argument("--repeats", type=int, default=3)
    args = parser.parse_args()

    binary = args.binary.resolve()
    root = args.scratch.resolve()
    root.mkdir(parents=True, exist_ok=False)

    # A binary without the direct transport would answer every request from
    # something else entirely and the numbers would look fine.
    # The literal as it appears in .rodata: fpm_http_direct_request.c logs
    # "[pool %s] %s requires pm = static", so the subject is a format argument
    # and the rendered sentence is never in the binary at all.
    strings = subprocess.check_output(["strings", binary], text=True)
    if "requires pm = static" not in strings:
        raise RuntimeError("wrong binary: missing direct transport marker")

    (root / "index.php").write_text(FRONT_CONTROLLER)
    metadata = {
        "binary": str(binary),
        "sha256": hashlib.sha256(binary.read_bytes()).hexdigest(),
        "version": subprocess.check_output([binary, "-v"], text=True).strip(),
        "settings": vars(args) | {"binary": str(binary), "scratch": str(root)},
        "platform": subprocess.check_output(["uname", "-a"], text=True).strip(),
        "load_before": os.getloadavg(),
    }
    (root / "metadata.json").write_text(json.dumps(metadata, indent=2))

    results = []
    for index, workers in enumerate(args.workers):
        port = args.base_port + index
        directory = root / f"workers-{workers}"
        directory.mkdir()
        config = directory / "fpm.conf"
        config.write_text(f"""[global]
error_log = {directory}/fpm.log
pid = {directory}/fpm.pid
daemonize = no
[fairness]
listen = 127.0.0.1:{port}
pool.type = http-direct
pool.executor = classic
http.front_controller = /index.php
pm = static
pm.max_children = {workers}
pm.max_requests = 0
chdir = {root}
catch_workers_output = no
request_cpu_tracking = no
php_admin_value[max_execution_time] = 0
php_admin_value[opcache.enable] = 0
""")
        log = open(directory / "stderr.log", "w")
        fpm = subprocess.Popen([binary, "-n", "-F", "-y", config], stdout=log, stderr=log)
        try:
            wait_until_serving(port, fpm)
            for repeat in range(args.repeats):
              for name, records in [
                ("burst", burst(port, args.connections)),
                ("keepalive", keepalive(port, args.connections, args.seconds)),
                ("slow-peer", slow_peer(port, args.connections, args.slow_ms)),
              ]:
                entry = {"workload": name, "workers": workers, "repeat": repeat + 1} | summarise(records)
                entry["idle_workers"] = workers - entry["workers_used"]
                if name == "slow-peer":
                    entry |= slow_peer_verdict(records, args.slow_ms)
                (root / f"{name}-w{workers}-r{repeat + 1}.json").write_text(
                    json.dumps(records, indent=2, default=str))
                results.append(entry)
                print(json.dumps(entry))
        finally:
            # By our own pid file, never by binary name: the box is shared.
            pid_file = directory / "fpm.pid"
            if pid_file.exists():
                try:
                    os.kill(int(pid_file.read_text().strip()), 15)
                except (OSError, ValueError):
                    pass
            try:
                fpm.wait(timeout=15)
            except subprocess.TimeoutExpired:
                fpm.kill()
            log.close()

    (root / "results.json").write_text(json.dumps(results, indent=2))
    print(f"\nwrote {root / 'results.json'}")


if __name__ == "__main__":
    main()

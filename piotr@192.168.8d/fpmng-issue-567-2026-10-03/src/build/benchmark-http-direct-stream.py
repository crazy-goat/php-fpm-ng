#!/usr/bin/env python3
"""Compare buffered and streamed HTTP-direct responses (issue #56) on Linux.

Same shape as benchmark-http-direct.py: an explicit scratch folder, its own
port range, only its own processes, JSON and raw samples left behind.

What it measures, for one response size, on two otherwise identical pools that
differ only in http.stream:

  ttfb_ms   milliseconds from the request going out to the first body byte
  total_ms  milliseconds to the last body byte
  peak_rss  the largest resident size the pool's worker child reached while
            producing the response, sampled from /proc

The buffered pool cannot answer a response above FPM_DIRECT_RESPONSE_MAX at
all; the run records that as an outcome rather than a number.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import socket
import statistics
import subprocess
import threading
import time
import urllib.request

FRONT_CONTROLLER = """<?php
/* One 64 KiB piece per iteration, so the response size is the only variable
 * and the script's own memory stays flat whichever pool runs it. */
$pieces = (int) ($_GET['pieces'] ?? 16);
for ($i = 0; $i < $pieces; $i++) {
    echo str_repeat(chr(65 + $i % 26), 65536);
}
"""


def descendants(pid):
    result = [pid]
    try:
        for child in Path(f"/proc/{pid}/task/{pid}/children").read_text().split():
            result.extend(descendants(int(child)))
    except FileNotFoundError:
        pass
    return result


def rss_bytes(root):
    total = 0
    for pid in descendants(root):
        try:
            fields = Path(f"/proc/{pid}/stat").read_text().rsplit(")", 1)[1].split()
            total += int(fields[21]) * os.sysconf("SC_PAGE_SIZE")
        except (FileNotFoundError, IndexError, ProcessLookupError):
            pass
    return total


class RssSampler(threading.Thread):
    """Peak RSS of the pool while one response is in flight. 5 ms is short
    enough to catch an 8 MiB buffer that exists for a few tens of ms."""

    def __init__(self, pid, interval=0.005):
        super().__init__(daemon=True)
        self.pid, self.interval, self.peak, self.stop = pid, interval, 0, False

    def run(self):
        while not self.stop:
            self.peak = max(self.peak, rss_bytes(self.pid))
            time.sleep(self.interval)


def fetch(port, pieces):
    """Raw HTTP/1.1, so the first body byte is observed and not a library's
    idea of when the response began. Returns (ttfb, total, status, bytes)."""
    with socket.create_connection(("127.0.0.1", port), timeout=30) as s:
        started = time.monotonic()
        s.sendall(f"GET /?pieces={pieces} HTTP/1.1\r\nHost: b\r\nConnection: close\r\n\r\n".encode())
        buffered = b""
        while b"\r\n\r\n" not in buffered:
            part = s.recv(65536)
            if not part:
                raise RuntimeError("no response head")
            buffered += part
        head, rest = buffered.split(b"\r\n\r\n", 1)
        status = int(head.split(b" ", 2)[1])
        ttfb = time.monotonic() - started if rest else None
        body = len(rest)
        while True:
            part = s.recv(1 << 20)
            if not part:
                break
            if ttfb is None:
                ttfb = time.monotonic() - started
            body += len(part)
        return ttfb, time.monotonic() - started, status, body, head.decode(errors="replace")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path)
    parser.add_argument("scratch", type=Path)
    parser.add_argument("--base-port", type=int, default=28184)
    parser.add_argument("--rounds", type=int, default=15)
    # 64 pieces = 4 MiB, which both pools can answer; 256 = 16 MiB, which only
    # the streaming one can.
    parser.add_argument("--pieces", type=int, nargs="+", default=[64, 256])
    args = parser.parse_args()
    binary = args.binary.resolve()
    root = args.scratch.resolve()
    root.mkdir(parents=True, exist_ok=False)
    strings = subprocess.check_output(["strings", binary], text=True)
    marker = "http.stream requires http.stream_write_timeout > 0"
    if marker not in strings:
        raise RuntimeError("wrong binary: missing the http.stream marker")
    (root / "index.php").write_text(FRONT_CONTROLLER)
    metadata = {
        "binary": str(binary), "sha256": hashlib.sha256(binary.read_bytes()).hexdigest(),
        "marker": marker, "version": subprocess.check_output([binary, "-v"], text=True),
        "settings": vars(args) | {"binary": str(binary), "scratch": str(root)},
        "platform": subprocess.check_output(["uname", "-a"], text=True).strip(),
        "load_before": os.getloadavg(),
    }
    (root / "metadata.json").write_text(json.dumps(metadata, indent=2, default=str))

    pools = {"buffered": "http.stream = no", "streamed": "http.stream = yes"}
    ports = dict(zip(pools, range(args.base_port, args.base_port + len(pools))))
    processes, results = [], {}
    try:
        for name, extra in pools.items():
            directory = root / name
            directory.mkdir()
            config = directory / "fpm.conf"
            config.write_text(f"""[global]
error_log = {directory}/fpm.log
pid = {directory}/fpm.pid
daemonize = no
[bench]
listen = 127.0.0.1:{ports[name]}
pool.type = http-direct
pool.executor = classic
pm = static
pm.max_children = 1
pm.max_requests = 0
chdir = {root}
http.front_controller = /index.php
http.read_timeout = 30000
catch_workers_output = no
request_cpu_tracking = no
php_admin_value[max_execution_time] = 0
php_admin_value[output_buffering] = 0
php_admin_value[opcache.enable] = 0
{extra}
""")
            log = open(directory / "stderr.log", "w")
            processes.append((subprocess.Popen([binary, "-n", "-F", "-y", config],
                                               stdout=log, stderr=log), log))
        deadline = time.monotonic() + 20
        for name, port in ports.items():
            while time.monotonic() < deadline:
                try:
                    socket.create_connection(("127.0.0.1", port), timeout=1).close()
                    break
                except OSError:
                    time.sleep(0.05)
            else:
                raise RuntimeError(f"{name} never started; see {root}/{name}/fpm.log")

        for pieces in args.pieces:
            for name, port in ports.items():
                key = f"{name}/{pieces * 64}KiB"
                fpm = [p for p, _ in processes][list(pools).index(name)]
                samples, peak, status, size, head = [], 0, None, None, None
                failure = None
                for _ in range(args.rounds):
                    sampler = RssSampler(fpm.pid)
                    sampler.start()
                    try:
                        ttfb, total, status, size, head = fetch(port, pieces)
                    except Exception as error:      # noqa: BLE001 - recorded, not raised
                        failure = repr(error)
                        sampler.stop = True
                        break
                    finally:
                        sampler.stop = True
                        sampler.join()
                    peak = max(peak, sampler.peak)
                    samples.append((ttfb * 1000, total * 1000))
                results[key] = {
                    "status": status, "bytes": size, "failure": failure,
                    "rounds": len(samples),
                    "ttfb_ms_median": round(statistics.median(s[0] for s in samples), 2) if samples else None,
                    "total_ms_median": round(statistics.median(s[1] for s in samples), 2) if samples else None,
                    "peak_rss_bytes": peak,
                    "head": head,
                }
                print(f"{key}: {json.dumps(results[key], indent=None)[:200]}")
    finally:
        for fpm, log in processes:
            fpm.terminate()
        for fpm, log in processes:
            try:
                fpm.wait(timeout=10)
            except subprocess.TimeoutExpired:
                fpm.kill()
            log.close()
    (root / "results.json").write_text(json.dumps(results, indent=2))
    print(f"\nresults: {root}/results.json")


if __name__ == "__main__":
    main()

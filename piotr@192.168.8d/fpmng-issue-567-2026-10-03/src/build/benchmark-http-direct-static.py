#!/usr/bin/env python3
"""What a static file costs on an HTTP-direct pool, against the two alternatives.

Three backends serve the byte-identical file at the same URL:

  direct-static   pool.type = http-direct with http.static = yes (issue #58)
  direct-php      the same pool with http.static = no, front controller
                  readfile()ing the file -- the path every asset takes today
  nginx           nginx serving it from disk, the reference implementation

Reported per backend: server CPU microseconds per response and per delivered
byte, throughput, and latency. CPU is read from /proc for our own process trees
only (never matched by name), which is what makes the comparison a comparison of
servers and not of the machine.

Requires nginx and wrk. Uses only its own processes, its own scratch folder and
its own port range. The raw wrk output and results.json stay in the scratch
folder.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import signal
import socket
import subprocess
import time
import urllib.request


def descendants(pid):
    result = [pid]
    try:
        for child in Path(f"/proc/{pid}/task/{pid}/children").read_text().split():
            result.extend(descendants(int(child)))
    except FileNotFoundError:
        pass
    return result


def usage(roots):
    ticks = rss = 0
    pids = set()
    for root in roots:
        pids.update(descendants(root))
    for pid in pids:
        try:
            fields = Path(f"/proc/{pid}/stat").read_text().rsplit(")", 1)[1].split()
            ticks += int(fields[11]) + int(fields[12])
            rss += int(fields[21]) * os.sysconf("SC_PAGE_SIZE")
        except FileNotFoundError:
            pass
    return ticks / os.sysconf("SC_CLK_TCK"), rss, sorted(pids)


def latency_ms(value):
    match = re.fullmatch(r"([0-9.]+)(us|ms|s)", value)
    return float(match[1]) * {"us": 0.001, "ms": 1, "s": 1000}[match[2]]


def wait_for(url, body_len, proc, deadline_s=10):
    deadline = time.monotonic() + deadline_s
    while True:
        try:
            body = urllib.request.urlopen(url, timeout=1).read()
            if len(body) != body_len:
                raise RuntimeError(f"unexpected body length from {url}: {len(body)}")
            return
        except OSError:
            if time.monotonic() > deadline or (proc and proc.poll() is not None):
                raise
            time.sleep(0.05)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path)
    parser.add_argument("scratch", type=Path)
    parser.add_argument("--base-port", type=int, default=28174)
    parser.add_argument("--seconds", type=int, default=10)
    parser.add_argument("--rounds", type=int, default=3)
    parser.add_argument("--workers", type=int, default=4)
    # Two sizes on purpose: at 4 KiB the per-request work dominates and at
    # 256 KiB the per-byte work does, and "CPU per byte" only means something
    # when both are reported.
    parser.add_argument("--sizes", default="4096,262144")
    args = parser.parse_args()
    binary = args.binary.resolve()
    root = args.scratch.resolve()
    root.mkdir(parents=True, exist_ok=False)
    sizes = [int(s) for s in args.sizes.split(",")]
    strings = subprocess.check_output(["strings", binary], text=True)
    marker = "requires pm = static"
    if marker not in strings:
        raise RuntimeError("wrong binary: missing direct transport marker")
    nginx = shutil.which("nginx") or "/usr/sbin/nginx"
    metadata = {
        "binary": str(binary), "sha256": hashlib.sha256(binary.read_bytes()).hexdigest(),
        "version": subprocess.check_output([binary, "-v"], text=True),
        "nginx": subprocess.run([nginx, "-v"], capture_output=True, text=True).stderr.strip(),
        "settings": vars(args) | {"binary": str(binary), "scratch": str(root)},
        "platform": subprocess.check_output(["uname", "-a"], text=True).strip(),
        "load_before": os.getloadavg(),
    }
    (root / "metadata.json").write_text(json.dumps(metadata, indent=2))

    docroot = root / "docroot"
    docroot.mkdir()
    for size in sizes:
        # Incompressible and non-repeating, so nothing along the way can be
        # clever about it; the same bytes for all three backends.
        (docroot / f"asset-{size}.css").write_bytes(os.urandom(size))
    # readfile() and nothing else: this arm is meant to be the cheapest honest
    # way to serve a file through PHP, not a straw man.
    (docroot / "front.php").write_text("""<?php
$path = realpath(__DIR__ . $_SERVER['PATH_INFO']);
if ($path === false || !str_starts_with($path, __DIR__ . '/')) {
    http_response_code(404);
    return;
}
header('Content-Type: text/css; charset=UTF-8');
header('Content-Length: ' . filesize($path));
readfile($path);
""")

    names = ["direct-static", "direct-php", "nginx"]
    ports = dict(zip(names, range(args.base_port, args.base_port + len(names))))
    reservations = []
    for port in ports.values():
        s = socket.socket()
        s.bind(("127.0.0.1", port))
        reservations.append(s)
    for s in reservations:
        s.close()

    processes = []
    roots = {}
    logs = []
    try:
        for name, port in ports.items():
            directory = root / name
            directory.mkdir()
            log = open(directory / "stderr.log", "w")
            logs.append(log)
            if name == "nginx":
                nginx_config = directory / "nginx.conf"
                nginx_config.write_text(f"""worker_processes {args.workers};
pid {directory}/nginx.pid;
error_log {directory}/nginx.log warn;
events {{ worker_connections 1024; }}
http {{
    access_log off;
    sendfile on;
    include {Path(nginx).parent.parent}/../etc/nginx/mime.types;
    default_type application/octet-stream;
    client_body_temp_path {directory}/body;
    proxy_temp_path {directory}/proxy;
    fastcgi_temp_path {directory}/fastcgi;
    uwsgi_temp_path {directory}/uwsgi;
    scgi_temp_path {directory}/scgi;
    server {{
        listen 127.0.0.1:{port};
        root {docroot};
    }}
}}
""")
                proc = subprocess.Popen(
                    [nginx, "-p", str(directory) + "/", "-c", str(nginx_config), "-g", "daemon off;"],
                    stdout=log, stderr=log)
                processes.append(proc)
                roots[name] = [proc.pid]
                wait_for(f"http://127.0.0.1:{port}/asset-{sizes[0]}.css", sizes[0], proc)
                continue
            static = "yes" if name == "direct-static" else "no"
            config = directory / "fpm.conf"
            config.write_text(f"""[global]
error_log = {directory}/fpm.log
pid = {directory}/fpm.pid
daemonize = no
[bench]
listen = 127.0.0.1:{port}
pool.type = http-direct
pool.executor = classic
pm = static
pm.max_children = {args.workers}
pm.max_requests = 0
chdir = {docroot}
http.front_controller = /front.php
http.static = {static}
catch_workers_output = no
request_cpu_tracking = no
php_admin_value[max_execution_time] = 0
php_admin_value[opcache.enable] = 0
""")
            proc = subprocess.Popen([binary, "-n", "-F", "-y", str(config)], stdout=log, stderr=log)
            processes.append(proc)
            roots[name] = [proc.pid]
            wait_for(f"http://127.0.0.1:{port}/asset-{sizes[0]}.css", sizes[0], proc)

        results = []
        for size in sizes:
            for concurrency in [1, 32]:
                for repeat in range(args.rounds):
                    # Rotate the order so no backend is always measured first.
                    order = names[repeat % len(names):] + names[:repeat % len(names)]
                    for name in order:
                        url = f"http://127.0.0.1:{ports[name]}/asset-{size}.css"
                        body = urllib.request.urlopen(url, timeout=5).read()
                        if len(body) != size:
                            raise RuntimeError(f"body length mismatch: {name}/{size}: {len(body)}")
                        command = ["wrk", "-t", str(min(2, concurrency)), "-c", str(concurrency),
                                   "--timeout", "5s", "--latency"]
                        subprocess.run(command + ["-d", "2s", url], check=True, stdout=subprocess.DEVNULL)
                        before = usage(roots[name])
                        started = time.monotonic()
                        output = subprocess.check_output(command + ["-d", f"{args.seconds}s", url], text=True)
                        elapsed = time.monotonic() - started
                        after = usage(roots[name])
                        label = f"size{size}-c{concurrency}-r{repeat + 1}-{name}"
                        (root / f"{label}.txt").write_text(output)
                        count = int(re.search(r"(\d+) requests in", output)[1])
                        rejected = re.search(r"Non-2xx or 3xx responses:\s+(\d+)", output)
                        rejected = int(rejected[1]) if rejected else 0
                        if rejected:
                            raise RuntimeError(f"{label}: {rejected} non-2xx responses")
                        socket_errors = re.search(r"Socket errors: (.+)", output)
                        rps = float(re.search(r"Requests/sec:\s+([0-9.]+)", output)[1])
                        cpu_s = after[0] - before[0]
                        result = {
                            "backend": name, "size": size, "concurrency": concurrency, "round": repeat + 1,
                            "rps": rps, "requests": count,
                            "socket_errors": socket_errors[1] if socket_errors else None,
                            "p50_ms": latency_ms(re.search(r"^\s+50%\s+(\S+)", output, re.MULTILINE)[1]),
                            "p99_ms": latency_ms(re.search(r"^\s+99%\s+(\S+)", output, re.MULTILINE)[1]),
                            "server_cpu_us_per_response": cpu_s * 1e6 / count,
                            "server_cpu_ns_per_byte": cpu_s * 1e9 / (count * size),
                            "server_rss_bytes_end": after[1], "pids": after[2],
                            "elapsed": elapsed, "load": os.getloadavg(),
                        }
                        results.append(result)
                        (root / "results.json").write_text(json.dumps(results, indent=2))
                        print(json.dumps(result), flush=True)
    finally:
        # Every PID here came back from our own Popen; never match by name.
        for proc in reversed(processes):
            if proc.poll() is None:
                proc.send_signal(signal.SIGQUIT)
        for proc in reversed(processes):
            try:
                proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                proc.terminate()
                try:
                    proc.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.wait()
        for log in logs:
            log.close()


if __name__ == "__main__":
    main()

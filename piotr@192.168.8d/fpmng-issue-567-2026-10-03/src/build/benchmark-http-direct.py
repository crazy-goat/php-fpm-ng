#!/usr/bin/env python3
"""Compare nginx+FastCGI, the HTTP gateway, and HTTP-direct on Linux.

Requires nginx and wrk. Uses only its own processes and an explicit scratch
folder/port range. JSON and raw wrk output remain in the scratch folder.
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


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path)
    parser.add_argument("scratch", type=Path)
    parser.add_argument("--base-port", type=int, default=28154)
    parser.add_argument("--seconds", type=int, default=10)
    parser.add_argument("--rounds", type=int, default=3)
    parser.add_argument("--workers", type=int, default=4)
    args = parser.parse_args()
    binary = args.binary.resolve()
    root = args.scratch.resolve()
    root.mkdir(parents=True, exist_ok=False)
    strings = subprocess.check_output(["strings", binary], text=True)
    # The literal in the binary, not a rendered one: fpm_http_direct_request.c
    # logs "[pool %s] %s requires pm = static", so the pool type is a format
    # argument and never appears in .rodata next to the rest. This guard used to
    # look for "http-direct requires pm = static" and therefore rejected every
    # binary ever passed to it, including correct ones.
    marker = "requires pm = static"
    if marker not in strings:
        raise RuntimeError("wrong binary: missing direct transport marker")
    nginx = shutil.which("nginx") or "/usr/sbin/nginx"
    metadata = {
        "binary": str(binary), "sha256": hashlib.sha256(binary.read_bytes()).hexdigest(),
        "marker": marker, "version": subprocess.check_output([binary, "-v"], text=True),
        "nginx": subprocess.run([nginx, "-v"], capture_output=True, text=True).stderr.strip(),
        "settings": vars(args) | {"binary": str(binary), "scratch": str(root)},
        "platform": subprocess.check_output(["uname", "-a"], text=True).strip(),
        "load_before": os.getloadavg(),
    }
    (root / "metadata.json").write_text(json.dumps(metadata, indent=2))
    (root / "index.php").write_text("""<?php
if (($_GET['work'] ?? '') === 'cpu') {
    $sum = 0;
    for ($i = 0; $i < 30000; $i++) { $sum = ($sum + $i) % 1000003; }
    echo $sum, "\\n";
} else { echo "OK\\n"; }
""")
    # No application cache: both transports use this same file and PHP binary.
    expected_cpu = str(sum(range(30000)) % 1000003).encode() + b"\n"
    ports = dict(zip(["nginx-fastcgi", "http", "http-direct"], range(args.base_port, args.base_port + 3)))
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
            pool_type = "fastcgi" if name == "nginx-fastcgi" else name
            listen = f"127.0.0.1:{port}" if name == "http-direct" else str(directory / "fpm.sock")
            extra = "pool.executor = classic\n" if name == "http-direct" else ""
            # Issue #388: the "http" arm is now a gateway in front of a fastcgi
            # pool, not the retired pool.type = http. The public port is the
            # gateway's `listen`; the workers stay on the unix socket.
            gateway = ""
            if name == "http":
                pool_type = "fastcgi"
                gateway = (
                    f"[gw]\npool.type = gateway\nlisten = 127.0.0.1:{port}\n"
                    f"chdir = {root}\nhttp.gateways = 1\nhttp.static = no\n"
                    f"http.route[bench] = /\n\n"
                )
            config = directory / "fpm.conf"
            config.write_text(f"""[global]
error_log = {directory}/fpm.log
pid = {directory}/fpm.pid
daemonize = no
{gateway}[bench]
listen = {listen}
pool.type = {pool_type}
pm = static
pm.max_children = {args.workers}
pm.max_requests = 0
chdir = {root}
catch_workers_output = no
request_cpu_tracking = no
php_admin_value[max_execution_time] = 0
php_admin_value[opcache.enable] = 0
{extra}""")
            log = open(directory / "stderr.log", "w")
            logs.append(log)
            fpm = subprocess.Popen([binary, "-n", "-F", "-y", config], stdout=log, stderr=log)
            processes.append(fpm)
            roots[name] = [fpm.pid]
            if name == "nginx-fastcgi":
                nginx_config = directory / "nginx.conf"
                nginx_config.write_text(f"""worker_processes 1;
pid {directory}/nginx.pid;
error_log {directory}/nginx.log warn;
events {{ worker_connections 1024; }}
http {{
    access_log off;
    client_body_temp_path {directory}/body;
    fastcgi_temp_path {directory}/fastcgi;
    upstream php {{ server unix:{listen}; }}
    server {{
        listen 127.0.0.1:{port};
        location / {{
            fastcgi_pass php;
            fastcgi_keep_conn off;
            fastcgi_param SCRIPT_FILENAME {root}/index.php;
            fastcgi_param SCRIPT_NAME /index.php;
            fastcgi_param REQUEST_METHOD $request_method;
            fastcgi_param QUERY_STRING $query_string;
            fastcgi_param REQUEST_URI $request_uri;
            fastcgi_param CONTENT_TYPE $content_type;
            fastcgi_param CONTENT_LENGTH $content_length;
            fastcgi_param SERVER_PROTOCOL $server_protocol;
            fastcgi_param SERVER_NAME $host;
            fastcgi_param SERVER_PORT $server_port;
            fastcgi_param REMOTE_ADDR $remote_addr;
            fastcgi_param REMOTE_PORT $remote_port;
            fastcgi_param DOCUMENT_ROOT {root};
            fastcgi_param GATEWAY_INTERFACE CGI/1.1;
        }}
    }}
}}
""")
                proc = subprocess.Popen([nginx, "-p", str(directory) + "/", "-c", nginx_config, "-g", "daemon off;"], stdout=log, stderr=log)
                processes.append(proc)
                roots[name].append(proc.pid)
            deadline = time.monotonic() + 10
            while True:
                try:
                    body = urllib.request.urlopen(f"http://127.0.0.1:{port}/index.php", timeout=1).read()
                    if body != b"OK\n":
                        raise RuntimeError(f"unexpected response from {name}: {body!r}")
                    break
                except OSError:
                    if time.monotonic() > deadline or fpm.poll() is not None:
                        raise
                    time.sleep(0.05)
        results = []
        names = list(ports)
        for work in ["tiny", "cpu"]:
            for concurrency in [1, 32]:
                for repeat in range(args.rounds):
                    # Rotate ordering so a backend is not always measured first.
                    for name in names[repeat % 3:] + names[:repeat % 3]:
                        url = f"http://127.0.0.1:{ports[name]}/index.php?work={work}"
                        body = urllib.request.urlopen(url, timeout=5).read()
                        if body != (expected_cpu if work == "cpu" else b"OK\n"):
                            raise RuntimeError(f"body mismatch: {name}/{work}: {body!r}")
                        command = ["wrk", "-t", str(min(2, concurrency)), "-c", str(concurrency), "--timeout", "5s", "--latency"]
                        subprocess.run(command + ["-d", "2s", url], check=True, stdout=subprocess.DEVNULL)
                        before = usage(roots[name])
                        started = time.monotonic()
                        output = subprocess.check_output(command + ["-d", f"{args.seconds}s", url], text=True)
                        elapsed = time.monotonic() - started
                        after = usage(roots[name])
                        label = f"{work}-c{concurrency}-r{repeat + 1}-{name}"
                        (root / f"{label}.txt").write_text(output)
                        count = int(re.search(r"(\d+) requests in", output)[1])
                        rejected = re.search(r"Non-2xx or 3xx responses:\s+(\d+)", output)
                        rejected = int(rejected[1]) if rejected else 0
                        socket_errors = re.search(r"Socket errors: (.+)", output)
                        rps = float(re.search(r"Requests/sec:\s+([0-9.]+)", output)[1])
                        result = {
                            "backend": name, "work": work, "concurrency": concurrency, "round": repeat + 1,
                            "rps": rps, "successful_rps": rps * (count - rejected) / count,
                            "non_2xx_3xx": rejected,
                            "socket_errors": socket_errors[1] if socket_errors else None,
                            "p50_ms": latency_ms(re.search(r"^\s+50%\s+(\S+)", output, re.MULTILINE)[1]),
                            "p99_ms": latency_ms(re.search(r"^\s+99%\s+(\S+)", output, re.MULTILINE)[1]),
                            "server_cpu_us_per_response": (after[0] - before[0]) * 1e6 / count,
                            "server_cpu_us_per_success": (after[0] - before[0]) * 1e6 / (count - rejected) if count > rejected else None,
                            "server_rss_bytes_end": after[1], "pids": after[2],
                            "elapsed": elapsed, "requests": count, "load": os.getloadavg(),
                        }
                        results.append(result)
                        (root / "results.json").write_text(json.dumps(results, indent=2))
                        print(json.dumps(result), flush=True)
    finally:
        # Every PID here was returned by our own Popen; never match by name.
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

#!/usr/bin/env python3
"""Measure gateway client-index scaling with many idle keep-alive sockets.

This is the manual benchmark for issue #490. It deliberately keeps the oldest
connection and sends local /ping requests to it, so the old linked-list lookup
is on the measured path. It also measures close/reopen churn separately because
establishing N clients used to be quadratic as well.

The application pool is present but /ping is answered by the gateway. A marker
file proves no application request ran.

Example:
    build/benchmark-gateway-client-index.py \
        --binary /path/to/php-fpm-ng --label before \
        --base-port 28400 --output before.json \
        --php-src-ref php-8.5.9
"""
import argparse
import datetime
import hashlib
import http.client
import json
import os
from pathlib import Path
import platform
import re
import resource
import signal
import socket
import statistics
import subprocess
import sys
import tempfile
import time

GATEWAY_MARKER = "http.gateways must be at least 1"
CONNECTION_COUNTS = (1, 1000, 5000, 10000)
BATCHES = 3
REQUESTS_PER_BATCH = 500
CHURN_ROUNDS = 3
CHURN_PER_ROUND = 500
WARMUP = 50


def percentile(values, fraction):
    ordered = sorted(values)
    return ordered[min(len(ordered) - 1, int(len(ordered) * fraction))]


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def command_output(command):
    try:
        return subprocess.check_output(command, text=True, stderr=subprocess.STDOUT).strip().splitlines()[0]
    except (OSError, subprocess.CalledProcessError, IndexError):
        return "not measured"


def raise_nofile(wanted):
    soft, hard = resource.getrlimit(resource.RLIMIT_NOFILE)
    target = min(hard, max(wanted, soft))
    if target < wanted:
        raise SystemExit(f"need {wanted} descriptors; hard limit is {hard}")
    resource.setrlimit(resource.RLIMIT_NOFILE, (target, hard))
    return target


class PortRange:
    """Refuse a foreign listener before the measured master can start."""

    def __init__(self, ports):
        self.sockets = []
        try:
            for port in ports:
                sock = socket.socket()
                sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
                sock.bind(("127.0.0.1", port))
                self.sockets.append(sock)
        except Exception:
            self.close()
            raise

    def close(self):
        for sock in self.sockets:
            sock.close()
        self.sockets.clear()


def process_tree(root):
    parents = {}
    for entry in Path("/proc").iterdir():
        if not entry.name.isdigit():
            continue
        try:
            stat = (entry / "stat").read_text()
            tail = stat[stat.rfind(")") + 2:].split()
            parents[int(entry.name)] = int(tail[1])
        except (OSError, ValueError, IndexError):
            continue
    tree = {root}
    changed = True
    while changed:
        changed = False
        for pid, ppid in parents.items():
            if ppid in tree and pid not in tree:
                tree.add(pid)
                changed = True
    return tree


def listening_inodes(port):
    inodes = set()
    for name in ("/proc/net/tcp", "/proc/net/tcp6"):
        try:
            lines = Path(name).read_text().splitlines()[1:]
        except OSError:
            continue
        for line in lines:
            fields = line.split()
            if len(fields) > 9 and fields[3] == "0A" and int(fields[1].rsplit(":", 1)[1], 16) == port:
                inodes.add(fields[9])
    return inodes


def process_socket_inodes(pids):
    inodes = set()
    for pid in pids:
        fd_dir = Path("/proc") / str(pid) / "fd"
        try:
            entries = list(fd_dir.iterdir())
        except OSError:
            continue
        for fd in entries:
            try:
                target = os.readlink(fd)
            except OSError:
                continue
            if target.startswith("socket:["):
                inodes.add(target[8:-1])
    return inodes


def port_owned_by_tree(port, root_pid):
    return bool(listening_inodes(port) & process_socket_inodes(process_tree(root_pid)))


def connect(host, port, timeout=10):
    return http.client.HTTPConnection(host, port, timeout=timeout)


def ping(conn, host, port, path="/ping"):
    started = time.perf_counter()
    conn.request("GET", path, headers={"Host": host, "Connection": "keep-alive"})
    response = conn.getresponse()
    body = response.read()
    elapsed_ms = (time.perf_counter() - started) * 1000
    if response.status != 200 or body != b"pong":
        conn.close()
        raise RuntimeError(f"{path}: status={response.status} body={body!r}")
    return elapsed_ms


def metrics_connections_open(port):
    conn = connect("127.0.0.1", port)
    try:
        conn.request("GET", "/metrics", headers={"Host": "operator", "Connection": "close"})
        response = conn.getresponse()
        body = response.read().decode("utf-8", "replace")
    finally:
        conn.close()
    match = re.search(r'fpmng_gateway_connections_open\{pool="gw"\} (\d+)', body)
    if not match:
        raise RuntimeError("connections_open is absent from /metrics")
    return int(match.group(1))


def wait_for_port(port, process, log_path):
    deadline = time.monotonic() + 15
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError(
                f"gateway exited with {process.returncode}:\n{log_path.read_text(errors='replace')}"
            )
        if port_owned_by_tree(port, process.pid):
            try:
                with socket.create_connection(("127.0.0.1", port), timeout=0.2):
                    return
            except OSError:
                pass
        time.sleep(0.05)
    raise RuntimeError(
        f"measured process tree never owned {port}:\n{log_path.read_text(errors='replace')}"
    )


def wait_for_gauge(port, expected, timeout=15):
    deadline = time.monotonic() + timeout
    value = None
    while time.monotonic() < deadline:
        value = metrics_connections_open(port)
        if value == expected:
            return
        time.sleep(0.05)
    raise RuntimeError(f"connections_open: expected {expected}, got {value}")


def stop_process(process):
    try:
        os.killpg(process.pid, signal.SIGTERM)
    except ProcessLookupError:
        pass
    try:
        process.wait(timeout=10)
    except subprocess.TimeoutExpired:
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        process.wait(timeout=5)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", required=True)
    parser.add_argument("--label", required=True)
    parser.add_argument("--base-port", type=int, required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--php-src-ref", required=True)
    parser.add_argument("--scratch", default=os.path.expanduser("~/rd"))
    parser.add_argument("--keep-scratch", action="store_true")
    args = parser.parse_args()

    binary = Path(args.binary).resolve()
    try:
        binary_strings = subprocess.check_output(["strings", str(binary)], text=True)
    except (OSError, subprocess.CalledProcessError) as exc:
        raise SystemExit(f"refusing {binary}: cannot inspect strings ({exc})")
    if GATEWAY_MARKER not in binary_strings:
        raise SystemExit(f"refusing {binary}: gateway marker is absent")
    binary_sha = sha256(binary)
    gateway_port = args.base_port
    operator_port = args.base_port + 1
    app_port = args.base_port + 2
    nofile = raise_nofile(32768)
    Path(args.scratch).mkdir(parents=True, exist_ok=True)
    root = Path(tempfile.mkdtemp(prefix=f"gw-client-index-{args.label}-", dir=args.scratch))
    docroot = root / "docroot"
    docroot.mkdir()
    marker = docroot / "application-ran"
    (docroot / "index.php").write_text("<?php file_put_contents(__DIR__ . '/application-ran', '1'); echo 'app';\n")
    config_path = root / "fpm.conf"
    log_path = root / "fpm.log"
    config = f"""[global]
error_log = {log_path}
pid = {root / 'fpm.pid'}
daemonize = no
[gw]
pool.type = gateway
listen = 127.0.0.1:{gateway_port}
chdir = {docroot}
http.gateways = 1
http.reuseport = off
http.read_timeout = 0
http.idle_timeout = 0
http.route[app] = /app
ping.path = /ping
ping.response = pong
operator.metrics_listen = 127.0.0.1:{operator_port}
[app]
pool.type = fastcgi
listen = 127.0.0.1:{app_port}
pm = static
pm.max_children = 1
chdir = {docroot}
catch_workers_output = yes
php_admin_value[max_execution_time] = 0
"""
    config_path.write_text(config)

    metadata = {
        "label": args.label,
        "measured_at_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "binary": str(binary),
        "binary_sha256": binary_sha,
        "binary_version": command_output([str(binary), "-n", "-v"]),
        "php_src_ref": args.php_src_ref,
        "compiler": command_output(["cc", "--version"]),
        "libevent_pkgconfig": command_output(["pkg-config", "--modversion", "libevent"]),
        "python": sys.version,
        "platform": platform.platform(),
        "cpu": next((line.split(":", 1)[1].strip() for line in Path("/proc/cpuinfo").read_text().splitlines()
                      if line.startswith("model name")), "not measured"),
        "nofile": nofile,
        "loadavg_before": os.getloadavg(),
        "ports": {"gateway": gateway_port, "operator": operator_port, "app": app_port},
        "ports_reserved_and_listener_owner_verified": True,
        "connection_counts": list(CONNECTION_COUNTS),
        "batches": BATCHES,
        "requests_per_batch": REQUESTS_PER_BATCH,
        "churn": {"rounds": CHURN_ROUNDS, "per_round": CHURN_PER_ROUND},
        "config": config,
    }
    results = []
    reserved = PortRange((gateway_port, operator_port, app_port))
    log_handle = log_path.open("wb")
    try:
        process = subprocess.Popen(
            [str(binary), "-n", "-y", str(config_path), "-O", "-F"],
            stdout=log_handle,
            stderr=subprocess.STDOUT,
            start_new_session=True,
        )
    except Exception:
        reserved.close()
        log_handle.close()
        raise
    try:
        for port in (gateway_port, operator_port, app_port):
            wait_for_port(port, process, log_path)
        reserved.close()
        for count in CONNECTION_COUNTS:
            connections = []
            setup_started = time.perf_counter()
            for _ in range(count):
                conn = connect("127.0.0.1", gateway_port)
                ping(conn, "127.0.0.1", gateway_port)
                connections.append(conn)
            setup_ms = (time.perf_counter() - setup_started) * 1000
            wait_for_gauge(operator_port, count)
            for _ in range(WARMUP):
                ping(connections[0], "127.0.0.1", gateway_port)

            batches = []
            for batch in range(BATCHES):
                samples = [ping(connections[0], "127.0.0.1", gateway_port)
                           for _ in range(REQUESTS_PER_BATCH)]
                batches.append({
                    "batch": batch + 1,
                    "mean_ms": statistics.fmean(samples),
                    "p50_ms": percentile(samples, 0.50),
                    "p95_ms": percentile(samples, 0.95),
                    "p99_ms": percentile(samples, 0.99),
                    "samples_ms": samples,
                })

            churn_started = time.perf_counter()
            churn_operations = 0
            for round_number in range(CHURN_ROUNDS):
                for offset in range(min(CHURN_PER_ROUND, count)):
                    index = (round_number * 337 + offset * 97) % count
                    connections[index].close()
                    replacement = connect("127.0.0.1", gateway_port)
                    ping(replacement, "127.0.0.1", gateway_port)
                    connections[index] = replacement
                    churn_operations += 1
            churn_ms = (time.perf_counter() - churn_started) * 1000
            wait_for_gauge(operator_port, count)
            barrier_ms = ping(connections[-1], "127.0.0.1", gateway_port)
            if marker.exists():
                raise RuntimeError("application marker exists: /ping was proxied")

            results.append({
                "connections": count,
                "setup_ms": setup_ms,
                "batches": batches,
                "churn": {
                    "operations": churn_operations,
                    "total_ms": churn_ms,
                    "barrier_ms": barrier_ms,
                },
            })
            for conn in connections:
                conn.close()
            wait_for_gauge(operator_port, 0)
    finally:
        stop_process(process)
        reserved.close()
        log_handle.close()
        metadata["loadavg_after"] = os.getloadavg()
        metadata["fpm_log"] = log_path.read_text(errors="replace")
        output = Path(args.output).resolve()
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_text(json.dumps({"metadata": metadata, "results": results}, indent=2) + "\n")
        if not args.keep_scratch:
            for path in sorted(root.rglob("*"), reverse=True):
                path.unlink() if path.is_file() or path.is_symlink() else path.rmdir()
            root.rmdir()

    print(f"{args.label}: binary={binary_sha}")
    for row in results:
        means = "/".join(f"{batch['mean_ms']:.4f}" for batch in row["batches"])
        print(
            f"connections={row['connections']:5d} setup_ms={row['setup_ms']:.1f} "
            f"batch_means_ms={means} churn_ms={row['churn']['total_ms']:.1f}"
        )


if __name__ == "__main__":
    main()

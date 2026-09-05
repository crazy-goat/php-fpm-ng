#!/usr/bin/env python3
"""Minimal FastCGI client: one TCP connection with FCGI_KEEP_CONN, N sequential
requests for a script whose output exceeds the 8 KB FastCGI output buffer.
Prints per-request wall time. Usage: fcgi_nodelay_repro.py HOST PORT /abs/path/script.php [N]"""
import socket, struct, sys, time

FCGI_BEGIN_REQUEST, FCGI_END_REQUEST, FCGI_PARAMS, FCGI_STDIN, FCGI_STDOUT = 1, 3, 4, 5, 6
FCGI_RESPONDER, FCGI_KEEP_CONN = 1, 1

def rec(t, body, rid=1):
    pad = (-len(body)) % 8
    return struct.pack(">BBHHBx", 1, t, rid, len(body), pad) + body + b"\0" * pad

def kv(k, v):
    k, v = k.encode(), v.encode()
    def ln(n): return struct.pack(">B", n) if n < 128 else struct.pack(">I", n | 0x80000000)
    return ln(len(k)) + ln(len(v)) + k + v

def request(sock, script):
    params = b"".join(kv(k, v) for k, v in {
        "SCRIPT_FILENAME": script, "REQUEST_METHOD": "GET", "REQUEST_URI": "/",
        "SCRIPT_NAME": "/x.php", "SERVER_PROTOCOL": "HTTP/1.1", "GATEWAY_INTERFACE": "CGI/1.1",
        "QUERY_STRING": "", "CONTENT_LENGTH": "0"}.items())
    sock.sendall(rec(FCGI_BEGIN_REQUEST, struct.pack(">HB5x", FCGI_RESPONDER, FCGI_KEEP_CONN))
                 + rec(FCGI_PARAMS, params) + rec(FCGI_PARAMS, b"") + rec(FCGI_STDIN, b""))
    out = 0
    while True:
        hdr = b""
        while len(hdr) < 8:
            c = sock.recv(8 - len(hdr)); assert c; hdr += c
        _, t, _, ln, pad = struct.unpack(">BBHHBx", hdr)
        body = b""
        while len(body) < ln + pad:
            c = sock.recv(ln + pad - len(body)); assert c; body += c
        if t == FCGI_STDOUT: out += ln
        if t == FCGI_END_REQUEST: return out

host, port, script = sys.argv[1], int(sys.argv[2]), sys.argv[3]
n = int(sys.argv[4]) if len(sys.argv) > 4 else 20
s = socket.create_connection((host, port))
times = []
for i in range(n):
    t0 = time.perf_counter(); out = request(s, script); times.append((time.perf_counter() - t0) * 1e3)
times.sort()
print(f"{n} requests, {out} bytes of stdout each: median {times[n//2]:.2f} ms, min {times[0]:.2f} ms, max {times[-1]:.2f} ms")

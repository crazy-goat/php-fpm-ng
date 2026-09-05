# Draft bug report for php/php-src (NOT yet filed)

Status: tekst gotowy, nic nie wyslane. Zglosic jako issue na github.com/php/php-src
(komponent FastCGI / FPM), a lacze potem wpisac do `patches/README.md` przy 0002.

---

**Title:** FastCGI: TCP_NODELAY is never enabled on keep-alive connections (req->tcp only set on Windows)

**Description**

`main/fastcgi.c` decides whether to set `TCP_NODELAY` on a `FCGI_KEEP_CONN`
connection based on `req->tcp`:

```c
/* fcgi_read_request() */
req->keep = (b->flags & FCGI_KEEP_CONN);
#ifdef TCP_NODELAY
if (req->keep && req->tcp && !req->nodelay) {
    ...
    setsockopt(req->fd, IPPROTO_TCP, TCP_NODELAY, (char*)&on, sizeof(on));
    req->nodelay = 1;
}
#endif
```

But `req->tcp` is only ever assigned inside `#ifdef _WIN32` in
`fcgi_init_request()`:

```c
#ifdef _WIN32
	req->tcp = !GetNamedPipeInfo((HANDLE)_get_osfhandle(req->listen_socket), NULL, NULL, NULL, NULL);
#endif
```

On every other platform the field keeps its `calloc()` zero, so the branch is
dead code and `TCP_NODELAY` is never set. The code clearly intends to set it
(the field, the flag and the `setsockopt` are all there), it just never fires.

**Two separate claims**

1. *The defect itself* (proven by reading the code, independent of any
   measurement): `req->tcp` is assigned only under `#ifdef _WIN32` and read
   unconditionally, so on non-Windows platforms the `TCP_NODELAY` branch can
   never execute. The field, the `nodelay` flag and the `setsockopt()` exist
   precisely to enable it for keep-alive TCP connections; today they are dead
   code everywhere except Windows. This alone is worth fixing.

2. *The practical impact* (demonstrated only partially, see below). With
   `FCGI_KEEP_CONN` over TCP, a response larger than the 8 KB output buffer
   leaves php-fpm as several `write()` calls; the last one (`FCGI_STDOUT` end
   record + `FCGI_END_REQUEST`, 8-24 bytes) is a small segment sent while
   earlier data is still unacknowledged, so Nagle holds it until the peer ACKs,
   and the peer's delayed ACK can hold that for up to 40 ms (Linux default).

**What we could and could not show**

- With a minimal FastCGI client (Python, below) on Linux loopback: median
  **41.0 ms** per request for a 20 KB response vs **0.08 ms** with the fix,
  and 0.09 ms for a <8 KB response without the fix. That is the 40 ms
  delayed-ACK timer, not CPU.
- With nginx 1.28 (`fastcgi_keep_conn on`) on the same loopback we did **not**
  reproduce it: responses > 8 KB take microseconds with and without the fix.
  We have not yet found a real-nginx configuration that shows the delay, so we
  do not claim that a typical nginx + php-fpm deployment is affected.
- Hypothesis (not verified): nginx reads the response immediately and its
  kernel ACKs as soon as it has two full segments or piggybacks on nothing;
  with a client that reads more slowly, or over a real network, the ACK is
  delayed and Nagle bites. The Python client above is such a client.

**Reproduction** (Linux, loopback)

php-fpm pool:

```
[www]
listen = 127.0.0.1:9900
pm = static
pm.max_children = 1
```

`/tmp/mid.php`:

```php
<?php echo str_repeat("x", 20000);
```

Minimal keep-alive FastCGI client (Python, one TCP connection, N requests):
`fcgi_nodelay_repro.py` (attached below). Run:

```
python3 fcgi_nodelay_repro.py 127.0.0.1 9900 /tmp/mid.php 50
```

Observed (php-fpm from master, 4e55e35ead7, Linux 7.0, loopback):

```
50 requests, 20076 bytes of stdout each: median 41.01 ms, min 0.68 ms, max 41.79 ms
50 requests, 20076 bytes of stdout each: median 41.00 ms, min 0.27 ms, max 41.29 ms
50 requests, 20076 bytes of stdout each: median 41.00 ms, min 0.33 ms, max 41.34 ms

(same client, hello.php with a 77-byte response: median 0.09 ms)
```

With the fix:

```
50 requests, 20076 bytes of stdout each: median 0.08 ms, min 0.07 ms, max 0.71 ms
50 requests, 20076 bytes of stdout each: median 0.08 ms, min 0.07 ms, max 0.28 ms
50 requests, 20076 bytes of stdout each: median 0.08 ms, min 0.07 ms, max 0.27 ms
```

**Fix**

Set `req->tcp` from the peer address family right after `accept()` on
non-Windows platforms (Windows keeps the pipe-vs-socket detection in
`fcgi_init_request()`):

```c
#ifndef _WIN32
	if (req->fd >= 0) {
		req->tcp = (sa.sa.sa_family != AF_UNIX);
	}
#endif
```

Full patch: `0002-fastcgi-tcp-nodelay-never-set.patch`. Applies to PHP-8.3,
PHP-8.4, PHP-8.5 and master.

---

## fcgi_nodelay_repro.py

```python
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
```

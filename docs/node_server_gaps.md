# Differences from a Node.js application server

Status as of 2026-09-06. This document is analysis material, not an approved roadmap.

The comparison covers a typical Node.js application server (for example, `node:http` with Express, Fastify, or Nest), not the bare runtime without libraries. The goal is to identify features that may make sense for FPM-NG without automatically expanding the project's scope.

## Model differences

Production FPM-NG `classic` uses a request-response model: the gateway accepts a request, passes it to a PHP worker, the worker executes the request, and returns the response.

Node.js gives the application direct control over the event loop, listener, connection, and response lifecycle. FPM-NG should not copy every Node feature — some would require changing the SAPI model rather than merely extending the gateway.

## Gaps that matter to ordinary HTTP applications

### Routing and front controller

Node can programmatically map any method and path to a handler. FPM-NG still maps a URI to a script or `index.php`.

There is no configurable equivalent of:

```nginx
try_files $uri $uri/ /index.php?$query_string;
```

This item is already in the FPM-NG plan and matters to PHP frameworks.

### Client timeouts

A typical Node server can control header, request, idle-socket, and keep-alive timeouts separately. The FPM-NG HTTP gateway does not yet have complete protection against slow clients and slow loris attacks.

This item is already in the plan.

### Request-body streaming and backpressure

Node exposes the request body as a stream and can pause receiving when the consumer is slower. The FPM-NG gateway buffers the body in memory before passing the request on.

What is missing:

- streaming request-body forwarding;
- backpressure;
- safe handling of large and slow uploads;
- control over gateway memory growth.

Backpressure is already on the list of known gaps. The implementation method, such as a file buffer, is not yet a design decision.

### Full-pool overload response

When the pool is full, FPM-NG currently returns `502`. The plan is to return a correct `503 Service Unavailable` with `Retry-After`.

Possible short-burst queuing is not currently part of the approved plan.

## Long-lived and bidirectional connections

### Server-Sent Events

Node naturally handles long responses, periodic data flushing, and client-disconnect detection. FPM-NG does not currently have a production-validated model for SSE.

To investigate:

- whether the response is streamed without unbounded buffering;
- whether flush reaches the client;
- behavior after the client disconnects;
- write timeouts and backpressure;
- the effect of a long request on worker occupancy.

SSE is not currently an approved roadmap item.

### WebSocket

Node can perform an HTTP Upgrade and take over a bidirectional socket. FPM-NG does not support WebSockets or handing the connection to a PHP application.

Potential options:

1. do not support WebSockets and leave them to a reverse proxy;
2. add WebSocket tunneling in a future `pool.type = proxy`;
3. expose a separate application model, which would be a much larger change.

The second option is the most compatible with the current architecture. WebSocket is not currently an approved roadmap item.

### Long polling

It can technically work as a long request, but it occupies a worker in the `classic` executor. It requires tests for limits, shutdown, client disconnects, and behavior when the pool is full.

## Application-level transport control

Node gives the application direct access to:

- chunked encoding;
- flushing response fragments;
- HTTP trailers;
- HTTP Upgrade;
- closing or taking over the socket;
- client-disconnect events.

PHP controls the status, headers, and body through the SAPI, but the gateway remains the owner of the transport. Do not give the application direct socket access without a separate security and lifecycle project.

### Cancelling work after a client disconnect

Node can propagate cancellation through socket events and `AbortSignal`. PHP has `connection_aborted()`, but FPM-NG has no consistent mechanism for cancelling active application or asynchronous operations.

To investigate:

- when the worker learns about the disconnect;
- whether blocking I/O can be interrupted;
- whether cancellation can safely trigger a bailout;
- how `classic`, Fiber, and True Async behave.

This is not currently an approved roadmap item.

## Protocol and server features

### TLS

Node has `tls` and `https` modules. FPM-NG does not yet have TLS or ACME. TLS + ACME are already part of the plan and precede `pool.type = proxy`.

### HTTP/2

Node has an `http2` module. FPM-NG supports HTTP/1.1.

HTTP/2 is listed as a nice-to-have after the basic plan is complete. Any implementation should use a proven library such as `nghttp2` and should come only after TLS/ALPN, timeouts, and backpressure.

### Compression

In the Node ecosystem, middleware usually adds compression. FPM-NG does not compress responses.

Streaming `gzip` is listed as a nice-to-have after the basic plan. Brotli is not currently part of the plan.

### Gateway middleware

Node frameworks offer middleware chains for authentication, logging, rate limiting, routing, and response modification. FPM-NG has no extensible middleware system inside the gateway process.

Most application logic should remain in the PHP framework. Gateway middleware makes sense only for transport features or work performed before PHP starts. It is not currently part of the plan.

## Application execution model

### State between requests

Node naturally keeps process state between requests. The production `classic` executor runs the complete PHP request lifecycle and preserves the isolation expected from PHP-FPM.

Changing this model risks leaking application and extension state. This is not a gap that should be removed automatically — isolation is a compatibility property.

### Asynchronous I/O

Node can handle many I/O operations in one process. `classic` assigns a request to a worker. FPM-NG has experimental Fiber and True Async executors, but they are not production-ready.

Known Fiber problems are documented in `docs/fiber_errors.md`. True Async requires a separate PHP engine fork.

### Custom protocols

Node can open TCP/UDP sockets and implement arbitrary protocols. FPM-NG is deliberately focused on FastCGI, HTTP, and running PHP. MQTT, raw TCP/UDP, and custom protocols are not currently project goals.

## Features Node usually does not provide without an additional stack

The comparison does not mean that Node provides everything in a standard installation. A typical installation needs additional libraries or services for:

- ACME;
- framework routing;
- access logging;
- middleware compression;
- process management and restarts;
- cron jobs and supervised processes;
- metrics;
- graceful-shutdown configuration;
- serving static files at the reverse-proxy layer.

FPM-NG already integrates some of these features in one binary: worker management, supervisor, cron, status, static files, ACLs, trusted proxies, and access logging.

## Candidates for further investigation

Without automatically adding them to the roadmap, it is worth running small technical tests in this order:

1. **SSE and response flushing** — check what already works and where buffering occurs.
2. **Client disconnects** — measure when the gateway and worker detect an interruption.
3. **Upload streaming** — describe the current memory flow and possible backpressure points.
4. **Long polling** — check shutdown, timeouts, and behavior when the pool is full.
5. **WebSocket through a future proxy** — evaluate tunneling instead of implementing it in the SAPI.
6. **Cancellation API** — only after the disconnect behavior of each executor is understood.

SSE, reliable client-disconnect detection, and request-body streaming have the best chance of being useful without changing the PHP model. WebSockets are best considered a future reverse-proxy feature, not an executor feature.

## Classification

### Already in the plan

- routing / a `try_files` equivalent;
- client timeouts;
- request-body backpressure;
- `503 Retry-After` for a full pool;
- TLS + ACME;
- `pool.type = proxy`;
- HTTP/2 and gzip as final nice-to-haves.

### Investigate, but not on the roadmap

- SSE;
- WebSocket / WebSocket tunneling;
- long polling;
- cancelling work after a client disconnect;
- an extended streaming API;
- gateway middleware.

### Deliberately not treated as compatibility gaps

- persistent global application state between requests;
- direct socket takeover by an ordinary PHP script;
- arbitrary TCP/UDP servers in the request model;
- a custom HTTP/2 implementation.

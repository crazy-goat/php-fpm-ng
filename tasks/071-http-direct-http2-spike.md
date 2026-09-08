# 071 — Spike: HTTP/2 (and the road to QUIC) for HTTP-direct

Status: open
Type: spike (decision-support analysis, not a feature commitment)

## Why

libevent's evhttp speaks HTTP/1.x only, so HTTP-direct inherits that limit
(documented in `docs/http-direct.md`). Intermediaries (nginx, the `http`
gateway) get HTTP/2 from their own stacks; a direct pool has no intermediary,
so if HTTP/2 matters it must live in the worker. Feasibility depends on
library options (nghttp2 integration versus a different event/HTTP stack) and
on how much of the task 054 code survives; that is a decision-support question.

## Questions to answer

1. Candidate approaches: nghttp2 framing on top of the existing libevent event
   loop; swapping the HTTP layer while keeping the SAPI integration; or
   rejecting HTTP/2 for direct and documenting a front-proxy recommendation.
2. What does HTTP/2 actually require from the rest of the direct stack:
   streaming responses (task 058), per-stream flow control interacting with
   the connection policy (task 063), priority/fairness implications (task 055)?
3. Cost estimate: lines of integration, new dependencies, CI/build impact
   (static-musl and canonical builds), and expected maintenance.
4. Is HTTP/2 worth it without TLS-ALPN first (task 057), and is QUIC/HTTP/3 a
   realistic later step on the chosen approach or a dead end?

## Acceptance criteria

- A written comparison of the candidate approaches with dependency, build, and
  maintenance costs, and a recommendation (implement via X / front-proxy /
  not now) grounded in a minimal proof-of-concept handshake if the
  recommendation is to implement.
- Interaction analysis with tasks 055/058/063 enumerated, not hand-waved.
- Raw artifacts and prototype code retained outside the tree; negative result
  (front-proxy recommendation) is an acceptable outcome.

## Out of scope

- Implementing HTTP/2 support; the follow-up feature task is separate.
- QUIC beyond the viability assessment in question 4.

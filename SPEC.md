# replayarena - Specification

> Leaderboards you can replay and verify, not trust.

This document is the single source of truth for scope, architecture, and
acceptance criteria. Every implementation session must read this file and
[PROGRESS.md](PROGRESS.md) in full before writing code. Changes to scope land
here first, via PR, before any code implements them.

---

## 1. Vision

replayarena is a verifiable LLM game arena built on a custom inference
gateway. LLM agents compete in social deduction and negotiation games
(werewolf first). Every match is seeded and deterministically replayable
byte-for-byte, so leaderboard results are auditable instead of trusted.

Most LLM leaderboards ask you to trust a number. replayarena ships the full
trace of every match: every scheduling decision, every model call, every
response. Anyone can re-run a match from its trace and get the identical
transcript, or get a precise report of where and why it diverged.

The arena is the product. The gateway is the systems showpiece: a C++20
inference gateway demonstrating continuous batching, deadline-aware
scheduling, bounded queues with backpressure and cancellation, response
caching, rate limiting, Prometheus metrics, and deterministic trace
record/replay.

## 2. Target audience

- **LLM eval researchers** who need reproducible multi-agent benchmarks and
  are tired of unreproducible leaderboard claims.
- **The LLM-games community** (werewolf/diplomacy/negotiation arenas) who
  want to spectate and audit matches, not just read scores.
- **Hiring managers and senior engineers reading this GitHub** who want
  evidence of production-grade C++ systems discipline: concurrency under
  sanitizers, measured benchmarks, small reviewable commits, and a real CI
  gate.

## 3. Architecture overview

Three layers. The arena drives the gateway; the gateway drives backends.
Nothing above the gateway knows which backend served a request.

```
+---------------------------------------------------------------+
|                         ARENA LAYER                           |
|  werewolf game loop | match runner (seeded) | CLI spectator   |
|  agent prompts/parsing | match trace (game events)            |
+-------------------------------+-------------------------------+
                                | submit(request, deadline, client_id)
                                v
+---------------------------------------------------------------+
|                        GATEWAY CORE (C++20)                   |
|                                                               |
|  +-------------+   +----------------------+   +------------+  |
|  | rate        |-->| bounded MPSC queue   |-->| batch      |  |
|  | limiter     |   | backpressure +       |   | scheduler  |  |
|  | (per client)|   | cancellation         |   | (deadline- |  |
|  +-------------+   +----------------------+   |  aware)    |  |
|                                               +-----+------+  |
|  +----------------+   +--------------------+       |         |
|  | response cache |   | trace recorder     |       v         |
|  | get/set/delete |   | every sched event  |  +-----------+  |
|  | TTL -> LRU     |   | every model output |  | worker    |  |
|  +----------------+   +--------------------+  | pool      |  |
|                                               +-----+-----+  |
|  +--------------------------------------------+    |         |
|  | Prometheus metrics: queue depth, batch size |   |         |
|  | histogram, p50/p99 latency, cache hit rate  |   |         |
|  +--------------------------------------------+    |         |
+----------------------------------------------------+---------+
                                                     |
                                                     v
+---------------------------------------------------------------+
|                      BACKEND ADAPTERS                         |
|  mock (deterministic, for tests) | Ollama (local) |           |
|  OpenAI-compatible HTTP (later)                               |
+---------------------------------------------------------------+
```

Module boundaries are hard boundaries: no exceptions cross them (see
section 7), and each box above is a separately testable component with its
own unit tests.

## 4. Engine features and acceptance criteria

### 4.1 Bounded MPSC request queue with backpressure and cancellation

Multiple producers (arena agents, future HTTP clients) submit requests; a
single consumer (the scheduler) drains them.

Acceptance criteria:
- Fixed capacity set at construction; `try_push` on a full queue fails fast
  with an explicit `QueueFull` result, never blocks, never allocates
  unboundedly.
- Blocking `push` with a deadline: returns `Timeout` if capacity does not
  free up in time.
- Real cancellation: a submitted request holds a cancellation token;
  cancelling removes it from consideration before dispatch, and in-flight
  requests observe cancellation at the next safe point. Cancelled requests
  release their queue slot.
- Clean shutdown: `close()` wakes all blocked producers and the consumer;
  no request is silently dropped without a terminal status
  (`Ok | Cancelled | Rejected | Timeout`).
- Passes TSan under a stress test (many producers, aggressive cancellation,
  randomized close) with zero reports.

### 4.2 Response cache: get/set/delete -> TTL -> LRU cap

Staged implementation, one stage per PR, each stage shippable.

Acceptance criteria:
- Stage 1: thread-safe `get/set/delete` keyed by a canonical hash of
  (model, prompt, sampling params). Hit returns the stored response
  byte-identical.
- Stage 2: per-entry TTL; expired entries are never returned and are
  reclaimed lazily on access plus on a sweep.
- Stage 3: LRU eviction under a byte-size cap; eviction order is
  deterministic given identical access order.
- Cache reads/writes are recorded in traces (hit/miss/evict) so cached
  matches replay identically.
- Passes TSan under concurrent mixed get/set/delete/expire load.

### 4.3 Deadline-aware batch scheduler and worker pool

Continuous batching: the scheduler forms batches from queued requests and
dispatches them to a worker pool without waiting for a full batch when
deadlines are at risk.

Acceptance criteria:
- Each request carries a deadline; the scheduler orders dispatch so that no
  request misses its deadline while a feasible schedule exists (earliest
  deadline first within batch-size and backend constraints).
- Batch formation is bounded by max batch size and max wait time; a batch
  dispatches early when the head-of-line request's slack requires it.
- Requests that cannot meet their deadline are rejected with
  `DeadlineExceeded` before wasting backend work.
- Worker pool size is configurable; workers never busy-wait.
- Scheduling decisions (batch composition, dispatch time, rejections) are
  emitted as trace events.
- Benchmarked: throughput and p50/p99 latency vs a naive one-request-at-a-
  time baseline, measured with the mock backend, numbers reported in the PR.

### 4.4 Per-client rate limiting

Acceptance criteria:
- Token bucket per client id; limits configurable at construction.
- Over-limit requests are rejected fast with `RateLimited` and never enter
  the queue.
- Deterministic under replay: bucket refill is driven by the gateway clock
  abstraction (section 6), not wall time reads scattered through the code.

### 4.5 Prometheus metrics

Acceptance criteria:
- Exposes a `/metrics` endpoint (cpp-httplib) in Prometheus text format.
- Minimum metric set: queue depth (gauge), batch size (histogram), request
  latency p50/p99 (histogram), cache hit rate (counters: hits, misses),
  rejections by reason (counter).
- Metrics collection is wait-free or lock-lite on the hot path; a scrape
  never blocks scheduling.
- Metrics are observational only: enabling or scraping them must not change
  scheduling behavior (verified by replay: traces are identical with
  metrics on and off).

### 4.6 Trace recorder and deterministic replay

The feature the whole project stands on. See section 6 for the determinism
model.

Acceptance criteria:
- Records every scheduling decision, every cache event, every backend
  request and response (full bytes), and every terminal request status,
  each stamped with a logical tick.
- Traces are append-only, self-describing (versioned header with config
  snapshot and seed), and streamable to disk.
- A replay harness re-runs a trace against the gateway with the backend
  replaced by the recorded responses and asserts byte-for-byte identical
  outputs and identical scheduling decisions.
- On divergence, replay stops and reports: first divergent tick, component,
  expected vs actual event (with content hashes), and the last N matching
  events for context.
- Replaying a recorded match produces the identical game transcript.

## 5. Arena (v0.4)

- Werewolf with N LLM agents: seeded role assignment, day/night phases,
  discussion, voting, win-condition detection.
- The game loop consumes the gateway API only; it has no knowledge of
  backends.
- Every model interaction goes through the gateway and therefore lands in
  the trace; a match id maps to exactly one trace.
- CLI spectator: renders a live or replayed match from its trace.

## 6. Determinism requirements

### What is seeded
- Every match has a single 64-bit master seed. All randomness (role
  assignment, speaking order, tie-breaks, any sampling decisions made by
  the arena) derives from named sub-seeds of the master seed via a seeded
  PRNG (`std::mt19937_64` with documented derivation). No component may
  call an unseeded RNG or read entropy.
- Model sampling: requests carry explicit sampling params including seed
  where the backend supports it. Where a backend cannot guarantee
  deterministic sampling, determinism is provided by the trace: replay
  substitutes recorded responses, so replay never depends on backend
  determinism.

### What is recorded
- Logical tick for every event. Time inside the deterministic core is a
  logical clock owned by the scheduler, not wall time. Wall time may be
  recorded for humans but is never an input to a scheduling decision.
- Every scheduling decision: enqueue, batch formation (members and order),
  dispatch, completion, rejection (with reason), cancellation.
- Every cache event: hit, miss, insert, expire, evict.
- Every backend call: request bytes, response bytes, backend id.
- Config snapshot and code version (git hash) in the trace header.

### Definition of "identical replay"
A replay is identical iff the replayed event stream is byte-for-byte equal
to the recorded event stream after stripping wall-time annotations: same
events, same order, same logical ticks, same payload bytes. For a match,
this implies a byte-identical game transcript.

### Divergence detection and reporting
- Replay compares event-by-event against the recorded stream.
- On first mismatch it halts and emits a divergence report: tick, component
  (queue/scheduler/cache/backend/arena), expected event and actual event
  with SHA-256 content hashes, plus the trailing window of matched events.
- Exit code distinguishes clean replay, divergence, and trace corruption.
- CI runs replay of a fixture trace on every PR from v0.3 onward; any
  divergence fails the build.

### Threat model for nondeterminism (what we design against)
- Thread interleaving: the deterministic core makes decisions single-
  threaded at the scheduler; worker completion order is an input event that
  gets recorded, not a hidden source of order.
- Wall time: banned as a decision input inside the core; injected clock
  abstraction everywhere.
- Hash/iteration order: no iteration over unordered containers may
  influence a recorded decision; use ordered structures or explicit sort.
- Floating point: scores and metrics may be float; scheduling decisions may
  not depend on float comparison without an explicit documented epsilon.

## 7. Tech constraints

- **Language**: C++20. GCC and Clang must both build clean.
- **Build**: CMake (>= 3.24), warnings as errors (`-Wall -Wextra` baseline).
- **Tests**: GoogleTest, fetched via FetchContent pinned to a release tag.
  Tests land in the same PR as the feature they cover.
- **Sanitizers**: ASan and TSan builds run the full test suite in CI on
  every PR. Concurrency code is not done until TSan is silent.
- **Exceptions**: no exceptions across module boundaries. Module APIs
  return status/expected-style results (`Result<T, Error>`). Internal use
  of exceptions inside a module is allowed but discouraged; third-party
  code that throws is wrapped at the boundary.
- **Dependencies**: exactly one third-party HTTP library: **cpp-httplib**
  (header-only, MIT; client and server in one dependency; blocking I/O is
  acceptable because the gateway owns its own worker pool and the HTTP
  layer sits outside the deterministic core). GoogleTest is test-only.
  Any new dependency requires a written justification added to this
  section in the same PR, covering: what it replaces, why hand-rolling is
  worse, build impact, and license.
- **Style**: `.clang-format` and `.clang-tidy` are enforced; CI rejects
  unformatted code.

## 8. Roadmap

| Version | Scope | Gate to next |
|---------|-------|--------------|
| v0.1 | Bounded MPSC queue with backpressure + cancellation; response cache (get/set/delete -> TTL -> LRU); CI with ASan+TSan | All 4.1/4.2 criteria green in CI |
| v0.2 | Deadline-aware batch scheduler + worker pool; Prometheus metrics; deterministic mock backend | 4.3/4.5 criteria; benchmark vs naive baseline published |
| v0.3 | Trace recorder + replay harness with divergence detection; Ollama adapter | 4.6 criteria; CI replays a fixture trace |
| v0.4 | Werewolf game loop driving the gateway with N agents; CLI spectator | A full match records and replays identically |
| v0.5 | Leaderboard + match browser | **Out of scope until v0.4 ships.** Not planned in detail on purpose. |

Rate limiting (4.4) lands alongside v0.2 (it needs the clock abstraction).

## 9. Non-goals

- No authentication or multi-tenancy.
- No Kubernetes, no orchestration; single-process gateway.
- No web UI before v0.5.
- No model training or fine-tuning of any kind.
- No GPU management; backends own their own hardware.
- No general-purpose OpenAI proxy feature set (streaming SSE fan-out,
  function-calling passthrough, etc.) beyond what the arena needs.

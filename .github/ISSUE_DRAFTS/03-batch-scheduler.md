# #3 Deadline-aware batch scheduler and worker pool

Labels: `v0.2`, `gateway-core`, `concurrency`, `benchmark`

## Motivation

This is the heart of the gateway and the headline systems feature:
continuous batching with deadline awareness. Naive gateways either batch
greedily (head-of-line latency blowups) or not at all (throughput dies).
The scheduler forms batches opportunistically but dispatches early whenever
the head request's deadline slack demands it, and rejects requests it can
prove will miss their deadline instead of wasting backend work. It also
introduces the injected `Clock` and the single-threaded decision core that
the determinism story (v0.3) depends on.

## Design sketch

- Single scheduler thread owns all decisions (deliberately: one decision
  thread makes the decision sequence recordable and replayable). Workers
  execute; they never decide.
- Inputs are events: request arrival (from the #1 queue), worker
  completion, cancellation, clock ticks. Worker completions re-enter the
  scheduler as events, so completion order is data, not hidden
  interleaving.
- Policy: earliest-deadline-first admission into the forming batch,
  bounded by `max_batch_size` and `max_wait`; dispatch early when
  `now + estimated_batch_latency >= head.deadline - slack_margin`.
- Infeasible requests (deadline already unmeetable at admission time)
  rejected with `DeadlineExceeded` before dispatch.
- `WorkerPool`: fixed-size, condition-variable driven (no busy-wait),
  executes a `Batch` against a `Backend` interface. The deterministic mock
  backend (same milestone) returns seeded synthetic responses with
  configurable latency, so tests and benchmarks need no model.
- Rate limiter (SPEC 4.4) lands with this issue or immediately after: it
  shares the injected clock.
- All decisions emitted as structured events on an interface the v0.3
  trace recorder will implement; until then a null sink.

## Acceptance criteria

- [ ] No request misses its deadline while a feasible EDF schedule exists
      (verified with fake clock and mock backend with known latency).
- [ ] A batch dispatches early when head-of-line slack requires it; never
      waits `max_wait` when that would break a deadline.
- [ ] Provably-late requests are rejected without reaching a worker.
- [ ] Workers never busy-wait (no spin loops; CPU near zero when idle).
- [ ] Every scheduling decision is emitted as a structured event.
- [ ] TSan-clean under stress (arrivals + completions + cancels racing).
- [ ] Benchmark vs one-request-at-a-time baseline on the mock backend:
      throughput and p50/p99 latency, measured numbers in the PR
      description with hardware and flags. No fabricated numbers.

## Test plan

- Unit (fake clock, mock backend): EDF ordering; early dispatch trigger
  math; `max_batch_size` and `max_wait` bounds; infeasibility rejection;
  cancellation of a queued-but-not-dispatched request.
- Scenario tests: crafted arrival patterns (burst, trickle, adversarial
  mixed deadlines) with exact expected dispatch sequences asserted.
- Determinism smoke: identical event sequence in -> identical decision
  sequence out, run twice.
- TSan stress in CI: high-rate arrivals with random cancels for several
  seconds.
- Benchmark harness (not a unit test): scripted run comparing scheduler vs
  baseline; output pasted into PR.

# #1 Bounded MPSC request queue with backpressure and cancellation

Labels: `v0.1`, `gateway-core`, `concurrency`

## Motivation

Every request into the gateway flows through one structure: a bounded
multi-producer single-consumer queue between clients (arena agents, later
HTTP handlers) and the scheduler. Getting this right first gives the whole
project its concurrency backbone: explicit capacity, honest backpressure
instead of unbounded memory growth, and cancellation that actually removes
work instead of letting it run to completion and discarding the result.
It is also the first component that must survive TSan, which forces the
locking/notification discipline everything later builds on.

## Design sketch

- `RequestQueue<T>` with capacity fixed at construction.
- API (no exceptions cross this boundary; all results are explicit):
  - `try_push(T) -> PushResult{Ok, QueueFull, Closed}` - non-blocking.
  - `push(T, deadline) -> PushResult{Ok, Timeout, Closed}` - blocks until
    space, deadline, or close.
  - `pop(deadline) -> std::optional<T>` - single consumer only; empty
    optional means timeout or closed-and-drained.
  - `close()` - wakes all waiters; subsequent pushes fail with `Closed`;
    consumer drains remaining items.
- Cancellation: each queued request carries a shared `CancelToken`.
  `CancelToken::cancel()` marks the request; the consumer skips cancelled
  entries at pop time and their terminal status is reported as `Cancelled`.
  Slot accounting: a cancelled entry frees its capacity slot when skipped.
- Implementation: `std::mutex` + two `std::condition_variable`s
  (not-full / not-empty) around a ring buffer. No lock-free cleverness in
  v0.1; correctness first, then measure. Note the tradeoff in the PR.
- Every terminal outcome is one of `Ok | Cancelled | Rejected | Timeout`,
  reported exactly once (this invariant becomes a trace requirement in
  v0.3).

## Acceptance criteria

- [ ] `try_push` on a full queue returns `QueueFull` without blocking or
      allocating.
- [ ] `push` with a deadline returns `Timeout` when capacity never frees.
- [ ] Cancelled requests are never delivered to the consumer and release
      their slot.
- [ ] `close()` wakes all blocked producers and the consumer; every
      submitted request gets exactly one terminal status.
- [ ] No request is silently dropped under any interleaving of
      push/cancel/close.
- [ ] TSan-clean stress test passes in CI (see test plan).

## Test plan

- Unit: single-threaded semantics of every API result (full, empty,
  timeout, closed, cancel-before-pop).
- Property-ish: N producers x M requests, random cancellation of ~30%,
  random close point; assert exactly-once terminal status per request and
  conservation (delivered + cancelled + rejected + timed out == submitted).
- TSan stress: 8 producers, tight loop, aggressive cancel from a separate
  thread, run under `-DREPLAYARENA_SANITIZE=thread` in CI.
- Shutdown test: close while producers are blocked mid-push; assert no
  deadlock (test-level watchdog timeout) and correct statuses.

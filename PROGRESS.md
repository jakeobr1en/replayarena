# PROGRESS.md - living dev log

Rules for this file:
- Update at the end of every session, no exceptions.
- Anything that cost more than 30 minutes gets written down under "Dead
  ends" in that session's entry. That pain is the writing material for
  blog posts and interview stories.
- Decisions get a "why", not just a "what".

---

## Status

- **Current phase**: v0.2 in progress (v0.1 complete, tagged v0.1.0).
- **What works today**: bounded MPSC `RequestQueue<T>` with backpressure,
  cancellation, and close/drain semantics; injected `Clock` abstraction
  (`SteadyClock` production, `SimulatedClock` manual advance); canonical
  `CacheKey`; complete `ResponseCache` (issue #2 done): get/set/erase,
  per-entry TTL, and LRU eviction under a byte cap with deterministic
  eviction order; 54 tests, all TTL/eviction behavior stepped on the
  simulated clock with zero sleeps.
- **Next issue up**: [#3](https://github.com/jakeobr1en/replayarena/issues/3)
  part 3: the threaded driver (ingress wiring, scheduler thread, worker
  pool) and then the benchmarks. Parts 1 (interfaces + mock backend) and
  2 (pure scheduler core) are done.

---

## Session log

### 2026-08-04 - Project conceived, scope defined

**Shipped**
- SPEC.md: full vision, architecture, acceptance criteria per engine
  feature, determinism model (seeding, recording, identical-replay
  definition, divergence reporting), roadmap v0.1-v0.5, non-goals, tech
  constraints.
- CLAUDE.md: standing workflow rules for all future sessions.
- PROGRESS.md: this file.
- Scaffold: CMake C++20 project, hello-world `replayarena` binary, one
  passing GoogleTest via FetchContent (pinned), `.clang-format`,
  `.clang-tidy`, `.gitignore`, MIT LICENSE, README with architecture
  diagram and roadmap.
- CI: GitHub Actions workflow building and running tests under ASan and
  TSan on every PR and push to main, plus a clang-format gate.
- Five issue drafts in `.github/ISSUE_DRAFTS/` (queue, cache, scheduler,
  trace/replay, werewolf loop) to be opened as real GitHub issues.

**Decisions**
- HTTP dependency: cpp-httplib, chosen over Boost.Beast and libcurl.
  Why: single header-only MIT dep covering both client (backend adapters)
  and server (/metrics); blocking I/O is acceptable because the gateway
  owns its own worker pool and HTTP sits outside the deterministic core.
  Beast would drag in Asio and shift the project's center of gravity to
  I/O plumbing; libcurl would still need a second dep for the server side.
- Determinism model: logical clock owned by the scheduler; wall time
  banned as a decision input; worker completion order treated as a
  recorded input event rather than a hidden interleaving. This is the
  design bet the whole replay story rests on.
- No exceptions across module boundaries; `Result<T, Error>`-style returns
  at module APIs. Matches safety-critical habits and keeps failure paths
  visible in signatures.
- GoogleTest via FetchContent pinned to v1.15.2 rather than a system
  package, so CI and local builds agree byte-for-byte.
- Repo slug jakeobr1en/replayarena baked into badges and links.
- Rate limiting scheduled with v0.2 (not v0.1) because it needs the clock
  abstraction the scheduler introduces.

**Dead ends**
- `gtest_discover_tests` default mode runs the test binary at build time
  with a 5s timeout; sanitizer-instrumented binaries can blow past that on
  first launch. Fixed with `DISCOVERY_MODE PRE_TEST` + `DISCOVERY_TIMEOUT
  60` in tests/CMakeLists.txt.
- Both sanitizers are unusable locally on this host (macOS 26.5, Apple
  clang 17.0.0, arm64). ASan hangs forever at startup: sampling shows its
  own init deadlocking - `AsanInitInternal -> InitializeShadowMemory ->
  MemoryRangeIsAvailable -> get_dyld_hdr ->
  dyld_shared_cache_iterate_text_swift -> _Block_copy -> malloc ->
  __sanitizer_mz_malloc`, i.e. ASan's malloc interposer re-enters ASan
  init before init finishes. TSan segfaults immediately (exit 139, no
  output). Conclusion: sanitizer verification is CI-only (Ubuntu,
  GCC 13 + Clang 16) until Apple ships a fixed runtime. Cost: ~45 minutes
  of diagnosis; the recursive-init stack trace is good writing material.

### 2026-08-04 - Repo published, CI shaken out, issues opened

**Shipped**
- Repo live at github.com/jakeobr1en/replayarena; initial history pushed.
- First CI run green across the whole matrix (clang-format + ASan/TSan x
  GCC 13/Clang 16) on the first attempt; GCC's -Wconversion had nothing
  to say about the scaffold. The real shakeout comes with issue #1 code.
- Bumped actions/checkout v4 -> v5 to clear Node 20 deprecation warnings
  that annotated every job.
- Issues #1-#5 opened from the drafts; numbers match the draft numbering
  exactly, so all cross-references (#1 in #3, #3 in #4, #4 in #5) hold.
- Branch protection on main: PRs required, all five CI checks required,
  zero required approvals (solo repo: self-merge allowed, direct push
  blocked; requiring approvals would deadlock a single maintainer).

**Decisions**
- Issue #3 gained an acceptance criterion: benchmark the single decision
  thread's throughput ceiling (decisions/sec, zero-latency backend) and
  publish it next to the expected request rate. The single-decision-thread
  scheduler is the determinism bet; the measured headroom is the prepared
  answer to "doesn't one scheduler thread bottleneck you?"

### 2026-08-04 - Issue #1: bounded MPSC queue shipped

**Shipped**
- `src/gateway/cancel_token.h` + `src/gateway/request_queue.h`: bounded
  MPSC ring-buffer queue, pre-allocated storage (no hot-path allocation),
  try_push/push-with-deadline/pop-with-deadline/close, per-request
  CancelToken, Stats counters (pushed/popped/skipped_cancelled) that later
  feed the queue-depth metric.
- 13 unit tests (single-threaded semantics of every result path) plus 3
  concurrency tests: 1600-request producer storm with ~30% concurrent
  cancellation asserting exactly-once delivery and conservation
  (popped + skipped_cancelled == pushed), close-wakes-blocked-producers,
  and a 20-round randomized-close-point conservation test. Stress suite
  repeated 25x locally without a failure; all PRNG seeds fixed.

**Decisions**
- Mutex + two condition variables around a ring buffer, not lock-free.
  Why: correctness and TSan-provability first; the queue feeds a scheduler
  that makes decisions single-threaded anyway, so queue throughput is not
  the system bottleneck. Revisit only with a measured need.
- Cancellation race semantics: delivery wins. A request popped before its
  token is cancelled counts as delivered; in-flight cancellation is the
  consumer's job (scheduler, issue #3). This keeps the queue's
  exactly-once terminal-status contract crisp and testable.
- Cancelled entries free their slot when the consumer skips them, not at
  cancel() time. cancel() stays wait-free and never touches the queue
  lock; the cost is that a cancelled entry can occupy a slot briefly.
- Deadlock watchdog for concurrency tests is ctest's per-test TIMEOUT
  property rather than in-test watchdog threads; a hang fails CI instead
  of hanging it.

**Dead ends**
- None over the 30-minute bar this session.

### 2026-08-04 - Issue #2 stage 1: response cache get/set/erase

**Shipped**
- `src/gateway/cache_key.h`: canonical request identity. Length-prefixed
  fields (embedded NULs safe, no boundary confusion), sampling params
  sorted by name then value so call-site order cannot matter.
- `src/gateway/response_cache.h`: thread-safe get/set/erase behind a
  shared_mutex; hits/misses as atomics (mutated under the shared lock);
  size_bytes accounting (key + payload, exact across overwrite and erase);
  Stats already carries evictions/size_bytes so stages 2 and 3 do not
  break the API.
- 15 new tests including a torn-read check under 8-thread mixed load and
  an identical-ops-produce-identical-caches determinism sanity test.

**Decisions**
- Deviation from the issue draft, on purpose: the cache key is the
  canonical byte string itself, not a SHA-256 of it. Nothing needs a
  content hash until the trace recorder (issue #4) does divergence
  reports, and hand-rolling crypto in a cache PR is the wrong place for
  it. When SHA-256 arrives for traces, keying can switch to the digest if
  the memory ever matters. Noted in the PR description.
- std::hash is used only for bucket placement inside the map and is never
  recorded, so unordered-container iteration order cannot leak into
  replayable state.
- CLAUDE.md gained a rule (separate docs PR): sessions end with the
  branch pushed and the PR URL printed.

**Dead ends**
- None over the 30-minute bar this session.

### 2026-08-04 - Issue #2 stage 2: TTL and the Clock abstraction

**Shipped**
- `src/gateway/clock.h`: `Clock` interface (monotonic `now()`),
  `SteadyClock` (production; the single place wall time enters the
  system), `SimulatedClock` (manual `advance()`, atomic, thread-safe, no
  background progression). Shared infrastructure for the scheduler (#3),
  rate limiter, and replay harness (#4).
- Response cache stage 2: per-entry TTL evaluated only against the
  injected clock. Expiry boundary defined as now() >= set_time + ttl
  (visible strictly before, gone at the boundary). Expired entries are
  never returned; reclaimed lazily on access and in bulk via `sweep()`.
  Overwrites replace the expiry (set without ttl makes an entry
  immortal). New `expirations` stat, kept separate from stage 3's
  `evictions`.
- 15 new tests (45 total): clock unit tests (including a
  concurrent-advance loss check), TTL boundary tests stepped to exact
  nanosecond instants, lazy-reclaim accounting, sweep idempotence, and a
  same-ops-same-clock-steps determinism check. The stress test now runs
  a dedicated clock-advancer thread so expiry races get/set/erase/sweep
  under TSan. Zero sleeps in any TTL test.
- Mechanical enforcement of the wall-clock ban: grep for
  steady_clock/system_clock/high_resolution_clock over src/ returns hits
  only in clock.h. Worth automating as a CI step when a third component
  starts using the clock.

**Decisions**
- Clock vocabulary reuses std::chrono::steady_clock's time_point/duration
  types rather than inventing a parallel unit system: free interop with
  cv waits and chrono literals, zero conversion code. The determinism
  boundary is who calls now(), not the type of the timestamp.
- SimulatedClock is an atomic counter, not mutex-guarded: advance() and
  now() are wait-free, so tests can advance time from a separate thread
  while workers hammer the cache, which is exactly what the stress test
  does.
- get() on an expired entry re-checks under the exclusive lock before
  reclaiming: between the shared-lock observation and the upgrade, a
  set() may have replaced the entry with a fresh one, which must not be
  reclaimed. This upgrade race is the kind of bug TSan does not catch
  (it is a logic race, not a data race), so it is pinned by the
  OverwriteReplacesTtl test instead.

**Dead ends**
- None over the 30-minute bar this session.

### 2026-08-07 - Issue #2 stage 3: LRU byte cap; issue #2 complete

**Shipped**
- LRU eviction under a configurable byte cap (`ResponseCache(clock,
  max_size_bytes)`; nullopt = unbounded). set() evicts least-recently-used
  entries until the invariant size_bytes <= cap holds, so size never
  exceeds the cap after set returns. "Use" = hit or set. Recency lives in
  an intrusive std::list of pointers into the map's keys (node-based map,
  stable addresses); eviction order is a pure function of the operation
  sequence, never of unordered_map iteration order.
- Oversized-entry semantics: an entry larger than the whole cap is
  admitted and then immediately evicted by the same invariant loop; the
  cache ends empty and the eviction is counted. No special-case rejection
  path to test separately.
- Expired-but-unreclaimed entries keep their list position and are
  evicted like any other entry (counted as evictions, not expirations);
  the two stats stay strictly separate.
- 9 new tests (54 total): exact eviction-order assertions, recency
  refresh via get and via overwrite, cap invariant held across every set,
  oversized entry, expiry/eviction stat separation, and a two-instance
  determinism test asserting key-by-key survival agreement (counts alone
  could mask same-count-different-keys divergence). Stress test now runs
  with the cap at half the working-set size, so eviction races TTL
  expiry, erase, and sweep under TSan; a sampled cap-invariant check
  runs inside the worker loop.

**Decisions**
- Dropped shared_mutex for a plain mutex (own refactor commit before the
  feature). LRU touch makes every hit a writer, so shared locking would
  only help miss-heavy loads nobody has measured a need for; the plain
  mutex also deleted stage 2's shared-to-exclusive reclaim dance and the
  atomic hit/miss counters - less machinery, one less subtle race to
  reason about. Revisit only with v0.2 metrics showing contention.
- Digest keying deliberately not revisited (per issue #2 note): key bytes
  are visible in size_bytes accounting and nothing suggests they matter
  yet.

**Dead ends**
- None over the 30-minute bar this session.

### 2026-08-07 - v0.1.0 tagged; issue #3 part 1: interfaces and mock backend

**Shipped**
- Tagged and released v0.1.0 (queue, cache, CI/sanitizers).
- `src/gateway/request.h`: `Request` (id, client, model, prompt, deadline,
  cancel token), `BackendResponse`, `Batch`. Submitter-assigned unique
  request ids are the identity that traces, events, and responses share.
- `src/gateway/backend.h`: `Backend` interface. Contract: synchronous
  execute() from worker threads, thread-safe, exactly one response per
  request in order, failures reported in-payload (no exceptions across
  the boundary), and output bytes must never be shaped by clock reads.
- `src/gateway/scheduler_events.h`: the decision stream. Seven event
  types as a std::variant, each with defaulted equality (equality is
  load-bearing: replay divergence detection in #4 is event comparison);
  `SchedulerEventSink` interface + `NullEventSink` (also the baseline for
  measuring recording overhead later).
- `src/gateway/mock_backend.h`: deterministic mock. Output is FNV-1a of
  (seed, model, prompt) with explicit little-endian seed mixing and field
  separators, so bytes are identical across platforms, instances, and
  batch positions. Optional simulated latency is a real sleep on the
  worker thread: it models work, never shapes bytes or decisions.
- 13 new tests (67 total): mock determinism across calls/instances/batch
  positions, field-boundary and endianness-stability checks, and event
  equality semantics (tick, payload kind, batch member order, and
  rejection reason all participate; a reordered batch is a divergence,
  not an equivalent batch).

**Decisions**
- Issue #3 split into three PRs: interfaces + mock backend (this one),
  scheduler core + worker pool, benchmarks. Keeps each diff reviewable
  and lets the scheduler PR land against already-stable interfaces.
- BatchCompleted is an explicit event: worker completions re-enter the
  scheduler as inputs, so the order the decision thread processes them
  is recorded data, not hidden interleaving. This is the design bet from
  SPEC.md section 6 becoming API.
- MockBackend latency is real sleep, not simulated-clock time: workers
  hold no clock, and the point is to occupy a worker thread so batching
  is observable. Zero in tests, nonzero only in benchmarks.

**Dead ends**
- None over the 30-minute bar this session.

### 2026-08-07 - Issue #3 part 2: the pure scheduler core

**Shipped**
- `src/gateway/scheduler_core.h`: the gateway's decision core. No
  threads, no locks, no waits, no clock - time is an argument. Inputs
  (on_arrival / on_completion / on_time) return a SchedulerStep (batches
  to dispatch, terminal outcomes to deliver); every decision is emitted
  to the event sink with a strictly increasing logical tick.
  next_wakeup() tells the future driver when a time-based trigger could
  fire, so the driver never invents timing decisions of its own.
- Policy: EDF with arrival-order tie-break; dispatch on any of full
  batch, head waited max_wait, or last-safe-moment slack trigger
  (now + estimated_batch_latency + slack_margin >= head deadline);
  infeasibility rejection at arrival and again at formation time;
  cancellation observed at arrival and formation (in-flight cancellation
  is v0.3 scope).
- 20 tests pinning exact behavior: trigger boundaries tested at the
  exact instant either side, EDF and tie-break order asserted on
  BatchFormed member lists, formation-time rejection when a worker
  frees too late, cancelled entries not consuming batch capacity,
  next_wakeup semantics (min of triggers, nullopt when input-only,
  clamped to now), strictly increasing ticks, exactly-once terminal
  outcomes, and a two-core identical-input determinism check that
  drains all dispatches through completions.

**Decisions**
- The scheduler is split into a pure core and a threaded driver
  (next PR) rather than one threaded class. The core is a deterministic
  function of (state, input, now): unit-testable to the exact event
  stream with no threads and no sleeps, and replayable by driving it
  from a trace (issue #4 needs exactly this seam). The driver will own
  blocking mechanics and make zero decisions.
- Feasibility boundary: a request needing exactly its remaining slack
  (now + estimate == deadline) is feasible; infeasible strictly after.
  Tested at the boundary.
- RequestOutcome.reject_reason is std::optional rather than a defaulted
  enum: a meaningless-but-set default value would silently participate
  in equality comparisons, and outcome equality is what the determinism
  tests (and later replay) rely on. Caught while writing the tests.

**Dead ends**
- None over the 30-minute bar; two test-config mistakes (relying on
  batch accumulation while max_wait defaulted to zero, i.e.
  dispatch-immediately) were caught by the first local run.

---

## Insights (one-liners worth expanding into posts)

- A leaderboard is a claim; a replayable trace is a proof.
- Determinism is not a feature you add, it is a list of things you ban:
  wall clocks, unordered iteration, unseeded RNG, hidden interleavings.
- Treating worker completion order as recorded input turns a race
  condition into data.
- The tool that checks your memory bugs can have its own: ASan on macOS 26
  deadlocks re-entering its own initializer through Apple's malloc zone
  hooks.
- A lock upgrade is a time machine: everything you observed under the
  shared lock is ancient history by the time you hold the exclusive one.
- TSan proves you have no data races, not that your races are benign;
  logic races need a test that makes the interleaving deterministic.
- Sometimes the best concurrency refactor is deleting concurrency: the
  LRU list turned every reader into a writer, and admitting that deleted
  a shared_mutex, two atomics, and a lock-upgrade race.
- A scheduler you can unit-test to the exact event stream is a scheduler
  you can replay: purity is not a style choice, it is the feature.

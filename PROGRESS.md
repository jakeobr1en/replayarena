# PROGRESS.md - living dev log

Rules for this file:
- Update at the end of every session, no exceptions.
- Anything that cost more than 30 minutes gets written down under "Dead
  ends" in that session's entry. That pain is the writing material for
  blog posts and interview stories.
- Decisions get a "why", not just a "what".

---

## Status

- **Current phase**: v0.1 in progress.
- **What works today**: bounded MPSC `RequestQueue<T>` with backpressure
  (try_push fail-fast, blocking push with deadline), cancellation via
  shared `CancelToken` (cancelled entries skipped at pop, slots reclaimed),
  and clean close/drain semantics; canonical `CacheKey` (length-prefixed,
  param-order-independent) and thread-safe `ResponseCache` get/set/erase
  with hit/miss/size stats; 30 tests including TSan-targeted stress tests
  for both components.
- **Next issue up**: [#2](https://github.com/jakeobr1en/replayarena/issues/2)
  stage 2 (per-entry TTL via an injected Clock; the Clock abstraction gets
  introduced there). Stage 1 (get/set/erase) is done.

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

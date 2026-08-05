# PROGRESS.md - living dev log

Rules for this file:
- Update at the end of every session, no exceptions.
- Anything that cost more than 30 minutes gets written down under "Dead
  ends" in that session's entry. That pain is the writing material for
  blog posts and interview stories.
- Decisions get a "why", not just a "what".

---

## Status

- **Current phase**: pre-v0.1. Docs, scaffold, and CI exist; no engine code.
- **What works today**: hello-world gateway binary builds; one GoogleTest
  smoke test passes; CI runs build + tests under ASan and TSan on every PR.
- **Next issue up**: #1 Bounded MPSC request queue with backpressure and
  cancellation (draft in `.github/ISSUE_DRAFTS/01-bounded-mpsc-queue.md`).

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

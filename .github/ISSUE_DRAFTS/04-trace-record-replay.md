# #4 Trace recorder and replay harness with divergence detection

Labels: `v0.3`, `gateway-core`, `determinism`

## Motivation

This is the project's reason to exist: "leaderboards you can replay and
verify, not trust." The recorder captures every scheduling decision, cache
event, and backend request/response; the replay harness re-executes a
trace and proves, byte for byte, that the gateway is deterministic - or
pinpoints exactly where it is not. Once this lands, nondeterminism is a CI
failure instead of a debate, and every werewolf match (v0.4) becomes an
auditable artifact.

## Design sketch

- `TraceRecorder` implements the event-sink interface introduced in #3.
  Events: enqueue, batch formation (members + order), dispatch, worker
  completion, rejection (reason), cancellation, cache hit/miss/insert/
  expire/evict, backend request bytes, backend response bytes. Every event
  carries the logical tick assigned by the scheduler.
- Format: length-prefixed binary records, append-only, streamed to disk.
  Versioned header: format version, config snapshot, master seed, git
  hash. Wall-time annotations allowed but segregated so they can be
  stripped for comparison.
- `ReplayBackend`: a `Backend` implementation that serves the recorded
  response bytes for each request (matched by request hash + sequence).
- Replay harness: loads a trace, reconstructs config, runs the gateway
  with `ReplayBackend` and a replay clock driven by recorded ticks, and
  compares the emitted event stream against the recording event-by-event.
- Divergence report on first mismatch: tick, component, expected vs actual
  event with SHA-256 hashes, plus the last N matched events for context.
  Exit codes: 0 clean, 1 divergence, 2 trace corruption.
- Ollama adapter (same milestone): `Backend` over cpp-httplib against a
  local Ollama; its responses are recorded like any other, so replay does
  not depend on Ollama determinism.

## Acceptance criteria

- [ ] Record -> replay of a mock-backend traffic run is byte-for-byte
      identical (definition in SPEC.md section 6) across repeated replays.
- [ ] Injected fault tests produce correct divergence reports (right tick,
      component, hashes) for at least: reordered batch, altered response
      byte, missing event, extra event.
- [ ] Truncated/corrupt trace detected and reported as corruption, not
      divergence.
- [ ] Recording overhead measured and reported in the PR (throughput with
      recorder on vs null sink).
- [ ] CI replays a committed fixture trace on every PR; divergence fails
      the build.
- [ ] Ollama adapter records real traffic that then replays without Ollama
      running.

## Test plan

- Unit: record format round-trip (write -> read -> identical events);
  header versioning; corruption detection (truncation, bad length, bad
  hash).
- Golden: fixture trace committed to the repo; replay in CI must be clean.
- Fault injection: deliberately perturb a replay (shuffled completion
  event, mutated byte) and assert the divergence report contents exactly.
- Determinism soak: record one 60s mock run, replay it 10x, assert clean
  every time.
- Integration (local only, not CI): record against real Ollama, replay
  offline.

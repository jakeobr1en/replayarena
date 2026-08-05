# #2 Response cache, staged: get/set/delete -> TTL -> LRU cap

Labels: `v0.1`, `gateway-core`

## Motivation

Werewolf matches re-issue near-identical prompts (same system prompt, same
public game state) across agents and rounds. A response cache cuts backend
load and latency, and, because cache hits are recorded in traces (v0.3),
cached matches stay byte-for-byte replayable. Building it in three
shippable stages keeps each PR small and reviewable and demonstrates
incremental delivery discipline.

## Design sketch

- Key: canonical SHA-256 over (model id, prompt bytes, sampling params
  serialized in a fixed field order). Canonicalization is part of the API
  so two call sites can never disagree about what "the same request" is.
- `ResponseCache` API: `get(key) -> std::optional<Entry>`,
  `set(key, Entry, ttl)`, `erase(key)`, `stats() -> {hits, misses,
  evictions, size_bytes}`.
- Stage 1 (PR 1): `std::unordered_map` behind a `std::shared_mutex`;
  correct concurrent get/set/delete; stats counters.
- Stage 2 (PR 2): per-entry expiry timestamp from an injected `Clock`
  (never wall time directly; this is the same clock abstraction the
  scheduler will use). Expired entries are invisible to `get` and
  reclaimed lazily on access plus an explicit `sweep()`.
- Stage 3 (PR 3): intrusive LRU list under a byte-size cap; eviction is
  deterministic given identical access order (a determinism requirement,
  see SPEC.md section 6).
- Hit returns the stored response byte-identical (no re-serialization).

## Acceptance criteria

- [ ] Stage 1: concurrent get/set/delete correct under TSan; hit returns
      byte-identical payload.
- [ ] Stage 2: an expired entry is never returned, even if not yet swept;
      TTL driven by the injected clock, testable without sleeping.
- [ ] Stage 3: size never exceeds the byte cap after `set` returns;
      eviction order is deterministic given identical access order.
- [ ] Stats counters accurate under concurrency (hits + misses == gets).
- [ ] Each stage lands as its own PR, each independently shippable.

## Test plan

- Unit per stage: hit/miss/overwrite/erase; TTL boundary cases (expires
  exactly at deadline tick); LRU order after mixed get/set patterns;
  byte-cap enforcement with variable-size entries.
- Determinism test: two cache instances fed the identical operation
  sequence end with identical contents and identical eviction logs.
- TSan stress: mixed get/set/delete/sweep from 8 threads.
- Fake clock throughout; zero `sleep()` calls in tests.

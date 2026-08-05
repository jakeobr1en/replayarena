# replayarena

[![CI](https://github.com/jakeobr1en/replayarena/actions/workflows/ci.yml/badge.svg)](https://github.com/jakeobr1en/replayarena/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![C++20](https://img.shields.io/badge/C%2B%2B-20-blue.svg)](https://en.cppreference.com/w/cpp/20)

**Leaderboards you can replay and verify, not trust.**

replayarena is a verifiable LLM game arena built on a custom C++20 inference
gateway. LLM agents compete in social deduction games (werewolf first).
Every match is seeded and deterministically replayable byte-for-byte, so
leaderboard results are auditable instead of trusted.

## Architecture

```
+---------------------------------------------------------------+
|                         ARENA LAYER                           |
|  werewolf game loop | match runner (seeded) | CLI spectator   |
+-------------------------------+-------------------------------+
                                | submit(request, deadline, client_id)
                                v
+---------------------------------------------------------------+
|                        GATEWAY CORE (C++20)                   |
|  rate limiter -> bounded MPSC queue -> deadline-aware batch   |
|  scheduler -> worker pool                                     |
|  response cache (get/set/delete -> TTL -> LRU)                |
|  trace recorder (every scheduling decision + model output)    |
|  Prometheus metrics (queue depth, batch sizes, p50/p99,       |
|  cache hit rate)                                              |
+-------------------------------+-------------------------------+
                                v
+---------------------------------------------------------------+
|                      BACKEND ADAPTERS                         |
|  mock (deterministic) | Ollama (local) | OpenAI-compatible    |
+---------------------------------------------------------------+
```

Full architecture, acceptance criteria, and the determinism model live in
[SPEC.md](SPEC.md).

## Why deterministic replay

Most LLM leaderboards are trust exercises: a number, a methodology section,
and no way to check either. replayarena takes the position that a result
you cannot re-derive is an anecdote.

Every match runs from a single 64-bit seed. The gateway records every
scheduling decision, every cache event, and every model request and
response, stamped with a logical tick. Replaying a trace substitutes the
recorded model outputs and re-executes everything else, then asserts the
event stream is byte-for-byte identical to the original. If it is not, the
replay harness reports the first divergent tick, the component responsible,
and the expected vs actual event.

This gets you three things:

1. **Auditable results.** Anyone can download a match trace and verify the
   transcript that produced a leaderboard entry.
2. **Debuggable concurrency.** Scheduling is reconstructed from the trace,
   so "it only happens under load" bugs become replayable test cases.
3. **Honest engineering.** Divergence detection in CI means nondeterminism
   is a build failure, not a philosophy debate.

## Roadmap

| Version | Scope | Status |
|---------|-------|--------|
| [v0.1](SPEC.md#8-roadmap) | Bounded MPSC queue + response cache + CI/sanitizers | in progress |
| [v0.2](SPEC.md#8-roadmap) | Deadline-aware batch scheduler + worker pool + metrics + mock backend | planned |
| [v0.3](SPEC.md#8-roadmap) | Trace record/replay + Ollama adapter | planned |
| [v0.4](SPEC.md#8-roadmap) | Werewolf game loop + CLI spectator | planned |
| [v0.5](SPEC.md#8-roadmap) | Leaderboard + match browser | out of scope until v0.4 ships |

## Building

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Sanitizer builds (what CI runs on every PR):

```sh
cmake -S . -B build-tsan -DREPLAYARENA_SANITIZE=thread -DCMAKE_BUILD_TYPE=Debug
cmake --build build-tsan -j
ctest --test-dir build-tsan --output-on-failure
```

Requires CMake >= 3.24 and a C++20 compiler (GCC 12+ or Clang 15+).
GoogleTest is fetched automatically.

## License

[MIT](LICENSE)

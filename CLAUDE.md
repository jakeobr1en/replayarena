# CLAUDE.md - standing rules for every session in this repo

These rules are non-negotiable. They exist so the git history reads like a
disciplined engineer worked here, because one did.

## Before writing any code

1. Read [SPEC.md](SPEC.md) in full. It is the source of truth for scope,
   architecture, and acceptance criteria.
2. Read [PROGRESS.md](PROGRESS.md) in full. It tells you what actually
   works today, what the next issue is, and which dead ends have already
   been explored. Do not re-explore documented dead ends.

## Workflow (non-negotiable)

- Every feature starts from a GitHub issue. No issue, no branch.
- One feature branch per issue: `feat/<n>-short-name`, `fix/<n>-...`, etc.
- Never commit directly to `main`. All changes land via PR.
- Commits are small and conventional: `feat:`, `fix:`, `perf:`, `test:`,
  `refactor:`, `docs:`, `chore:`, `ci:`. Every commit must build and pass
  tests on its own.
- PR descriptions state: what changed, how it was tested, and benchmark
  deltas where relevant (before/after numbers, measurement setup).
- Prefer small, reviewable diffs. If a task wants more than ~400 changed
  lines, split it into multiple PRs and say so explicitly.

## Testing

- Tests land in the same PR as the feature. A feature PR without tests is
  incomplete.
- Concurrency code must pass TSan with zero reports before the PR is
  considered done. ASan must be silent too.
- When fixing a bug, first write a failing test that reproduces it.

## Honesty rules

- Never fabricate benchmark numbers. Only report measured output, and
  include how it was measured (hardware, build flags, iterations).
- If a test is flaky, that is a bug; fix or quarantine it with an issue,
  never ignore it.
- Report failures verbatim. Do not soften failing output in PROGRESS.md or
  PR descriptions.

## Session hygiene

- At the end of EVERY session, update PROGRESS.md before finishing:
  status, dated session log entry (what shipped, decisions and why, dead
  ends that cost > 30 minutes), and any new insight one-liners.
- Style: never use the em dash character; use a plain dash instead.
- No co-author attribution lines in commit messages.

## Build / test / lint commands

```sh
# Configure + build (plain)
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j

# Run tests
ctest --test-dir build --output-on-failure

# Sanitizer builds (what CI runs; Linux only - on this macOS host both
# sanitizer runtimes are broken, see PROGRESS.md 2026-08-04 dead ends.
# Verify sanitizer results via CI, not locally.)
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DREPLAYARENA_SANITIZE=address
cmake --build build-asan -j && ctest --test-dir build-asan --output-on-failure

cmake -S . -B build-tsan -DCMAKE_BUILD_TYPE=Debug -DREPLAYARENA_SANITIZE=thread
cmake --build build-tsan -j && ctest --test-dir build-tsan --output-on-failure

# Format (check / fix)
clang-format --dry-run -Werror src/*.cpp tests/*.cpp
clang-format -i src/*.cpp tests/*.cpp

# Static analysis (requires compile_commands.json from the configure step)
clang-tidy -p build src/*.cpp
```

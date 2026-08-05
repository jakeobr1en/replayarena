# #5 Werewolf game loop driving the gateway with N agents

Labels: `v0.4`, `arena`

## Motivation

The payoff layer: real LLM agents playing werewolf through the gateway,
producing matches that are seeded, recorded, and replayable end to end.
This exercises the whole stack under realistic load (bursts of parallel
agent calls at each phase) and produces the first artifacts worth showing:
auditable match traces and a spectator view. It is also the acceptance
test for everything below it - if a match does not replay identically, a
lower layer is lying.

## Design sketch

- Roles v1: werewolves, villagers, seer. Phases: night (werewolf kill
  choice, seer inspection), day (open discussion rounds, then vote,
  majority lynch), repeat until a win condition.
- `Match` runs from a single 64-bit master seed; role assignment, speaking
  order, and tie-breaks derive from named sub-seeds (SPEC.md section 6).
  The game state machine is pure and deterministic: (state, seed, agent
  responses) -> next state, with no I/O.
- Agents are prompt templates + response parsers over the gateway API
  (`submit(request, deadline, client_id)`); each agent is a distinct
  client id, so rate limiting and per-client metrics apply. Malformed
  model output is handled by a bounded retry-then-default-action policy
  (recorded, deterministic), never a crash.
- All model calls go through the gateway; one match id maps to exactly one
  gateway trace plus a game-event log derived from it.
- CLI spectator: renders a match from its trace (live tail or replay);
  phase banners, discussion text, votes, reveals. Werewolf chat hidden by
  default, `--omniscient` shows all.
- Match verification command: `replayarena verify <trace>` replays the
  match (#4 harness) and prints the transcript hash.

## Acceptance criteria

- [ ] A full N-agent match (N >= 5) runs to a win condition against the
      mock backend with zero manual intervention.
- [ ] Same seed + same recorded responses -> byte-identical transcript;
      `verify` exits 0.
- [ ] Different seeds produce different role assignments and games
      (sanity, not statistics).
- [ ] Malformed agent output triggers the documented fallback policy and
      is visible in the trace; the match still completes.
- [ ] Spectator renders both a live match and a replayed trace
      identically.
- [ ] A real-model match (Ollama) records and replays cleanly offline.

## Test plan

- Unit: game state machine transitions (kills, votes, ties, win
  conditions) as pure functions; seed derivation (fixed master seed ->
  known role assignment); response parser on malformed inputs (empty,
  garbage, wrong player name, injection-ish text).
- Scripted-agent integration: deterministic fake agents with scripted
  answers drive full matches; assert exact transcripts.
- End-to-end determinism: run match with mock backend, record, replay,
  diff transcript bytes - in CI.
- Load sanity: one phase fans out N parallel agent calls; assert all
  deadlines respected with the mock backend.
- Manual: one real Ollama match, trace committed as a demo artifact
  (subject to size), replayed in README instructions.

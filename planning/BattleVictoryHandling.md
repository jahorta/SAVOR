# Persist Victory selection and support continued exploration

## Summary

Remove the global Victory short-circuit from `battle.single_turn`.

Evaluate Victory and next-turn candidates independently, persist deterministic selections for both, and add a durable `continue_after_victory` launch policy controlling whether valid `ReachedNextTurn` branches continue after a Victory is found.

Default the new policy to `false`.

## Implementation changes

### Durable run policy

- Add the Boolean workflow argument `continue_after_victory` to the Battle unit.
- Default it to `false`.
- Read it when `battle.start` creates the BattleSet and persist it in `ab_battle_set`.
- Add it to `CreateBattleSetCommand`, `BattleSetSnapshot`, archive/rehydration, payload verification, UI-read projections, and E2E graph arguments.
- Treat it as immutable BattleSet execution policy so retries use the original value.

### Candidate classification and ranking

- Remove the cohort-wide branch that immediately completes every wave when any Victory exists.
- Classify successful candidates into:
  - `Victory`: valid terminal candidate with a successor savestate.
  - `ReachedNextTurn`: valid continuation candidate with ending RNG and successor savestate.
  - Nonviable outcomes: excluded or rejected using the existing rules.
- Use the existing deterministic ranking tuple for both candidate types:
  - cumulative fake attacks;
  - VI delta;
  - predicate score;
  - stable execution-job identity.

### Victory selection

- Create a durable Victory advancement pool for each turn containing all valid Victory candidates across its waves.
- Select one canonical Victory candidate using the shared ranking.
- Record every candidate as `SELECTED` or `NOT_SELECTED`, with explicit decision reasons.
- Preserve the selected turn-job and its source wave through the advancement decisions.
- Exact replay must resolve to the same selection; a conflicting selection is a canary failure.

### Next-turn selection

- Continue creating per-wave, per-ending-RNG pools for `ReachedNextTurn` candidates.
- When no Victory exists, retain current continuation behavior.
- When Victory exists and `continue_after_victory=false`:
  - persist the Victory selection;
  - record continuation candidates as not selected because exploration stopped at Victory;
  - create no child waves;
  - complete current waves and set the BattleSet to `VICTORY`.
- When Victory exists and `continue_after_victory=true`:
  - persist the Victory selection;
  - independently select eligible `ReachedNextTurn` candidates;
  - create child waves when another Battle Plan turn exists;
  - keep the BattleSet `ACTIVE` while those children remain open.
- If no later turn exists, finalize as `VICTORY` regardless of the flag.

### Continued-exploration lifecycle

- Separate "Victory observed" from terminal BattleSet status.
- Derive observed Victory from durable selected advancement decisions while the BattleSet remains `ACTIVE`.
- Once continued exploration has no remaining child branches:
  - finalize as `VICTORY` if any turn produced a selected Victory;
  - otherwise retain existing plan-complete/no-survivor behavior.
- If multiple turns produce selected Victories, choose the final canonical Victory deterministically from the per-turn winners using the same ranking.
- Ensure `AggregateBattleSetStatus` respects `continue_after_victory` and cannot prematurely terminalize the BattleSet merely because a raw Victory result exists.

### Atomic advancement

- Extend `ApplyBattleTurnAdvancementCommand` so one transaction persists:
  - expected execution identities and terminal hashes;
  - Victory pool and decisions;
  - next-turn pools and decisions;
  - child waves;
  - current-wave statuses;
  - BattleSet lifecycle state.
- Continue accepting terminal advancement with no child waves, but require a persisted selected Victory whenever the requested BattleSet state is `VICTORY`.
- Preserve exact-replay idempotency and reject partial persisted shapes.

### Manual continuation

- Show both Victory and `ReachedNextTurn` candidates in manual selection.
- Require exactly one selected Victory when Victory candidates exist.
- With `continue_after_victory=false`, reject simultaneous next-turn selections.
- With it enabled, allow the selected Victory and selected continuation candidates to coexist.
- Route manual application through the same atomic advancement and workflow-transition services as automatic selection.

### Qt and E2E

- Expose `Continue exploring after Victory` in SavorQt's workflow launcher.
- Display the selected Victory in Battle Analysis even while the BattleSet remains active.
- Update SavorE2E Battle graph declarations and verification to include the new argument, defaulting to `false`.
- Hard-cut workflow graph/static-config hashes affected by the added argument.

## Test plan

- Verify multiple Victory candidates select the deterministic best candidate.
- Verify Victory plus continuation candidates with the flag disabled records the Victory and creates no children.
- Verify the enabled flag records Victory while creating next-turn waves and keeping the BattleSet active.
- Verify exploration eventually finalizes as Victory using the canonical selected candidate.
- Verify retry replay produces the same decisions and terminal-SHA conflicts are rejected.
- Modify or remove tests expecting the old global Victory shortcut.
- Build Debug `SavorTests`, then Release `SavorE2E` and `SavorQt`.

# BattleTurnRunner Implementation Plan (Updated Decisions)

## Goal
Add a **new**, separate runner that executes exactly **one battle turn per job** from a savestate that is already at (or just before) turn input.

This runner should:
- Preserve deterministic behavior when repeatedly starting from the same RNG seed.
- Accept per-turn commands (one turn only).
- Support predicates, including turn-specific subsets passed by coordinator.
- Preserve max fake-attack accounting **across turns** (coordinator responsibility).
- Coexist with the current multi-turn `BattleRunner` without replacing it.

---

## Current baseline
- `PK_BattleTurnRunner` currently maps to the existing multi-turn battle-path runner (`BattleRunnerScript` + `BattleRunnerPayload`).
- That implementation loops turns internally and consumes a full `BattlePath`.

Implication: we will keep old behavior as-is and add a distinct one-turn program kind.

---

## 1) Program kind decision
- ✅ New enum: **`PK_BattleSingleTurnRunner`**
- ✅ Keep existing `PK_BattleTurnRunner` unchanged for backward compatibility.

---

## 2) New one-turn module
Create:
- `SavorCore/Phases/Programs/BattleTurnRunner/`
  - `BattleTurnRunnerScript.h`
  - `BattleTurnRunnerPayload.h/.cpp`
  - (optional) DB helper later if needed

### Script behavior (single-turn, decided)
1. Arm phase/predicate breakpoints.
2. Load snapshot.
3. Infer turn-start mode from `current_turn` + encountered breakpoints:
   - If snapshot starts at first turn / pre-turn state: tolerate/load battle prelude (`BattleLoadComplete`) and apply `INITIAL_INPUT` as needed.
   - If current turn `> 1`: **do not apply initial input**, but run **one frame** before materializing/applying this turn (desync prevention).
4. If snapshot already starts on `TurnInputs`, treat as no-prelude start.
5. Materialize/apply exactly one turn from a single `TurnPlan` payload.
6. Run until one of:
   - `TurnInputs` (next turn reached),
   - `EndBattleVictory`,
   - `EndBattleDefeat`,
   - timeout/error.
7. Evaluate predicates at hit BPs.
8. Return result values (see below).
9. Do **not** call `OpCapturePredBaselines()` here (defer to caller/orchestrator).

### Required return fields
- battle outcome with distinct `ReachedNextTurn` outcome value.
- predicate pass/fail summary.
- executed turn index (input/output).
- fake-attack count used this turn.
- cumulative fake attacks used (if provided by coordinator).
- **ending RNG seed** (required for downstream branch filtering).
- (if enabled) artifact/savestate handle for next-turn continuation.

---

## 3) Payload contract decision
New payload should carry:
- `run_ms`, `vi_stall_ms`
- `current_turn`
- optional `initial` frame (used only when needed)
- **single** `TurnPlan`
- predicates for this job (already filtered by coordinator)
- fake-attack bookkeeping fields as metadata

### Validation/Enforcement decisions
- ✅ Encode a single `TurnPlan` directly (not 1-entry `BattlePath`).
- ✅ Enforce fake-attack budget in coordinator only (not payload decoder).
- ✅ Decoder still validates structural integrity (malformed blob, malformed predicates, malformed plan bytes).

---

## 4) Registry/wire integration
- Add `PK_BattleSingleTurnRunner` in `Wire.h`.
- Update `ProgramRegistry.cpp`:
  - map new kind to `MakeBattleTurnRunnerProgram()` (new one-turn script function)
  - map decode to new `BattleTurnRunnerPayload::decode_payload(...)`
- Keep existing `PK_BattleTurnRunner` mappings untouched.

---

## 5) Orchestration strategy (decided)
Start with **Option A** (in-memory coordinator/explorer mode), defer DB-staged mode.

Coordinator responsibilities per turn:
1. Expand candidate actions for this turn.
2. Pre-filter by fake-attack budget (`used_so_far + this_turn <= max`).
3. Build one-turn jobs with turn-scoped predicate lists.
4. Run jobs and keep only branches that:
   - pass predicates,
   - produce successful runnable outcomes.
5. Persist per-branch metadata for survivors, including:
   - ending RNG seed,
   - fake attacks used so far,
   - savestate/checkpoint artifact reference,
   - optional context snapshot.

### New dedup/filter rule (important)
At end of each turn, keep unique branch states by:
1. predicate success required,
2. deduplicate by **ending RNG seed only** (for now),
3. scope dedup to branches with equivalent non-fake-attack turn inputs (i.e., do **not** dedupe across different real action choices),
4. among RNG duplicates, keep branch with **minimum cumulative fake attacks used**.

Savestate retention for duplicates:
- persist savestate/checkpoint only for the kept winner branch (fewest total fake attacks).

Tie-break chain when RNG + fake-attack totals are equal:
1. optional predicate-success preference (when additional optional predicates are present),
2. higher number of PCs still alive,
3. higher remaining total party HP,
4. deterministic stable fallback (e.g., lowest branch id).

Design note: implement tie-break evaluation as a pluggable ordered comparator list so additional criteria can be appended later without restructuring coordinator logic.

---

## 6) Predicates
- ✅ Coordinator-side filtering only.
- ❌ No schema extension for active-turn masks at this time.

---

## 7) Savestates and contexts
- ✅ One-turn runner should accept/emit savestate artifacts.
- ✅ For duplicate RNG branches, store savestate only for the kept winner branch.
- ✅ Keep `BattleContext` snapshots only for **surviving** branches.
- ✅ `INITIAL_INPUT` is not required for turns that already start at `TurnInputs`.
- ✅ Timeouts should be generous overall, with longer timeout for first turn.

---

## 8) Testing updates
### Unit/component
- payload encode/decode roundtrip for single-turn payload.
- script behavior tests for:
  - first-turn/prelude path,
  - turn>1 one-frame-advance behavior,
  - distinct `ReachedNextTurn` outcome,
  - ending RNG seed emitted.

### Integration
- deterministic repeatability from same savestate + same turn command => same outcome + same ending RNG.
- multi-turn orchestration smoke test:
  - fake-attack budget enforced across turns,
  - turn-scoped predicates honored,
  - dedup keeps unique RNG outcomes,
  - duplicate RNG survivors resolve to min fake-attack branch.

### Regression
- existing `PK_BattleTurnRunner` multi-turn flow remains unchanged.

---

## 9) Implementation order
1. Add `PK_BattleSingleTurnRunner` and registry wiring stubs.
2. Implement single-turn payload + tests.
3. Implement one-turn script (including turn>1 frame-step rule) + tests.
4. Implement in-memory turn coordinator with survivor filtering + dedup.
5. Add savestate artifact plumbing and survivor-context retention.
6. Add integration/regression coverage.

---

## Finalized decisions (no open blockers in this plan revision)
- Dedup key: RNG seed only (for now), scoped to equivalent non-fake-attack action branches.
- Savestate persistence: keep only the winner savestate among duplicates.
- Prelude handling: infer from `current_turn` + observed breakpoints; no explicit payload flag required.
- Tie-breaks: use flexible comparator chain (optional predicate success -> PCs alive -> total party HP -> stable fallback).
- Outcome bucketing: `ReachedNextTurn` goes to a separate intermediate bucket.

---

## Expected file touch list (implementation phase)
- `SavorCore/Runner/IPC/Wire.h`
- `SavorCore/Phases/Programs/ProgramRegistry.cpp`
- `SavorCore/Phases/Programs/BattleTurnRunner/*` (new)
- `SavorCore/Phases/BattleExplorer.*` or new `BattleTurnExplorer.*`
- `SavorTests/*`

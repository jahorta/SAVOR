# Battle Interactome Gap Register

This file tracks the main first-battle battle-interactome gaps that remain
after the current aggregate `trace-job` refinements. It is a staging artifact:
do not promote any of these notes to `D:\SoAInvestigate\Ghidra Updates` until
the first-battle predictor is validated.

Database policy: `D:/SoaSimDBDebug` is mutable and must only be used as the
source for an explicit `prepare-db` copy. All aggregate traces, checkpoint
imports, smoke tests, and research reads should use the pinned
`D:/SavorPredictDB` copy.

Call-stack attribution policy: when a checkpoint needs a caller callsite, do
not trust the live PowerPC LR register by itself. Our standard attribution path
is to walk the PowerPC backchain from `r1`: each frame stores the next stack
pointer at `sp + 0x0` and the saved return address at `sp + 0x4`; for a direct
`bl` caller, the callsite is `saved_return_address - 4`. Capture output should
retain raw `lr` as a diagnostic field, but `caller_return_pc` and
`caller_callsite_pc` should come from the first readable stack frame with a
nonzero saved return address. Frame `0` is preferred when it is valid, but some
rand pauses have a zero saved return there and require frame `1`.

RNG ownership policy: never add a predictor draw only to preserve downstream
seed alignment. A draw can become default predictor behavior only when it is
owned by a statically identified callsite/function/thread path or by a visually
verified gameplay event. If a live capture proves a seed advance but the owner
is not yet known, keep it as an unattributed residual or ambiguous event and use
it to drive the next static/live investigation.

## 2026-06-23 Live Gap Refresh Status

The refresh campaign artifacts live under
`Analyses/battle_runs_first_battle/gap_refresh_20260623/`. The machine capture
profiles used for this campaign intentionally avoided battle-progress PCs;
stored progress text was kept as comparison evidence only.

## 2026-06-25 RNG Seed Watch Stack Attribution

The Aika-to-Vyse divergence capture was rerun with the RNG seed write watchpoint
on `core.RNG_SEED` (`0x803469A8`) armed after `setupTurnAction_80082134`.
Artifacts are under `C:\savor\aika_vyse_149113\seed_watch_stack2\`. The cloned
job still matched stored exec job `149113`: Aika hit `[4]Soldier` for 29, Vyse
hit `[4]Soldier` for 44, `[4]Soldier` died and dropped Electri Box, then
`[5]Soldier` hit Vyse for 46.

The stack-walk attribution confirmed why raw LR was unusable: LR remained
`0x00000000` at the rand seed-store PC, but the PowerPC backchain yielded usable
callers. The previously unclassified post-Aika/pre-Vyse draw window points to
callsite `80042EB8` (`FUN_80042b10` effect-spawn region) for draw indexes
`114..121`. Vyse's later attack-result draws attribute to `80081BE4`, damage
draws to `80010B2C`, action-view/camera draws to `80051B48` / `80051320`, and
the successful Electri Box drop draw to `8002BD20` through its caller stack. This
keeps the predictor mismatch classified as missing effect/update RNG ownership
between Aika's resolved effect burst and Vyse's setup, not as a drop-table
ordering issue.

Follow-up static/live validation focused specifically on `FUN_8003ba08` and
`FUN_80042b10` lives under
`Analyses/battle_runs_first_battle/effect_spawn_loop_static_20260625/`. The
successful narrow run is `C:\savor\effloop_149113_0625b`. It confirms six
`FUN_8003ba08:8003BB24` copy-complete rows for exec job `149113`, each with
copied source key/loop-count fields matching the serialized source record.
Those copied buffers produced three complete effect pairs: key `5` `16+6`,
key `4` `16+6`, and key `4` `16+6`. The `FUN_80042b10` loop gate at
`80043278` fired `loop_count + 1` times per buffer, and the observed key-4/key-5
rows used the binary position draw, x/y/z scale draws, and variant draw per
iteration. This validates the 110-draw rule for selected key-4/key-5 first
battle effect pairs in the same capture that attributes 330 RNG seed writes to
stack callsite `80042EB8`.

Important attribution distinction: `80042EB8` is the stack-owner/region
attribution for these effect RNG writes, not the direct RNG callsite for every
draw. Direct local draw checkpoints in the loop are `80042FBC`, `80043020`,
`80043048`, `80043070`, and `800430FC`; this run did not exercise the
four-way position branch `80042F3C` or random-axis branch `80043200`.

## 2026-06-25 Action-View Pathing Tail Static/Live Refresh

The first post-Aika predictor divergence in exec job `149113` is now mapped to
an action-view/pathing tail before the key-5 `FUN_80042b10` effect burst.
Staging artifacts are under
`Analyses/battle_runs_first_battle/action_view_pathing_draw_conditions_20260625/`,
with the tracked contract at
`SavorPredict/planning/static_support/action_view_pathing_tail_contract.txt`.

Disassembly confirms this call path:

- `UpdateActionViewRecord_80051264:80051320 -> FUN_800519f4`;
- `UpdateActionViewRecord_80051264:800514B0 -> FUN_8005259c`;
- `FUN_8005259c:800526EC -> FUN_8005174c`;
- `FUN_8005174c:800518A8/800518C4 -> FUN_80011694`;
- `FUN_80011694:80011794 -> RNG::rand_8025ecc4` only when the accumulated
  candidate score is zero at `80011790`.

The narrow live run used a no-progress-PC profile and an RNG seed write
watchpoint armed after input. The profile was intentionally heavy and timed out
later, but it captured the complete first divergent cluster:

- one `FUN_800519f4:80051C00` draw for Aika's selected payload mode `1`;
- seven `FUN_80011694:80011794` zero-score fallback draws from the target-side
  caller stack `80011724 <- 800518C4 <- 800526EC`;
- the tail ends at seed `0xE2809E36`, after which the previously mapped key-5
  `FUN_80042b10` effect burst begins.

This closes the immediate "eight missing draws before Aika's effect burst"
classification for `149113`: the predictor needs a modular action-view/pathing
tail component between synchronous attack/damage resolution and effect burst
prediction. The structural `FUN_800117ec` scoring rules are now mapped,
including the raw absolute X/Z degree-difference gate. The remaining
first-battle implementation gap is maintaining frame-equivalent positions and
worksheet fields so those rules can be evaluated from simulated state; until
that state replay exists, nonzero-score `FUN_80011694` cases should remain
explicit ambiguous/unsupported events.

2026-06-26 passive participant follow-up: the passive worker discovery portion
of this gap is now narrowed. `FUN_8008e2b0` / `FUN_8008e338` stage non-active
participant workers, `FUN_8008deec` dispatches concrete callbacks, and the
observed `149113` Aika window reached `FUN_8008178c` through passive
`FUN_8008c21c` workers for slots `0` and `5`. `FUN_8008c21c`,
`FUN_8008c7b0`, and `FUN_8008d4a4` are position-relevant at the `posHolder`
layer, but no passive callback in the current static exports was found to call
the `FUN_8001ab60` bridge. The remaining implementation target is therefore
frame-state replay across `posHolder` commits, `FUN_8001ab60` bridge copies,
and `FUN_800117ec` scoring, not an unknown passive RNG source.

Runtime validation under `C:\savor\passive_validate_20260626_b1` confirmed the
same first-geometry-window pattern for source exec jobs `147884`, `148016`,
`149113`, and `158364`: every job had `FUN_8008178c` movement commits after the
last recorded `FUN_8001ab60` bridge and before the first `FUN_80011694`
geometry call.

The terrain-input side of the frame-replay gap is also closed for first battle.
SPICE now exports SST record-0 post-command tail data as
`records[0].sstCommandBlock.battleGridTerrainSource9x9` in
`s001.sst_sml_command_map.json`. A focused live validation at
`C:\savor\terrain_live_147884_20260626` captured `PTR_80347354` and
`PTR_80347350` at `setupGridAndCombatants_800849a8:800849C8`, immediately after
`setupBattleGrid_800840bc` returns and before combatant placement. The mapped
SPICE 9x9 terrain source matched both live 11x11 grids with zero mismatches.
SavorPredict can initialize first-battle active/base terrain from that SPICE
artifact, then overlay enemy-event id `0` combatant positions.

2026-06-26 action-view tail handoff follow-up: the reduced combined run at
`C:\savor\avtail149113_full_20260626` completed and matched stored exec job
`149113` progress. It validates the exact Aika boundary ordering for the
current mismatch case: after mechanical attack/damage, Aika's selected payload
mode `1` consumes one `FUN_800519f4:80051C00` draw, target-side
`FUN_80011694` zero-score fallback consumes seven draws, `FUN_8003ba08` copies
the key-5 source records with loop counts `16` and `6`, and only then does the
`80042EB8` / `FUN_80042b10` effect region start consuming RNG. This closes the
handoff classification for `149113`. The remaining first-battle implementation
gap is not ordering or scorer branch semantics; it is computing
`FUN_800117ec` scores from frame-replayed combatant worksheet positions so
`FUN_80011694` fallback draws are predicted from state instead of from a
case-specific count. Angle-gate details are staged in
`SavorPredict/planning/static_support/fun_800117ec_angle_gate_resolution_20260626.md`.

## 2026-06-24 Aux-List Gate Static Refresh

The mode-`0xe` action-view question now has a disassembly-backed answer from
the refreshed local Ghidra project. Staging artifacts are under
`D:\SoAInvestigate\Analyses\20260624_aux_list_gate_mode0e_static\` and
`D:\SoAInvestigate\Analyses\20260624_aux_list_ownership_static\`.

Closed by static analysis:

- `FUN_80012f58` is the field6 consumer that decides whether a basic/crit
  action-view synthetic child is spawned.
- Field6 `4`, `8`, and `9` enter category `2`.
- Category `2` calls
  `STD::CountMatchingStd0Entries_80009030(aux_root, 4, -1, 0x2a, 3)` at
  `8001331c`.
- Only a zero count reaches `80013334`
  `SpawnSyntheticActionViewRecord_80053f38(slot, 0)`.
- `SpawnSyntheticActionViewRecord_80053f38(slot, 0)` maps field6 `4` and `8`
  to selected action-view record mode `0xe`.
- `UpdateActionViewRecord_80051264` dispatches mode `0xe` to `FUN_80052b24`,
  whose direct RNG call is `80052bf0`.
- A nonzero aux-list count suppresses the synthetic mode-`0xe` path. In that
  case the serialized selected-record path can instead spend the mode-0 draw at
  `UpdateActionViewRecord_80051264:800513d4`.
- `STD::LoadStd0EntryTable_80035d4c` is the producer for the runtime
  0x10-byte aux-entry table scanned by
  `STD::CountMatchingStd0Entries_80009030`.
- Direct callers of `STD::LoadStd0EntryTable_80035d4c` are
  `STD::LoadCombatantStdResourcePair_80021934` and
  `STD::LoadBattleCombatantStdResourcePair_80067e8c`.
- In the battle path, `STD::LoadBattleCombatantStdResourcePair_80067e8c`
  stores the copied `%s_STD` root at `loaded_resource +0x30` (`80067fc4`),
  appends `_0_STD`, and calls `STD::LoadStd0EntryTable_80035d4c` with
  `r4 = loaded_resource+0x30` (`80067fd0..80067fd8`).
- `STD::LoadStd0EntryTable_80035d4c` writes the materialized table back
  through that pointer (`80035e0c` cache path, `80035fa0` file-load path),
  walks entries in 0x10-byte steps, and rewrites recognized payload offsets to
  runtime payload buffers using the `GetSTDEntryBufferSize03Type` /
  `GetSTDEntryBufferSize05Type` allocation path.
- The `_0_STD` aux-entry owner is deterministic, not an observation cache:
  cache lookup reads filename keys at `8030a244 + slot * 4` (`80035d80`) and
  table pointers at `8030a214 + slot * 4` (`80035da8`). On a miss,
  `STD::LoadStd0EntryTable_80035d4c` consumes and clears the transient
  `DAT_8030a20c` loaded-buffer handoff (`80035e34`, `80035e48`) or calls
  `Battle::Resource::FindLoadedBattleResourceByName_8006daf8` (`80035e54`).
- Cache entries are produced by
  `Battle::Resource::ProcessQueuedBattleResourceFile_8006ddbc` after a queued
  resource file finishes loading: it calls `FUN_80035c38` at `8006df8c`,
  stores the returned materialized table pointer through `8006dfa4`, and
  stores the matching filename key through `8006dfc4`. `FUN_80035c38` copies
  loaded `_0_STD` 0x10-byte rows to runtime/cache storage, allocates recognized
  type-3/type-5 payload buffers, copies source payload bytes, and frees the
  loaded source buffer; it does not synthesize new entries from live battle
  state.
- The sibling direct caller
  `STD::LoadCombatantStdResourcePair_80021934` uses the same `_0_STD`
  materialization function for combatant resources after consuming
  `DAT_8030a208` or resolving `%s_STD` by name, storing the copied root at
  `loaded_resource+0x30` (`80021a88`), then calling
  `STD::LoadStd0EntryTable_80035d4c` at `80021a9c`.
- `STD::CountMatchingStd0Entries_80009030(root, action_key, secondary_key,
  location_code, opcode)` returns zero exactly when no entry before the
  negative-location sentinel matches the payload action key, passes the
  optional combined-id filter, and is not excluded id `0x00030041`.

Closed for first-battle basic-action prediction:

- `SavorPredict write-first-battle-action-view-resource-profile` now stages
  and `trace-checkpoints` summarizes the live evidence needed for that
  table-identity step. It captures the battle STD pair loader (`80067e8c`),
  the `_0_STD` materializer call and write-back (`80067fd8`, `80035e0c`,
  `80035fa0`), cache lookup and cache producer rows (`80035d80`,
  `80035da8`, `8006df8c`, `8006dfa4`, `8006dfc4`), transient handoff rows
  (`80035e34`, `80035e48`), and the existing selector query matrix. On the
  `149113` chain run at `C:\savor\avres149113_chain\run`,
  `trace-checkpoints` linked all three later `FUN_80012f58` aux roots to
  earlier cache hits and cache producers:
  `MA001/ma0010.std`, `ma000/ma0000.std`, and `MB000/mb0000.std`. The same
  run also proved the intermediate active-combatant chain
  `r31+0x24 -> +0x10 -> +0x30` inside each selector row: all three rows
  reported `chain_matches=true`, and each selected loaded-resource pointer
  plus `0x30` matched the materializer `root_field_ptr`.
- `ActionViewStdResourceResolver` encodes the validated first-battle
  basic-action resource/cache identity without hardcoding heap aux-root
  addresses: slot `0` uses `ma000/ma0000.std`, cache key `0x00989680`, cache
  slot `0`; slot `1` uses `MA001/ma0010.std`, key `0x00989681`, cache slot
  `1`; slots `4` and `5` use `MB000/mb0000.std`, key `0x00989A68`, cache
  slot `2`. The runtime aux root consumed by `FUN_80012f58` is modeled as
  `loaded_resource + 0x30`, with the selected table contents reproduced from
  SPICE JSON by prepending the first primary `%s_STD` action row before the
  companion `%s0_STD` rows.
- `ActionViewSelectorModel` now reports disassembly-confirmed helper-call
  outcomes for `FUN_80053f38` and `FUN_80032bbc`: call PC, callee, current
  actor slot, mode argument, and branch role. For known first-battle
  field6 values `4` and `8`, `FUN_80053f38(slot, 0)` is reported as selecting
  action-view record mode `0xe`; other field6-to-record-mode rows remain
  unknown until their switch rows are validated.
- The remaining goal-1 static pass closed the direct caller/helper side-effect
  portion of the `FUN_80012f58` branch map. `FUN_800136dc` reaches the
  selector at `80013b20..80013b24` from one action/view state-dispatch case.
  `FUN_80012f58` itself reaches or skips mode `0xe` by selecting helper call
  sites; `SpawnSyntheticActionViewRecord_80053f38` writes the selected record
  mode at `80054054` and assigns mode `0xe` at `80053ff0` for field6 `4`/`8`
  when called with mode argument `0`. `FUN_80032bbc` was audited as a
  deterministic slot/view helper path with no direct selected-record mode
  write in the function body.
- With the complete SPICE STD JSON export from
  `Analyses/tmp_first_battle_std_json`, the `149113` chain capture now
  validates one live helper branch at the call-site level:
  `action_view_spawn` at `80013334` for active slot `5`, field6 `4`,
  category `2`, state `2`. The pure selector predicted
  `mode2_count_zero_spawn_mode0`, `FUN_80053f38(slot, 0)`, and selected
  action-view record mode `0xe`; `trace-checkpoints` reported one helper-call
  comparison, one match, and zero mismatches/missing expected calls.
- The later helper-profile batch at `C:\savor\avhelp0624a` armed all seven
  selector helper PCs and validates the observed first-battle helper subset
  beyond the single `80013334` row. Clean captures hit and compared
  `8001318c` (`FUN_80053f38(slot,1)`, state-0 actor-change spawn),
  `8001321c` (`FUN_80053f38(slot,0)`, mode-0/mode-4 state-2 spawn),
  `80013334` (`FUN_80053f38(slot,0)`, mode-2 count-zero synthetic spawn), and
  `8001338c` (`FUN_80032bbc(slot,1)`, mode-2 tail helper). Representative
  trace `job-880449.txt` reported `MatchesExpected`, `201` helper-call
  comparisons, `201` matches, and zero helper-call mismatches or missing
  expected calls. Source job `158364` cloned as `880447` failed, so that
  clone's capture remains partial evidence only.

Still open:

- The complete static decision tree around `FUN_80012f58` is no longer a
  first-battle-basic blocker. Remaining selector items are live validation
  coverage or broader action-resource generalization.
- The general selected resource/table identity path for non-first-battle or
  non-basic-action cases still needs the full data-backed source-path model
  from `FUN_8006721c`/`FUN_8001be2c` and the selected serialized
  source/action resource row. This is a generalization gap, not a first-battle
  basic-action blocker.
- `SavorPredict/ActionViewSelectorModel.*` now implements the pure
  `CountMatchingStd0Entries` predicate over decoded STD0 rows, including the
  sentinel walk, payload key match, `0x00030041` exclusion, and the exact
  mode-`0xe` predicate. `ActionViewStdJsonLoader.*` can import SPICE
  `spice_std_ir_v1` entry-table JSON into that model, and
  `BattlePredictor` can now consume that table through the opt-in
  `--action-view-std-json-dir` path. For first-battle basic actions, the
  resolver maps actor slot to the validated cache-backed `%s0_STD` table. The
  remaining implementation gap is the general runtime-equivalent resolver for
  non-first-battle and non-basic-action resource selections.
- Remaining selector helper call sites that still need live branch-level
  validation are now narrowed to `8001329c`, `800133e4`, and `800134a4`. The
  current trace/model path can compare them when a capture exercises effective
  mode `1`, mode `3` count-zero, or mode `5` with selector operands and a
  selected aux table. The generated selector/resource profiles already arm all
  seven helper PCs. A local `Analyses`/planning artifact search after the
  helper-profile batch found static disassembly references for those three PCs
  but no live JSONL hits.

  A follow-up SPICE JSON count pass over the current first-battle resolved
  resource pairs (`ma000/ma0000.std`, `MA001/ma0010.std`, and
  `MB000/mb0000.std`, each with the first primary action row prepended) narrows
  the reason:

  - `800133e4` needs effective mode `3`, selector state `2`, and
    `CountMatchingStd0Entries(aux_root, 5, -1, 0x2a, 3) == 0`. All three
    first-battle resource pairs return count `1`, so the count-zero branch is
    not table-reachable for the current basic first-battle resource selection.
  - `800134a4` needs effective mode `5` or the mode-5 count-zero branch. The
    same first-battle resource pairs have no special direct-secondary candidate
    rows at combined id `0x0003002e`, so this path appears outside the current
    basic first-battle resource/action selection.
  - `8001329c` remains a true job/action-selection gap: it requires effective
    mode `1` and selector state `2`, and is not explained by the aux-count
    query results.

  A follow-up coverage pass over `C:\savor\avhelp0624a\traces` confirms this
  is a live-case gap rather than a missing checkpoint in that batch: the
  helper-profile traces contain no `pc=8001329c`, no
  `checkpoint=action_view_mode1_state2_call`, and no
  `gate_category_0x2f=1` row. Observed selector helper/query coverage stayed
  within categories `0`, `2`, `3`, and `4`. The pure selector model now has
  unit coverage for the `8001329c` branch, but branch-level live validation
  still needs a case that actually reaches effective mode `1` with selector
  state `2`. Use the narrow
  `write-first-battle-action-view-selector-coverage-profile` generator for
  that search; it records selector entry/classification, requested-mode
  readiness, state normalization, dispatch, and all seven helper call sites
  without adding battle-progress PCs or the targeting-camera `800608DC` PC.

  The later lightweight selector-coverage batch at
  `C:\savor\avselcov0624\run` included the defend and crit/counter
  representative jobs and reached `FUN_80012f58` 2553 times, but still found
  no requested category `1`, no dispatch category `1`, and no `8001329c`
  helper call. Therefore `8001329c` is now treated as statically mapped but
  not first-battle-observed. It is deferred to broader battle action classes
  such as items, magic, S-Moves, or other non-basic actions unless a future
  first-battle capture first proves category `1` is consumed by
  `FUN_80012f58`.

  Therefore the next first-battle-basic step is not more profile plumbing for
  `8001329c`; it is to keep validating the observed category `0`, `2`, `3`,
  and `4` selector paths and leave `8001329c`, `800133e4`, and `800134a4`
  as non-basic/generalization candidates unless new first-battle evidence
  appears.

## 2026-06-23 Predictor/Worker Comparison Status

The predictor-validation campaign artifacts live under
`Analyses/battle_runs_first_battle/predictor_worker_comparison_20260623/`.
The generated profile explicitly excludes battle-progress PCs and `800608DC`;
it captures first-battle queued-instruction fields for slots `0`, `1`, `4`,
and `5`, plus setpoint/helper/consumer checkpoints around
`instr_param_0x6`, action setup, enemy movement setup, damage, counter,
death/drop, and action-view ambiguity sites.

The refreshed batch (`live_batch_v2`) ran source exec jobs `147884`, `147896`,
`148016`, `149113`, `158364`, `880442`, and `173344` with `--max-workers 7`.
All seven cloned jobs succeeded. `trace-checkpoints` was then regenerated from
the captures after fixing DB-backed comparison to seed prediction from the
first live captured RNG row when no explicit override seed is present.

Closed or corrected by this batch:

- The profile/comparison mechanism is validated on baseline job `147884`
  (clone `880443`): all three live attack chains match predictor actor,
  target, and consumed `instr_param_0x6`.
- `r0` at `80010BDC` and `80010C44` is no longer treated as the live
  `instr_param_0x6`. It can be stale at those PCs. The broad profile now
  records it only as a candidate, while the model reads queued-instruction
  memory and the actual `80010C40` crit branch gate.
- The old `149113` Electri Box mismatch is narrowed: the first two PC attacks
  match live against prediction, and the remaining mismatch is the Soldier
  attack parameter (`live instr_param_0x6 = 0`, predicted `1`). That points at
  enemy movement/parameter setup rather than the Aika/Vyse crit gate.

Still open after this batch:

- The broad validation profile did not by itself prove turn-order qsort
  permutation, but the later narrow turn-order replay closed that gap for
  first-battle action-queue qsort. Remaining multi-chain mismatches should be
  investigated in movement/action setup, action-source selection, counter, or
  victory/death interleavings rather than treating qsort tie order as open.
- Many setpoint/helper rows still lack reliable actor correlation, so
  `trace-checkpoints` often reports `live_worker=unknown`.
- Counter validation still reports missing live gate fields in this broad
  profile; use a narrower no-progress-PC counter profile before changing
  counter prediction from this evidence.

## 2026-06-23 Movement/Source/Counter Validation Infrastructure Update

SavorPredict now has stricter no-progress-PC checkpoint support for the three
remaining first-battle mechanism gaps:

- Enemy action movement/setup: generated first-battle profiles include split
  `HandleECInst` helper rows for `FUN_8008a174`, `FUN_80082340`,
  `FUN_8008a280`, target-adjacent checks, Soldier `instrParam_0x6` stores, and
  worker-select PCs. `ActionSetupCheckpointModel` now merges those helper rows
  back into the owning `8008bc68` enemy setup draw, derives `draw % 10`,
  final `instrParam_0x6`, and validates selected worker agreement with
  `instrParam_0x6 == 0 -> FUN_80087f6c` and fallback `FUN_80087844`.
- Serialized action-source bridge: generated profiles now capture a cluster of
  `FUN_8006782c` source-selection candidates (`8006782c`, `80067a9c`,
  `80067ad0`, `80067b50`, `80067bd0`) plus the existing `FUN_8006721c`
  field6 bridge. This gives live runs a chance to prove the selected source
  slot before the bridge, instead of relying on a single late source-selection
  PC that previous runs did not observe.
- Counter gate/follow-up: generated profiles now avoid the progress-owned
  `80081d84` PC and instead capture internal rows around
  `shouldCounter_800819d0`: gate input (`800819fc`), chance compare
  (`80081ab0`), queue/result writes (`80081b54`, `80081b7c`, `80081b80`), and
  adjacent follow-up handoff rows (`80081d78`, `80081d80`, `80081d88`).
  `CounterCheckpointModel` now merges split gate-input, roll, result, and
  follow-up rows into one logical gate before validating expected rolls,
  result, queue field, current-counter update, and follow-up presence.
- Dynamic field6 access-watchpoint capture is now implemented as generic VM
  infrastructure. Capture profiles can arm static queue-field access
  watchpoints and derive runtime worksheet watches from a captured PC plus
  `base_gpr + offset`; memory records are classified as reads or writes from
  the decoded current instruction. `SavorPredict
  write-first-battle-field6-watch-profile` emits a no-progress-PC profile for
  queued `instr_param_0x6`, action/source `field6_0x6`, and SST/STD bridge
  worksheet fields, and `trace-checkpoints` now has a field6 access-watchpoint
  report section.

This update does not close the live evidence gaps by itself. The next run
campaign should use a short run root and the generated predictor-validation
profile plus the generated field6 access-watch profile on representative jobs
that exercise (1) Soldier direct/fallback setup, (2) source-key `4`/`5`/`8`
action-source bridge rows, and (3) both no-draw and draw-consuming counter
gates. Progress text remains comparison evidence only; machine profiles must
continue to exclude battle-progress PCs.

Closed for first-battle v1:

- `BattleInstance + 0x8` is now closed as the current-counter-chance increment
  source and is implemented in the predictor. Current counter chance should be
  seeded from `CombatantInstance.current_counter_chance` when present; the
  damage-side counter-increase path applies the active instance byte at `+0x8`,
  while successful normal counters reset current chance.
- First-battle Soldier drop row order and stop-after-success are closed:
  Electri Box is row 1/item `273`, Moonberry is row 2/item `258`, and no-drop
  consumes both failed enabled rows. The refresh captured Electri Box success,
  Moonberry success, and no-drop with matching table/outcome rows.
- Supported first-battle 8004 effect burst shapes are closed for source keys
  `4`, `5`, and observed crit key `8`: keys `4`/`5` use `16 + 6` loops for
  `110` draws; key `8` uses `16 + 4` loops for `100` draws.

Partially validated:

- The pre-AI v1 predictor contract remains
  `pre_ai_draws = fake_attacks + 1 + pc_count`, but direct `800608DC`
  target-camera/fake-attempt checkpointing still perturbs the input macro. The
  explicit `800608DC` batch failed for fake counts `0..2`; the matching
  no-`800608DC` control batch succeeded. Keep `800608DC` out of default
  machine profiles.
- First-battle dead-target PC retargeting is validated enough for v1: job
  `158364` repairs queued target `4` to slot `5` via
  `GetClosestCombatant_80083c50(self=0, group_mask=2)` with zero RNG draws.
  Exact current-instruction memory-watch proof of the target-byte store remains
  partial because the observed memcheck deltas were unattributed.
- Action-view gate/camera counts are live-observed across the refresh and the
  2026-06-23 short-root validation run `C:\savor\spv\asrc_20260623_002`.
  That validation run completed ten representative first-battle jobs and
  observed `8001331c`/`80013320` action-view query rows in every job, but did
  not hit the disassembly-backed `FUN_8006782c -> FUN_8006721c` source-selection
  bridge PCs. Keep the bridge as a static contract for other paths; for
  first-battle v1, the open live target is now the `8001331c` query path and
  the state that feeds actor/source `field6_0x6` and effect source keys.
- The 2026-06-24 short-root rerun
  `C:\savor\avtbl32_149113_20260624\run` validates the updated 32-row
  aux-table fingerprint for exec `149113` with override seed `0x9702c2b0`:
  all three action-view query-call rows had sampled table counts matching the
  live `FUN_80009030` results (`1`, `1`, `0`). This closes the prior Aika
  mode-3 key-5 mismatch as a 12-row sampling artifact, not a predicate error.
  The same trace now identifies the materialized companion table contents for
  all three calls: slot `1` matches `MA001.std` / `ma0010.std`, slot `0`
  matches `ma000.std` / `ma0000.std`, and slot `5` matches `MB000.std` /
  `mb0000.std`. In every case the runtime sample has a one-row prefix before
  the `_0_STD` rows. `ActionViewStdJsonLoader` now imports that prefix from
  the primary action-row JSON, so the source-equivalent materialized table
  matches live at sampled row offset `0`. Runtime aux-root-to-resource-loader
  identity is still open.
- Predictor integration now has an opt-in selector-backed path: DB-backed and
  context-file `predict-battle` calls can pass `--action-view-std-json-dir`
  pointing at SPICE `--export-std-json` output. When supplied, first-battle
  basic attacks resolve the actor's companion `_0_STD` JSON, run
  `ActionViewSelectorModel`, and feed that branch result into the visual RNG
  camera step. This explicitly separates queued attack/movement
  `instrParam_0x6` from combatant `InstructionWorksheet+0x6 field6_0x6`; the
  first-battle profile should derive the attack's direct/melee versus
  fallback/ranged class from the validated per-action command parameter
  (`instrParam_0x6 == 0` versus nonzero), not from actor identity, while the
  source-path model becomes fully data-backed.
- Turn order is closed for first-battle v1: the narrow qsort replay captures
  queue records, qsort call count, sorted output, and final
  `s8_ARRAY_803092f4`; all 14 completed captures match the injected-comparator
  byte-record `SoaQSortModel`, including the previous mismatch cases `433890`
  and `433896`.

Still open first-battle gaps:

- A non-perturbing capture strategy for target-camera/fake-attempt internals
  around `800608DC`.
- Full live validation of the no-progress-PC `shouldCounter_800819d0` internal
  rows, including actor/target, crit suppression, gate reason, current-counter
  update, and follow-up identity.
- Exact first-battle source path from serialized/action-view data through
  actor/source `field6_0x6` and observed action/effect source keys. The
  `FUN_8006782c -> FUN_8006721c` source-selection bridge did not fire in the
  ten-job validation batch, so the next capture should focus on the selected
  record mode, action-view spawn state, the source of the one-row runtime
  aux-table prefix, and the subsequent `FUN_80042b10` effect source fields.
  The new field6 access-watchpoint profile should be used for that pass so
  every confirmed read/write of the relevant `+0x6` fields is classified by
  decoded access instruction rather than inferred from checkpoint order alone.
- The 2026-06-24 fixed-address instruction-worksheet selector run narrowed one
  `field6` ambiguity: guarding-hit and normal-hit damage reactions both write
  `InstructionWorksheet+0x6 = 0x0b`, `+0x8 = 0xffff`, and `+0x1c = 0x2`.
  The differentiating candidate is now `InstructionWorksheet+0xe4`: normal
  damage stays on selected row `8`, while guarded damage repeatedly toggles
  row `9 -> 8`. The row-index semantics for `+0xe4` values `8` and `9` remain
  an open static/data-table target.
- Predictor total-draw exactness after turn order, especially counter/victory
  paths and late action-view/effect interleavings.
- Operational batch robustness for the heavy merged profile shape: the
  `attack_damage_counter_death_drop` refresh run never started workers due
  `copy runtime tree failed`, so it is infrastructure evidence, not
  battle-model evidence.

Deferred beyond first battle:

- Pathing-aware closest-target semantics for multi-candidate retargets.
- Non-Soldier AI, status effects, items, magic, S-Moves, multi-target actions,
  non-normal turns, battle-end rewards, EXP thresholds, and level-up stat
  rolls.

## Filled In Current Aggregate Model

- First-battle status effects are modeled as inactive data, not absent engine
  mechanics. Status-attempt draws are zero for the first battle, while status
  branches remain in the general interactome.
- Soldier AI action, target, and attack-parameter draws are modeled from the
  start seed and fake-attack cursor. The Soldier attack parameter feeds
  `instrParam_0x6`.
- The full current first-turn job-set comparison in
  `Analyses/battle_runs_first_battle/model_progress_all_20260621` validates
  Soldier AI action and target against parsed progress for all `215352`
  turn-one jobs in scope: `215352 / 215352` action matches and
  `215352 / 215352` target matches.
- The broader observable comparison in
  `Analyses/battle_runs_first_battle/all_observable_comparisons_20260621`
  inventories `219764` completed jobs, keeps `4412` turn-two jobs out of the
  first-turn model scope, and reports both all first-turn jobs and the primary
  `fake_attacks >= 1` analysis filter. Under that filter, Soldier AI action
  and target both match parsed progress for `179460 / 179460` turn-one jobs.
- The all-set observable comparison in
  `Analyses/battle_runs_first_battle/job_set_observable_comparisons_20260621`
  compares the same model/progress signals across every emitted partition:
  execution job set, battle set, fake count, outcome, turn variant, battle-set
  status, and RTC. It inventories `19267` execution job sets and `997` battle
  sets from `D:/SavorPredictDB`. Under the primary `fake_attacks >= 1` filter,
  Soldier AI action, Soldier AI target, Soldier attack count, supported
  PC-to-Soldier damage bands, and known first-battle drop-table membership have
  zero mismatches in every grouped slice. The remaining grouped mismatches are
  limited to known weak progress-only observables: workflow PC-before-EC, raw
  progress attack order, one counter-candidate anomaly, and 30 death/drop text
  pairing anomalies.
- The regenerated all-job-set digest in
  `Analyses/battle_runs_first_battle/job_set_observable_comparisons_20260621/all_comparison_digest.md`
  now makes the all-comparisons pass explicit. In the primary
  `fake_attacks >= 1` scope it covers `179460` jobs and `19981` grouped rows.
  The closed grouped signals are `plan_present`, `soldier4_action`,
  `soldier5_action`, `soldier_action_pair`, `soldier4_target`,
  `soldier5_target`, `soldier_target_pair`, `soldier_attack_count`,
  `damage_class_supported`, and `drop_known_first_battle_table`.
  Counter/drop anomalies are bounded and job-level examples are staged in
  `counter_drop_anomaly_examples.csv`.
- The all-set residual pass also shows that fake counts `1..5` share the same
  residual-after-turn-order mode (`362`), so the next residual clue is not a
  fake-count offset. The two player-command variants split cleanly instead:
  both PCs attacking `[4]Soldier`
  (`b802ee917adce86c736765afda0732c6ad2bcdfe7bf6a6f2592a256f61dde6a8`)
  has residual mode `362` and current static-bucket residual mode `347`,
  while both PCs attacking `[5]Soldier`
  (`7b8c29f062997477e1497f91534c06bc3ed119920f7cdc0fb6cf44d9e9a2639b`)
  has residual mode `371` and current static-bucket residual mode `356`.
  Treat this target-dependent nine-draw split as a static-analysis target
  around action source, target-camera/action-view records, and target-dependent
  attack handling, not as a fitted constant.
- Enemy-side attack execution setup has its own model in
  `SavorPredict/EnemyAttackSetupModel.*`. It models the statically proven
  `HandleECInst:8008bc68` RNG gate and branch family from queued instruction
  and movement flags, while leaving final `instrParam_0x6` to live helper
  checkpoints.
- Turn-order draw count and priority assignment are modeled in
  `SavorPredict/TurnOrderModel.*`: queued first-battle basic attacks spend one
  `setupTurn:800711f8` draw each when `jitter_modulus` is nonzero, and the
  helper records `sumQuick`, `jitter_modulus`, assigned priorities, execution
  slots, and tie/unresolved-priority exactness flags. Live checkpoint staging
  now checks first-battle quick values, `FUN_8006ee54` fixed-priority helper
  results, metadata-derived `jitter_modulus`, priority math, queued entries,
  and final execution-order entries.
- Post-turn-order aggregate buckets now separate:
  - observed draw floor;
  - static expected observed bucket, adding expected mode-`0xe` camera draws;
  - static expected-with-crit bucket, adding crit-gated draws;
  - candidate simple-roll ceiling, adding counter candidates only.
- Counter candidates are reported per nonlethal observed attack with
  first-battle target-side base counter chance: Soldier `10%`, Vyse `15%`,
  Aika `6%`.
- `SavorPredict/CounterModel.*` now includes `simulate_counter_check`, modeling the
  static `shouldCounter_800819d0` gate order from live inputs: status
  suppression, same-side suppression, critical suppression, forced counters,
  normal one-draw chance roll, movement/action-marker suppression, and current
  counter chance reset/update. Current counter chance is stateful combatant
  data: it increments during `zzDealDamage -> zzIncreaseCounterChance` damage
  application when target `field5_0x8` is neither `0` nor `4`, resets after
  successful normal counters, and is maintained across battles for PCs.
  `shouldCounter` reads current chance at `BattleInstance + 0xb4` and the
  counter-enabled/base gate at `+0xba`; `zzIncreaseCounterChance` increments
  current chance by the active instance field at `+0x8`. Predictor inputs
  should seed current chance from the byte-faithful
  `CombatantInstance.current_counter_chance` already present in
  `BattleContext`, not from base chance alone. The `+0x8` increment source is
  closed and implemented for first-battle v1; remaining counter work is live
  gate/follow-up field coverage.
- Static counter-follow-up analysis now shows the nested successful-counter
  path uses `setupTurnAction_80082134`'s basic self/counter branch
  (`FUN_80081de0`), not a fresh ordinary `performAttack_80081b94` burst.
  `FUN_80081de0` forces hit result `1` through constant helper `FUN_80010b5c`,
  calls shared `calculateDamage_80010a40`, and does not call
  `getAttackResult_80010b64` or `shouldCounter_800819d0`. First-battle counter
  follow-up damage should therefore spend the normal damage draws but no hit,
  crit, or recursive counter draw.
- Crit gates are reported per observed damage event. First-battle PC basic
  attacks are expected `instrParam_0x6 == 0`; Soldier attacks use the modeled
  AI attack-parameter result. Soldier `instrParam_0x6 == 1` attacks are counted
  as crit skips, not crit candidates.
- A reusable basic-attack burst simulator exists in
  `SavorPredict/AttackResolutionModel.*`.
  It models hit, crit, damage spread, low-bit bonus, elemental scaling,
  guard/status damage changes, and draw counts from supplied live runtime stats.
- First-battle Soldier drop draws are estimated from death/drop progress events
  using the entry-id-0 drop table.
- First-battle Soldier drop outcome simulation now exists in
  `SavorPredict/DropModel.*`: Electri Box consumes one successful row-1 draw,
  Moonberry consumes one failed row-1 draw plus one successful row-2 draw, and
  no drop consumes two failed enabled rows. The static contract records the
  disassembly distinction between `zzDealDamage` calling
  `HandleCombatantDeath_8002bc4c` and the death handler actually entering
  enemy drop logic only when `curHp < 1`.
- The initial `trace-checkpoints` importer exists for ordered debugger/emulator
  checkpoint streams. It validates required fields, checks monotonic draw
  indices, and maps known RNG PCs to staged owner labels for live validation.
- Live checkpoint capture plumbing now exists in SAVOR Core and SavorPredict:
  battle-turn payloads can carry `core.capture.profile_path` and
  `core.capture.output_path`; the VM can arm profile PCs alongside existing
  breakpoints; JSONL records include `capture_sequence`, per-checkpoint hit
  count, movie/VI/frame counters, `tbr_high`, `tbr_low`, `tbr_u64_hex`, and
  requested absolute memory, GPR, and register-relative memory samples; and
  SavorPredict can generate a first-battle capture profile from staged known
  RNG callsite owners plus action-view state checkpoints. `trace-checkpoints`
  can parse those JSONL rows as well as the older key-value smoke streams.
- `SavorPredict run-battle-job` can now run a single first-battle job from
  `D:/SavorPredictDB` through a disposable minimal sandbox, inject live
  checkpoint capture, preserve ordered JSONL artifacts, and feed the result
  through `trace-checkpoints`. Capture-active VM waits now force a 1 ms
  run-to-breakpoint poll when the script leaves `RUN_POLL_MS=0`; this fixes
  dense same-frame checkpoint bursts that previously consumed the 120 s VM
  deadline at the dynamic 500 ms poll tier.
- The pre-AI camera/fake-attack cursor contract is now explicit in
  `SavorPredict/PreAiCameraModel.*`: v1 assumes every fake attack consumes one
  RNG draw and that camera draws before enemy AI consume `1 + pc_count`, so the
  predictor uses `fake_attacks + 1 + pc_count`. The older `fake_attacks + 2`
  aggregate shape is retained only as historical evidence of target-camera
  suppression/interference in earlier macros.
- Pre-AI fake/camera live validation is now staged in
  `SavorPredict/PreAiCheckpointModel.*`. `trace-checkpoints` can compare
  `8001413c` battle-start camera draws, fake-attack RNG draws, `800608dc`
  targeting-camera draws, and the first `8008b428` Soldier AI draw against
  `--expected-fake-attacks`; it also records fake-attack attempts that skip
  RNG after a target-camera A-to-B frame gap. Gap handling is observational:
  `trace-checkpoints` reports min/max frame gaps for both rand-consuming fake
  attempts and no-rand fake attempts, and now summarizes consecutive fake
  attempt transitions (`draw->skip`, `skip->draw`, `draw->draw`,
  `skip->skip`) with previous-attempt gap ranges rather than using the
  7-frame minimum tested value as a threshold. The normal targeting-camera
  checkpoint `800608dc` is intentionally excluded from the default live profile
  for now because capturing it during target selection can perturb the macro
  path; keep it in explicit/narrow fake-camera profiles until that stepping
  interaction is isolated.
- Planned Soldier attack execution is now modeled from progress-event order in
  `SavorPredict/SoldierActionExecutionModel.*`. It separates planned Soldier
  attacks that reached execution from attacks prevented by the Soldier dying
  before its turn, giving a stronger first-battle estimate for which
  `HandleECInst:8008bc68` setup rolls are actually reachable.
- Action setup validation is now staged in
  `SavorPredict/ActionSetupCheckpointModel.*`. `trace-checkpoints` validates
  setupAction routing to PC/enemy handlers, expected enemy setup draw counts,
  setupAction-to-handler preservation of instruction, target, and
  `instrParam_0x6` fields, enemy helper fields that determine final
  `instrParam_0x6`, and selected worker path agreement between
  `instrParam_0x6 == 0 -> FUN_80087f6c` and fallback `FUN_80087844`.
- First-battle action-view camera expectations are staged in
  `SavorPredict/ActionViewCameraModel.*`, but the aggregate expectation is now
  explicitly split between serialized mode-0 rewrite gates and conditional
  runtime mode-`0xe` camera draws. The older rule counted one mode-`0xe`
  camera draw at `80052bf0` per observed attack and rejected the mode-0 path at
  `800513d4`. Current SpiceStd `%s0_STD` evidence refines this: PC resources
  `ma0000` and `ma0010` contain serialized `0x0003002a` records for keys `4`
  and `8` with mode `0`, while Soldier resource `mb0000` currently lacks
  serialized key `4`/`8` records. A selected mode-0 record first spends the
  `UpdateActionViewRecord_80051264:800513d4` rewrite-gate draw; only the
  even/override-allowed branch rewrites the payload to runtime mode `0xe` and
  reaches `FUN_80052b24:80052bf0`.
- Action-view gate validation is now staged in
  `SavorPredict/ActionViewGateCheckpointModel.*`. `trace-checkpoints` can
  summarize `FUN_80012f58`/`FUN_80009030` checkpoints that expose the aux-list
  root, query args `(4, -1, 0x2a, 3)`, query result, selected serialized
  record mode, and ordering before camera/hit draws. It also stages
  scheduler-chain fields for the selected action-view child thread, child
  payload, nested payload, child thread state byte, observed `800513d4`
  rewrite-gate draws, and sequence-id correlated ordering proof that
  `FUN_80012f58` precedes the camera and shared attack-hit draw for the same
  action. The generated first-battle capture profile keeps
  `FUN_80052b24:80052bc4` mode-`0xe` start-gate rows, with register-relative
  samples for the selected `0x0003002a` payload fields, worksheet saved/effective
  modes, instruction flags, and camera override globals. The
  `UpdateActionViewRecord_80051264:80051424` dispatch-state checkpoint remains
  useful in narrow action-view profiles, but it is intentionally omitted from
  the default first-battle profile because it fires every update tick and can
  overwhelm broad RNG capture runs.
  Live run `action_view_gate_20260622_0100` now corrects the older
  zero-gate assumption for the observed Vyse path: the category-2 query fired
  at `8001331c` with active slot `0`, target slot `4`, `field6_0x6=4`,
  aux root `0x80F40080`, and args `(4, -1, 0x2a, 3)`; `80013320` returned
  `1`; no `80013334` synthetic `FUN_80053f38(slot, 0)` spawn followed. The
  same action spent the mode-0 fallback draw at `800513d4` and still produced
  the key-4 `16 + 6` 8004 effect burst. Treat action-view camera ownership as
  gate/record-driven and separate from the 8004 combat-effect burst trigger.
- The 8004 combat-effect RNG family is now staged from the generated
  `Battle1_007.xlsx` `RNG Calls` tab plus static Ghidra callsite evidence in
  `SavorPredict/planning/static_support/8004_effect_rng_contract.txt`. The
  workbook's first-battle `FUN_80042b10` path should now be read as landed
  attacks flattened into 22 apparent iterations, not as single 22-loop effect
  buffers. Live checkpoints with `r29_effect_buffer` prove each landed
  first-battle basic attack is two selected buffers. Keys `4` and `5` use
  16 binary/variant loops followed by 6 binary/variant loops, for 110 draws;
  the observed successful-crit key `8` path uses 16 followed by 4, for
  100 draws. The `effect_8004_validation_capture_poll_worker_20260621_2138`
  run completed with
  353 RNG draw events, including three complete `16 + 6` `FUN_80042b10`
  landed-attack buffer pairs and zero seed-transition mismatches before the
  current first-battle predicate stopped the job. Static staging now also traces
  selected effect records through `FUN_8003ba08` and predicate `FUN_8003dcf4`;
  `80042EB8` is an allocation/fill call inside the loop body, not the loop
  head. Sibling helpers `FUN_80041e64` and `FUN_800422d0` have staged static
  owner labels but remain live-validation buckets until their first-battle
  execution/tick counts are proven. `SavorPredict/EffectRngModel.*` now encodes
  the static draw formulas and first-battle source-key-specific buffer
  sequences, and
  `SavorPredict/EffectCheckpointModel.*` summarizes captured 8004-family rows
  by `r29_effect_buffer` so the model can count complete `16 + 6` and
  `16 + 4` pairs.
- The `effect_record_bridge_20260622_0205` live run closes the selected-record
  copy edge for the observed key-4/key-5 path. Six
  `FUN_8003ba08:8003BB24` copy-complete rows matched six later
  `FUN_80042b10` buffers by pointer; each live copied source key and loop count
  matched the serialized `r31_source_record` fields. The observed attack draw
  clusters were:
  - attack 0: hit/damage/counter draws `8..11`, then copied key-5 buffers at
    draw `12` and `92`;
  - attack 1: hit/crit/damage/counter plus mode-0 camera draw `122..127`, then
    copied key-4 buffers at draw `128` and `208`;
  - attack 2: hit/damage/counter draws `239..242`, then copied key-5 buffers
    at draw `243` and `323`.
  This makes the 110-draw rule solid for selected source keys `4` and `5`, but
  actor/action-to-source-key linkage still needs a shared sequence id or
  equivalent bridge before it becomes a universal actor rule.
- Resource-level 8004 staging now has a narrower static map in
  `SavorPredict/planning/static_support/8004_effect_record_selection_static_map.txt`.
  `FUN_8003ba08` copies serialized SPARC source records to live
  `FUN_80042b10` buffers starting at live `r29 + 0x28`; therefore the live
  loop count at `r29 + 0x5c` is serialized `source + 0x34`. Decoded
  `bchara/damage.std` rows for source keys `4` and `5` are adjacent `16 + 6`
  SPARC records, while source key `8` is `16 + 4` at the main loop
  field. The `8006d954`/`8006d8dc` calls around `FUN_80042b10` are resource-id
  retain/release helpers, not second-buffer creation. The
  `effect_8004_source_fields_20260622_0001` live run validates copied
  source-identifying fields for three landed attacks: source-key pairs
  `5`, `4`, and `5`, each with `16` then `6` loops. This validates keys `4`
  and `5` as first-battle 110-draw sources. Draw order provisionally maps
  those pairs to Aika, Vyse, and Soldier respectively, but that is still weaker
  than a shared live sequence id. The `successful_crit_bridge_20260622_0335`
  run validates source key `8` for one unguarded Vyse successful crit: its
  copied buffers are `16` then `4` loops and spend 100 effect draws. The
  actor/action-to-source-key linkage and broader crit variants remain open
  before generalizing the rule to every first-battle basic attack.
- The staged 8004 trigger matrix lives at
  `SavorPredict/planning/static_support/8004_effect_trigger_matrix.txt`.
  It records the current rule boundary: landed supported records spend the full
  burst, an `80010BDC` hit-result call alone is insufficient, guard/no-action
  spends no burst, lethal hit does not suppress the observed supported burst,
  successful Vyse crit selects key `8` with a 100-draw burst, and broader
  crit/counter-follow-up source-row selection remains open.
- The representative five-job capture batch in
  `Analyses/battle_runs_first_battle/live_capture_runs/rep5_20260622` confirms
  those first-battle draw-count rules across the default spread: both-Soldier
  guard, one guard/one attack, both attack, and one-turn victory. Keys `4` and
  `5` again spend 110 draws as `16 + 6`; key `8` again spends 100 draws as
  `16 + 4`. The only effect-count mismatch is source exec job `173344`, where
  a consecutive duplicate `800430FC` variant-index checkpoint repeats the same
  seed, loop index, and effect buffer. Treat that as a capture/re-hit artifact
  unless future evidence proves a real same-seed draw; deduping it restores the
  expected two `16 + 6` landed-hit pairs.
- The `counter_guard_bridge_20260622_0218` live run adds one counter-follow-up
  example. Progress shows `[4]Soldier counter attacks...`; the trace has three
  complete `16 + 6` effect pairs despite only two normal PC damage lines. The
  third inferred counter-follow-up pair selects source key `4` and matches
  serialized source fields through `FUN_8003ba08:8003BB24`. This proves a
  first-battle Soldier counter-follow-up can spend the same supported 110-draw
  key-4 burst. The later `counter_followup_narrow` run directly validates the
  successful counter identity handoff for one Soldier case:
  `performAttack_80081b94:80081d84` calls `setupTurnAction_80082134` with the
  countering slot in `r3`. Counter hit/miss variants and unobserved counter
  source-key selections remain open.
- The `lethal_drop_bridge_20260622_0225` live run adds one death/drop ordering
  example. Progress kills `[5]Soldier` and drops nothing. The lethal attack
  spends hit/crit/damage at draws `122..125`, camera at `126..127`, and two
  failed Soldier drop rolls at `128..129`; the key-4 `16 + 6` effect pair then
  starts at draw `130`. Death/drop therefore does not suppress the supported
  8004 burst, but drop RNG can interleave before the lethal attack's visual
  effect pair.
- The `pc_counter_bridge_20260622_0300` live run adds one PC counter-follow-up
  example and one live miss/no-burst example. Progress has Aika hitting
  `[5]Soldier`, `[5]Soldier` hitting Aika, `Aika counter attacks...`,
  `[5]Soldier died...`, a no-drop result, then Vyse hitting `[4]Soldier`.
  The trace has four complete `16 + 6` effect pairs: Aika normal hit key `5`,
  `[5]Soldier` normal hit key `5`, inferred Aika counter follow-up key `5`,
  and Vyse normal hit key `4`. The counter follow-up damage draws at
  `130..131` are followed by two drop rolls at `242..243`, then the counter
  follow-up key-5 effect pair starts at `244`. The final actor `4` -> target
  `0` attack reaches `80010BDC` at draw `472` and a mode-`0xe` camera draw at
  `473`, but has no effect-record copy rows and no `FUN_80042b10` burst.
- Live action-source `field6_0x6` validation is now staged in
  `SavorPredict/ActionSourceCheckpointModel.*`. `trace-checkpoints` can
  summarize `FUN_8006782c` source-selection checkpoints and `FUN_8006721c`
  field6-bridge checkpoints. It validates source selection from
  `DAT_80346bd8+0x90` against bridge source slot, source `field6_0x6` against
  actor `field6_0x6`, selected handler PC, and the
  `InstructionWorksheet+0xe0` callback pointer. The first-battle expectation
  currently checks the static handler/callback `800662bc` while preserving live
  field values for review. When live rows include `action_sequence_id`, source
  selection is paired to the field6 bridge by sequence id before falling back
  to trace order.
- Shared attack-result and damage checkpoint expectations are now staged in
  `SavorPredict/AttackResolutionCheckpointModel.*`. Aggregate traces publish
  `--expected-attack-events` and `--expected-crit-draws` arguments, while
  `trace-checkpoints` validates hit, crit, damage-spread, and low-bit bonus
  draw counts plus spread-before-bonus ordering.
- Live attack damage value validation is now staged in
  `SavorPredict/AttackDamageValueCheckpointModel.*`. `trace-checkpoints` can
  group hit/crit/spread/bonus draw bursts, read live attacker and target stats,
  derive draw values from explicit fields or RNG seeds, and compare observed
  attack result and damage against the shared basic-attack formula. It now
  also ties formula damage to `zzDealDamage` application fields, checking
  applied damage, HP after clamping, lethal flag, and ordering after the damage
  rolls.
- The all-job progress comparison also records first-battle observed
  PC-to-Soldier damage bands by actor and planned guard state. These bands
  classify all `405294` supported turn-one PC-to-Soldier damage events with
  zero mismatches: normal `336857`, crit `23914`, guard-normal `41632`, and
  guard-crit `2891`. This is progress classification only; exact prediction
  still requires the static damage formula plus live stats/draw fields.
- Live crit-gate checkpoint validation is now staged in
  `SavorPredict/CritGateCheckpointModel.*`. `trace-checkpoints` can use live
  `instr_param_0x6` plus `hit_success`/`hit_result`/`attack_result` fields on
  `80010bdc` hit checkpoints to determine whether observed `80010c44` crit
  draws match the actual gate.
- Turn-order priority-jitter checkpoint expectations are now staged in
  `SavorPredict/TurnOrderCheckpointModel.*`. Aggregate traces publish
  `--expected-turn-order-draws`, while `trace-checkpoints` validates the
  observed `800711f8` draw count, live first-battle quick values, captured
  `FUN_8006ee54` fixed-priority results, queue metadata-derived
  `jitter_modulus`, and `quick + rand % jitter_modulus` priority math. It now
  also validates qsort call metadata, sorted action-queue records, and final
  `s8_ARRAY_803092f4` execution order. The 14-capture replay closes
  first-battle qsort tie behavior: `SoaQSortModel` is byte-record based,
  comparator-injected, sorts only the qsort `count` prefix, and leaves captured
  sentinel/tail rows untouched.
- Static comparator ownership is now identified: `setupTurn_80070c18` calls
  `qsort` with comparator `80010ce4`, whose disassembly subtracts
  `lhs->field4_0x4 - rhs->field4_0x4`. The sorted array is then copied in
  reverse into `s8_ARRAY_803092f4`, so larger assigned priority executes
  earlier. The exported `MSL::qsort::qsort` body at `8025eb4c` matches the
  external `SOA_qSort` heap-style clone: equal keys are deterministic but
  nonstable. The current model implements that permutation over raw records
  with dependency-injected comparators, so other qsort callsites can provide
  their own comparator and element decoder.
- Aggregate progress comparisons are now evidence against using raw progress
  attack order as the exact turn-order oracle. With `fake_attacks >= 1`, only
  `21530 / 39301` fully observed attack orders match the current turn-order
  model, and even no-priority-tie rows match only `14938 / 26938`. The
  workflow PC-before-EC predicate fits better when interpreted as both PCs
  before the first enemy (`149312 / 177276`), but it is still a workflow
  predicate rather than a game queue checkpoint.
- Counter-roll checkpoint expectations are now staged in
  `SavorPredict/CounterCheckpointModel.*`. Aggregate traces publish
  `--expected-counter-roll-ceiling` based on nonlethal observed attacks, while
  `trace-checkpoints` validates that observed `80081a88` rolls stay within
  that ceiling. When live gate/result fields are present, it now runs the
  static counter simulator against the captured rand value and validates the
  exact counter result, queued field, and counter-chance update. The same
  model now also stages non-RNG `shouldCounter_800819d0` gate-attempt
  checkpoints, proving no-draw suppression cases when live inputs complete
  before the roll, and explicit counter follow-up checkpoints through
  `setupTurnAction_80082134`. The narrow
  `counter_followup_narrow/run_148016_20260623_narrow` capture proves one
  successful first-battle Soldier counter follow-up: the second Soldier
  counter candidate rolled `rand15 % 100 = 4` against current/base chance
  `10 / 10`, reset current counter chance to `0`, returned true, re-entered
  `setupTurnAction_80082134` with the countering slot in `r3`, and then wrote
  counter bookkeeping fields. This upgrades successful counter from
  progress-only annotation to modeled nested follow-up action.
- Aggregate progress parsing now recognizes explicit `counter attacks...`
  lines, infers the counter target from the previous attack when possible, and
  separates lethal counter events from lethal normal attack events. This keeps
  progress-only summaries from attributing a post-counter death to the
  preceding incoming attack.
- Across the current turn-one job set, progress parsing finds `6288` observed
  counters and `1139` lethal counters. One counter row remains unmatched to an
  inferred target/candidate because the progress log only says
  `Aika counter attacks...` after the attacked Soldier has already died; it
  needs a live counter follow-up checkpoint rather than aggregate text
  inference.
- Enemy-drop checkpoint expectations are now staged in
  `SavorPredict/DropCheckpointModel.*`. Aggregate traces publish
  `--expected-drop-rolls` when first-battle Soldier drop draws can be inferred,
  while `trace-checkpoints` validates observed `8002bad8` roll counts,
  first-battle entry-id-0 row identity, item/amount, one-percent success
  outcome, stop-after-success behavior, and ordering context relative to
  damage bonus, counter, action-view camera, and status-attempt draws.
- Death/drop control-flow validation is now staged in
  `SavorPredict/DeathDropCheckpointModel.*`. `trace-checkpoints` can validate
  live `zzDealDamage -> HandleCombatantDeath -> enemyDropItem -> drop row`
  ordering, distinguish lethal from nonlethal damage by HP fields, and reject
  nonlethal drop entry or drop rolls.
- Outcome-branch checkpoint expectations are now staged in
  `SavorPredict/OutcomeCheckpointModel.*`. Aggregate traces publish
  `--expected-end-turn-status-draws 0 --expected-level-up-stat-rolls 0` for
  first-battle `ReachedNextTurn` jobs, while `trace-checkpoints` validates
  observed `8006ff38` end-turn status cleanup and `801f2xxx` level-up stat
  roll counts. The same model now also stages non-RNG branch validation for
  `runCase9_8006f020`, `endBattleSuccess_8006f4b0`, and `LevelUp_801f2a34`
  entry/context checkpoints, including EXP/threshold fields needed to make
  victory level-up draw expectations exact later.

## Main Remaining Gaps

1. Pre-AI fake/camera mechanism.
   The aggregate cursor model and live checkpoint summary are now staged in
   code, but the live mechanism still needs callsite/state proof around the
   expected `800608DC` suppression/replacement. Also preserve the new fake
   attack timing clue: after a fake attack, a short gap between switching to
   the target camera with A and returning with B could make the next fake
   attack attempt skip its rand call; 7 frames is only the minimum tested gap
   that sometimes worked, so the consistent threshold may be higher. Current
   CLI summaries record observed gap ranges and draw/skip transition ranges;
   the threshold and underlying camera-state condition remain open
   live-capture questions. The `gap_refresh_20260623` campaign confirms the
   capture constraint: the explicit `800608DC` batch failed for fake counts
   `0..2`, while the matching no-`800608DC` control batch succeeded. The v1
   formula `fake_attacks + 1 + pc_count` is therefore a required macro
   contract until a non-perturbing target-camera probe exists.

2. Live action/source combatant action mode.
   Terminology update, 2026-06-24: `combatant action mode` means
   `Battle_CombatantInstructionWorksheet+0x6`, previously called
   `field6_0x6` or the action-view/animation field. `combatant command
   parameter` means queued-instruction `instrParam_0x6` /
   `QUEUED_INSTRUCTIONS[slot]+0x6`, which describes attack flavor or selected
   item/spell/S-Move data. For first-battle basic attacks, this is now
   validated as the visual ranged/melee discriminator: `0` means
   direct/melee, and nonzero means fallback/ranged. This rule is per action,
   not per combatant identity. These are separate fields and should not be
   conflated.
   Static resource extraction says first-battle `ma000`, `MA001`, and `MB000`
   have action ids `4` and `8` that install `FUN_800662bc`.
   `ActionSourceCheckpointModel.*` now gives live traces a validation target
   for source selection at `FUN_8006782c`, bridge fields at `FUN_8006721c`,
   selected handler, instruction callback, and sequence-correlated
   source-selection/bridge pairing. The static key-8 pass narrows the
   successful-crit source: attack result `2` writes target reaction state `6`
   through `FUN_80081168`, while `Battle::PollCombatantInstructionState_8007ffe8`
   still accepts actor combatant action mode `4` or `8` for that reaction state. No nearby
   immediate `r4=8`/`r5=8` setter was found in the earlier static pass, but a
   2026-06-24 manual Dolphin write-watch trace on a value-8 run showed
   `80022798` writing the live instruction worksheet field from `6 -> 8`.
   The same trace observed `0xc` combatant action mode transitions during Aika's ranged
   attack turn while Vyse visually blocked and a Soldier visually attacked
   him; leave `0xc` ambiguous for now as a possible block, reactive visual,
   or non-primary attack animation state rather than treating it as a proven
   Soldier attack state. Current hypothesis after the 2026-06-24 fixed-address
   write-watch runs: `Battle_CombatantInstructionWorksheet+0x6` is a broad
   per-combatant visual/action animation state, not a queued command
   parameter. Working values are `0x02` standing/normal, `0x04` normal melee
   attack, `0x05` ranged attack, `0x06` running, `0x08` performing crit
   attack, `0x0b` taking damage, `0x0d` dodge, `0x0e` death animation,
   `0x13` backing away/guarding, and `0x20` taking crit. Earlier `0x0c`
   observations should remain tentative; current live evidence favors `0x13`
   for the visible back-away guard motion and `0x0d` for dodge. Guarding
   targets can still receive `0x0b` when hit, so the actual animation variant
   likely depends on adjacent instruction worksheet selectors rather than
   combatant action mode alone. The first selector candidates to validate are
   `InstructionWorksheet+0x8` (subtype), `+0xe4` (selected row index), `+0xe0`
   (callback), and `+0x1c` (previous combatant action mode). The live
   ownership chain for this field is:
   `PTR_ARRAY_80309e24[packed_index] -> Thread_BattleCombatant -> +0x24
   Battle_CombatantWorksheet -> +0x4c Battle_CombatantInstructionWorksheet
   -> +0x6 combatant action mode`. `PTR_ARRAY_80309e24` is a packed live thread array, not a
   battle-slot-indexed array; first-battle slots `0`, `1`, `4`, and `5`
   occupy packed entries `0..3`, and the actual battle slot must be read from
   `Battle_CombatantInstructionWorksheet+0x00`. Do not use
   `CombatantMovementBuffers_80309700` as the owner of this instruction
   worksheet combatant-action-mode path; that array belongs to movement scheduling/pathing.
   The current writer inventory and value-flow graph are staged in
   `SavorPredict/planning/static_support/combatant_action_mode_writer_inventory.md`
   and
   `SavorPredict/planning/static_support/combatant_action_mode_writer_value_graph.md`.
   The current per-store assessment matrix is
   `SavorPredict/planning/static_support/combatant_action_mode_writer_assessment_matrix.md`.
   The current callsite/value-origin matrix is
   `SavorPredict/planning/static_support/combatant_action_mode_callsite_value_origins.md`;
   it classifies every confirmed direct writer as lifecycle, persistent,
   temporary, or restore and records the immediate mode/subtype source.
   The movement/callback relationship is now staged in
   `SavorPredict/planning/static_support/combatant_action_mode_movement_callback_graph.md`:
   movement worksheet `+0x10` schedules handlers, while instruction worksheet
   `+0xe0` stores the selected row callback and `+0x6` stores the combatant
   action mode. The confirmed callback table entries `802DBF54 ->
   FUN_80022850` and `802DBF7C -> FUN_80021fcc` write lifecycle mode `1`,
   resolve persistent mode transitions, and call the installed row callback.
   The earlier SST/STD store-complete checkpoints at `8000c4c8` and
   `8000c6e8` remain useful negative controls, but they are no longer the
   preferred first explanation for the observed combatant action mode `8`.
   The current `FUN_800221fc` refinement proves one staged-field producer:
   `80022550` snapshots current combatant action mode `IW+0x6` into
   `IW+0xa` when the flag tested at `80022544` is set. The current
   `FUN_800208f4` refinement proves one saved-field producer family: when its
   selected `0x18`/`0x19`/`0x1d` remap paths act, it sets the saved-mode flag
   in `IW+0xec`, writes restorable mode/subtype to `IW+0x14/+0x16`, and can
   rewrite the selected mode/subtype by pointer before the later mode commit.
   The forced-mode field `IW+0x18` now has one concrete producer from the
   current Ghidra disassembly: `FUN_8004281c` reads service record `+0x26`,
   writes it to a target combatant instruction worksheet at `800429C0`, and
   sets that target's `IW+0xec |= 0x40000000` at `800429C4..800429CC`.
   The 2026-06-24 pointer-shape provenance export and manual disassembly
   review expands the confirmed direct combatant action mode writer set to 21
   PCs. Two newly promoted direct stores are:
   `FUN_80030b34:80030C08`, which copies combatant worksheet `+0xa` into
   instruction worksheet action mode after thread-state/readiness gates and
   copies combatant worksheet `+0xc` into subtype; and
   `FUN_800405ec:800409D0`, which increments the current action mode in place
   inside a thread-state/progression helper. Both are closed as direct writer
   sites. The immediate producer for `FUN_80030b34` is now closed:
   `FUN_80031254` calls `FUN_80020b50` to snapshot the parent saved/current
   combatant action mode and subtype, creates a child `Thread_BattleCombatant`
   whose callback is `FUN_80030b34`, and writes the snapshot into child
   combatant worksheet `+0xa/+0xc` for delayed replay. That makes `80030C08`
   a propagation edge, not a fresh selector. It remains open for first-battle
   reachability and for the deeper parent selector root. `800409D0` remains
   open for first-battle reachability and progression-state predicate
   modeling. The same export also reinforces why `SST::Command::Dispatch`
   stores at `8000C4C4`/`8000C6E4` and `FUN_8007dee0:8007DFA0` are negative
   controls: they use a similar `+0x24/+0x4c/+0x6` payload shape, but
   disassembly shows child payload/subpayload copies rather than the
   `Thread_BattleCombatant -> Battle_CombatantWorksheet -> InstructionWorksheet`
   ownership chain.
   The helper pass adds a disassembly-backed exact jump-table contract for
   `MapQueuedStateToStdActionId_800217d0`: queued state `4` maps to action mode
   `0x06`, state `6` maps to crit action mode `0x08`, and state `15` maps to
   `0x13`. The remaining gap is to prove when first-battle action handling
   stages those queued state values and whether the downstream gates
   `FUN_80020470`, row lookup, `FUN_800208f4`, and `FUN_8001bac0` admit the
   commit to `80022798`. `FUN_80020470` and `FUN_8001bac0` now have
   disassembly-backed first-pass predicate shapes in
   `combatant_action_mode_selector_helper_contracts.md`, but their row/global
   field semantics still need names and first-battle constants. Field producer
   work remains for forced-mode subtype field `IW+0x1a`, any additional
   `IW+0x18` producers beyond `FUN_8004281c`, and any additional producers of
   `IW+0xa/+0x14/+0x16` not covered above. If `FUN_80030b34` proves live in
   first-battle runs, model its combatant worksheet `+0x8` readiness counter
   and parent snapshot replay, then continue upstream from the parent
   mode/subtype. Additional producer work is still explicit for the
   thread-state/progression predicates that admit `FUN_800405ec:800409D0` if
   that increment path appears in first-battle captures. The
   `gap_refresh_20260623/runs/action_source_action_view`
   batch did not close this: it captured useful action-view gate/camera counts
   but reported zero source-selection and action-source events, so the
   source-key bridge remains an explicit live-profile target. The next
   instrument should use the current nomenclature: watch the known combatant
   command parameter fields separately from the known combatant action mode
   fields, then classify each observed access from the decoded instruction as
   read or write. This should tell us whether a producer was missed, whether
   the consumer is reading a different worksheet than expected, or whether the
   source key is already serialized before the currently staged anchors.
   2026-06-25 live campaign:
   `Analyses/battle_runs_first_battle/action_mode_writer_encounter_20260625/`
   used a no-progress-PC profile with all 21 confirmed action-mode writer PCs
   plus fixed write watchpoints for the four first-battle
   `Battle_CombatantInstructionWorksheet+0x6` fields after
   `setupTurnAction_80082134`. The long-timeout retry produced raw captures for
   all 13 selected jobs. Twelve cloned jobs reached `SUCCEEDED`; source job
   `677349` / clone `880453` stayed in a noisy watchpoint loop and was stopped
   after preserving its raw capture. The aggregate found 8/21 writer PCs in the
   selected first-battle spread: `800086F4`, `800087E0`, `8001C0D8`,
   `800202FC`, `80020CF4`, `80020D2C`, `80022798`, and `800229E8`.
   Fixed-watch decoded stores all attributed to audited PCs; no extra decoded
   writer PC was found. Durable live transitions were dominated by
   `80022798`, while temporary probe/restore helpers also touched the watched
   fields, often as no-op old->new writes. Confirmed written values in this
   spread were `0x02`, `0x04`, `0x05`, `0x06`, `0x09`, `0x0b`, `0x0c`,
   `0x0d`, `0x0e`, and `0x13`. The spread did not prove a fixed-watch
   `0x08` write, despite including the crit-candidate job; keep the manual
   `6 -> 8` observation as a lead, but mark first-battle `0x08` reachability
   under this capture window as still open. The fixed-watch rows are the
   authoritative transition evidence because they include decoded store proof,
   old/new values, slot, TBR fields, and capture sequence; writer-PC rows are
   encounter/call-path evidence unless paired with a fixed-watch store.

3. Action-view scheduler and camera draw index.
   Static action-view expectations, checkpoint counting, live gate-field
   validation, scheduler-chain field capture, mode-0 rewrite-gate capture, and
   dispatch effective-mode capture are staged in code. The representative
   five-job batch shows the count is conditional, not fixed per attack:
   `800513d4` appeared once in every selected job, while `80052bf0` appeared in
   only three of five jobs.
   SpiceStd `%s0_STD` evidence shows PC resources have serialized
   `0x0003002a` mode-0 records for first-battle keys `4` and `8`; Soldier
   resource `mb0000` currently lacks serialized key `4`/`8` records. The
   remaining static/live gap is therefore actor/action-key specific: prove the
   live aux-list root, `FUN_80009030(root, key, -1, 0x2a, 3)` result, selected
   record mode, child-thread/payload chain, child state byte, rewrite-gate
   result, effective dispatch mode, and sequence ids. When live rows include a
   shared action sequence id, `ActionViewGateCheckpointModel.*` should validate
   either the zero-gate synthetic mode-`0xe` camera path or the serialized
   mode-0 path, instead of assuming every observed attack reaches
   `FUN_80052b24:80052bf0`.

4. Exact side-specific action setup.
   Enemy attack setup now has a reusable static RNG-gate model for
   `HandleECInst:8008bc68`, and aggregate progress order now separates
   reached Soldier actions from death-prevented planned actions.
   `ActionSetupCheckpointModel.*` now stages direct checkpoint proof for the
   route, setup-to-handler instruction/target/`instrParam_0x6` preservation,
   enemy setup draw, helper fields, final `instrParam_0x6`, and selected
   worker path. The 2026-06-26 15-job no-progress capture under
   `Analyses/battle_runs_first_battle/attack_param_worker_spread_20260626`
   captured those fields across player and enemy turns. It observed 47/47
   attack-begin events matching worker selection by final `instrParam_0x6`:
   PC `0 -> FUN_80086308`, PC nonzero `-> FUN_80085ce0`, enemy
   `0 -> FUN_80087f6c`, and enemy nonzero `-> FUN_80087844`. Manual visual
   review of `visual_ranged_metric_candidates.csv` confirmed every nonzero
   row was a ranged attack and other attacks in those jobs were melee. Mark
   the first-battle basic-attack ranged/melee discriminator closed; broader
   non-basic command payloads remain deferred.

5. Exact shared attack-result and damage simulation.
   The helper exists, aggregate damage events prove hit plus two damage rolls,
   checkpoint-count/order validation is now staged for the shared attack
   burst, and live crit-gate validation can now verify `instrParam_0x6` plus
   hit success against observed crit draws. Per-burst damage-value validation
   is now staged from live stat/draw fields and can tie predicted damage to
   `zzDealDamage` HP/lethal fields. The remaining gap is capturing those
   fields from real first-battle runs and combining the resulting lethal state
   with the already staged death/drop flow checkpoints. For attack effects,
   current evidence proves ordinary landed first-battle basic attacks with
   source keys `4`/`5` select a `16 + 6` `FUN_80042b10` buffer pair, while
   misses and guards do not produce the full 8004 burst. One successful Vyse
   crit selects source key `8` and a `16 + 4` pair. One Soldier
   counter-follow-up key-4 case and one Aika counter-follow-up key-5 case now
   select the same supported pair shape as normal key-4/key-5 hits, but
   broader crit variants, counter misses, non-key-4/key-5 counter selections,
   and actor/action-specific source-key selection still need validation before
   the rule becomes a general counter rule. Direct successful counter identity
   is validated for one Soldier case through
   `setupTurnAction_80082134(r3=<countering slot>)`. The representative
   one-turn-victory
   job `880442` makes this concrete: it has four normal hit-gate draws but
   five damage-roll pairs, so a counter/follow-up damage path can spend damage
   rolls without a matching normal attack-begin row.
   DB-backed predictor smoke runs on `fake_attacks=0` jobs exposed another
   action-resolution gap: queued PC attacks can retarget in live game when the
   originally selected target dies before the actor executes. Exec job
   `158364` is the current representative case. The staged model has Aika
   crit-kill slot `4`, then skips Vyse's queued attack because its target is
   already dead, producing `ReachedNextTurn`; the DB result is `Victory`.
   Static tracing now identifies the likely first-battle PC repair point:
   `Battle::HandlePCInst:80086c68` calls `FUN_800855ac` before a normal PC
   attack executes. That helper treats missing, PC-side, or dead targets as
   invalid, calls `GetClosestCombatant_80083c50(self_slot,2)`, and writes the
   returned slot back to `QUEUED_INSTRUCTIONS[self_slot].target_0x4`. Enemy
   setup has an analogous PC-side retarget helper at `FUN_8008a174`. For job
   `158364`, live action-view state shows slot `0` first associated with target
   `4`, then later with target `5` before the final mode-0e camera, damage, and
   drop path. `GetClosestCombatant_80083c50` now has decoded semantics in
   `Analyses/battle_runs_first_battle/retargeting_20260623/retargeting_findings.md`:
   it builds a side-filtered candidate list (`group_mask & 1` for PC slots,
   `group_mask & 2` for enemy slots), filters non-null combatants with
   `(status_flags & 0x2100) == 0`, scores candidates by `field4_0x4` distance
   from current movement worksheet positions, sorts ascending with a comparator
   that only reads `field4_0x4`, then returns the first sorted candidate for
   which `FUN_80083728(actor,candidate)` proves presence/reachability. For
   first-battle cases with exactly one living enemy left, the retarget should be
   exact and not an RNG-consuming step; pathing is not needed for that case
   because the remaining Soldier is the only candidate. The
   `gap_refresh_20260623/runs/retargeting` live run confirms this for job
   `158364`: slot `0` enters the repair helper with queued target `4`, calls
   `GetClosestCombatant_80083c50(self=0, group_mask=2)`, receives slot `5`,
   and reaches the target-store checkpoint with no RNG draws in the retarget
   trace. The remaining retargeting proof gap is not the first-battle v1
   outcome; it is direct current-instruction attribution of the target-byte
   store, because the write watchpoint deltas in that run were unattributed.
   The external
   `SOA_qSort` clone lines up with the exported `8025eb4c` sort body, so
   equal-distance candidate ties should be modeled as deterministic but
   nonstable rather than ambiguous once the sort permutation is ported.
   Full movement worksheet/pathing data is a future generalization goal for
   multi-candidate retargets, likely starting with the second battle where four
   enemies can remain in the candidate set.
   First-battle and likely second-battle combatant starting grid positions
   should be sourced from the fixed encounter layout data in the ALX dataset,
   probably `enemyevent.csv`, rather than inferred from runtime movement
   traces. These ALX X/Z fields are grid coordinates only. The movement
   worksheet can carry both grid-position state and raw stage-unit position
   state, so keep those representations separate until the worksheet
   conversion/pathing contract is proven. The
   ALX 5.0.0 US GC final rows are now recorded in
   `static_support/scripted_battle_start_positions_alx_v5.txt`: entry `0` is
   the first battle with Vyse `(4,6)`, Aika `(6,6)`, Soldier slot `4` `(4,2)`,
   and Soldier slot `5` `(6,2)`; entry `1` is the second battle with Vyse
   `(4,8)`, Aika `(6,8)`, and four Guards at slots `4..7` with coordinates
   `(4,2)`, `(6,2)`, `(2,3)`, and `(8,3)`. Random encounters are a separate
   setup class: they may spend RNG to choose or jitter combatant starting
   positions before any fake-attack/input-camera draws, so the pre-AI draw
   model must eventually gain a pre-fake-attack encounter position phase for
   non-scripted battles.
   Operational rule: first-battle `predict-battle` comparisons should pass
   `--enemy-event-id 0` so `MovementModel` can use those scripted ALX grid
   positions. Without that option, combatant command parameter generation
   (`instr_param_0x6`) lacks first-battle path evidence and should be treated
   as ambiguous/default rather than as a validated mismatch.

6. Exact turn-order live values.
   `TurnOrderModel.*` now simulates the static rule from supplied quick and
   fixed-priority inputs, and `trace-job` uses the staged first-battle quick
   values (Vyse 22, Aika 24, Soldier 18) for the current aggregate estimate.
   `TurnOrderCheckpointModel.*` now stages the expected priority-jitter draw
   count and `trace-checkpoints` can validate observed `800711f8` count, live
   first-battle quick values, fixed-priority helper results, queue
   `sumQuick`/`numActions` metadata, `quick + rand % jitter_modulus` priority
   assignments, queued-entry fields, qsort output entries, qsort call
   metadata, and final execution order. The
   representative five-job batch validates the draw-count side of the model:
   two draws when both Soldiers defend, three draws when one Soldier attacks,
   and four draws when both Soldiers attack. The
   `gap_refresh_20260623/runs/turn_order_qsort` batch further confirms four
   priority-jitter draws in each selected attack-heavy first-battle job. The
   later short-root turn-order validation replay at `Analyses/tov14/run`
   captures queue records, qsort count/element/comparator metadata, sorted
   entries, and `s8_ARRAY_803092f4`. All 14 completed captures now match
   `SoaQSortModel` for both qsort output and final execution order, including
   previous mismatch cases `433890` and `433896`.

   First-battle turn order is therefore closed for the current normal-turn
   action-queue path. The qsort engine remains intentionally modular because
   other game qsort callsites may use different comparators or element layouts;
   those callsites need their own comparator injection and validation before
   being treated as covered.

7. Counter branch and follow-up actions.
   Static counter gate order and a reusable simulator are now staged.
   `CounterCheckpointModel.*` now validates observed `80081a88` rolls against
   the aggregate nonlethal-attack ceiling and, for roll checkpoints with live
   fields, validates target status flags, side, critical-result suppression,
   movement flags, current counter chance, exact counter result, queued field,
   and counter-chance update against the simulator. It also now validates live
   gate attempts at `shouldCounter_800819d0` that should suppress before RNG,
   gate attempts that should require a following `80081a88` roll, and expected
   counter follow-up actions through `setupTurnAction_80082134`. Progress-only
   traces now mark observed counter follow-ups from `counter attacks...` rows,
   but those rows do not carry damage values or draw indices. The remaining gap
   is no longer whether a successful counter re-enters action setup; the
   `counter_followup_narrow/run_148016_20260623_narrow` capture proves that it
   does. The `BattleInstance + 0x8` increment source is now closed and
   implemented for first-battle v1; the `gap_refresh_20260623/runs/counter2`
   traces preserve stateful current-counter values while avoiding the unsafe
   progress PC. The remaining gaps are:
   - capturing live actor/target/gate-reason fields for both draw and no-draw
     `shouldCounter_800819d0` attempts;
   - capturing enough queued/action bookkeeping to predict the follow-up
     damage path without relying on progress text;
   - checking exact ordering against damage, action-view camera, and death/drop
     events;
   - enforcing a no-progress-PC rule for all machine capture profiles. The
     progress hook for `counter attacks...` is
     `performAttack_80081b94:80081d84`; counter profiles should use adjacent
     internal PCs such as `80081d78`, `80081d80`, and `80081d88`, or memory
     watchpoints on queue/action fields. Progress PCs belong only to the
     progress-reporting channel, not checkpoint capture profiles.
   The all-observable progress pass found `5135 / 5136` fake-attack-filtered
   jobs with observed counters covered by a nonlethal counter candidate. The
   single exception has `[5]Soldier` die and drop before a later
   `Aika counter attacks...` line, so text-only inference cannot safely attach
   the counter; this remains a live follow-up checkpoint case.
   The `pc_counter_bridge_20260622_0300` capture targets that exception shape:
   it shows the Aika counter as an inferred key-5 `16 + 6` effect pair, with
   counter damage draws before the normal attack visual completes and
   death/drop before the counter visual burst. This resolves the burst-shape
   question for that example, but that older checkpoint profile still lacks
   live gate/result fields for the Aika case. The later no-progress counter
   strategy should be used for a direct PC-counter identity/source-key rerun.

8. Death/drop interleaving.
   Static death/drop ownership and first-battle drop outcome simulation are now
   staged. `DropCheckpointModel.*` now validates observed `8002bad8` drop-roll
   counts, first-battle row identity, item/amount, one-percent success
   outcome, stop-after-success behavior, and ordering context relative to
   damage bonus, counter, action-view camera, and status-attempt draws.
   `DeathDropCheckpointModel.*` now stages live control-flow proof through
   `zzDealDamage`, `HandleCombatantDeath_8002bc4c`, and
   `enemyDropItem_8002ba8c` with target HP/death fields. The
   `gap_refresh_20260623` live runs close the first-battle Soldier drop row and
   stop-after-success behavior: `baseline_broad` captured Electri Box row-1
   success, `drop2` captured row-1-fail/row-2 Moonberry success, and `drop2`
   also captured two failed rows producing no drop. The remaining gap is exact
   per-lethal-attack damage/HP field validation across all variants, not the
   Soldier drop table row order or stop-after-success rule.
   Aggregate progress comparison also exposed a smaller text-log pairing gap:
   with `fake_attacks >= 1`, parsed drop names all map to the first-battle
   Soldier table (`143299 / 143299`), but only `143266 / 143296` jobs with
   parsed death/drop evidence have one parsed drop event per parsed death
   event. Representative mismatches include a `dropped nothing` line without a
   parseable preceding `died...` line and duplicate death suppression. Treat
   these as progress-log ordering/parser limits until live death/drop
   checkpoints link each drop-row sequence to its lethal damage flow.
   The static death-handler verification corrected one model boundary:
   `8002bd20` is not the handler entry. It is the internal
   `HandleCombatantDeath_8002bc4c -> enemyDropItem_8002ba8c` callsite reached
   only after the handler HP gate accepts a dead enemy. The model now treats
   `8002bc80` as the true live handler gate checkpoint and treats `8002bd20`
   as an enemy-drop path checkpoint. Existing captures that named `8002bd20`
   as `death_handler` should be reinterpreted by PC as drop-path evidence.

9. Resource-level attack-effect record decoding.
   Static Ghidra evidence now traces selected combat-effect source records
   through `FUN_8003ba08` and predicate `FUN_8003dcf4`, and live copied-buffer
   fields prove the first-battle landed-hit key-4/key-5 sequence is 16-loop
   buffer then 6-loop buffer for the observed normal path. The decoded
   `bchara/damage.std` SPARC records now prove source keys `4` and `5` have
   adjacent `16 + 6` rows, while source key `8` has adjacent main loop counts
   `16 + 4`. The latest live source-field and bridge runs identify observed
   pairs as source keys `5`, `4`, and `5`; the PC-counter bridge further ties
   normal attack-begin rows to Aika key `5`, Soldier key `5`, and Vyse key `4`,
   and infers Aika's counter follow-up as key `5`. The successful-crit bridge
   proves key `8` reachability and behavior for one Vyse crit: `16 + 4`,
  100 draws. The selector-side key-8 path is now narrowed: for keys `4`, `5`,
  and `8`, `FUN_8003dcf4` reduces to matching serialized source key against
  actor combatant action mode, and `FUN_8006721c` should preserve
  key `8` if the active/source worksheet already carries it because the
  first-battle resources have key-8 rows. The upstream key-8 path is now
  likely data-driven through `SST::Command::Dispatch_8000c19c` rather than a
  direct immediate battle-side setter. SavorPredict now generates and
  summarizes the two candidate SST legacy field6 store-complete checkpoints. The
  remaining gaps are live proof of the serialized command-data path that makes
  the active/source worksheet combatant action mode become `8` for successful crits, broader
  successful-crit variants, and counter-follow-up source-key selection beyond
  the validated Soldier handoff identity before claiming a single first-battle
  rule applies beyond the selected source-key model.
  The representative five-job batch now makes the first-battle loop-shape
  portion high confidence for keys `4`, `5`, and `8`; the resource-level gap
  has shifted toward proving the upstream source-key assignment for crits,
  counters, and actor-specific action records rather than proving the draw
  cost once a supported source key is selected.

10. End-turn and battle-success branches.
   First-battle next-turn zero-draw validation is now staged for
   `endTurn:8006ff38` status cleanup, victory-branch absence, and level-up
   stat rolls. `OutcomeCheckpointModel.*` can now validate live
   `runCase9 -> endBattleSuccess -> LevelUp` branch checkpoints and report
   missing reward/EXP threshold context. The remaining gap is capturing real
   first-battle victory jobs with battle reward row/source, live PC EXP, level
   thresholds, level-up amount, and exact `LevelUp_801f2a34` draw indices.

11. Generalization beyond first battle.
   Later milestones need non-Soldier AI, active status effects, magic, items,
   S-Moves, multi-target actions, non-normal turn types, and data-driven drop
   tables beyond first-battle Soldier entry id `0`. Pathing/reachability should
   also be treated as a post-first-battle target; the second battle's four
   enemies are the first likely pressure test for exact multi-candidate
   retargeting.

12. Capture-enabled battle job selection.
   The SavorPredict-owned entry point now exists and has a validated smoke run:
   it hydrates a minimal sandbox from `D:/SavorPredictDB`, clones one
   first-battle turn job, injects the generated capture profile, runs
   `SavorWorker`, copies `capture.jsonl`, and writes `trace_checkpoints.txt`.
   Remaining operational gaps are broader job selection/sampling strategy,
   deciding which high-volume checkpoints belong in default versus narrow
   research profiles, and teaching `trace-checkpoints` enough live fields to
   turn the current observed-only sections into strict validations.

13. First-battle action-motion speed source.
   The frame scheduler now uses live-capture defaults for first-battle
   `InstructionWorksheet` motion speeds: Vyse `IW+0x12c=2.55` and
   `IW+0x130=0.45`, Aika `2.25 / 0.6`, and Soldier slots `2.399997 /
   2.399997`. These values are first-battle-only fallback inputs derived from
   the `20260628_float_motion_checkpoint_profile` and
   `20260628_move_increment_read_watch` captures, where `FUN_8001fabc` seeds
   `pos_to_move_to_0x110` / `move_increment_0x104` and `FUN_8001e910 ->
   moveCombatantIncrement_80061340` consumes the increment frame-by-frame.
   The unresolved source question is where `IW+0x12c` and `IW+0x130` are
   originally populated: candidate owners remain STD action-row data, a loaded
   global/table, combatant resource data, or an unclassified setup helper. Do
   not generalize these speed constants beyond first battle until that source
   is traced and validated.

## Next High-Value Research Step

Use a static-first pass on the action-view/action-source path before treating
aggregate residuals as model constants:

- trace `FUN_80012f58`, `FUN_80009030`, `FUN_80053f38`,
  `UpdateActionViewRecord_80051264`, and the serialized `0x0003002a` record
  path in disassembly for PC `ma0000`/`ma0010` key `4`/`8` attacks and Soldier
  `mb0000` attacks;
- map how the live action/source key, `%s0_STD` root, selected record pointer,
  and selected mode are carried through the worker thread/payload chain;
- explain the target-variant residual split (`[4]Soldier` target mode `362`,
  `[5]Soldier` target mode `371`) from static ownership candidates before
  adding a heuristic offset.

Then capture or synthesize a live checkpoint stream for one representative
`ReachedNextTurn` job that includes:

- pre-AI fake/camera rows for every fake-attack attempt, including
  `fake_attack_index`, `owns_rng_draw`, target-camera A-to-B frame gap, normal
  `800608DC` reach/suppress state, and the first Soldier AI draw index;
- draw indices at `FUN_8006721c`, `FUN_80012f58`, `FUN_80052b24:80052bf0`,
  `HandleECInst:8008bc68`, `getAttackResult_80010b64`, `rollDamage_800108ec`,
  `shouldCounter_800819d0`, and `enemyDropItem_8002ba8c`;
- shared `action_sequence_id` values across source selection, combatant-action-mode bridge,
  action-view gate/camera, attack result, damage, counter/death/drop, and
  outcome rows where the capture can provide them;
- live queued instruction fields, especially queue index, execution index,
  assigned priority, priority-tie membership, combatant command parameter,
  target, and `_maybe_0x9`;
- live active/source combatant action mode and action-view aux-list suppression result;
- live action-view child thread, payload chain, child state byte, selected
  record mode, and whether `UpdateActionViewRecord_80051264:800513d4` was
  reached;
- live current stats for hit, dodge, Agile, attack, defense, element, HP, and
  counter chance;
- live target validation and retargeting fields when an actor's queued target
  is dead before execution: original target slot, candidate replacement slots,
  selected replacement target, and whether retargeting occurs before action
  setup, action-view selection, or attack resolution;
- lethal-flow fields from `zzDealDamage` through `HandleCombatantDeath` and
  `enemyDropItem`: applied damage, HP before/after clamp, lethal flag, enemy
  entry id, drop row, drop roll, selected item, and stop-after-success state.

That checkpoint stream is the shortest path from aggregate residual clustering
to a validated first-battle predictor.

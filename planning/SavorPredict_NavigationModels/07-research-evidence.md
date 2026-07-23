# 07 - Research Evidence

## Status

Living source index for planning. Findings should be revised when stronger static or live evidence
becomes available.

## Scope boundary

- `NavigationEncounterSolver` targets non-overworld fields.
- Area 99 (`a099*`) is the overworld and is explicitly unsupported in the initial model.
- Area 99 adds a second spatial lookup through `fldEfcontrol`; non-overworld fields use the decoded
  collision-selector encounter-table ID directly.

Sources:

- `D:\SoAInvestigate\Analyses\20260715_1113_dungeon_encounter_grnd\20260715_1143_dungeon_encounter_grnd_summary.txt`
- `D:\SoAInvestigate\Analyses\20260713_2111_encounter_formula_disassembly_validation\20260713_2140_encounter_formula_disassembly_validation_summary.txt`

## Non-overworld encounter-region encoding

- In the validated Catacombs assets (`a106a.mld` and `a106c.mld`), the decimal tens digit of each
  collision triangle's low-15-bit packed selector is the encounter-table ID.
- Encounter ID `0` is a no-encounter region.
- Other decimal digits independently carry surface properties, so the complete raw selector is not the
  encounter ID.
- Encounter regions can be carried by GRND or GOBJ collision resources referenced by an MLD ground entry.
- The ground-entry TBLID selects a registered collision resource; it is not the encounter-table ID.
- The collision hit's decoded low nibble is copied to `DAT_8034740E`, which
  `StepCounter_800C1C24` uses directly outside area 99.

Sources:

- `D:\SoAInvestigate\Analyses\20260715_1113_dungeon_encounter_grnd\20260715_1143_dungeon_encounter_grnd_summary.txt`
- `D:\SoAInvestigate\Analyses\20260715_1113_dungeon_encounter_grnd\20260715_1143_catacombs_report_selector_match.tsv`

Confidence:

- High for Catacombs IDs 0 through 7 and the traced runtime chain.
- Not yet universal across every non-overworld field. `a101b` is the first planned implementation field,
  and broader spot checks remain future work.

## Movement gate and step advancement

- The player path accumulates 3D distance between current and previous positions in
  `EncounterMovementDistanceAccum_80347410`.
- On an otherwise eligible update, StepCounter checks only whether the accumulator is nonzero.
- StepCounter clears it and increments `stepCount` by exactly one.
- Movement magnitude does not multiply the increment or probability threshold.
- A stationary frame does not advance the StepCounter.

Sources:

- `D:\SoAInvestigate\Analyses\20260713_2111_encounter_formula_disassembly_validation\20260713_2140_encounter_formula_disassembly_validation_summary.txt`
- `D:\SoAInvestigate\Analyses\20260622_1329_a099_mld_handlers\20260622_1445_encounter_step_counter_resolution_summary.txt`

## Encounter RNG consumption

- Each eligible moved-step check consumes one shared-RNG draw for the encounter probability test.
- A successful probability test reaches sequential conditional ECT row selection.
- Row selection consumes another RNG draw for every inspected row until one is accepted.
- The seeded model must preserve this variable draw count; it cannot substitute one cumulative-weight
  draw.

Source:

- `D:\SoAInvestigate\Analyses\20260713_2111_encounter_formula_disassembly_validation\20260713_2140_encounter_formula_disassembly_validation_summary.txt`

## Field update order and traversal RNG

- Active field threads run before the encounter StepCounter call.
- MLD callbacks can therefore advance the shared RNG before the encounter probability draw.
- Confirmed non-overworld direct RNG consumers include:
  - `MLD::motrand_80106EE0`, direct call at `0x8010729C`;
  - `MLD::pcharand_80106428`, direct call at `0x80106794`;
  - `MLD::pcharandever_80105DD0`, direct calls at `0x8010615C` and `0x80106340`.
- Their execution is state- and timing-dependent, which is why later field profiles require
  `TraversalRngModel`.
- `a101b` is intentionally treated as a step-counter-only first slice; no MLD callback state is captured
  for that profile.

Sources:

- `D:\SoAInvestigate\Analyses\mld_handler_function_living_document.txt`
- `D:\SoAInvestigate\Analyses\20260615_1130_mld_handler_functions\remaining_addressed_handlers_decompile.txt`
- `D:\SoAInvestigate\Analyses\20260615_1130_mld_handler_functions\next10c_handlers_decompile.txt`
- `D:\SoAInvestigate\Analyses\20260615_1130_mld_handler_functions\next10b_handlers_decompile.txt`
- `D:\SoAInvestigate\Analyses\20260713_2111_encounter_formula_disassembly_validation\20260713_2140_encounter_formula_disassembly_validation_summary.txt`

## Planning implications

- Encounter overlays must retain decoded encounter-table IDs per collision region and raw selector
  provenance.
- Per-frame movement must resolve geometry and region before encounter progression is evaluated.
- Step-only profiles can eliminate RNG-neutral idle waits.
- Later profiles must model traversal RNG in update order and allow waits only where they can change RNG
  or future modeled state.
- Internal predictor traces must preserve enough movement, region, step, and RNG detail to locate the
  first divergence from a live worker run.

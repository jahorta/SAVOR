# Stage 2 - Schema and Migration Plan

## Objective
Create concrete schemas and migrations for all new context DBs, including a single Analysis DB file with Analysis.Spine, Analysis.SeedProbe, and Analysis.Battle schema groups.

## Exit Criteria
- All target DB files can be created from migrations.
- Core indexes and uniqueness constraints exist.
- Schema docs match implementation.

---

## 2.0 Design Constraints Agreed During Planning

- SimCoreDB is a blank-slate implementation; schema decisions should model workflows and UI needs, not mirror legacy table names.
- Avoid JSON storage where possible. Prefer typed relational columns and bridge tables.
- Authoring DB is source-of-truth for worker input specifications (blueprints, options, presets).
- Analysis DB stores typed outputs needed for downstream derivation and UI, instead of re-parsing INI payloads.
- Execution DB remains orchestration-only.
- For Authoring specs: immutable rows, `spec_id` surrogate PK, and `name UNIQUE NOT NULL`.

---

## 2.1 Execution DB (Ephemeral, Orchestration-Only)

### Purpose
Queueing, claiming, retries, parent/child orchestration, and event/outbox mechanics.

### Tables

#### 1) `exec_job_set`
- `job_set_id` (PK)
- `parent_job_set_id` (nullable FK -> `exec_job_set.job_set_id`)
- `program_kind` (int)
- `purpose` (text)
- `created_by` (text nullable)
- `created_at_utc` (int)
- `priority_boost` (int default 0)
- `expected_total` (int nullable)
- `domain_ref_kind` (text nullable)
- `domain_ref_id` (int nullable)
- `meta_note` (text nullable)

#### 2) `exec_job`
- `job_id` (PK)
- `job_set_id` (FK -> `exec_job_set.job_set_id`)
- `parent_job_id` (nullable FK -> `exec_job.job_id`)
- `program_kind` (int)
- `program_version` (int)
- `program_ref_kind` (text)
- `program_ref_id` (int)
- `fingerprint` (text UNIQUE)
- `priority` (int)
- `state` (text enum)
- `attempts` (int)
- `max_attempts` (int)
- `claimed_by_token` (text nullable)
- `lease_expires_at_utc` (int nullable)
- `queued_at_utc` (int)
- `started_at_utc` (int nullable)
- `ended_at_utc` (int nullable)
- `error_code` (text nullable)
- `error_text` (text nullable)

#### 3) `exec_job_event`
- `job_event_id` (PK)
- `job_id` (FK -> `exec_job.job_id`)
- `event_kind` (text)
- `event_ts_utc` (int)
- `message` (text nullable)
- `artifact_id` (int nullable; cross-context reference)

#### 4) `exec_trigger`
- `trigger_id` (PK)
- `scope_kind` (text enum: `job`, `job_set`)
- `scope_id` (int)
- `condition_kind` (text)
- `condition_value` (text)
- `action_kind` (text)
- `action_value` (text)
- `active` (bool)
- `created_at_utc` (int)

#### 5) `exec_outbox_message`
- `outbox_id` (PK)
- `event_id` (text UNIQUE)
- `event_type` (text)
- `event_version` (int)
- `context_name` (text)
- `aggregate_kind` (text)
- `aggregate_id` (text)
- `correlation_id` (text nullable)
- `causation_id` (text nullable)
- `occurred_at_utc` (int)
- `payload_ref_kind` (text)
- `payload_ref_id` (int)
- `published_at_utc` (int nullable)
- `attempt_count` (int)
- `last_error` (text nullable)

#### 6) `exec_archive_cursor`
- `cursor_id` (PK)
- `cursor_kind` (text)
- `last_scanned_at_utc` (int)
- `last_job_set_id` (int nullable)

#### 7) `exec_workflow_instance`
- `workflow_instance_id` (PK)
- `workflow_kind` (text)
- `state` (text)
- `root_scope_kind` / `root_scope_id` (nullable operational scope)
- `workflow_graph_revision_id` (nullable FK by convention -> `au_workflow_graph_revision.workflow_graph_revision_id`)
- `created_by`
- lifecycle timestamps and failure fields

Cleanup status: instance-level `input_ref_kind` / `input_ref_id` bootstrap columns have been removed from the active workflow instance schema. External launch inputs use typed input bindings below.

#### 8) `exec_workflow_step`
- `workflow_step_id` (PK)
- `workflow_instance_id` (FK -> `exec_workflow_instance.workflow_instance_id`)
- `step_key`
- `step_kind`
- `state`
- guard, priority, attempts, job-set, lifecycle, output, and failure fields

Current note: step-level `input_ref_kind` / `input_ref_id` remains useful for actual runtime refs produced or consumed by descriptors and dynamic downstream steps. External launch inputs come from `exec_workflow_instance_input_binding`.

#### 9) `exec_workflow_edge`
- `workflow_edge_id` (PK)
- `workflow_instance_id` (FK -> `exec_workflow_instance.workflow_instance_id`)
- `from_step_id` / `to_step_id`
- `condition_kind` / `condition_value`
- `created_at_utc`

#### 10) `exec_workflow_instance_input_binding`
- `workflow_instance_input_binding_id` (PK)
- `workflow_instance_id` (FK -> `exec_workflow_instance.workflow_instance_id`)
- `workflow_graph_revision_id`
- `node_key`
- `input_key`
- `data_kind`
- `ref_kind`
- `ref_id`
- `source_kind` (nullable)
- `created_at_utc`

This table is the target home for external inputs selected by the launcher. Authored workflow graphs must not store these values.

#### 11) `exec_workflow_instance_argument`
- `workflow_instance_argument_id` (PK)
- `workflow_instance_id` (FK -> `exec_workflow_instance.workflow_instance_id`)
- `node_key` (empty string for graph/global argument)
- `argument_key`
- `value_type` (`integer`, `text`, `json`, `boolean`)
- `integer_value` / `text_value`
- `source_kind` (nullable)
- `created_at_utc`

This table is the target home for scalar launch values that vary by instance, such as TAS RTC value and battle fake-attack min/max overrides.

### Key Constraints / Indexes
- Unique job fingerprint (`exec_job.fingerprint`).
- `exec_job_set.domain_ref_kind/domain_ref_id` are lightweight links only; source-of-truth domain facts remain in domain context tables.
- Parent pointers indexed (`exec_job.parent_job_id`, `exec_job_set.parent_job_set_id`).
- Claim queue index on `(state, priority DESC, queued_at_utc ASC)`.
- Traversal index on `exec_job(job_set_id, state, queued_at_utc DESC)`.

---

## 2.2 State DB (Durable)

### Purpose
Store artifacts, savestates, and deterministic derivation lineage.

### Tables

#### 1) `state_artifact`
- `artifact_id` (PK)
- `sha256` (text UNIQUE)
- `size_bytes` (int)
- `compression_kind` (int)
- `filename` (text)
- `file_ext` (text)
- `artifact_kind` (text enum: `DTM`, `DTMINI`, `SAV`, `LOG`, `OTHER`)
- `created_at_utc` (int)

#### 2) `state_savestate`
- `savestate_id` (PK)
- `artifact_id` (FK -> `state_artifact.artifact_id`)
- `savestate_type` (text enum)
- `note` (text nullable)
- `is_complete` (bool)
- `created_at_utc` (int)

#### 3) `state_savestate_derivation`
- `derivation_id` (PK)
- `from_savestate_id` (FK -> `state_savestate.savestate_id`)
- `to_savestate_id` (FK -> `state_savestate.savestate_id`)
- `method_kind` (text)
- `source_context_kind` (text)
- `source_context_id` (int)
- `created_at_utc` (int)

#### 4) `state_tas_movie_variant`
- `tas_variant_id` (PK)
- `name` (text UNIQUE NOT NULL)
- `base_dtm_artifact_id` (FK -> `state_artifact.artifact_id`)
- `dtmini_artifact_id` (nullable FK -> `state_artifact.artifact_id`)
- `mutation_mode` (text enum: `NONE`, `RTC_OVERRIDE`, `INSERT_NEUTRAL_FRAME`)
- `rtc_value` (int nullable)
- `bookmark_name` (text nullable)
- `insert_frame_count` (int nullable)
- `parent_tas_variant_id` (nullable FK -> `state_tas_movie_variant.tas_variant_id`)
- `produced_savestate_id` (nullable FK -> `state_savestate.savestate_id`)
- `created_at_utc` (int)

#### 5) `state_outbox_message`
- same envelope fields as `exec_outbox_message`

### Key Constraints / Indexes
- Unique object hash (`state_artifact.sha256`).
- Derivation chain indexes:
  - `state_savestate_derivation(from_savestate_id)`
  - `state_savestate_derivation(to_savestate_id)`

---

## 2.3 Analysis DB - Spine Schema Group (Durable, Minimal)

### Purpose
Small shared provenance graph for cross-mode traceability.

### Tables

#### 1) `asp_run`
- `run_id` (PK)
- `run_kind` (text)
- `state` (text)
- `started_at_utc` (int nullable)
- `completed_at_utc` (int nullable)
- `created_at_utc` (int)

#### 2) `asp_state_ref`
- `state_ref_id` (PK)
- `run_id` (FK -> `asp_run.run_id`)
- `savestate_id` (int cross-context reference)
- `role_kind` (text enum: `ENTRY`, `CHECKPOINT`, `OUTPUT`)
- `created_at_utc` (int)

#### 3) `asp_lineage_edge`
- `lineage_edge_id` (PK)
- `parent_run_id` (FK -> `asp_run.run_id`)
- `child_run_id` (FK -> `asp_run.run_id`)
- `edge_kind` (text)
- `created_at_utc` (int)

#### 4) `asp_artifact_ref`
- `artifact_ref_id` (PK)
- `run_id` (FK -> `asp_run.run_id`)
- `artifact_id` (int cross-context reference)
- `role_kind` (text)
- `created_at_utc` (int)

#### 5) `asp_outbox_message`
- same envelope fields as `exec_outbox_message`

### Design Rule
- Keep this context intentionally small.
- Only data needed for cross-mode provenance lives here.

---

## 2.4 Analysis DB - SeedProbe Schema Group (Durable)

### Purpose
Persist canonical seed probe outputs as durable analysis facts.

### Tables

#### 1) `sp_probe_set`
- `probe_set_id` (PK)
- `name` (text UNIQUE NOT NULL)
- `probe_flavor` (text enum: `BATTLE_PRE`, `DUNGEON_PRE`, `OVERWORLD_PRE`)
- `breakpoint_policy_name` (text)
- `dungeon_segment_file_num` (int nullable)
- `dungeon_segment_file_letter` (text(1) nullable)
- `dungeon_segment_code` (text nullable, e.g. `123A`)
- `segment_source_kind` (text enum, e.g. `WRAPPER_CONTEXT`, `MANUAL`)
- `created_at_utc` (int)

#### 2) `sp_probe_run`
- `probe_run_id` (PK)
- `probe_set_id` (FK -> `sp_probe_set.probe_set_id`)
- `entry_savestate_id` (int cross-context reference)
- `seed_probe_spec_id` (int cross-context reference)
- `codec_version` (int)
- `status` (text)
- `requested_at_utc` (int)
- `completed_at_utc` (int nullable)

#### 3) `sp_probe_result`
- `probe_result_id` (PK)
- `probe_run_id` (FK -> `sp_probe_run.probe_run_id`, UNIQUE)
- `neutral_seed_value` (int nullable)
- `grid_count` (int)
- `unique_count` (int)
- `result_status` (text)
- `recorded_at_utc` (int)

#### 4) `sp_axis_xy`
- `axis_xy_id` (PK)
- `x` (int 0..255)
- `y` (int 0..255)
- UNIQUE(`x`,`y`)

#### 5) `sp_input_frame`
- `input_frame_id` (PK)
- `main_axis_xy_id` (FK -> `sp_axis_xy.axis_xy_id`)
- `cstick_axis_xy_id` (FK -> `sp_axis_xy.axis_xy_id`)
- `trigger_axis_xy_id` (FK -> `sp_axis_xy.axis_xy_id`)
- UNIQUE(`main_axis_xy_id`,`cstick_axis_xy_id`,`trigger_axis_xy_id`)

#### 6) `sp_neutral_seed`
- `neutral_seed_id` (PK)
- `probe_result_id` (FK -> `sp_probe_result.probe_result_id`)
- `neutral_seed_value` (int)
- `source_kind` (text enum: `CALCULATED`, `IMPORTED`)
- `recorded_at_utc` (int)

#### 7) `sp_grid_seed`
- `grid_seed_id` (PK)
- `probe_result_id` (FK -> `sp_probe_result.probe_result_id`)
- `source_family` (text enum: `MAIN`, `CSTICK`, `TRIGGER`)
- `axis_xy_id` (FK -> `sp_axis_xy.axis_xy_id`)
- `seed_value` (int)
- `seed_delta` (int)
- `recorded_at_utc` (int)
- **No uniqueness constraint on (`probe_result_id`,`seed_value`)**.

#### 8) `sp_unique_seed`
- `unique_seed_id` (PK)
- `probe_result_id` (FK -> `sp_probe_result.probe_result_id`)
- `input_frame_id` (FK -> `sp_input_frame.input_frame_id`)
- `seed_value` (int)
- `seed_delta` (int)
- `recorded_at_utc` (int)

#### 9) `sp_encounter_projection`
- `encounter_projection_id` (PK)
- `probe_run_id` (FK -> `sp_probe_run.probe_run_id`)
- `seed_value` (int)
- `option_ordinal` (int)
- `encounter_id` (text)
- `encounter_frame` (int)
- `stutter_step_at` (int nullable)
- `movement_required` (bool)
- `recorded_at_utc` (int)

#### 10) `sp_outbox_message`
- same envelope fields as `exec_outbox_message`

### Design Notes
- `sp_axis_xy` and `sp_input_frame` are intentionally normalized to reduce storage from repeated probe grids.
- `sp_grid_seed` stores family + axis coordinate references to preserve raw grid probing multiplicity.
- `sp_unique_seed` stores complete frame references for downstream battle derivation.

---

## 2.5 Analysis DB - Battle Schema Group (Durable)

### Purpose
Persist explicit battle exploration tree, turn waves, selection decisions, and manual follow-up state.

### Tables

#### 1) `ab_battle_set`
- `battle_set_id` (PK)
- `name` (text UNIQUE NOT NULL)
- `entry_savestate_id` (int cross-context reference)
- `battle_run_spec_id` (int cross-context reference)
- `explorer_settings_id` (int cross-context reference)
- `status` (text)
- `created_at_utc` (int)
- `completed_at_utc` (int nullable)

#### 2) `ab_seed_candidate`
- `seed_candidate_id` (PK)
- `battle_set_id` (FK -> `ab_battle_set.battle_set_id`)
- `source_unique_seed_id` (nullable int cross-context reference)
- `seed_value` (int)
- `source_kind` (text enum: `SP_UNIQUE`, `MANUAL`, `SYNTHETIC`)
- `candidate_status` (text enum)
- `created_at_utc` (int)

#### 3) `ab_turn_wave`
- `wave_id` (PK)
- `battle_set_id` (FK -> `ab_battle_set.battle_set_id`)
- `turn_index` (int)
- `parent_wave_id` (nullable FK -> `ab_turn_wave.wave_id`)
- `seed_candidate_id` (FK -> `ab_seed_candidate.seed_candidate_id`)
- `selection_pool_id` (nullable FK -> `ab_selection_pool.selection_pool_id`)
- `status` (text)
- `created_at_utc` (int)
- `completed_at_utc` (int nullable)

#### 4) `ab_turn_job`
- `turn_job_id` (PK)
- `wave_id` (FK -> `ab_turn_wave.wave_id`)
- `exec_job_id` (nullable int cross-context reference, UNIQUE)
- `plan_id` (int cross-context reference)
- `fake_attacks_this_turn` (int)
- `fake_attacks_used_before` (int)
- `job_state` (text)
- `started_at_utc` (int nullable)
- `ended_at_utc` (int nullable)

**Result columns in same row**
- `has_results` (bool)
- `vi_start` (int nullable)
- `vi_end` (int nullable)
- `delta_vi` (int nullable)
- `rng_seed` (int nullable)
- `battle_outcome` (int nullable)
- `plan_materialize_err` (int nullable)
- `pred_passed` (int nullable)
- `pred_total` (int nullable)
- `pred_abort_run` (int nullable)
- `output_savestate_id` (int nullable, cross-context reference)
- `recorded_at_utc` (int nullable)

#### 5) `ab_selection_pool`
- `selection_pool_id` (PK)
- `battle_set_id` (FK -> `ab_battle_set.battle_set_id`)
- `turn_index` (int)
- `pool_name` (text)
- `criterion_kind` (text)
- `created_at_utc` (int)

#### 6) `ab_selection_decision`
- `selection_decision_id` (PK)
- `selection_pool_id` (FK -> `ab_selection_pool.selection_pool_id`)
- `turn_job_id` (FK -> `ab_turn_job.turn_job_id`)
- `decision_kind` (text enum: `WINNER`, `DUPLICATE`, `REJECTED`)
- `decision_reason` (text nullable)
- `created_at_utc` (int)

#### 7) `ab_terminal_followup`
- `terminal_followup_id` (PK)
- `turn_job_id` (FK -> `ab_turn_job.turn_job_id`, UNIQUE)
- `is_victory` (bool)
- `manual_followup_status` (text enum: `UNREVIEWED`, `RECORDED`; default `UNREVIEWED`)
- `recorded_dtm_artifact_id` (int nullable cross-context reference)
- `recorded_dtmini_artifact_id` (int nullable cross-context reference)
- `recorded_sav_artifact_id` (int nullable cross-context reference)
- `note` (text nullable)
- `updated_at_utc` (int)
- Check: `manual_followup_status='RECORDED'` requires non-null `recorded_dtm_artifact_id`.

#### 8) `ab_outbox_message`
- same envelope fields as `exec_outbox_message`

### Key Constraints / Indexes
- Parent index on `ab_turn_wave(parent_wave_id)`.
- Turn traversal indexes:
  - `ab_turn_wave(battle_set_id, turn_index)`
  - `ab_turn_job(wave_id, battle_outcome)`
- Decision filter indexes:
  - `ab_selection_decision(selection_pool_id, decision_kind)`
  - `ab_terminal_followup(manual_followup_status, is_victory)`

---

## 2.6 Authoring DB (Durable, Immutable Specs)

### Purpose
Versioned, user-selectable input specs and composition rows used to build worker payloads.

### Tables

#### 1) `au_seed_probe_spec`
- `seed_probe_spec_id` (PK)
- `name` (text UNIQUE NOT NULL)
- `priority` (int)
- `run_ms` (int)
- `vi_stall_ms` (int)
- `clear_result_winners` (bool)
- `samples_per_axis` (int)
- `min_value` (int)
- `max_value` (int)
- `cap_trigger_top` (bool)
- `ignore_trigger_minmax` (bool)
- `combo_attempts_per_target` (int)
- `combo_sampler_tries` (int)
- `auto_schedule_battle_run` (bool)
- `created_at_utc` (int)

Target cleanup: `auto_schedule_battle_run` should be removed or ignored by new graph-style execution. Downstream scheduling belongs to authored graph edges plus descriptor-produced actual refs.

#### 2) `au_tas_spec_base`
- `tas_spec_base_id` (PK)
- `name` (text UNIQUE NOT NULL)
- `priority` (int)
- `run_ms` (int)
- `vi_stall_ms` (int)
- `headroom_x10` (int)
- `progress_enable` (bool)
- `auto_queue_seeds` (bool)
- `created_at_utc` (int)

#### 3) `au_tas_spec`
- `tas_spec_id` (PK)
- `tas_spec_base_id` (FK -> `au_tas_spec_base.tas_spec_base_id`)
- `base_dtm_artifact_id` (int cross-context reference)
- `rtc_low` (int)
- `rtc_high` (int)
- `created_at_utc` (int)

Target cleanup: selected DTM artifacts are workflow instance input bindings, and RTC values/ranges are launcher input that fan out to per-instance arguments. Authored TAS specs should retain reusable TAS behavior only.

#### 4) `au_battle_run_spec`
- `battle_run_spec_id` (PK)
- `name` (text UNIQUE NOT NULL)
- `priority` (int)
- `run_ms` (int)
- `vi_stall_ms` (int)
- `progress_enable` (bool)
- `use_single_turn_runner` (bool)
- `auto_wave_trigger_enable` (bool)
- `min_fake_attacks` (int)
- `max_fake_attacks` (int)
- `created_at_utc` (int)

Target cleanup: fake-attack min/max bounds are launch-time exploration arguments when they vary per run. Authored battle specs should keep reusable runner settings.

#### 5) `au_battle_plan`
- `plan_id` (PK)
- `name` (text UNIQUE NOT NULL)
- `fingerprint` (text UNIQUE NOT NULL)
- `num_turns` (int)
- `created_at_utc` (int)

#### 6) `au_battle_plan_turn`
- `plan_turn_id` (PK)
- `plan_id` (FK -> `au_battle_plan.plan_id`)
- `turn_index` (int)
- UNIQUE(`plan_id`, `turn_index`)

#### 7) `au_battle_plan_action`
- `plan_action_id` (PK)
- `plan_turn_id` (FK -> `au_battle_plan_turn.plan_turn_id`)
- `actor_slot` (int)
- `macro` (int)
- `target_kind` (int)
- `target_slot` (int nullable)
- `item_id` (int nullable)
- `ordinal` (int)

#### 8) `au_predicate_spec`
- `predicate_spec_id` (PK)
- `name` (text UNIQUE NOT NULL)
- typed predicate columns (required breakpoint(s), lhs/rhs, cmp op, flags, masks)
- `abort_on_fail` (bool)
- `created_at_utc` (int)

#### 9) `au_predicate_set`
- `predicate_set_id` (PK)
- `created_at_utc` (int)

#### 10) `au_predicate_set_item`
- `predicate_set_id` (FK -> `au_predicate_set.predicate_set_id`)
- `predicate_spec_id` (FK -> `au_predicate_spec.predicate_spec_id`)
- `ordinal` (int)
- PRIMARY KEY(`predicate_set_id`, `ordinal`)

#### 11) `au_explorer_settings`
- `explorer_settings_id` (PK)
- `name` (text UNIQUE NOT NULL)
- `description` (text nullable)
- `default_plan_id` (nullable FK -> `au_battle_plan.plan_id`)
- `default_predicate_set_id` (nullable FK -> `au_predicate_set.predicate_set_id`)
- `created_at_utc` (int)

#### 12) `au_template`
- `template_id` (PK)
- `name` (text UNIQUE NOT NULL)
- `description` (text nullable)
- `seed_probe_spec_id` (nullable FK -> `au_seed_probe_spec.seed_probe_spec_id`)
- `tas_spec_id` (nullable FK -> `au_tas_spec.tas_spec_id`)
- `battle_run_spec_id` (nullable FK -> `au_battle_run_spec.battle_run_spec_id`)
- `explorer_settings_id` (nullable FK -> `au_explorer_settings.explorer_settings_id`)
- `created_at_utc` (int)

#### 13) `au_outbox_message`
- same envelope fields as `exec_outbox_message`

#### 14) `au_workflow_graph`
- `workflow_graph_id` (PK)
- `name` (text UNIQUE NOT NULL)
- `description` (nullable)
- `active_revision_id` (nullable FK -> `au_workflow_graph_revision.workflow_graph_revision_id`)
- `created_at_utc`

#### 15) `au_workflow_graph_revision`
- `workflow_graph_revision_id` (PK)
- `workflow_graph_id` (FK -> `au_workflow_graph.workflow_graph_id`)
- `graph_version`
- `graph_hash`
- `parent_revision_id` (nullable FK -> `au_workflow_graph_revision.workflow_graph_revision_id`)
- `status`
- `created_at_utc`

#### 16) `au_workflow_graph_revision_node`
- `workflow_graph_revision_node_id` (PK)
- `workflow_graph_revision_id` (FK -> `au_workflow_graph_revision.workflow_graph_revision_id`)
- `node_key`
- `unit_kind`
- `display_name` (nullable)
- `authored_ref_kind` / `authored_ref_id` (nullable authored record reference)
- `ordinal`

#### 17) `au_workflow_graph_revision_node_input`
- `workflow_graph_revision_node_input_id` (PK)
- `workflow_graph_revision_node_id` (FK -> `au_workflow_graph_revision_node.workflow_graph_revision_node_id`)
- `input_key`
- `data_kind`
- `display_name` (nullable)
- `required`
- `ordinal`

#### 18) `au_workflow_graph_revision_node_output`
- `workflow_graph_revision_node_output_id` (PK)
- `workflow_graph_revision_node_id` (FK -> `au_workflow_graph_revision_node.workflow_graph_revision_node_id`)
- `output_key`
- `data_kind`
- `display_name` (nullable)
- `ordinal`

#### 19) `au_workflow_graph_revision_edge`
- `workflow_graph_revision_edge_id` (PK)
- `workflow_graph_revision_id` (FK -> `au_workflow_graph_revision.workflow_graph_revision_id`)
- `from_revision_node_id`
- `output_key`
- `to_revision_node_id`
- `input_key`
- `guard_kind` / `guard_value` (nullable)
- `ordinal`

Authoring workflow graph rows define reusable graph shape only. They do not contain external input values or per-launch scalar arguments.

---

## 2.7 UI Read DB (Rebuildable)

### Purpose
Denormalized read models to keep UI queries simple and fast.

### Tables
- `ui_job_summary`
- `ui_job_detail`
- `ui_job_artifact`
- `ui_seed_probe_summary`
- `ui_seed_probe_delta_point`
- `ui_seed_probe_unique_value`
- `ui_battle_group`
- `ui_battle_wave`
- `ui_battle_turn_job`
- `ui_battle_followup`
- `ui_artifact_browser`
- `ui_archive_catalog`
- `ui_projection_checkpoint`

### Notes
- `ui_seed_probe_summary` must expose neutral/grid/unique counts.
- `ui_seed_probe_delta_point` uses family + x/y spans for graph rendering.
- `ui_battle_turn_job` includes typed outcome metrics (`rng_seed`, `delta_vi`, predicate counts, outcome code).

---

## 2.8 Archive DB (Durable Index)

### Purpose
Catalog archive packages and map rehydrated identities.

### Tables

#### 1) `ar_archive_package`
- `archive_package_id` (PK)
- `source_context` (text)
- `source_root_job_set_id` (int)
- `created_at_utc` (int)
- `schema_version` (int)
- `event_catalog_version` (int)
- `time_range_start_utc` (int)
- `time_range_end_utc` (int)
- `manifest_path` (text)
- `checksum_status` (text)

#### 2) `ar_archive_item`
- `archive_item_id` (PK)
- `archive_package_id` (FK -> `ar_archive_package.archive_package_id`)
- `item_kind` (text)
- `item_count` (int)
- `blob_path` (text nullable)
- `checksum` (text nullable)

#### 3) `ar_rehydrate_request`
- `rehydrate_request_id` (PK)
- `archive_package_id` (FK -> `ar_archive_package.archive_package_id`)
- `status` (text enum)
- `requested_at_utc` (int)
- `completed_at_utc` (int nullable)
- `error_text` (text nullable)
- `target_namespace` (text)

#### 4) `ar_rehydrate_map`
- `rehydrate_map_id` (PK)
- `rehydrate_request_id` (FK -> `ar_rehydrate_request.rehydrate_request_id`)
- `entity_kind` (text)
- `old_id` (text)
- `new_id` (text)

---

## 2.9 How Tables Work Together (Cross-Context Design)

1. **Authoring -> Execution**
   - User selects immutable `au_*` specs.
   - Execution rows store only references to chosen specs and run entities.

2. **State <-> Analysis**
   - State persists artifacts and savestates.
   - Analysis run rows reference state ids for entry/output facts.
   - Spine (`asp_*`) records provenance edges and artifact roles.

3. **SeedProbe -> Battle**
   - `sp_unique_seed` supplies candidate source seeds.
   - `ab_seed_candidate` bridges those seeds into battle-set local selection lifecycle.
   - `ab_turn_wave` and `ab_turn_job` encode turn-by-turn branching and outcomes.

4. **Manual Follow-up**
   - Terminal battle jobs map to `ab_terminal_followup`.
   - All start `UNREVIEWED`; `RECORDED` requires a DTM artifact reference.

5. **Execution/Analysis/State -> UIRead**
   - Projectors denormalize operational and analysis data into UI-specific rows.
   - UIRead remains rebuildable from source contexts via outbox/event replay.

6. **Archive -> Execution**
   - Archive package/index DB tracks rehydration requests and old/new id mapping.

---

## Migration Execution Plan

1. Create DB files and metadata pragmas.
2. Create tables without heavy constraints.
3. Add critical indexes for queueing/list traversal and UI reads.
4. Add uniqueness/check constraints.
5. Seed enum/reference rows where needed.
6. Produce schema snapshots in `/planning/DBMigrateStages/snapshots`.

---

## Verification Tasks

- Schema diff checks against table inventory above.
- Index existence checks for:
  - execution claim queue,
  - seed probe detail/list queries,
  - battle turn/wave traversal,
  - UIRead projection keys.
- SeedProbe mandatory queries:
  1. get neutral seed for run
  2. list grid seeds by family
  3. list unique seeds with full input frame
  4. count all by class
- Battle mandatory queries:
  1. list root groups
  2. list waves by turn
  3. list turn job metrics
  4. list manual follow-up by status

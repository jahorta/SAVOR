# Stage 2 - Schema and Migration Plan

## Objective
Create concrete schemas and migrations for all new context DBs, including the new Analysis.SeedProbe tables and hybrid provenance spine.

## Exit Criteria
- All target DB files can be created from migrations.
- Core indexes and uniqueness constraints exist.
- Schema docs match implementation.

---

## 2.1 Execution DB (Ephemeral)

### Tables
1. `exec_job_set`
2. `exec_job`
3. `exec_job_event`
4. `exec_trigger`
5. `exec_outbox_message`
6. `exec_archive_cursor`

### Key Constraints
- Unique job fingerprint (`exec_job.fingerprint`).
- Parent pointers indexed (`parent_job_id`, `parent_job_set_id`).
- Claim queue index on `(state, priority desc, requested_at asc)`.

### Notes
- Keep domain-neutral: no battle-specific grouping semantics here.

---

## 2.2 State DB (Durable)

### Tables
1. `state_object_ref`
2. `state_savestate`
3. `state_derivation`
4. `state_outbox_message`

### Key Constraints
- Unique object hash (`sha256`).
- Derivation chain indexes for from/to savestate traversal.

---

## 2.3 Analysis Spine DB (Durable, Minimal)

### Tables
1. `asp_run`
2. `asp_state_ref`
3. `asp_lineage_edge`
4. `asp_artifact_ref`
5. `asp_outbox_message`

### Design Rule
- Keep this context intentionally small.
- Only data needed for cross-mode provenance should live here.

---

## 2.4 Analysis.SeedProbe DB (Durable, New)

## Purpose
Persist canonical seed probe outputs as durable analysis facts.

### Tables
1. `sp_seed_probe_set`
   - Domain grouping for probe campaigns (not `job_set`).
2. `sp_seed_probe_run`
   - One probe execution intent against one entry savestate/config.
3. `sp_seed_probe_result`
   - Summary row per run.
4. `sp_seed_probe_neutral_seed`
   - Canonical neutral seed result(s).
5. `sp_seed_probe_grid_seed`
   - Grid-discovered seed values.
6. `sp_seed_probe_unique_seed`
   - Unique-discovered seed values.
7. `sp_seed_probe_input_frame`
   - Optional normalized table for per-result frame payload refs.
8. `sp_outbox_message`

### Required Fields (minimum)

#### `sp_seed_probe_run`
- `seed_probe_run_id` (PK)
- `seed_probe_set_id`
- `entry_savestate_id`
- `codec_version`
- `status`
- `requested_at`, `completed_at`

#### `sp_seed_probe_result`
- `seed_probe_result_id` (PK)
- `seed_probe_run_id`
- `neutral_seed_value` (nullable if unresolved)
- `grid_count`
- `unique_count`
- `result_status`
- `recorded_at`

#### `sp_seed_probe_neutral_seed`
- `neutral_seed_id` (PK)
- `seed_probe_result_id`
- `neutral_seed_value`
- `source_kind` (calculated/imported)
- `recorded_at`

#### `sp_seed_probe_grid_seed`
- `grid_seed_id` (PK)
- `seed_probe_result_id`
- `seed_value`
- `seed_delta`
- `input_frame_ref` (nullable)
- `recorded_at`
- `UNIQUE(seed_probe_result_id, seed_value)`

#### `sp_seed_probe_unique_seed`
- `unique_seed_id` (PK)
- `seed_probe_result_id`
- `seed_value`
- `seed_delta`
- `input_frame_ref` (nullable)
- `recorded_at`
- `UNIQUE(seed_probe_result_id, seed_value)`

#### `sp_seed_probe_input_frame`
- `input_frame_id` (PK)
- `object_ref_id` or `frame_hex`
- `frame_kind` (neutral/grid/unique)
- `created_at`

### Design Notes
- Neutral/grid/unique values are separated for query clarity.
- Keep a summary row (`sp_seed_probe_result`) for fast reads.
- Large frame payloads should be object refs, not large inline blobs.

---

## 2.5 Analysis.Battle DB (Durable)

### Tables
1. `ab_battle_set`
2. `ab_seed_candidate`
3. `ab_turn_node`
4. `ab_turn_outcome`
5. `ab_branch_selection`
6. `ab_outbox_message`

### Key Constraints
- `ab_turn_node` parent index.
- Winner/selection indexes by turn.
- Optional unique on `(battle_set_id, turn_index, action_key, seed_candidate_id, fake_attacks_total)` to limit exact duplicates.

---

## 2.6 Authoring DB (Durable)

### Tables
1. `au_plan`
2. `au_plan_turn`
3. `au_template`
4. `au_predicate_spec`
5. `au_settings`
6. `au_outbox_message`

---

## 2.7 UI Read DB (Rebuildable)

### Tables
1. `ui_run_summary`
2. `ui_seed_probe_summary`
3. `ui_seed_probe_values`
4. `ui_battle_tree_node`
5. `ui_job_tail`
6. `ui_archive_catalog`
7. `ui_projection_checkpoint`

### Notes
- `ui_seed_probe_summary` should include neutral/grid/unique totals per run.
- `ui_seed_probe_values` supports browse/filter by value type and seed value.

---

## 2.8 Archive DB (Durable Index)

### Tables
1. `ar_archive_package`
2. `ar_archive_item`
3. `ar_rehydrate_request`
4. `ar_rehydrate_map`

---

## Migration Execution Plan

1. Create DB files and metadata pragmas.
2. Create tables without heavy constraints.
3. Add critical indexes.
4. Add uniqueness constraints and check constraints.
5. Seed enum/reference tables if needed.
6. Produce schema snapshots in `/planning/DBMigrateStages/snapshots`.

---

## Verification Tasks

- Schema diff checks against table inventory.
- Index existence checks for all claim/list traversal queries.
- SeedProbe mandatory query tests:
  1. get neutral seed for run
  2. list grid seeds
  3. list unique seeds
  4. count all by class

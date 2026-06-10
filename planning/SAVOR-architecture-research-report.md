# SAVOR Architecture Research for Seed-Probed Battles and Multi-Mode Exploration

## Problem framing and information needs

Your new requirements describe a “state → probe → branch → select → derive next state” cycle that repeats across three operational modes:

Battle analysis: a battle entry state is probed to discover the reachable initial RNG seeds; each seed becomes a branch, and branches can be further explored by deliberately advancing RNG during battle (via “fake attacks”), producing a turn-by-turn branching tree until either a next-turn continuation state or a terminal victory/defeat is reached.

Dungeon exploration: pathfinding to a load zone is run with encounters disabled (a shortest-path search), while encounter timing/availability is explored by input delays + seed probing; the chosen encounter is then materialized into a new encounter-entry savestate, re-probed, and then turned into a battle-turn exploration.

Overworld exploration: similar to dungeon exploration in structure, but with a different navigation model (3D + height affecting encounter mechanics), implying different path search inputs, heuristics, and state features.

Two underlying “mechanics anchors” emerge from the repo itself:

* The simulator already distinguishes “prebattle” RNG seed set points with explicit breakpoints before and after the RNG seed is written (useful for seed probing). fileciteturn39file0L1-L1  
* The simulator already has battle breakpoints that align with turn boundaries, including TurnInputs / EndTurn / Victory / Defeat, enabling one-turn-at-a-time execution and turn-to-turn chaining. fileciteturn39file0L1-L1  

Given the above, the key research objectives were:

* Verify what the current SAVOR repository already implements for TAS→probe→battle, seed dedupe, and turn-chaining.
* Identify where “battle turn state/results + previous turn” are currently stored (schema + code), and how close that is to your requested tables/relationships.
* Determine the cleanest bounded-context split for dungeon vs overworld vs battle analysis without over-fragmenting the data model (a risk you explicitly called out), while keeping UI aggregation easy through projections.

## What the current SAVOR repo already implements for your pathway

The current SAVOR schema and program codecs already implement a substantial portion of the battle-side requirements, including the concept of “one job = one battle turn,” explicit dedupe rules keyed by RNG seed, and an explicit previous-turn link.

### Canonical state + probe storage primitives already exist

The initial migration defines a canonical “content-addressed object + savestate” model and attaches both seed probes and seed-deltas to a savestate. In particular, the schema includes:

* `savestate` rows with an `object_ref_id` pointing to stored binary data (savestate file), and a `savestate_type` field (commented as battle vs overworld). fileciteturn3file0L1-L1  
* `seed_probe` rows keyed by `savestate_id`, and `seed_delta` rows keyed by `probe_id` that store a signed delta plus an input frame blob (GCInputFrame). fileciteturn3file0L1-L1  

On the code side, `SeedProbeRepo` exposes lifecycle operations like create/mark-running/set-neutral-seed/mark-done and stores probes using `(savestate_id, codec_version, neutral_seed, status, complete)` (so the “probe belongs to a state” invariant is already explicit). fileciteturn24file0L1-L1  

This matches your “battle entry should use a seed probe to determine available starting random seeds,” because the probe is simply a derived fact keyed to a specific savestate.

### Execution primitives map well to workflow-like orchestration today

SAVOR maintains an execution substrate built around:

* `job_sets` (logical groupings of work, with optional parent pointers for hierarchies). fileciteturn22file0L1-L1 fileciteturn20file0L1-L1  
* `jobs` that represent scheduled work units, keyed by a unique fingerprint, with state transitions like QUEUED/CLAIMED/RUNNING/SUCCEEDED/FAILED and battle-probe-specific terminal states like SUCCEEDED_WINNER and SUCCEEDED_DUPLICATE. fileciteturn5file0L1-L1  
* An explicit `parent_job_id` column for “derived jobs,” which is the exact relational primitive you described for “how to say what the previous turn was.” fileciteturn31file0L1-L1 fileciteturn40file0L1-L1  

Additionally, the repo already includes a generic trigger mechanism (`triggers` table with job/job_set scope plus an action kind). fileciteturn9file0L1-L1 fileciteturn19file0L1-L1  
This is effectively an embryonic orchestrator: it is a persisted “when condition holds, schedule action” artifact, which SAVOR uses to chain phases.

### TAS → SeedProbe → Battle is already wired as an execution chain

The SeedProbe program codec is explicitly stateful and phase-driven: it models Neutral → Grid → Unique phases and stores the current phase in INI-encoded blueprint state. fileciteturn25file0L1-L1  

The SeedProbe codec supports an on-trigger setup path from a TAS job into a seed probe job set: when the previous program kind is a TAS movie, it creates a seed probe for the TAS-produced savestate and enqueues the Neutral phase. fileciteturn26file0L1-L1  

Seed probing also already has a “winner” concept keyed by realized deltas via `seed_probe_winners`, which records first-seen winners per `(job_set_id, result_delta)` and includes optional metadata like frame hex and metrics. fileciteturn18file0L1-L1  

This is highly aligned with your stated need: “each unique seed probe will give an initial GC input frame that can be used to generate the starting random seed,” because the codec persists that GC input frame as `frame_hex` and emits `rng_seed` as the job result. fileciteturn25file0L1-L1 fileciteturn26file0L1-L1  

### One-turn battle exploration with RNG advancement and dedupe is implemented

The repository contains a dedicated DB codec for a single-turn battle runner, `PK_BattleSingleTurnRunner`. The job-level contract explicitly includes:

* `turn_index`
* `fake_attacks_used_before` and `fake_attacks_this_turn`
* an `action_key` (a stable identifier for the planned turn action set)
* output fields including ending `rng_seed` and an `output_savestate_id` when the run reaches the next turn or victory. fileciteturn28file0L1-L1  

For the *first* battle turn, the codec will apply the “initial input” when a delta seed is present, meaning it directly converts seed-probe results into battle start RNG seeds. fileciteturn29file0L1-L1  

Crucially, the runner explores RNG advancement during battle by enumerating fake-attack counts per turn (bounded by min/max fake attacks in the battle blueprint). This already operationalizes your “single RNG advances can be generated during battle time using fake attacks.” fileciteturn29file0L1-L1  

The next-turn linkage you asked for is also already encoded in two places:

* Continuation savestates are created and persisted as new savestate rows when a job reaches the next turn (or victory). fileciteturn29file0L1-L1  
* Jobs in the next wave are created with their `parent_job_id` set to the job that produced the continuation state. fileciteturn29file0L1-L1 fileciteturn31file0L1-L1  

The codec’s post-wave step also implements a dedupe policy: it scans results under a job set tree, keeps only survivors that reached next turn/victory, then selects winners keyed by `rng_seed` preferring the minimum total fake attacks used (ties broken deterministically). fileciteturn29file0L1-L1  
This directly matches the “branch by initial seed, then explore RNG-advanced variants, and keep best branches” intent.

Finally, there is a separate “battle context probe” program that can persist a serialized battle context blob as an artifact and insert a `battle_contexts` row linking job/job_set and savestate. This provides a concrete mechanism for “store per-turn battle state” beyond just savestate snapshots. fileciteturn9file0L1-L1 fileciteturn13file0L1-L1 fileciteturn42file0L1-L1  

### The current UI/data service shows why a projection DB is attractive

`DataService` currently aggregates querying and command helpers across scheduling (jobs/job_sets/job_events), state (savestates/object store), analysis (seed probes), and authoring (templates/presets/predicate specs). fileciteturn41file0L1-L1  
This confirms, in code, the coupling that your new architecture wants to relieve: the UI needs a “page-shaped” read model rather than directly joining across operational tables.

## Domain separation and database boundaries for battle, dungeon, and overworld

Your instinct to treat dungeon vs overworld vs battle as meaningfully different “rule systems” is correct. The architectural question is whether those differences should map to **separate databases** or to **separate subdomains within one analysis ownership boundary**.

### Favor boundaries by enduring ownership, not by transient phase

A standard microservice/DDD guidance is: keep each service’s data private and expose it via APIs or events rather than shared direct DB access. This is the core of the “database per service” pattern: it emphasizes loose coupling and independent evolution, while warning that cross-service queries become hard and often require patterns like CQRS/materialized views. citeturn0search0  

For SAVOR, that implies a stable split that matches ownership:

* State lineage and artifacts (savestates, object store references)
* Execution/orchestration (workflow steps, jobs, leases, retries)
* Analysis outputs (seed probes, navigation results, battle-branch graphs, encounter candidates)
* Authoring/configuration (plans, presets, encounter lookup tables, map metadata)
* UI read models (denormalized query tables)

This is also compatible with “vertical slice” thinking: organize around features and business flows so that each slice can evolve independently without forcing global abstractions too early. citeturn2search1  

### Answering your “should these be separate DBs?” question

A practical recommendation is:

* Keep **one Analysis ownership boundary**, but treat “battle analysis,” “dungeon navigation analysis,” and “overworld navigation analysis” as **separate namespaces/schemas/modules** inside it.
* Split **physically** (separate DB files) only when there is a concrete need: performance isolation, dramatically different retention policies, different storage types (e.g., graph store for pathfinding), or a desire to distribute those analyses across processes. The database-per-service pattern explicitly calls out that different services may choose different storage technologies (e.g., graph DBs for graph-like data). citeturn0search0  

In a SQLite-based modular monolith, you can implement *physical separation* without distributed complexity by using `ATTACH DATABASE` to mount multiple DB files (e.g., `state.db`, `analysis.db`, `exec.db`, `authoring.db`, `ui_read.db`) into one connection for specific tasks (like projection building). citeturn3search3  

However, you should plan around a real SQLite constraint: **foreign keys cannot cross schema boundaries**, which includes attached databases. That means you won’t be able to enforce referential integrity across DB files using SQLite FKs, and must rely on IDs + application invariants/events instead. citeturn4search0  

So, separate databases for dungeon/battle/overworld are viable if you want to isolate storage and churn, but they should be “soft-coupled” through events and read-model projections rather than cross-database joins or FK constraints. citeturn0search0turn0search2  

## Data model additions for seed probes, battle turn graphs, and encounter planning

This section focuses on the missing “battle/dungeon/overworld analysis tables” that your new requirements imply, and how they can map onto what the current repo already does.

### Seed probes as canonical derived facts

The current repo already stores probes (`seed_probe`) and deltas (`seed_delta`) keyed to a savestate, and stores the input frame that realizes each delta. fileciteturn3file0L1-L1 fileciteturn26file0L1-L1  

To support dungeon/overworld timing analysis and “probe at battle entry frame,” you will likely want to extend the analysis model from “probe produces deltas” to “probe produces *candidates* that are consumable by multiple downstream analyses.” Concretely, a `seed_candidate` table (or view/projection) becomes valuable even if it is logically derivable from existing rows:

* candidate_id
* seed_probe_id
* input_frame_hex (or artifact pointer)
* neutral_seed
* seed_delta
* realized_rng_seed (denormalized = neutral + delta)
* classification (grid/unique/neutral)
* provenance metadata (probe phase, job_id/job_set_id that produced it)

This keeps seed probes canonical while making downstream joins cheaper and making the UI DB easier to build (especially if you often need `realized_rng_seed` and the input frame together). It’s also aligned with the existing `seed_probe_winners` dedupe approach, which already treats “realized delta” as a key for uniqueness. fileciteturn18file0L1-L1  

### Battle turns as a graph: what you already have vs what you likely want

You asked for “tables to store state per turn, results per turn, and a way to point to the previous turn.”

The battle single turn runner already yields:

* Per-turn action identity (`action_key`) and fake-attack counts per job. fileciteturn28file0L1-L1  
* Per-turn results including end RNG seed and continuation savestate ID (when it reaches the next turn). fileciteturn28file0L1-L1 fileciteturn29file0L1-L1  
* An explicit previous-turn link via `jobs.parent_job_id` when creating next-wave jobs. fileciteturn31file0L1-L1 fileciteturn29file0L1-L1  

So the “turn graph” already exists implicitly as:

*Nodes*: jobs (one job = one attempted turn execution)  
*Edges*: `parent_job_id` (child turn depends on parent turn)  
*State snapshots*: continuation savestates (output savestate IDs) saved for ReachedNextTurn or Victory outcomes. fileciteturn29file0L1-L1  

What’s missing is a **battle-focused query model** that makes turn graphs easy to traverse, filter, and render without parsing INI blobs or crawling job event payloads at runtime.

A good vertical-slice fit is to generate a **battle-turn projection** (ideally into the UI Read DB, not necessarily into the authoritative analysis DB). That projection would denormalize and index the key columns you care about:

* battle_run_id (or root_job_set_id / explorer_run_id)
* job_id
* parent_job_id
* turn_index
* action_key
* seed_candidate_id (or delta_seed_id)
* fake_attacks_used_before / this_turn / total
* rng_seed_end
* outcome (ReachedNextTurn/Victory/Defeat/MaterializeFailure/Timeout…)
* output_savestate_id (nullable)
* predicate summary fields (passed/total/abort)
* artifact refs (applied input tape artifact, battle context artifact if captured)

This projection-centric approach matches the repo’s current reality: authoritative results are already stored “append-only-ish” as job events (`RESULTS`, `PROGRESS`), while the UI layer polls and assembles page DTOs through `DataService`. fileciteturn41file0L1-L1  

It also matches the broader CQRS rationale: read models can be optimized separately from write models and are often eventually consistent projections of the underlying authoritative stores. citeturn0search2  

If you later need a *canonical* battle analysis store (not merely UI projections), you can promote the same projection into the Analysis DB; the schema will look similar, but its retention and versioning rules should be explicit.

### Dungeon exploration: two parallel analyses sharing a common anchor

Your dungeon requirements imply two parallel result streams: “shortest path to load zone” and “encounter schedule options,” both anchored to (start state, map/load-zone objective, and a method version).

For shortest-path work, you explicitly described a pseudo-A* search. The classic A* paper by entity["people","Peter Hart","computer scientist pathfinding"], entity["people","Nils Nilsson","ai researcher"], and entity["people","Bertram Raphael","ai researcher"] provides the canonical basis for heuristic shortest-path search in graphs, which is the right conceptual model even if your dungeon space is a layered 2D manifold with overlaps. citeturn1search0turn1search4  

A practical analysis schema for dungeon navigation needs:

* A navigation request (inputs, versions, constraints)
* One or more candidate paths (cost metrics + reproducible input tape/path encoding)
* Optional “tech usage” markers (skips, fast ramps) and validation artifacts

Because your dungeon space is “pseudo-2D planes connected, may overlap,” you likely want to store the *effective graph* (nodes/edges) in Authoring/Lookup form, and store only computed paths in Analysis.

For the encounter schedule work, you described a deterministic mapping from starting RNG seed → encounter composition using lookup tables. That suggests:

* Encounter timing analysis produces candidate encounter “windows” (frame ranges, delay offsets, derived seed probe IDs, resulting encounter IDs/formations).
* Operator selection is required (manual choice of encounter).
* After selection, you run inputs to reach the encounter start and capture a new savestate, then re-probe and enter battle turn exploration.

That is naturally modeled as:

* analysis.encounter_candidate_set (rooted at a navigation candidate or a specific “walk segment to next load zone”)
* analysis.encounter_candidate (each candidate provides: delay offset, seed_probe_id, deterministic encounter id/formation id)
* authoring.encounter_selection (operator picks one candidate, possibly with rationale tags)
* state.derived_savestate records the encounter-entry savestate derivation (input tape artifact + provenance)

This is structurally identical to your first pathway: state → probe → candidate set → select → derive next state → probe → battle. The difference is only what generates the candidate set.

### Overworld exploration as a separate navigation model, not necessarily a separate database

The repo already defines an overworld breakpoint table and includes a RandomEncounter event, which is consistent with the need to observe/measure encounter behavior in overworld space. fileciteturn39file0L1-L1  

However, no parallel “dungeon breakpoint table” is present in the breakpoint definitions, so dungeon-specific instrumentation (if needed) appears to still be open design/implementation. fileciteturn39file0L1-L1  

Given your stated rules (“3D + height affects whether you get a random encounter”), overworld navigation likely needs:

* A different state representation for path nodes (x,y,z and possibly zone/region ID)
* Different heuristics in the A* (or alternative) search
* Different “encounter model hooks” in the simulator (more than just a 2D step counter)

But architecturally, it still fits the same Analysis ownership boundary: a different *nav_engine_kind* and different candidate schema fields—not necessarily a different DB—unless you anticipate dramatically different storage requirements (e.g., a spatial index or a specialized 3D navigation graph store).

## Orchestration and event contracts for composable workflows

Your requirement “adhere to event-driven vertical slice databases but leave room so that it is easy to pull the data into the UI database” maps directly to a well-known pairing:

* Database-per-service (or at least strict private tables per owned domain) for write-side independence. citeturn0search0  
* CQRS/materialized views for cross-domain UI queries, maintained via events. citeturn0search2turn3search2  

### Why move beyond table polling and generic triggers

The current system already uses persisted triggers to chain phases (seed probe phases; seed probe → battle job set). fileciteturn19file0L1-L1 fileciteturn26file0L1-L1 fileciteturn29file0L1-L1  

This is effective, but it has the hazard you described: downstream consumers can become “whoever noticed the row first,” unless you carry explicit routing metadata.

A more explicit orchestrator model, conceptually similar to an orchestration-based saga, makes the routing and intended consumer first-class rather than implicit. The Saga pattern is a standard approach for spanning multiple independently owned stores and actions without distributed transactions, typically by sequencing local transactions plus messages/events. citeturn1search1  

For SAVOR specifically, the key benefit is that you can represent complex flows like:

* DungeonNav (encounters off) → EncounterCandidates (delays + probes) → OperatorSelectEncounter → MaterializeEncounterState → SeedProbe → BattleTurnWaves → MaterializePostBattleState → repeat

…without encoding that phase ordering into the database schema itself.

### Reliable propagation: transactional outbox as the backbone

Once you split ownership across multiple stores/services, you need reliable publication of events that represent committed state changes. The Transactional Outbox pattern is the canonical way to atomically update local state and enqueue an event for publication without requiring 2PC across DB and message broker. citeturn0search1  

In SAVOR terms:

* State DB writes: “DerivedSavestateCreated” (with object ref, provenance, compatibility stamps).
* Analysis DB writes: “SeedProbeCompleted,” “DungeonPathComputed,” “EncounterCandidatesProduced,” “BattleWaveCompleted.”
* Execution DB writes: “WorkflowStepReady,” “JobsEnqueued,” “WorkflowStepCompleted.”

Each service writes its own outbox row in the same transaction as its domain update, then a relay publishes events and the UI Read DB projector consumes them. citeturn0search1turn0search2  

### Designing workflow primitives to subsume the current job_set/trigger model

The current repo’s `job_sets` plus hierarchical children and `jobs.parent_job_id` already form a durable execution graph. fileciteturn20file0L1-L1 fileciteturn31file0L1-L1  

A practical “minimal orchestrator” evolution is therefore:

* Keep jobs/job_sets as the unit of worker scheduling and retry.
* Add explicit **workflow instances** and **workflow steps** that own routing intent:
  * step kind (SeedProbe, BattleTurnWave, DungeonNav, EncounterAnalyze, StateDerive…)
  * input state(s)
  * output state(s) and/or output artifact(s)
  * the job_set_id that backed execution
  * explicit dependency edges between steps

This replaces the “generic triggers scanning completions” model with explicit step transitions, while still permitting triggers as an internal mechanism (e.g., “ALL_FINISHED within this job_set tree”) if you want to reuse it under the hood.

## UI read-model strategy and incremental migration plan

### CQRS-style UI DB is the cleanest answer to “easy to pull data into the UI database”

Both the database-per-service guidance and CQRS guidance emphasize that cross-database joins are difficult and that a common solution is to maintain materialized read models updated from events. citeturn0search0turn0search2turn3search2  

This aligns with your goal: “leave room so that it is easy to pull the data into the UI database.”

In SAVOR terms, the UI read DB should own:

* “Page rows” for jobs/job sets/workflows, pre-aggregated for filtering and counts.
* Seed probe summaries: probes, candidate counts, selected winners, links to source savestate and downstream consumers.
* Battle turn trees: a denormalized “turn node” table keyed by job_id containing parsed job INI/results fields and parent_job_id edges (so the UI can render a battle tree without repeatedly parsing job event payloads).
* Dungeon/overworld navigation results: path candidates, encounter candidate sets, selected encounter, and the derivation lineage to the encounter-entry savestate.

This is precisely the kind of separation CQRS is intended to enable: you can optimize query tables separately from write tables and accept eventual consistency for UI views. citeturn0search2turn3search2  

It also directly addresses the coupling visible in the current `DataService`, which today already spans multiple domains (jobs, job sets, seed probes, savestates, authoring templates/presets). fileciteturn41file0L1-L1  

### Incremental migration that matches the repo’s existing direction

A low-risk migration plan that leverages what’s already present in the repo is:

Start with logical boundaries in one DB file: keep the current SQLite schema but enforce ownership in code (module boundaries), and begin emitting typed domain events (even if to an in-process bus).

Introduce UI projections first: build a projector that consumes job/job-event/seed-probe/battle-wave completion events and fills a UI read schema. This can be done without splitting physical databases and immediately reduces UI fan-out queries. citeturn0search2turn3search2  

Split physical databases only after boundaries stabilize: if/when you move to multiple SQLite files, use `ATTACH DATABASE` for projection-building convenience, but avoid relying on cross-DB foreign keys since SQLite explicitly does not support foreign keys that cross schema boundaries. citeturn3search3turn4search0  

This approach gives you the architectural benefits you want (bounded ownership + event-driven projection + composable workflows) while retaining the pragmatic advantages of the repository’s existing durable job graph and battle-turn semantics (jobs as turn nodes; parent_job_id edges; continuation savestates as durable state snapshots). fileciteturn29file0L1-L1 fileciteturn31file0L1-L1
1. To all phase repos, add/use field to store a results_ini:
    - TasMovieRepo
    - SeedProbeRepo
    - 
1. Setup all DBCodecs for flow, inputs, outputs, etc:
    - TasMovieRepo
    - SeedProbeRepo
    - ExplorerRunRepo
1. Rename ExplorerRun (everything) to BattleRun for clarity.
1. Added some keys for specifying what types of progress we want to return from certain scripts, still need to:
    - Add these keys to the SimCoreWorker and PhaseScriptVM
    - Allow the user to specify the values for those keys
    - Change the progress sink to be more general? or something.
1. try to remove frame limiter and test for consistency
1. work on the job storage database:
    - Optional reader pool: route OpType::Read to a small read thread pool with separate connections; keep single writer for OpType::Write
    - Micro-batching for writes: a short (e.g., 2-5 ms) coalescing window in the writer to group multiple pending write tasks into one transaction when safe.
    - Admin ops: Checkpoint(), Vacuum(), IntegrityCheck() as OpType::Admin.
    - Idempotency on hot ingest paths (e.g., predicate results keyed by (run_id, path_id, pred_id, turn)).

New shape to implement:
 - DB = source of truth. We add sim-centric tables: job_sets, jobs, job_events, workers, artifacts, and program_kinds.  All DB interaction should ultimately go through DBService.
 - PhaseCoordinators become DB clients. They only: setup_job_set -> queue_jobs -> poll_progress -> retrieve_results, and can register next-phase triggers in the DB.
 - Program DB codecs per ProgramKind. Each kind ships encode/decode job, encode/decode progress, encode/decode results. Coordinators uses these for all DB <-> wire conversions.
 - WorkerCoordinator replaces ParallelPhaseScriptRunner. One controller thread + N ProcessWorker threads; each thread owns a SimCoreWorker.exe child (one Dolphin per process).
 - Scheduling via DB. Pick QUEUED jobs by effective priority (per-kind base + aging + per-job overrides). Use leases; renew on heartbeats; requeue on expiry.
 - Program swaps = process restart. Reuse a child only if next job's ProgramKind matches; otherwise kill & spawn fresh (simple, clean isolation).
 - Online dedupe for SeedProbe. Winners table (job_set_id, delta_id) with UNIQUE constraint: first success becomes SUCCEEDED_WINNER; queued siblings become SUPERSEDED; later successes -> SUCCEEDED_DUPLICATE. Progress = winners_found / total_deltas.
 - Domain repos stay authoritative. SeedProbeRepo / TasMovieRepo / ExplorerRunRepo define WHAT to run and store domain summaries. Scheduling uses jobs; results recorded back into the domain repos (e.g., SeedDeltaRepo as a results ledger, not a queue).
 - Artifacts are content-addressed. Big inputs/outputs live once in artifacts and are referenced by jobs/results. Artifacts use the object_ref table.
 - Idempotency via fingerprints. jobs.fingerprint UNIQUE prevents duplicate enqueue across restarts.
 - Triggers in DB. After any terminal job update, WorkerCoordinator evaluates triggers (e.g., "all succeeded") and enqueues next phases via the relevant program codec.

## Milestone 0 - Foundations & guardrails
#### Goals:
 - Keep current flows working while we add a DB-centric path.
 - Minimal intrusion; retire ParallelPhaseScriptRunner only after parity.
#### Deliverables:
 - Enable WAL & pragmas (if not already) and add a tiny "DBHealth" check on startup.
 - Add a global schema_version migration row.
 - Decide a stable clock source (Coordinator monotonic) to timestamp leases, progress, events.

## Milestone 1 - Core scheduling schema (sim-centric)
#### Tables (new): 
 - program_kinds(kind_id PK, name, base_priority, learned_spawn_ms, created_at)
 - job_sets(job_set_id PK, purpose, program_kind, created_by, created_at, domain_ref_kind, domain_ref_id)
 - domain_ref_* points to SeedProbeRepo row, TasMovieRepo row, ExplorerRunRepo row, etc.
 - jobs(job_id PK, job_set_id FK, program_kind, fingerprint UNIQUE, priority, state, attempts, max_attempts, claimed_by_worker, lease_expires_at, queued_at, created_at)
 - state: PLANNED | QUEUED | CLAIMED | RUNNING | SUCCEEDED | FAILED | CANCELED | SUPERSEDED | SUCCEEDED_WINNER | SUCCEEDED_DUPLICATE
 - job_events(event_id PK, job_id FK, ts, event_kind, payload_json)
 - artifacts(artifact_id PK(hash), kind, bytes, meta_json, created_at)
 - workers(worker_id PK, pid, current_program_kind NULL, last_heartbeat, state)
#### Views:
 - v_job_queue_ready (QUEUED and not leased)
 - v_job_set_progress (by set; computed %)
 - (SeedProbe only) v_winners_progress (winners found / total deltas)
#### Notes:
 - fingerprint = hash(program_kind + program_version + domain_ref + payload blueprint) -> idempotent enqueue.

## Milestone 2 - Program DB codecs (per ProgramKind)
*Your note honored: SeedProbeRepo / TasMovieRepo / ExplorerRunRepo remain the domain definitions. Scheduling uses jobs; results land back in their domain repos.*
#### Each ProgramKind gets a ProgramXDB.{h,cpp} with:
 - encode_job_into_db(job_set_id, job_spec...) -> job_id
 - decode_job_from_db(job_id) -> WorkerDispatch (terminal PSJob payload, fully formed PSContext, ready to send)
 - encode_progress_into_db(job_id, ProgressDto)
 - encode_results_into_db(job_id, ResultDto)
 - decode_progress_from_db(job_id|job_set_id) -> ProgressDto
 - decode_results_from_db(job_id|job_set_id) -> ResultDto
#### Mapping to your repos:
 - ##### SeedProbe:
    - Inputs: read from SeedProbeRepo (probe metadata). For the grid, don't use SeedDeltaRepo to queue; instead, generate jobs directly into jobs.
    - Progress: sent to JobEventsRepo (each job should have no progress).
    - Results: on success, write a record to SeedDeltaRepo (this is now your results ledger).
 - ##### TasMovie:
    - Inputs: read from TasMovieRepo.
    - Progress: sent to TasMovieRepo.
    - Results: persist summary to TasMovieRepo.
  - #### ExplorerRun:
    - Inputs: ExplorerRunRepo row is the domain root; individual exploration jobs are jobs.
    - Progress: ExplorerRunRepo::AppendProgress() can keep the human log
    - Results: Results go into the explorer_run repo.

## Milestone 3 - Online dedupe for SeedProbe (no separate reduce step)
#### Tables (new):
 - seed_probe_winners(job_set_id, delta_id, winner_job_id, result_artifact_id, metrics_json, created_at, PRIMARY KEY(job_set_id, delta_id))
#### Codec behavior:
 - On first successful probe for (job_set_id, delta):
 - Transaction: insert into seed_probe_winners (unique enforces "first wins"), set job SUCCEEDED_WINNER, write SeedDeltaRepo row with is_unique=1, and optionally mark queued siblings SUPERSEDED.
 - On later successes: mark SUCCEEDED_DUPLICATE, write SeedDeltaRepo row with is_unique=0.
 - Scheduler excludes queued jobs whose (job_set_id, delta_id) already has a winner (via NOT EXISTS on the winners table).
 - Progress uses winners_found / total_deltas.
 - Result: live de-duplication while the batch runs; zero additional phase.

## Milestone 4 - WorkerCoordinator (no ParallelPhaseScriptRunner)
#### Responsibilities:
 - Scheduling: pick next QUEUED job by effective priority = base_priority(kind) + aging + job.override.
 - Leasing: claim job (CLAIMED, lease_expires_at), renew on heartbeat; return to QUEUED if lease expires.
 - Process control: spawn/kill SimCoreSandbox/SimCoreWorker child processes; one process per worker thread (as you do now).
 - Reuse a child only if next job's ProgramKind matches; else terminate and spawn fresh (you prefer process refresh over in-process resets).
 - Dispatch: resolve WorkerDispatch via the Program Registry decode; ship over IPC.
 - Progress/Results: route back into DB using the same program's encode_* functions.
 - Phase chaining: after any terminal state, evaluate triggers (next milestone).
#### Straw-man knobs:
 - max_concurrent_processes, child_launch_timeout_ms, child_shutdown_grace_ms
 - heartbeat_interval_ms, lease_seconds, aging_factor
 - idle_keepalive_ms for warm reuse within same ProgramKind
#### Reuse:
 - Keep your existing ProcessWorker class (pipes, reader thread) and retarget it under WorkerCoordinator.

## Milestone 5 - DB phase coordinators (compartmentalized)
#### API (per coordinator):
 - setup_job_set(domain_ref_id, options) -> job_set_id
 - queue_jobs(job_set_id, logical_specs) (bulk)
 - poll_progress(job_id|job_set_id) -> ProgressDto
 - retrieve_results(job_id|job_set_id) -> ResultDto
 - register_trigger(scope, scope_id, condition, action_kind, action_args)
#### Notes:
 - Use ProgramDBCodecs as much as possible, and domain repos if needed (SeedProbeRepo, TasMovieRepo, ExplorerRunRepo) to read/validate inputs and to persist domain-specific summaries.
 - All execution goes through jobs/job_sets.

## Milestone 6 - Next-phase triggers (DB, not in-process listeners)
#### Table (new):
 - triggers(trigger_id PK, scope ('job'|'job_set'), scope_id, condition_json, action_kind, action_args_ini, active)
#### Flow:
 - WorkerCoordinator evaluates triggers after each job terminal update:
 - If condition satisfied (e.g., ALL_SUCCEEDED, or winners_found == total_deltas), atomically deactivate and invoke program-specific encode_job_into_db to enqueue the next phase.
#### Example:
 - SeedProbe: Neutral -> trigger on single job complete -> Grid phase -> trigger on job_set complete -> Unique phase -> trigger -> ensure SeedDelta from winners and mark is_unique -> clear out winners table.
 - Explorer: Explore -> trigger -> Validate job_set.
 - phase transitions example: If user want to automatically transition between phases: TasMovie -> trigger on each completed job -> SeedProbe -> trigger on completion of unique phase -> Explorer (using some predetermined Explorer Settings id)

## Milestone 7 - Observability & ergonomics
#### Add:
 - job_events coalescing (limit to ~5-10/s per job).
 - worker_events (spawn, crash, exit, recycle).
 - CLI/Sandbox: progress panes that read from v_job_set_progress and program-specific decode_progress_from_db.
 - Simple dashboard of queue depth per ProgramKind and average spawn/run times (computed from events).

## Milestone 8 - Migration & deprecation
#### Plan:
 - SeedProbe: phase coordinator switches to DB-phase API, encoding grid as jobs (no SeedDeltaRepo::BulkQueue for queuing). SeedDeltaRepo remains the results ledger (with is_grid, is_unique, complete semantics preserved for history).
 - TasMovie & Explorer: same pattern-keep domain repos authoritative; route execution via jobs/job_sets.
 - When parity confirmed, retire ParallelPhaseScriptRunner entry points from phase code.

## Cross-cutting decisions & risks:
 - Idempotency: enforce via jobs.fingerprint UNIQUE; codecs must compute fingerprints deterministically.
 - Crash safety: at WorkerCoordinator startup, recover expired leases and reap orphan PIDs (store worker pid in workers table).
 - Event volume: throttle progress; batch DB writes with your existing DbService.
 - Starvation: apply aging to effective priority; online dedupe naturally shrinks the queue.

## Suggested order of work (concrete):
1. Schema & migrations for Milestone 1 + winners table; minimal indexes.
1. Program Registry extension: register codec vtables for SeedProbe, TasMovie, BattleRunner (job decode/encode, progress, results).
1. WorkerCoordinator MVP: claim->spawn->dispatch->report for one ProgramKind; then generalize.
1. SeedProbe end-to-end: Grid -> online dedupe -> results to SeedDeltaRepo; Sandbox progress via DB.
1. Add triggers and convert one multi-phase flow (e.g., Explorer: explore -> validate).
1. Port remaining coordinators to DB-phase API.
1. Remove runner references, keep ProcessWorker under Coordinator.


UI implementation plan:
0) Readiness & boundaries
Goal: Ensure the UI can depend on stable contracts.
Inventory repos you’ll call from UI: JobsRepo, JobEventsRepo, WorkersRepo, TriggersRepo, ProgramKindsRepo, ObjectStore, domain repos (TasMovie/SeedProbe/ExplorerRun/etc.).
Confirm ProgramDBCodecRegistry enumeration works at runtime.
Decide default polling cadences and time windows for lists (e.g., last 24h).
Done when: You can list available ProgramKinds and read a few rows from each main table via repos in a throwaway console.

1) App shell (ImGui)
Goal: A stable, dockable UI host that never blocks the DB thread.
Add ImGui docking+viewport; persistent layout (save to IniDoc).
Global status bar: DB connected badge, env, last refresh timestamp.
Left nav with panes (disabled until backed by data).
Done when: The app opens with docking, saves/restores layout, shows “connected” with a heartbeat timestamp.

2) DataService (UI-agnostic contracts)
Goal: A thin async layer that exposes paging/polling DTOs.
Define PagedQuery<T> (keyset cursor), ListScope (filters/time window), Lightweight DTOs for jobs/events/workers.
Implement keyset pagination using (updated_at, id); avoid OFFSET for big tables.
Background polling with coalescing; deliver snapshots to UI via thread-safe mailbox.
Done when: You can fetch “Jobs (last 24h, non-terminal)” page 1?N and back, outside any UI code.

3) Jobs pane (MVP list)
Goal: First real table.
ImGui table with clipper; columns: id, job_set_id, program_kind, state, updated_at, progress (codec summary).
Page controls (Prev/Next), quick filter by job_set_id, program_kind, state.
Auto refresh every 2–5s; preserve selection & scroll.
Done when: You can browse jobs smoothly across pages without stutter.

4) Job details drawer
Goal: On-demand heavy data.
Right-side details: Overview tab (all columns), Events tab (lazy), Artifacts tab (refs only), Payload/Results (lazy load via codec summary).
Deep links to related entities (job set, artifacts).
Done when: Clicking a row shows details; heavy blobs fetch only when the tab opens.

5) Job actions
Goal: Operator control for a single job.
Add Requeue, Cancel, Bump priority with confirmation modals.
Surface repo errors to a non-blocking toast/log area.
Append a JobEvents entry and reflect new state in list.
Done when: You can requeue/cancel/bump and see state changes propagate.

6) Phase Builder (MVP)
Goal: Create job sets from codec blueprints.
Render form from BlueprintIni schema (types, defaults, validation).
Preview panel: counts, artifacts to create, estimated payload size.
Submit: write job_set + jobs via repos; link to JobSet overview.
Done when: You can author and enqueue a basic job set end-to-end.

7) Auto-queue next phase
Goal: Chain phases without manual steps.
Add auto_queue_next checkbox in the builder.
If enabled: render Next Phase subform using the next codec’s schema (hard-coded map is fine).
On submit: create a trigger (ALL_FINISHED) with action_args carrying next_phase_kind and serialized next_phase_blueprint (+ fingerprint).
Add a minimal Triggers admin pane (list, inspect, delete).
Done when: Completing a job set auto-creates the next job set with the expected args.

8) Job Events stream
Goal: Live tail for diagnostics.
Events table with “Follow tail” toggle; filter by job_id or job_set_id.
Keyset paging backward in time; client clipper for visible rows.
Done when: You can tail events for a running job set and scrub history without UI stalls.

9) WorkerStatus pane
Goal: Fleet awareness.
Workers table: id, host, pid, current program_kind, job_id, last heartbeat, lease renew lag.
Draining toggle (if supported) and “nudge requeue stale leases” action (guarded).
Done when: You can spot unhealthy workers at a glance and toggle draining.

10) Catalog panes (lightweight)
Goal: Domain browsing with cross-links.
TasMovies, SeedProbes, ExplorerRun (BattleRuns), BattleContexts, Savestates, BattlePlans, PredicateSets.
Each: small list with filters + detail view; deep links to related jobs/artifacts.
Done when: You can navigate from a catalog item to its producing job and artifacts.

11) Artifacts/ObjectStore viewer
Goal: Inspect large outputs safely.
List artifacts by digest/name/type/time; filter by refcount or recent.
Preview text/json/ini/csv inline; binary shows metadata; “materialize temp” action.
Done when: You can open a result artifact from a job and view its contents inline (when textual).

12) UX polish & persistence
Goal: Make it pleasant daily.
Saved filters, column visibility/order, and polling cadences via IniDoc.
Global search box (id/fingerprint).
Keyboard nav (J/K to move, Enter to open).
Done when: UI remembers operator preferences and speeds up common flows.

13) Performance & resilience hardening
Goal: Handle load gracefully.
Incremental refreshes keyed on last_updated_at.
In-memory caches with TTL; dedupe identical snapshots.
Backpressure: pause polling when CPU/GPU > threshold or when window unfocused (optional).
Done when: Profiling shows stable frame times with 100k+ jobs across pages and sustained event flow.

14) Test & demo datasets
Goal: Repeatable validation.
Seed small/medium/large fixture datasets.
“Demo scripts” that run a short pipeline to exercise auto-queue and artifact flows.
Done when: One command populates data; manual test plan can be run in ~10 minutes.

15) Optional Qt track (parallel, no rush)
Goal: Richer tables when/if needed.
Reuse DataService contracts.
Implement QAbstractTableModel for Jobs and Job Events with keyset fetching.
Add table niceties (frozen columns, advanced sort, export/print).
Done when: A separate Qt viewer can browse the same data with enterprise table UX.

Guardrails & priorities
Land Steps 1->5 quickly for a functional ops console.
Steps 8->9 unlock real value (authoring + auto-queue).
Keep DataService pure and UI-agnostic; everything DB-touching routes through repos/DBService.
All heavy fields (payloads/results) are lazy—never fetch in lists.
If this sequence works for you, I can next propose the DataService DTOs and keyset cursor shape (very small, decisive spec) so Step 2 starts on solid ground.

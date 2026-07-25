# 06 - Phase, Job, and Artifact Integration

## Status

Future integration plan. This document maps the logical Navigation workflow onto current SAVOR
boundaries. The draft Navigation Context capture program and its `a101b` export exist; the Navmesh Survey
and the later logical steps below remain contract proposals rather than registered program kinds or
implemented database schemas.

The closest existing runtime pattern is Battle Context capture. The hidden `dungeon_explorer` workflow
unit is only a placeholder with entry and terminal savestate bindings; it does not contain the steps below.
The current worker path runs fixed `PhaseScriptVM` programs, and there is no local CPU workflow executor
lane yet.

## Logical Workflow

| Order | Logical step | Execution class | Principal output |
|---:|---|---|---|
| 1 | `nav.capture_context` | Dolphin/PhaseScript worker | `.nctx` export, matching Survey bootstrap savestate, and source lineage |
| 2 | `nav.materialize_content` | CPU/domain and DiscIO/parser services | `DiscImageIdentity`, manifest, `NavigationContentBundle` |
| 3 | `nav.build_world` | CPU/domain | Static `NavigationWorldModel` and traversal graph |
| 4 | `nav.explore_geometry` (Navmesh Survey worker portion) | Anchor-establishment wave followed by parallel Dolphin spatial probes | Verified positional anchors, door constraints, and `NavigationGeometryObservation` set |
| 5 | `nav.build_refinement` | CPU/domain | Immutable `NavigationWorldRefinement` |
| 6 | `nav.capture_prediction_start` | Future Dolphin/PhaseScript worker | Reset-qualified `NavigationPredictionStart`; separate from Survey bootstrap |
| 7 | `nav.search_predicted_outcomes` | `SavorPredict` future service/process | Candidate `NavigationPredictionResult` set |
| 8 | Explicit user/result selection | `SavorQt`/workflow command | Selected prediction result reference |
| 9 | `nav.solve_controls` | Dolphin-backed solver workers | `NavigationControlSolveResult` |
| 10 | `nav.validate_route` | Clean Dolphin worker | `NavigationValidationResult` and terminal state |
| 11 | `nav.publish_result` | CPU/domain/persistence | Final workflow result and UI projection |

`nav.search_predicted_outcomes` is the retained logical name. It must not be renamed casually while the
contract is being distributed across planning documents.

## Step Contracts

### `nav.capture_context`

Inputs:

- source savestate/runtime binding;
- expected game/runtime identity;
- output savestate path; and
- runner safeguard.

Outputs:

- encoded `.nctx` result;
- matching output savestate used as the Navmesh Survey common bootstrap;
- source/result artifact lineage; and
- implemented `Completed` or `Failed` outcome plus failure code.

The Survey names the exact exported `.nctx` and matching output savestate that provide its starting
position. It never resolves an implicit latest result or substitutes another context contract.

### `nav.materialize_content`

Inputs name an exact disc-image locator plus expected identity policy and an area identity. The step
streams the ISO hash, builds/uses a disc manifest, extracts exact internal files through Dolphin DiscIO,
parses them through SPICE/ALX, and emits a content-addressed `NavigationContentBundle`.

This is a CPU/I/O domain operation. It must not be disguised as a simulator PhaseScript job merely because
Dolphin supplies DiscIO libraries.

### `nav.build_world`

Deterministically converts the content bundle into SAVOR-owned scenario geometry, starts, region data,
triangle graph, static encounter selectors, profile facts, and diagnostics. It records coordinate-policy,
builder, and schema versions.

### `nav.explore_geometry`

Consumes one explicitly named Navigation Context `.sav`/`.nctx` pair as the common per-area bootstrap.
The source savestate is never overwritten.

The first implementation has two worker waves:

1. anchor-establishment jobs start from the common bootstrap, cross required in-area doors, and publish
   positional anchors only after replaying the position from the common bootstrap and passing the game's
   ordinary teleport-and-settle behavior; and
2. parallel spatial-probe jobs reload that same bootstrap, teleport to a verified anchor, settle, and test
   assigned surfaces, boundaries, and portals.

Encounter suppression writes one zero byte at `0x8030b7ad`. Trigger commit is normally bypassed at `0x80117e8c`
with word `0x48000018`; a door job temporarily restores the original `0x480F86C5` only around the intended
interaction and immediately suppresses again. It verifies the selected TBLID and physical crossing. A
job may read-modify-write a known lock BitVar to its unlocked value, provided it records the original and
override values and marks the portal `initially_locked`.

Workers emit positional anchor records and immutable spatial observations. They do not emit per-anchor
savestates, serialized ground-selector replay state, or timing evidence.

### `nav.build_refinement`

Deterministically reduces selected anchor and observation IDs into a per-area spatial refinement:
passability, collision boundaries, refined adjacency, door portals and lock constraints, and tested,
untested, contradictory, or unresolved coverage. New evidence creates a new refinement version.

### `nav.capture_prediction_start`

Future prediction-only capture. It consumes an explicitly selected unmodified runtime/savestate after the
Survey refinement is available and emits the reset/readiness/temporal state required by a selected
predictor model. It is not the implemented Survey-bootstrap `nav.capture_context`, and no Survey anchor or
modified Survey state can satisfy it.

### `nav.search_predicted_outcomes`

Submits one future `NavigationPredictionStart`, content/world/refinement IDs, objective, bounds, and model
bundle to an asynchronous `SavorPredict` navigation boundary. It emits immutable candidates with search
completeness and terminal reasons. The workflow does not treat the Survey-bootstrap `.nctx` or current CLI
as that final contract.

### Explicit result selection

Selection is an authored workflow command/event referencing one prediction result ID. A deterministic
automatic policy, if later supported, is itself versioned authoring data. There is no implicit "best" or
"latest" candidate.

### `nav.solve_controls`

Consumes the selected world-space plan and uses Dolphin-backed trials to produce a controller tape and
camera realization. Runtime modifications and trigger behavior are explicit per solver mode; this does not
inherit the Navmesh Survey's door-only suppression toggle or resolve broader trigger characterization. A
tape intended for clean execution must retain trials and provenance separately.

### `nav.validate_route`

Restores the clean source lineage, applies no exploration patch, executes the selected tape, and compares
runtime evidence to the prediction. Battle or field transition ends the epoch. Its terminal savestate may
be passed to another workflow but becomes a new navigation context only after capture qualification.

### `nav.publish_result`

Builds final relational summaries and UI projections from immutable references. It never collapses
prediction, control, and validation into one mutable status blob.

## Project Ownership

| Project | Responsibility |
|---|---|
| SPICE | AKLZ and MLD/SCT/ECT parsing and parser diagnostics |
| Dolphin DiscIO | Exact internal-file discovery/extraction from selected image |
| ALX parser/tooling | Assigned game-data file parsing |
| `SavorNavigation` | Content normalization, profiles, world/graph, survey-anchor/candidate generation, observation interpretation, refinement, static analysis, compatibility, and spatial result projection |
| `SavorCore` | Runtime reads, paused expected-original writes, reversible instruction toggling, inputs, teleport/settle checks, savestates, spatial telemetry, capture and probe PhaseScripts |
| `SavorWorker` | Isolated Dolphin execution for capture, exploration, control solving, and validation |
| `SavorPredict` | Future planning-level temporal/outcome search and candidate production |
| `SavorWorkflow` | Claiming, dispatch, fan-out, selection waits, transitions, retries, and terminal advancement |
| `SavorDb` | Authored specifications, execution lifecycle, analysis records, artifact lineage, and UI projections |
| `SavorQt` | Workflow authoring/monitoring, candidate comparison/selection, and product Navigation host |
| `SavorQt3D` | Standalone prototype and fixture host, not workflow owner |

## Execution-Class Boundary

### CPU/domain work

- disc hashing/manifest assembly and content parsing;
- static world/graph construction;
- deterministic refinement reduction;
- static encounter analysis and route projection; and
- final result materialization.

These stay in-process/domain operations until SAVOR has a deliberate local CPU executor. They are not
encoded as fake simulator jobs.

### Dolphin worker work

- context capture;
- Navmesh Survey door-anchor establishment and patched spatial passability exploration;
- later movement-anomaly probes outside the Navmesh Survey contract;
- later generalized automatic/interactable trigger characterization;
- controller solving; and
- clean runtime validation.

### Predictor work

- model-qualified path and movement/no-movement/interruption search;
- requested outcome evaluation;
- witness/frontier production; and
- modeled temporal traces.

This needs a deliberate asynchronous library/process boundary around `SavorPredict`; direct Qt ownership
or reliance on the exploratory CLI is not the target architecture.

## Persistence Model

SAVOR retains its relational-core plus content-addressed object-store split.

### Relational records

Store queryable identity, status, and lineage:

- workflow/step/job lifecycle and attempts;
- disc/content/world/context/refinement/prediction/control/validation IDs;
- exact parent/reference edges;
- area/profile/objective/status/terminal reason;
- versions, hashes, capabilities, metrics, and diagnostics summaries;
- candidate selection events; and
- UI projection fields.

### Object-store artifacts

Store large or replayable payloads:

- the common Navigation Context bootstrap savestate and adjacent `.nctx`;
- positional survey-anchor records and teleport/settle replay evidence;
- extracted internal files and normalized content blobs;
- static world/graph and refinement payloads;
- raw capture, runtime-modification history, and ordered spatial probe telemetry;
- prediction traces/frontier sets;
- controller tapes; and
- clean validation traces.

An artifact reference includes content hash, byte size, schema/media kind, producer, and semantic role. Two
identical byte blobs can be deduplicated while their lineage roles remain distinct.

## Required Lineage

The minimal reference chain is:

```text
DiscImageIdentity
  -> DiscContentManifest
  -> NavigationContentBundle
  -> NavigationWorldModel
  -> NavigationWorldRefinement

named Navigation Context .sav + .nctx
  -> verified positional NavigationSurveyAnchor records
  -> NavigationGeometryObservation set

explicit unmodified prediction source
  -> NavigationPredictionStart

NavigationPredictionStart + content + world/refinement + objective/model/bounds
  -> NavigationPredictionResult
  -> explicit selection
  -> NavigationControlSolveResult
  -> NavigationValidationResult
```

`NavigationGeometryObservation` references the static world, exact common bootstrap, runtime
modifications, BitVar overrides, and anchor/probe lineage. It feeds a refinement but never changes the
bootstrap. Local teleport validity and door-proven reachability are separate facts.

## Workflow Profiles

- `dungeon_explorer` is the future current-target workflow and adds ECT/encounter modeling, ordinary probe
  encounter suppression, outcome search, and encounter telemetry.
- `safe_explorer` may later reuse capture, content, world, exploration, control, event, and validation
  contracts without encounter components.
- `overworld_explorer` remains independent. Area 99 does not fall through to these 2.5D Dungeon rules.

## Retry, Idempotency, and Failure

- Step commands carry deterministic idempotency keys over immutable inputs.
- A retry creates another execution attempt, not another semantic result when output bytes/contracts are
  identical.
- Failed or partial results remain immutable and queryable.
- A patch cleanup/precondition failure quarantines the worker outcome.
- A failed reposition/settle check produces no survey anchor.
- A door job that cannot prove the intended TBLID, opening, physical crossing, and far-side settle produces
  no anchor; the common baseline and already published positional anchors remain usable.
- A predictor `ModelIncomplete` result is a valid analyzed failure, not an infrastructure retry.
- A worker crash/lease timeout may retry from the same clean input; it may not reuse an unknown live state.
- New content, model, refinement, objective, or search bounds create a new request/result lineage.
- Selection replacement is an explicit new event and never mutates the earlier selection record.

## Delivery Slices

1. Define codecs and persistence identities for disc, content, context, world, and result contracts.
2. Draft-implement `nav.capture_context` and export the concrete `a101b` `.sav`/`.nctx` bootstrap.
3. Implement disc identity/manifest/extraction and bundle materialization.
4. Persist the current static world/graph with exact content lineage.
5. Implement the narrow `a101b` Survey slice: encounter suppression, the reversible door-trigger toggle,
   BitVar-locked door `4101`, physical crossing, positional anchor replay, and parallel probes from both
   sides.
6. Add immutable spatial observations, deterministic per-area refinement, coverage reporting, and
   visualization.
7. Generalize worker batching and reproduction, then separately research movement anomalies and broad
   trigger characterization.
8. Define the separate `NavigationPredictionStart` contract and `nav.capture_prediction_start`.
9. Define and implement the asynchronous `SavorPredict` navigation boundary.
10. Add explicit selection, control solving, clean validation, and product UI projections.

Each slice must preserve the evidence layers even if later steps are still manual.

## Acceptance Rules

- Every step has typed immutable inputs and outputs with explicit IDs.
- Static CPU work is not masqueraded as a Dolphin job before a CPU lane exists.
- Patched and clean savestate roles cannot be confused in workflow bindings.
- The Navmesh Survey consumes one explicitly named `.sav`/`.nctx` bootstrap and never modifies it.
- Published anchors are positional records replayed from the common bootstrap; they do not own savestates
  or serialized ground-selector state.
- The first-slice trigger toggle and BitVar overrides are exact, reversible, verified, and retained in job
  provenance.
- Survey evidence is spatial; runner bounds and later timing models do not become navmesh observations.
- Parallel workers emit observations and verified anchor records; only deterministic CPU/domain reduction
  emits a refinement.
- The exact door-only toggle is normative for the first slice. No generalized modes or allowlist are
  assumed until broader trigger research closes that separate contract.
- Prediction, control, and validation remain separate results.
- The selected candidate is explicit.
- No workflow step resolves an implicit latest prediction start, world, refinement, or model.

# 06 - Phase, Job, and Artifact Integration

## Status

Future plan. This document maps the logical Navigation Context workflow onto current SAVOR boundaries.
Names below are contract proposals, not registered program kinds or implemented database schemas.

The closest existing runtime pattern is Battle Context capture. The hidden `dungeon_explorer` workflow
unit is only a placeholder with entry and terminal savestate bindings; it does not contain the steps below.
The current worker path runs fixed `PhaseScriptVM` programs, and there is no local CPU workflow executor
lane yet.

## Logical Workflow

| Order | Logical step | Execution class | Principal output |
|---:|---|---|---|
| 1 | `nav.capture_context` | Dolphin/PhaseScript worker | `NavigationContextResult` and clean source lineage |
| 2 | `nav.materialize_content` | CPU/domain and DiscIO/parser services | `DiscImageIdentity`, manifest, `NavigationContentBundle` |
| 3 | `nav.build_world` | CPU/domain | Static `NavigationWorldModel` and traversal graph |
| 4 | `nav.explore_geometry` | Dolphin workers with named patches | `NavigationGeometryObservation` set |
| 5 | `nav.build_refinement` | CPU/domain | Immutable `NavigationWorldRefinement` |
| 6 | `nav.search_predicted_outcomes` | `SavorPredict` future service/process | Candidate `NavigationPredictionResult` set |
| 7 | Explicit user/result selection | `SavorQt`/workflow command | Selected prediction result reference |
| 8 | `nav.solve_controls` | Dolphin-backed solver workers | `NavigationControlSolveResult` |
| 9 | `nav.validate_route` | Clean Dolphin worker | `NavigationValidationResult` and terminal state |
| 10 | `nav.publish_result` | CPU/domain/persistence | Final workflow result and UI projection |

`nav.search_predicted_outcomes` is the retained logical name. It must not be renamed casually while the
contract is being distributed across planning documents.

## Step Contracts

### `nav.capture_context`

Inputs:

- clean source savestate/runtime binding;
- expected disc/game/runtime identity;
- transition observation and capture-contract versions; and
- bounded stabilization/capture policy.

Outputs:

- immutable `NavigationContextResult`;
- raw capture telemetry artifact;
- clean source/derived savestate references when needed; and
- explicit readiness status.

Only `Ready` advances to prediction-capable workflow state. A `NonResetContinuation` is routed back into
the existing epoch, not transformed into a new context.

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

Fans out disposable Dolphin-backed probes by region, boundary, state signature, and approach. Inputs name
the exact static world, clean source lineage, patch profiles, telemetry schema, and budgets. Outputs are
immutable observations. Patched successor savestates remain exploration-only artifacts.

### `nav.build_refinement`

Deterministically reduces selected observation IDs into a state-qualified, sub-triangle world refinement.
Contradictory evidence remains represented. New evidence creates a new refinement version.

### `nav.search_predicted_outcomes`

Submits one explicit ready context, content/world/refinement IDs, objective, bounds, and model bundle to a
future asynchronous `SavorPredict` navigation boundary. It emits immutable candidates with search
completeness and terminal reasons. The workflow does not treat the current CLI as that final contract.

### Explicit result selection

Selection is an authored workflow command/event referencing one prediction result ID. A deterministic
automatic policy, if later supported, is itself versioned authoring data. There is no implicit "best" or
"latest" candidate.

### `nav.solve_controls`

Consumes the selected world-space plan and uses Dolphin-backed trials to produce a controller tape and
camera realization. Encounter and trigger suppression policy is explicit per solver mode; a tape intended
for clean execution must retain trials and provenance separately.

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
| `SavorNavigation` | Content normalization, profiles, world/graph, probe generation, refinement, static analysis, compatibility, and spatial result projection |
| `SavorCore` | Runtime reads/breakpoints, future patch writes, inputs, savestates, telemetry, capture and probe PhaseScripts |
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
- patched collision/passability exploration;
- movement-anomaly probes;
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

- clean and patched savestates, with distinct roles;
- extracted internal files and normalized content blobs;
- static world/graph and refinement payloads;
- raw capture and per-frame probe telemetry;
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

clean savestate + reset transition
  -> NavigationContextResult

context + content + world/refinement + objective/model/bounds
  -> NavigationPredictionResult
  -> explicit selection
  -> NavigationControlSolveResult
  -> NavigationValidationResult
```

`NavigationGeometryObservation` references static world, patch profile, and disposable probe lineage; it
feeds a refinement but never becomes an ancestor of the clean context.

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
- A predictor `ModelIncomplete` result is a valid analyzed failure, not an infrastructure retry.
- A worker crash/lease timeout may retry from the same clean input; it may not reuse an unknown live state.
- New content, model, refinement, objective, or search bounds create a new request/result lineage.
- Selection replacement is an explicit new event and never mutates the earlier selection record.

## Delivery Slices

1. Define codecs and persistence identities for disc, content, context, world, and result contracts.
2. Implement reset-boundary detection and a Battle-Context-like `nav.capture_context` vertical slice.
3. Implement disc identity/manifest/extraction and bundle materialization.
4. Persist the current static world/graph with exact content lineage.
5. Research and implement named patch profiles plus a narrow geometry-probe slice.
6. Add sub-triangle refinement and coverage reporting.
7. Define and implement the asynchronous `SavorPredict` navigation boundary.
8. Add explicit selection, control solving, clean validation, and product UI projections.

Each slice must preserve the evidence layers even if later steps are still manual.

## Acceptance Rules

- Every step has typed immutable inputs and outputs with explicit IDs.
- Static CPU work is not masqueraded as a Dolphin job before a CPU lane exists.
- Patched and clean savestate roles cannot be confused in workflow bindings.
- Prediction, control, and validation remain separate results.
- The selected candidate is explicit.
- No workflow step resolves an implicit latest context, world, refinement, or model.

# 05 - Prediction, Control, and Validation

## Status

Future plan. `SavorPredict` currently has an exploratory/battle-focused executable and CLI shape. A
reusable navigation search boundary, asynchronous invocation contract, navigation model bundle, and
durable result schemas must be designed before this workflow can execute.

## Three Separate Responsibilities

Navigation planning is divided into three stages:

1. **Prediction** decides what world-space route and movement/no-movement/interruption schedule can meet
   an objective from a clean context.
2. **Control solving** determines the camera-relative controller inputs that realize the selected
   planning result.
3. **Clean validation** executes those inputs from the original unmodified lineage and compares observed
   state and outcome with the prediction.

No stage silently absorbs another. The predictor is not a controller emulator, and the Navigation widget
is not a prediction search engine.

## NavigationPredictionRequest

The request explicitly references:

- one ready `navigation_context_result_id`;
- one `NavigationContentBundle`;
- one static world/graph and optional `NavigationWorldRefinement`;
- start anchor and one spatial/objective target;
- objective kind, such as no encounter or a requested encounter;
- path, update, pause, interruption, time, and search budgets;
- a versioned navigation/encounter/RNG model bundle;
- admissible route and movement/no-movement actions; and
- requested output/trace detail.

The request contains no "latest context," machine path, raw camera orientation, or precomputed controller
tape. Compatibility is checked before search begins.

## Model Bundle and Completeness

The model bundle identifies every temporal/runtime rule used by the search, including field-update,
encounter, RNG, script interruption, and movement abstractions. It records:

- component names, versions, code/data hashes, and evidence state;
- required `NavigationContextResult` capabilities;
- supported area/profile/runtime revisions;
- supported interruption types; and
- known omissions.

The unfinished Moonfish/field analysis is not an input contract and supplies no planning behavior yet.
Moonfish and other field RNG sources may only appear as unresolved component requirements until a reviewed,
versioned model exists. If a selected objective depends on an unsupported source, the result is
`ModelIncomplete` rather than a result under a guessed schedule.

## One Epoch Across Same-Script Interruptions

Prediction begins at the one clean context for the `NavigationEpoch`. If a same-script cutscene or event
interrupts movement and returns control, the model evolves through that event and continues from the
resulting state. It does not reset `stepCount`, RNG, or other state unless the model has explicit evidence
for an observed change.

When the event cannot be modeled, prediction stops at the interruption with `ModelIncomplete`. It must not
start a second fresh-entry prediction after control returns. Entering battle is terminal; battle return
requires a separately captured context and a later prediction.

## NavigationPredictionResult

Each immutable result records:

- request and explicit context/content/world/refinement/model identities;
- objective and search-bound identities;
- selected world-space route or reachable state set;
- movement/no-movement/interruption schedule in planning time;
- ordered modeled state trace or trace artifact reference;
- objective outcome, encounter/formation identity when applicable;
- route length/cost, movement frames, elapsed VI frames, and objective-specific metrics;
- terminal reason and last confirmed state;
- search coverage/completeness and frontier meaning;
- warnings, assumptions, and diagnostics; and
- predictor build/version and deterministic replay seed/configuration.

The schedule distinguishes movement eligibility and pauses at the model level. It does not prescribe raw
stick X/Y values. A route prefix, local reachable set, or frontier is qualified by the selected context,
objective, model, and search bounds; it is not a universal heatmap or probability field.

## Terminal Reasons

At minimum, results distinguish:

- `ObjectiveSatisfied`;
- `NoRoute` or `Unreachable`;
- `EncounterOccurred` with predicted encounter details when available;
- `FieldTransition`;
- `BattleEntered`;
- `SameScriptInterruption`;
- `ModelIncomplete`;
- `SearchBudgetExceeded`;
- `PredictionHorizonReached`; and
- `InvalidOrIncompatibleInput`.

Only `ObjectiveSatisfied` proves the requested modeled result. A budget or horizon frontier is search
evidence, not proof that further progress is impossible.

## Explicit Result Selection

Search may produce multiple candidates. The workflow pauses for an explicit selection or applies a
separately authored deterministic selection policy. The selected prediction result ID becomes an immutable
input to control solving. It is never inferred from a mutable "best/latest result" query.

The Navigation widget can compare and render compatible results, routes, schedules, and frontiers. It does
not rerun `SavorPredict` or rewrite the chosen result.

## NavigationControlSolveResult

The control solver consumes:

- selected prediction result;
- clean source context/savestate lineage;
- world-space route and movement/interruption schedule;
- empirical runtime camera/control observations; and
- solver bounds and runtime build identity.

It emits:

- controller input tape with VI-frame timing;
- camera-relative realization decisions;
- achieved route samples and local error estimates;
- solver attempts, objective metrics, and chosen witness;
- source prediction/context identities;
- terminal/continuation state; and
- diagnostics and completeness.

Control solving may use Dolphin-backed probes because the mapping from desired world movement to stick
input can depend on camera and runtime response. Those observations belong to this result, not the planning
context.

## NavigationValidationResult

Clean validation restores the original unpatched context lineage and applies the chosen controller tape
with no exploration-suppression patch. It records:

- context, prediction, control result, and exact savestate/input artifacts;
- runtime build and content/world identities;
- observed position/ground/state trace;
- predicted-versus-observed spatial and temporal divergence;
- actual interruption, transition, battle, encounter, and formation outcomes;
- objective success/failure;
- model/control divergence classification; and
- terminal savestate and next-boundary evidence when retained.

Validation never overwrites the prediction. Agreement, disagreement, and unvalidated portions remain
separate evidence.

## Continuations and New Contexts

- A completed objective inside the same script may end the workflow without creating a new context.
- A modeled same-script cutscene remains within the same epoch and state trace.
- A field script/context switch ends the current epoch; subsequent planning waits for
  `nav.capture_context` at the new stable boundary.
- Battle entry ends the current epoch; battle handling is a different workflow, and battle return must be
  captured as a new navigation context.

A terminal validation savestate is useful lineage, but it is not automatically a ready next context.

## Failure and Retry Rules

- Incompatible context/content/world/model identities fail before search.
- Missing required runtime fields yields `ModelIncomplete` or an invalid input result, never guessed state.
- Predictor retry uses the same immutable request unless a new request version is authored.
- Control divergence may cause another solver attempt against the same selected prediction.
- Validation divergence may motivate a new refinement/model version, but does not mutate historical
  artifacts.
- Any retry after a true field or battle boundary requires a newly captured context.

## Acceptance Rules

- Predictor inputs name one ready context and all versioned content/world/model dependencies.
- Predictor output is world-space planning plus a movement/no-movement/interruption schedule, not a raw
  controller tape.
- Same-script cutscenes never create a clean-entry reset assumption.
- Camera/control orientation appears only in probe/solver/validation evidence.
- Patched exploration state is absent from prediction and clean validation inputs.
- Unfinished field-RNG research cannot produce a `Ready` model capability.

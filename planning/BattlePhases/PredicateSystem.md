# Reusable Predicate System Investigation

Predicate evaluation progress follows the canonical observation contract in
[`../ExecutionRuntime/17-semantic-routing-workset-capture-canonical-progress.md`](../ExecutionRuntime/17-semantic-routing-workset-capture-canonical-progress.md).
`soa.progress.predicate.evaluations/1` is a registered phase-library provider
and is included by default for `battle.single_turn`. It reports requested typed
evaluations through the worker-to-DB progress stream; it does not change
predicate accounting, reactions, abort behavior, or execution routing.

Typed derived observations follow
[`../ExecutionRuntime/18-derived-state-runtime.md`](../ExecutionRuntime/18-derived-state-runtime.md).
A predicate imports an exact registered derived query and any pure reducer it
uses. That action dependency selects the static block during workset
materialization. Predicate observations always query with
`SameRoutedEvent`: turn-order evidence must match `TurnIsReady`, and reward
evidence must match the exact `EndTurn` or Victory receipt. Missing or stale
derived evidence fails the job; it is not a false predicate or a third
evaluation state. Capture samples are never predicate evidence.

## Purpose

This is a separate, evidence-first notebook for redesigning predicates as a
reusable system. Battle will consume this system, but neither
`battle.context` nor `battle.single_turn` owns it.

This is not an API proposal yet. It records what exists, what legacy Battle
actually did, and which choices need to be made together.

## Decisions already made

- Predicate preparation runs on the homogeneous-worker architecture described
  by
  [`../ExecutionRuntime/14-homogeneous-worker-architecture.md`](../ExecutionRuntime/14-homogeneous-worker-architecture.md).
  The prepared predicate-linked Full Phase package is workset input, not an
  installed worker catalog entry or advertised capability.

- The predicate system must be reusable outside Battle.
- Battle-specific automatic selection will not absorb predicate behavior.
- Predicates may support user-authored rejection and early abort, but those
  policies are not enabled by default.
- Completed Battle results retain predicate passed/total counts.
- Predicate rejection is a successful typed Battle domain result, not a failed
  execution job.
- Predicate rejection does not publish a successor savestate.
- Predicate definitions and predicate bundles are first-class authored database
  objects.
- Referenced bundle revisions are immutable. Editing an authored bundle creates
  a new revision rather than changing queued or historical execution inputs.
- Pure predicate definitions remain reusable across domains. A predicate bundle
  binds definitions to ordered checks, observations, semantic hooks, reactions,
  and aggregation policy.
- Predicate preparation does not occur per candidate job. An exact bundle is
  linked to the phase's stable hook contract once per active structural key,
  verified, and reused by every compatible workset. The first implementation
  does not require a durable prepared-program cache; a missing variant can be
  reconstructed from its immutable authored definition.
- An authored Battle turn may provide a default predicate bundle, but each wave
  freezes the exact bundle revision it will use and may select a different
  compatible revision.
- One wave/workset uses one exact bundle revision so its candidates share the
  same prepared program and produce comparable predicate results.
- Workers never query authoring, analysis, or execution databases. Coordination
  resolves database records into a self-contained workset package before
  dispatch.
- The canonical resolved predicate bundle is sent once as workset-shared input,
  alongside the exact prepared program definition. Database IDs may be carried
  for provenance but are never execution authorities for the worker.

## Current new-backend substrate

The new runtime has two relevant composition frontends:

- `SemanticObservationComposition` describes semantic points, typed reads and
  coherent queries, optional versus required evidence, named baseline updates,
  and optional publication. It lowers those declarations to ordinary program
  IR and registered actions.
- `PredicateComposition` describes typed witness parameters, a finite
  expression graph, four evaluation statuses, and several check-use policies.
  It also lowers to ordinary program IR.

Both are currently isolated composition components. Searches of the current
checkout found production definitions and focused unit tests, but no phase
module or program-kind adapter that calls either lowerer. The joint path from a
semantic observation to a predicate check therefore remains to be designed and
integrated.

The current `PredicateComposition` surface does not yet fully match the
separation described in the Execution Runtime planning documents:

- `PredicateDefinition` currently embeds `PredicateCheck`.
- witness required/optional policy currently lives in the definition;
- check point identity and effect policy live beside the pure expression;
- `StructuredFail`, domain rejection, and emission have distinct lowering,
  while `Branch` and `Accumulate` currently lower like ordinary status returns;
  and
- the semantic-observation composer records named baseline values internally,
  but currently returns only its last observed value and has no composed
  predicate caller.

These are observations about the unfinished integration surface, not decisions
that those APIs must be preserved.

## Current persisted authoring model

The authoring database still stores the legacy execution shape:

- physical breakpoint IDs;
- a scalar LHS and RHS plus width;
- legacy flag bits;
- optional address-program IDs;
- baseline breakpoint IDs;
- a value mask used as a turn mask;
- `abort_on_fail`; and
- named predicate sets attached through Explorer settings.

Validation calls the breakpoint registry directly and understands legacy
predicate flags. The current Qt editor exposes the same physical-breakpoint,
width, source-mode, turn-mask, negation, and abort controls.

The earlier Execution Runtime plan treated these records as compatibility
inputs that would be translated in memory. The new request for a lean,
flexible, reusable predicate system reopens whether that compatibility-only
strategy is still the desired authoring model.

## Legacy Battle behavior actually observed

A legacy predicate combines several concerns in one record:

1. where it evaluates;
2. how its LHS and RHS are acquired;
3. how values are compared;
4. how baseline values are captured;
5. whether failure aborts the run; and
6. how passed/total progress is accumulated.

Observed supported comparisons are `==`, `!=`, `<`, `<=`, `>`, and `>=` over
1-, 2-, 4-, or 8-byte unsigned values. A value may come from an absolute guest
address, registered address key, address program, immediate RHS, or captured
LHS baseline.

The legacy VM behavior is more specific than the stored shape suggests:

- each required breakpoint expands to a separate runtime predicate record;
- the predicate is evaluated and counted every time its required breakpoint is
  encountered;
- a successful evaluation increments both passed and total; an unsatisfied
  evaluation increments total only;
- an `AbortOnFail` evaluation sets the abort flag immediately and prevents
  later records in that evaluation loop from running;
- an unreadable LHS or RHS silently skips that record rather than producing
  false, unavailable, or a job failure;
- multiple baseline breakpoints overwrite the same stored baseline, so the
  latest successful capture is used;
- the baseline capture operation runs before predicate evaluation for a routed
  hit;
- multiple required breakpoints can cause the same authored predicate to count
  multiple times; and
- a turn mask is persisted and packed, but no legacy evaluation use of that
  mask was found. The VM checks only `Active` and required-breakpoint identity.

Two legacy Battle examples exercise distinct use cases:

- an Electribox-drop condition evaluates an address-program value at `EndTurn`
  against the current-turn key and aborts on failure; and
- a turn-order condition compares two derived address keys at `TurnIsReady`,
  records its score, and does not abort on failure.

Some legacy flags also appear to be data without matching VM behavior:
`LhsIsNeg`, `RhsIsNeg`, `PredKind`, and the turn mask are stored or packed but
are not consulted by the evaluation loop found in the legacy checkout. These
should not be called retained functionality unless another execution path is
found.

## Accepted architecture direction

### Separation of concerns

The reusable system has three distinct authored concepts:

- **Observation** obtains typed evidence at declared public semantic points and
  owns baseline timing and evidence availability.
- **Predicate definition** is a pure typed expression over supplied evidence.
- **Check use** binds one predicate definition to observations and a semantic
  hook, then declares what the evaluation does: continue, reject the domain
  candidate, explicitly fail execution, emit evidence, or contribute to an
  aggregate.

Battle-specific concepts such as turn selection, `TurnInputs`, wave
continuation, and Battle outcome types stay outside predicate definitions.

### Legacy capability mapping

The new authoring model retains the intended flexibility of the legacy system
through explicit typed concepts rather than packed VM flags:

| Legacy capability | New representation |
| --- | --- |
| Required breakpoint | Public semantic hook |
| Multiple required breakpoints | Multiple explicit check-hook bindings |
| Absolute address | Compatibility pinned-address observation |
| Address key | Registered typed observation |
| Address program | Checked address expression or coherent query |
| Read width | Typed value such as `u8`, `u16`, `u32`, or `u64` |
| Immediate RHS | Literal or wave-supplied parameter |
| Baseline breakpoints | Named baseline observation with explicit capture hooks |
| Comparison operator | Pure typed expression |
| `AbortOnFail` | Check reaction that rejects the domain candidate |
| Non-aborting predicate | Record/score reaction |
| `Active` and turn mask | Wave-level bundle binding and active-check selection |
| Predicate set | Versioned predicate bundle |

### Authored check model

One check use declares:

- stable identity;
- one public semantic hook;
- an explicit occurrence policy;
- named typed witness bindings;
- a pure predicate definition, either private to the bundle or reusable;
- required or optional evidence policy;
- reaction policy; and
- aggregation participation.

Reusing a predicate at multiple semantic points creates multiple explicit check
uses. This avoids the legacy behavior where one authored predicate silently
expanded into several runtime records and could be counted repeatedly.

The supported value-source categories are:

- typed guest observations;
- registered coherent domain queries;
- named baseline values;
- wave-supplied bundle parameters;
- bundle literals;
- typed hook-receipt fields;
- pure values derived by exact registered reducers; and
- earlier pure expression nodes.

Predicates perform no guest access. Observation binding supplies ordinary typed
values before the pure expression executes.

The initial expression surface includes typed literals, comparisons, Boolean
logic, modest typed arithmetic, and exact registered pure reducers for
specialized calculations. Arbitrary user scripts are not part of the accepted
initial design. Exact types replace legacy read-width flags and allow signed or
otherwise domain-specific semantics to be declared rather than inferred.

### Authoring experience

The authoring UI presents two views over the same stored model:

- a simple comparison editor for the common legacy form of semantic hook, left
  value, operator, right value, and reaction; and
- an advanced expression editor for multiple witnesses, Boolean composition,
  arithmetic, and pure reducers.

A simple one-off check may create a private inline predicate definition behind
the scenes. A definition intended for reuse may be promoted to the shared
predicate library without changing the check model.

### Authored and prepared forms

Predicate definitions and bundles are authored and stored durably in SavorDb.
A bundle revision contains an ordered set of check uses and the exact references
needed to resolve their predicates, semantic hooks, observations, reactions,
aggregation behavior, and typed parameter schema. Once a revision is referenced
by a queued or completed wave it is immutable; edits create a new revision.

A wave uses a `PredicateBundleBinding`, consisting conceptually of:

- the exact bundle revision and content hash;
- frozen typed parameter values;
- the active check set; and
- aggregation configuration.

Parameter values allow one prepared bundle to be reused with different item
identities, thresholds, limits, and similar data across turns and waves. Values
are workset inputs and do not change the prepared program key. Changes to hook
assignments, observation graphs, value types, check topology, or the
structurally active check set do require another prepared variant, which is
still built only once per unique structural key and reused by all matching
worksets.

Execution uses a separate prepared form. A stable phase module exposes a
versioned semantic-hook contract. Preparation links one exact bundle revision
to those hooks, lowers it to ordinary ExecutionRuntime IR, verifies the closed
module and dependency set, and reuses the resulting prepared phase variant in
memory. Preparation is keyed by the phase module revision, hook-contract
revision, bundle content hash, and exact capability dependency closure. It is
never candidate-specific. Prepared bytes are reconstructible and are not
durably cached in the first implementation.

Thousands of jobs using the same bundle and compatible phase revision therefore
reuse the same prepared program definition. Job-specific savestates, input
candidates, live Battle state, turn numbers, and fake-attack choices remain
runtime inputs and do not affect the preparation key. A persisted `.bctx` is a
Battle planning aid, not a predicate or phase execution dependency.

### Occurrence policies and baselines

Check evaluation frequency is explicit. The initial occurrence-policy surface
supports:

- first matching hit;
- every matching hit;
- a specified matching-hit ordinal; and
- once when a typed guard becomes satisfied.

Typed guards may use declared receipt or observation fields such as actor slot,
action index, or action kind. They do not restore a generic breakpoint VM or
hidden turn mask.

Baselines are named, candidate-local observation state. A baseline declaration
selects its capture hook, typed source, and `First` or `Latest` update policy.
Checks receive current and baseline values as ordinary witnesses and may compare
them directly or calculate a delta through pure arithmetic or a reducer.
Baseline state is scoped to one candidate invocation and is never shared across
jobs.

### Reactions

The initial author-facing reaction surface remains small:

- record and continue;
- contribute to the configured aggregate;
- reject the domain candidate when unsatisfied; and
- emit detailed condition evidence when explicitly requested.

Missing required evidence, invalid observation, dependency failure, or reducer
failure follows the ordinary ExecutionRuntime failure contract. Infrastructure
failure is not an authored predicate reaction.

### Battle turn and wave assignment

An authored Battle turn may name a default predicate bundle revision. Wave
creation resolves that default but may explicitly select a different compatible
bundle or an empty bundle. The created wave freezes the exact resolved revision,
content hash, typed parameter values, active check set, and aggregation
configuration.

The wave is the execution authority because separate waves for the same authored
turn may intentionally test different predicates. Every candidate within one
wave/workset uses the same bundle so predicate summaries remain comparable and
the workset can share one prepared program variant. Automatic continuation uses
the next authored turn's default. Manual continuation may select another
compatible revision before the wave is created.

### Self-contained workset materialization

Coordination is the only database-aware execution boundary. It resolves the
authored plan, selected bundle revision, predicate-definition references,
prepared program definition, capability dependencies, and input artifacts into
one immutable workset package.

The generalized workset has explicit common inputs and per-job inputs. It
carries the canonical resolved predicate bundle once as common input, not once
per candidate. Common inputs also carry the exact prepared program definition,
resolved `PredicateBundleBinding`, dependency identity, and any genuinely shared
baseline. Each job carries its own Battle Plan as per-job input, together with
candidate variation such as a SeedProbe-derived input, fake-attack choice,
execution identity, and correlation. The prepared program, bundle, and binding
are bound by content hashes, and the worker rejects the workset before candidate
execution if their identities disagree or required versions and dependencies
are unavailable.

Database row IDs may be included for diagnostics and provenance, but workers do
not dereference them. A worker must not query authoring, analysis, or execution
databases and should not require SavorDb services. It validates the supplied
package, executes candidates, stages output artifacts, and returns typed
results. Coordination registers artifacts and persists results after receiving
those outputs.

The initial transport may inline the canonical bundle and prepared definition.
An in-memory worker cache may omit bytes already admitted during that worker
process, but durable caching is not required. Cache misses are satisfied through
coordination or artifact transport rather than database access.

### Workset homogeneity and result provenance

A predicate-bearing workset is homogeneous over at least:

- phase module revision;
- semantic-hook contract revision;
- predicate bundle revision and content hash;
- structurally active check set; and
- exact capability dependency closure.

Worker results carry the predicate bundle hash and typed predicate summary.
Coordination maps those DB-independent outputs back to their workflow, wave,
job, and durable result records.

### Evaluation accounting and evidence retention

The initial accounting rules are:

- every executed check produces exactly `Passed` or `Failed` and increments
  total;
- `Passed` also increments passed, while `Failed` does not;
- checks whose hook or occurrence guard never activates produce no evaluation
  record and do not increment either count;
- missing required evidence follows the ordinary execution-failure contract;
  and
- checks skipped after candidate rejection do not count.

Battle's ending-RNG comparator uses the passed count (`pred_passed`), matching
the relevant legacy behavior. It does not rank by the number evaluated.

Every result retains the bundle identity and aggregate summary. Detailed
per-check evidence is durable for predicate rejection, execution failure, and
checks explicitly configured to emit evidence; it is otherwise transient.

### New-model boundary

The new predicate persistence model is designed cleanly for the new runtime.
There is no legacy predicate import, migration, or compatibility requirement.

## Deferred design details

The following remains open and should be refined separately from the accepted
preparation, persistence, wave-assignment, and worker-isolation architecture:

- the concrete clean-model SavorDb schema and authoring UI.

## Evidence locations

Current checkout:

- `SavorCore/Runner/Runtime/ProgramRuntime/Composition/PredicateComposition.h`
- `SavorCore/Runner/Runtime/ProgramRuntime/Composition/PredicateComposition.cpp`
- `SavorCore/Runner/Runtime/ProgramRuntime/Composition/SemanticObservationComposition.h`
- `SavorCore/Runner/Runtime/ProgramRuntime/Composition/SemanticObservationComposition.cpp`
- `SavorTests/test_predicate_composition.cpp`
- `SavorTests/test_semantic_observation_composition.cpp`
- `SavorDb/Authoring/IAuthoringDb.h`
- `SavorDb/Authoring/SqliteAuthoringDb.cpp`
- `SavorQt/GUI/Panes/BattleRunSettingsPane/PredicateSpecEditorWindow.cpp`
- `planning/ExecutionRuntime/03-program-modules-ir-and-types.md`
- `planning/ExecutionRuntime/07-current-phase-migration-matrix.md`

Read-only legacy checkout:

- `SavorCore/Runner/Breakpoints/Predicate.h`
- `SavorCore/Runner/Breakpoints/Predicate.cpp`
- `SavorCore/Runner/Script/PhaseScriptVMPredicates.cpp`
- `SavorDb/Execution/ProgramDB/BattleSingleTurn/BattleSingleTurnAdapters.cpp`
- `SavorE2E/BattleSingleTurnScenario.cpp`

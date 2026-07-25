# 01 - Future Prediction Entry Boundaries and Lifecycle

## Status

Future plan. This document defines when a prediction-safe navigation lifetime starts and ends. It is a
contract for workflow gating, not an assertion that the required runtime breakpoints and fields are
already implemented. It is separate from the implemented Navigation Context `.nctx` export and does not
gate the Navmesh Survey.

## Navigation Epoch

A `NavigationEpoch` is one continuous period of navigable field execution governed by one captured
reset-state contract. It begins after a qualifying transition has completed and remains active until a
terminal boundary occurs.

An epoch is identified by an immutable `navigation_epoch_id` and references exactly one future
`NavigationPredictionStart`. Re-entering the same area does not reuse the previous epoch merely because the
MLD/SCT/ECT content hashes match.

## Reset-Qualified Entry

An entry is reset-qualified only when all of the following are observed:

1. The source is a clean, unpatched savestate or runtime lineage.
2. A recognized reset-producing transition has completed.
3. Field placement and the active ground are settled.
4. Player control is enabled.
5. Player velocity is measured as zero.
6. Authored input is neutral and no input is queued by SAVOR.
7. No forced movement, cutscene, area transition, battle, or encounter is already pending.
8. The captured encounter/reset fields agree with the versioned working reset contract.

The recognized entry classes are:

- **Field script/context switch**: the new field script and context are active and control has been
  restored.
- **Battle return**: field state has been restored after battle and control has been restored.
- **Save load with confirmed switch**: admissible only when runtime evidence proves that this load path
  performs a reset-producing field script/context switch.

Opcode-77 placements can explain or select a spatial start. They do not by themselves prove a reset or
authorize context capture.

## Working Reset Contract

Current gameplay testing supports this working rule:

- the encounter `stepCount` resets on a field script/context switch;
- it resets across the transition into and back from battle; and
- a cutscene that executes inside the same field script and returns control does not reset it.

This is an explicit, versioned hypothesis to validate in runtime capture. It must not be generalized to
every encounter-related field. RNG state, table state, movement accumulators, modifiers, queued encounter
state, and any other relevant runtime fields are captured independently and compared against their own
evidence contracts.

## Non-Boundary Interruptions

A same-script cutscene, trigger, door animation, or other event that temporarily removes control but then
returns to the same field script is an interruption inside the existing `NavigationEpoch`.

Its required behavior is:

- preserve the epoch and original `NavigationPredictionStart` reference;
- preserve elapsed movement and encounter-state evolution;
- represent the interruption in the predicted schedule and trace when the model supports it;
- resume prediction from the evolved state, never from a fabricated fresh-entry state; and
- return `ModelIncomplete` if the interruption cannot be modeled safely.

It is invalid to split such an interruption into a new epoch merely to regain zero velocity or a simple
starting state.

## Terminal Boundaries

The following end the current epoch:

- entering battle;
- completing a field script/context switch to another navigable context;
- losing the ability to identify the active field/context reliably;
- divergence that invalidates the captured state lineage; or
- workflow cancellation or an unrecoverable runtime failure.

Battle entry is terminal even if the eventual battle return is to the same MLD. The return is captured as
a new context after its own readiness gate. A same-script cutscene is not terminal unless it actually
causes a field/context switch or otherwise invalidates the model.

## Lifecycle States

| State | Meaning | Allowed next operation |
|---|---|---|
| `AwaitingQualifiedEntry` | No reset-qualified field boundary has been observed | Continue runtime observation |
| `Stabilizing` | A candidate boundary occurred, but control/placement/velocity is not ready | Poll bounded readiness evidence |
| `PredictionStartReady` | Capture passed all reset and stability checks | Plan from the explicit prediction start |
| `Active` | Prediction, control solving, or clean execution is using the epoch | Continue within the same state lineage |
| `Interrupted` | Control is temporarily unavailable inside the same script | Model interruption or return `ModelIncomplete` |
| `Terminal` | Battle, context switch, invalidation, or cancellation ended the epoch | Capture a distinct next context when eligible |

An implementation may use different storage enums, but it must preserve these distinctions.

## Transition Provenance

Each candidate and accepted entry records:

- source savestate or runtime lineage identity;
- transition kind and source/target field identity when known;
- breakpoint or observation evidence used to mark transition completion;
- timestamps/VI frames for transition detection, control restoration, and capture;
- the active script/context and area identity;
- readiness measurements and the version of the reset contract applied; and
- rejection status and diagnostics when capture does not qualify.

This evidence prevents a spatially plausible start from being mistaken for a prediction-safe state.

## Save-Load Qualification

Save load is not globally declared a reset boundary. The workflow must identify the actual save-load path
and verify that it performs the same reset-producing script/context switch and readiness sequence used by
ordinary field entry. Until that evidence exists, save-load capture returns `ResetStateMismatch` or
`IncompleteCapture`, not a ready prediction start.

## Producing the Next Context

Control solving and clean validation may terminate at:

- a same-script objective, which remains inside the current epoch;
- a same-script interruption, which remains part of the current epoch if modeled;
- a field transition, which requires a new future `nav.capture_prediction_start`; or
- battle entry, which terminates the epoch and requires capture after battle return.

A validation result may reference the observed terminal transition and the candidate next savestate, but
it never manufactures the next `NavigationPredictionStart`. Only `nav.capture_prediction_start` can
establish that result.

## Acceptance Rules

- No prediction starts from a non-reset same-script continuation under a fresh-entry assumption.
- Entering and returning from battle creates two epoch boundaries, not one continuous prediction epoch.
- Zero velocity and neutral input are measured requirements.
- Save load remains unqualified until its reset-producing behavior is observed.
- A failure to model a same-script interruption is surfaced as `ModelIncomplete`, not hidden by restarting
  the epoch.

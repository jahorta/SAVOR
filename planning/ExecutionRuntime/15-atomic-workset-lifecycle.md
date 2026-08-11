# Atomic Workset Lifecycle

This document is the normative workset lifecycle contract. It supersedes every
earlier statement that `BeginWorkset` both establishes guest state and begins
item execution, or that a workset execution snapshot may be synthesized before
baseline initialization commits.

## Lifecycle

The production lifecycle is:

`Validating -> Admitted -> Initializing -> Ready -> Running <-> ResettingItem -> Draining -> Completed|Cancelled|Failed`

- `SubmitWorkset` acknowledges immutable host-only admission.
- An admitted successor owns no epoch, session resource, execution snapshot,
  guest state, or program instance.
- The worker automatically enters `Initializing`. There is no coordinator
  `Begin` command.
- `Ready` means the exact guest baseline transaction committed successfully.
- An internal actor operation starts `Running` on the next actor turn. No item
  start event is emitted before that transition.

## Initialization transaction

`OpenWorksetInitialization` allocates the workset epoch and creates the
workset-scoped services and stop routing. `WorksetStateCoordinator::Initialize`
may run or restart the guest while restoring a savestate or establishing a
movie-derived baseline. The guest is not required to remain paused during
those operations.

`CommitWorksetInitialization` performs the authoritative backend query. Only
that final evidence must prove a paused guest at the initialized baseline. The
commit then creates a fresh `ExecutionEngine` and publishes the baseline
receipt as authoritative. All earlier backend queries are disposable
eligibility or progress observations.

`AbortWorksetInitialization` releases all opened resources. Preserved integrity
returns the worker to general readiness after terminalizing the workset as
unstarted; unknown integrity taints the session.

## Execution evidence and reset

- The canonical execution snapshot is absent before commit and during item
  reset. It is never represented by a synthetic `Closed` snapshot.
- `Ready` may use committed evidence internally; ordinary external execution
  telemetry begins only after `Running`.
- Every later savestate-baseline item removes the prior engine, proves full
  invocation unwind, restores the same exact baseline under the unchanged
  workset epoch, and constructs a new engine from fresh evidence.
- No execution snapshot survives a guest restoration.
- A reset failure stops all remaining items. They never run against uncertain
  state.

## Movie baseline rule

Movie preparation is split into stopped-core startup/preparation and later
playback activation. Initialization may perform the former; the job installs
its subscriptions before it activates playback. Every TAS Movie Full Phase
kind accepts exactly one item per workset, regardless of baseline artifact
kind. A multi-item TAS Movie workset is invalid at admission.

Immutable module packages and prepared definitions may be cached by structural
identity. Guest/session/execution evidence may not be cached before
initialization commits or across a reset.

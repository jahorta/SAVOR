# Artifact-Atomic Worksets

This document records the current hard-cut execution contract and supersedes any earlier proposal that
allowed a phase to consume worker state established outside its active workset.

## Legacy comparison source

`C:\Users\jahor\.codex\worktrees\e4f9\SAVOR` is the designated read-only legacy comparison repository.
It may be inspected for behavioral evidence, especially the wrapper-preserving TAS Movie lifecycle, but
it must never be modified and must never receive build, test, migration, or generated output.

## One execution rule

Every submitted workset contains one exact artifact baseline:

- `Savestate` identifies an exact `.sav` and hash, plus its exact same-name DTM continuation sidecar and
  hash when the restored state is movie-paired.
- `ReadOnlyMovie` identifies an exact `.dtm` and hash, plus only the startup savestate declared by that
  DTM when it was originally recorded from a savestate.

`ReadOnlyMovie` is an explicit, opt-in baseline reserved for TAS Movie phases that intentionally start
from the DTM-declared origin. It is not the default baseline for TAS Movie phases, and no non-TAS phase
may select it. A checkpoint captured later in movie playback is not a DTM startup savestate: every phase,
including `battle.record`, restores that checkpoint through `Savestate` with its exact DTM continuation
sidecar.

The baseline includes complete compatibility and lineage. Worker staging verifies the declared files,
hashes, DTM shape, startup-state parity, sidecar naming, compatibility, and lineage before the workset
can mutate Dolphin.

There is no phase baseline based on booting Dolphin and no baseline based on guest state left by an
earlier workset. Program execution is submitted only as an immutable `WorkerWorkset`; a duplicate
transport delivery can repeat the existing acceptance receipt but cannot run the accepted workset a
second time.

## Worker and workset ownership

`EmulationSession::Open` performs one infrastructure boot and retains the Dolphin wrapper, controller
infrastructure, user directory, render surface, and session ID. It has no active `WorksetEpoch` and no
guest-dependent runtime services. The infrastructure guest state is never a phase baseline.

`BeginWorkset` allocates one fresh nonzero monotonic `WorksetEpoch` and constructs the input, movie,
execution, stop-point, capture, mutation, screenshot, savestate, and resource-ledger services. The epoch
is unchanged for every item, private baseline restore, and movie core restart in that workset.
`EndWorkset` proves cleanup, tears those services down, releases baseline movie context and memory
handles, and clears the active epoch without closing Dolphin.

A savestate workset imports and restores its declared `.sav`/DTM pair when it activates. A multi-item
workset captures one private in-memory baseline handle after that restore. Before each later item it
waits for the prior invocation and promoted savestate publication/compensation to finish, proves no live
action or execution resource remains, drains ingress, and restores the handle under the same epoch. The
handle is released when the workset ends and is never indexed or retained across worksets.

When that savestate is movie-paired, restoration also establishes the exact read-only playback session
and cursor encoded by the checkpoint and its DTM sidecar. A program that needs scoped movie authority
uses `runtime.movie.adopt_restored_read_only_playback` after restoration. Adoption validates and takes
invocation ownership of that existing session; it does not prepare a DTM-origin baseline, restart the
core, restore state, or advance the guest. The scoped handle may then be branched into recording.

Movie state is not inferred by the execution backend. `MovieService` owns one
epoch-bound canonical state and observes raw native playback/recording facts
whenever execution needs movie evidence. Restored adoption requires observed
`ReadOnlyPlayback`; branching atomically changes that state to `Recording`
before the first replay advance. Natural playback exhaustion produces the
owned terminal `PlaybackEnded` state and preserves final cursor/DTM evidence
until resource cleanup detaches it. Cleanup of that already-ended state is
host-only and does not issue a redundant native movie stop.

A TAS Movie phase that explicitly selects a read-only-movie workset stages its exact
DTM/DTM-declared-startup-savestate pair without starting playback. Each item begins unestablished and
independently establishes that same declared artifact through
`MoviePrepareReadOnlyPlayback`, a stopped-core breakpoint-installation boundary, and
`MovieStartPlayback`.

Compiled-module or process affinity may influence scheduling only when it cannot skip artifact staging,
materialization, or state establishment. There is no cross-workset savestate cache, lease, locality hint,
or warm baseline.

## Invocation state policies

The only policies are:

- `RestoreBaseline`: the active workset already restored its exact savestate baseline.
- `EstablishBaseline`: the invocation must establish its staged read-only movie before guest-dependent
  work.

For `EstablishBaseline`, workset initialization prepares the exact movie session and commits an
authoritative paused execution snapshot. Item startup installs its passive observation registrations
before `MovieStartPlayback` consumes that prepared session. Guest reads, execution advancement, input,
capture, screenshots, mutations, and successful return remain forbidden until playback starts. Verifier
control-flow analysis and runtime enforcement both fail closed.

Invocation templates are bound to the active workset epoch immediately before item execution. Workset
definitions and coordinator requests never contain a session ID or epoch. External runtime controls use
exact workset/item identity; session ID and epoch remain outbound diagnostic evidence.

## Workset-local savestate restoration

`SavestateService` owns only bytes, compatibility, lineage, bounded memory handles, and immutable
savestate publication records. It does not boot or restart Dolphin, allocate epochs, or coordinate other
services. Programs have no generic process-local state capture/restore action.

`WorksetStateCoordinator` owns the restore transaction. It prepares the exact DTM history and movie-input
reservation, quiesces native stop ingress, loads the savestate, verifies the movie cursor, force-reapplies
the physical stop plan, and resumes ingress. Initial artifact restoration and later private-handle
restoration use the same active epoch.

A preserved backend failure with successful rollback fails the workset without taint. Unknown backend
integrity, failed rollback, or failed post-load reconciliation taints the session. Cleanup appends its
diagnostics without masking the primary failure.

## TAS Movie core restart

`MovieService::PrepareReadOnlyPlayback` verifies the request against the active `ReadOnlyMovie` baseline,
acquires the movie-input reservation, stages the exact artifacts, stops only Dolphin's guest core, waits
for its uninitialized state, and drains pre-stop ingress. The module installs its breakpoint group at that
boundary. `StartPreparedReadOnlyPlayback` then invokes `Movie::PlayInput`, boots through the existing
`Core::System` and window system, and returns paused.

The restart preserves the wrapper, controllers, user directory, render surface, `EmulationSession`,
session ID, and `WorksetEpoch`. The stopped-core boundary advances dispatch/physical generations before
the group is installed; post-boot validation proves the manager-owned physical plan without force-reapply.
An unconsumed preparation is scope-owned, and abandoning it taints the session so the worker is retired.

`DolphinWrapperBackend::Close` and its destructor are the only normal wrapper-destruction paths. A failed
core restart retains the wrapper; unknown integrity taints the session and defers destruction until
explicit worker shutdown.

# Homogeneous Worker Architecture

This document is the normative worker-fleet contract for the current
ExecutionRuntime. It supersedes every earlier proposal or checkpoint that
used worker capability masks, installed-program catalogs, `Partial` versus
`CompleteExact` workers, capability preflight, module preloading, or worker
eligibility by program kind.

## Fleet invariant

Every built production worker has the same Full Phase execution surface. A
worker does not advertise which program kinds, actions, reducers, types, or
modules it can execute. Missing required backend services are startup
failures, not a different worker capability class.

The only deployment modes are:

- `Headless`: ordinarily scheduled, without a render surface;
- `Visual`: ordinarily scheduled, with a render surface; and
- `VisualDebug`: explicit interactive replay/debug only, never attached to
  `JobExecutionCoordinator`.

Worksets never request or constrain a mode. Headless and Visual workers are
equally eligible for every production workset.

## Runtime identity

`WorkerRuntimeContractV1` is one immutable contract computed from the runtime
itself. It contains the exact WRMS, workset, program-module, invocation, and
result versions; the static handler/action/type/reducer and uniform
capability-pack ABI hash; supported game/runtime and emulator-bridge
revisions; hard workset limits; build identity; and a canonical contract hash.

The coordinator compares the worker's exact canonical contract hash with its
own. A mismatch quarantines that physical slot and terminates its process.
Because a contract mismatch is non-retryable for that deployed binary, the
slot is not restarted repeatedly. Matching slots continue operating.

ProgramRuntime capability packs remain ordinary uniform IR service
registries. They describe what every matching runtime implements and
contribute to the ABI hash; they are not worker advertisements or scheduling
inputs.

## Authoritative workset admission

`SubmitWorkset` is the only program-admission path. Every workset carries its
exact closed Full Phase program package and common input, with canonical
content hashes. Before guest mutation the worker:

1. verifies package and common-input hashes;
2. admits the exact closed module set;
3. verifies the root entrypoint, type/action/reducer/hook imports, dependency
   lock, runtime profile, and static limits; and
4. prepares each item execution.

Each static Full Phase handler may narrow the global workset item-count bounds.
The policy contributes to the uniform runtime ABI and is checked during this
host-only admission. TAS Movie validation and checkpoint sterilization both
require exactly one item per workset; the rule follows the program kind, not
the read-only-movie baseline artifact kind.

The worker keeps only a private, transient cache by structural package
identity. Any worker can reconstruct an empty cache from the next workset.
There is no `RuntimeManifest`, installed module catalog, `PrepareModule`,
coordinator module preload, or enabled-program-kind list.

Package-admission failure is a deterministic invalid-workset result. It is not
rerouted to a supposedly more capable worker.

## Dispatch and persistence

Scheduling considers immediate worker availability, configured queue
capacity, item credits, and global size limits. A warm package or execution
key is only a tie-break among workers that are already available. It never
reserves a worker, delays a cold dispatch, or makes a cold worker ineligible.

Module identity, package hashes, runtime-profile hash, execution key, and
affinity key remain integrity, provenance, and cache-locality facts. They are
not worker-compatibility filters. Telemetry exposes worker mode, runtime
contract hash, quarantine reason, credits, residence, and warm-cache keys.

The execution database stores `contract_key` and exact
`program_package_sha256`. The capability mask and capability-oriented index
are removed. Pre-cut rows are preserved but have no invented package identity
and therefore cannot be reconstructed as current worksets.

## Hard-cut protocol

- WRMS is version 2.
- Worker worksets use wire version 4.
- WRMS v1 and workset v3 are rejected without adapters or negotiation.
- Coordinator and workers are rebuilt and deployed atomically.

The split coordinator is the sole production path:

- `WorkerCoordinator` owns the homogeneous physical fleet;
- `JobExecutionCoordinator` owns durable workset dispatch; and
- `WorkflowCoordinatorService` owns workflow progression.

The legacy `DBWorkflowWorkerCoordinator`, its factory, capability preflight,
runtime manifest/preparation messages, and SavorPredict's `run-battle-job` and
`run-battle-jobs` commands are retired.

## Battle validation provenance

Battle E2E may start only from a checkpoint produced by the approved
`tasmovie.validation` flow for the approved DTM and processed through
checkpoint sterilization. A fixed or legacy savestate is not an acceptable
substitute.

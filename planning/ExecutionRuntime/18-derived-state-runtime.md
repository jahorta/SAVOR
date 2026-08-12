# Typed Derived State Runtime

This document is the normative derived-state contract. It supersedes the
legacy derived-buffer, `Region::DERIVED`, dense item-table, byte-offset, and
capture-backed-decision models.

## Authority and selection

`DerivedStateRegistry` is part of the homogeneous static runtime ABI. It owns
exact block revisions, exact static refresh-provider identities, refresh
groups, semantic triggers, typed query actions, pure reducers, limits, and
descriptor hashes. The canonical CPU-observer descriptor map is likewise part
of the ABI. Workers do not advertise these blocks and worksets do not carry
provider code.

Every `ProgramKindDescriptor` declares a derived-state default list, including
an explicit empty list. Coordination resolves one immutable
`WorksetDerivedStateBindingV1` from those defaults plus the exact block
dependencies named by imported derived-query action descriptors. The binding
has its own wire payload, content hash, execution-key component, and Execution
DB columns. It is separate from common input, per-item input, capture, and
progress. Missing, duplicate, unknown, mismatched, or unconfigured dependencies
fail before dispatch or guest mutation.

## Item-local lifecycle

Admission validates immutable metadata only. After workset initialization or
item reset commits authoritative execution evidence, `DerivedStateService`
activates a fresh item-local state before `Running` and before item start. It
resolves a fixed-capacity immutable CPU plan from the workset binding,
preallocates its handoff storage, publishes that plan, and registers one
lossless passive subscription per unique trigger PC.

Native hits do not read through actor-owned `GuestMemory`. The frozen
session-local `StopCpuObserverDispatcher` routes the canonical derived observer
descriptor to the item sampler. On Dolphin's CPU thread, that sampler performs
only bounded read-only access through `IHitTimeGuestMemoryBackendPort` into
caller-owned fixed buffers. It performs no parsing, aggregation, allocation,
snapshot publication, or execution control. One fixed handoff packet contains
the routed identity plus every matching block/group record for that event.

On actor delivery, the service requires the exact packet, validates and derives
all affected replacements privately, and publishes them as one transaction.
Generations are assigned only at commit. A missing, stale, duplicate,
overflowed, unreadable, or malformed packet fails the job and publishes no
partial generation. Errors identify the block, group, trigger PC, address,
read size, and failure class; a null BattleState pointer is distinct from an
unreadable pointer.

Retained-current-point initialization is explicit descriptor metadata, not a
fallback. For the first Battle block, only `turn_entry` at `TurnInputs` permits
it. That path invokes the same static provider through paused actor-side
`GuestMemory` and requires the current workset epoch. A native hit with missing
CPU evidence can never fall back to a delayed actor read.

Item unwind first releases every derived subscription. Router release publishes
the replacement dispatch and waits for callbacks using the prior snapshot to
quiesce. Only then may the service unpublish the CPU plan and discard handoff
records, snapshots, and generations before reset. Nothing crosses an item,
workset, or wave boundary. Refresh and query failures fail the current job;
uncertain guest integrity retains the ordinary session-taint behavior.

Derived CPU acquisition runs in the passive stage before the foreground waiter
receives the same routed event. Successful observation returns
`request_break=false`; only a failed observation fails closed and asks Dolphin
to break. Derived state has no authority to stop, wake, resume, or interrupt
execution. `SameRoutedEvent` requires the exact routed identity and is mandatory
for predicate observations. `LatestInItem` uses the active host-authoritative
item and epoch and supports a later-turn baseline already paused at
`TurnInputs`.

## Capture isolation

Derived state reads only authoritative guest memory through registered static
providers. It has no capture-profile, capture-session, capture-output, or
capture-diagnostic interface. The dispatcher routes capture and derived
observer descriptors independently; they may receive the same routed identity
but share neither samples nor provider interfaces. Capture data cannot enter
programs, predicates, ranking, or control. `CaptureMark` remains one-way.
Capture incompleteness therefore cannot alter a derived-state decision.

## First block: `soa.derived.battle.core/1`

The first block provides three independent groups:

- `turn_entry` at `TurnInputs` (`0x80071740`): current turn and sparse sorted
  usable inventory totals;
- `turn_order` at `TurnIsReady` (`0x800715EC`): current turn and the ordered,
  unique active slot list terminated by `0xFF`;
- `rewards` at `EndTurn` (`0x800702A0`) and Victory (`0x800706D8`): current turn
  and sparse sorted cumulative drop totals.

Its typed queries return group snapshots with exact item, epoch, generation,
trigger, and routed-event provenance. Pure reducers expose current turn,
inventory/drop counts, player/enemy counts, and cohort min/max positions.
Absent items reduce to zero; min/max fails for an empty cohort. Duplicate item
IDs use checked sums. Invalid slots or positive drops with negative item IDs are
malformed evidence.

`battle.single_turn` selects this block by default. Battle Context, SeedProbe,
TAS Movie phases, and checkpoint sterilization declare empty defaults.
Predicate bundles may import these query actions and reducers; the exact action
imports select the block automatically. No general derived-state artifact is
published.

# Adaptive Synchronized Input

This document is the normative controller-input contract. It supersedes every
earlier input-tape, input-plan, pulse, held-neutral, neutral-witness, explicit
poll-action, static interaction sequence, or generic input-advance proposal in
this package.

## State-oriented invariant

One interaction retains one exclusive, epoch-bound controller lease. Lease
ownership and controller state are separate facts:

- `Neutral` means the lease owns no active controller command.
- `Held` means one non-neutral `GCInputFrame` is current.
- `DeliveryPending` means one exact frame is awaiting synchronized one-shot
  guest delivery.
- `NeutralTransitionPending` means a held command was released and the
  resulting neutral publication must be observed before another held command.

The canonical neutral GameCube frame has no buttons or triggers and centered
main/C sticks (`128`). It is not an eight-zero byte frame.

The public input ABI is:

- `runtime.input.acquire_lease`;
- `runtime.input.apply_state`, where a non-neutral frame establishes or
  replaces the held state and neutral implicitly releases it;
- `runtime.input.begin_delivery`, which publishes any exact frame, including
  neutral, for one synchronized guest delivery; and
- `runtime.input.complete_delivery`, which returns the one durable typed
  delivery receipt after exact guest observation.

Applying neutral while already neutral is an idempotent host-only operation:
it does not publish to Dolphin. Closing an already-neutral lease is likewise
host-only. Cleanup publishes neutral exactly once only when the backend remains
non-neutral; terminal cleanup does not invent a guest receipt when no later
guest execution will occur.

There is no public `InputPublishHeld`, `InputNeutralize`,
`InputAwaitGuestPoll`, neutral-witness type, or general poll-receipt type.

## Execution binding

`InputExecutionBinding` is ephemeral execution authority. It identifies the
exact lease, workset epoch, state generation, frame, and any backend
publication that must be observed. `ExecutionContinueUntil` and
`ExecutionStepFrames` accept an optional binding and `ExecutionEngine`:

1. validates the exact current lease state before guest advancement;
2. advances the guest;
3. completes the relationship only if Dolphin observed the exact fresh
   publication; and
4. cancels the relationship without fabricating evidence when execution is
   cancelled.

A stable-neutral no-op binding needs no publication or poll. Stale,
superseded, wrong-lease, wrong-epoch, or unobserved bindings fail. The verifier
rejects entrypoint results or emissions whose schema contains an
`InputExecutionBinding`; it cannot become durable phase output.

There is no:

- `InputPlan`, `ControllerInputSequence`, pulse or sequence payload;
- wrapper-owned cursor, next-frame operation, direct pad setter, or tape
  playback helper;
- generic vector input-advance port; or
- PhaseScript opcode that materializes or advances raw controller frames.

## Adaptive interactions

`InteractionComposition` lowers a finite verifier-known segment vocabulary,
not an executable list of controller frames. It keeps one lease for the full
interaction. A segment applies a semantic controller state and passes its
binding through every gate or frame advance for which that state must remain
authoritative.

A held segment releases by applying the canonical neutral frame. Its neutral
transition binding is consumed by the declared post-release gate, declared
neutral-frame dwell, or one synchronized frame before another held state may
begin. A neutral segment applies neutral; when the lease is already neutral
this causes no backend publication. Input-lease resource cleanup is the only
unwind authority, so composition adds no explicit neutral compensation.

Bounded memory-change waits advance one synchronized frame while the held
state remains authoritative. They fail structurally when their admitted finite
bound is exhausted. No observation is silently reused across guest
advancement.

## One-shot frame delivery

SeedProbe and first-turn `battle.single_turn` share one lowering:

1. acquire a scoped lease;
2. subscribe to the semantic endpoint set;
3. begin delivery of the exact candidate frame;
4. continue with its execution binding;
5. complete delivery after the endpoint; and
6. close the now-neutral lease.

SeedProbe retains one `InputDeliveryReceipt`. First-turn Battle validates and
discards that receipt before continuing from `AfterRandSeedSet` to
`TurnInputs`. A neutral candidate is published once for delivery and is not
followed by a duplicate neutral publication.

## Battle command entry

`battle.single_turn` captures a fresh live Battle Context at `TurnInputs`,
validates its per-job Battle Plan, and prepares adaptive command state. Dead or
unselectable actors/targets and missing inventory fail before command input.
`UseItem` remains representable and inventory-validatable but fails explicitly
as unsupported until its interaction is implemented.

For a fake attack, A remains held through the attack-target-ready semantic
point and the bounded RNG change. The interaction then releases to neutral,
advances exactly seven synchronized neutral frames, and applies B to return to
the command menu. The next adaptive transition is selected from live command
state; no target slot or menu path is precomputed as a raw frame tape.

## Durable evidence

Battle does not publish raw applied-input or input-trace artifacts, including
on failure. It retains semantic command diagnostics: stage/segment identity,
expected and observed point, PC, bounded poll/count evidence, and typed failure
status. Historical artifact rows and files remain readable but are not written
by the new path.

DTM handling remains exclusively in `MovieService`. A future explicit DTM
production phase may record raw controller input into a DTM; no other phase
stores raw input as its expected result surface.

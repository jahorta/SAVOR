# TAS Movie Phases

## Input epoch execution split

Production `tasmovie.annotate_input_epochs` is passive. A lossless capture
profile observes `PadReadReturned` without requesting a Dolphin break and
records the host movie-input cursor and guest `PADStatus` synchronously in the
routed CPU callback. The durable schedule remains only the ordered
`(movie_input_cursor, received_input)` pairs.

The schedule separately retains the source DTM poll count. Its final guest
epoch cursor may be lower when Dolphin consumes trailing SI polls after the
game's final `PADRead`; those polls do not fabricate additional guest epochs.

`tasmovie.input_epoch_breakpoint_diagnostic` retains the active stop/read/resume
loop solely for breakpoint-routing diagnosis. It is hidden from ordinary
authoring and uses reserved program kind `100`.

`tasmovie.rewrite_input_epochs` remains active because each held input must be
acknowledged by a guest PADRead before the next input is published. DTM cursor
counts are provenance and source-state lookup positions, not one-to-one timing
units.

## Current direction

TAS Movie production is organized around domain-specific recorders and guest-observed controller epochs.

- Domain phases such as `battle.record` create meaningful DTM segments. There is no generic Round 1 recorder.
- `tasmovie.annotate_input_epochs` replays a complete boot DTM and records the input state consumed at every return from the game's `PADRead` call.
- `tasmovie.rewrite_input_epochs` branches playback into recording, inserts one held neutral input epoch, and re-emits the remaining guest-input schedule.
- Later domain phases may consume the resulting DTM and movie-paired checkpoint. Rewriting does not create a TAS root or tree and does not run root validation automatically.

## Why guest input epochs

Dolphin may poll SI more than once between two game reads. A DTM record therefore is not a game input frame, and a presented video frame is not guaranteed to contain exactly one controller poll.

The canonical boundary is `soa.tasmovie.point.input.PadReadReturned` at `0x801D6E7C`. At this point the current game read has completed, so changing the macro input affects only future polls. The movie input cursor is used as provenance and to find the source state consumed by the completed read. It is not treated as a one-to-one timing unit.

## Annotation contract

`tasmovie.annotate_input_epochs` supports complete GameCube boot DTMs with controller port 0 only. Savestate-started, Wii, mixed-controller, empty, and malformed movies are rejected.

At each `PadReadReturned` stop the phase records:

- movie input cursor;
- canonical `GCInputFrame` decoded from Dolphin's packed DTM `ControllerState`.

The schedule is an ordered list, so position supplies the epoch ordinal and the
encoded list count supplies the epoch count. Neither value is duplicated in an
entry, and VI counts are not retained.

Each entry pairs the observed movie cursor with the actual port-0 `PADStatus` written by the just-completed guest `PADRead`. The cursor locates the visit relative to the source DTM; it is never used to infer the received input from a DTM record. Repeated cursors and cursor jumps are valid. Zero, regressing, or out-of-range cursors are divergence failures.

The immutable schedule is bound to the source DTM SHA-256 and stored as a `TAS_MOVIE_INPUT_EPOCH_SCHEDULE` State artifact.

## Rewrite contract

`tasmovie.rewrite_input_epochs` accepts a successful annotation attempt and an insertion epoch. It verifies the exact source DTM and unchanged prefix, then branches playback to recording while paused.

For the inserted neutral epoch and every remaining source epoch, the phase:

1. publishes the input through `InputBeginDelivery`;
2. holds that input until the next `PadReadReturned` using its exact delivery binding;
3. requires Dolphin to have sampled the binding;
4. completes delivery only after the guest return.

After the final source epoch, the phase captures a deferred endpoint savestate, publishes neutral, waits for a safe observed trailing poll, and finalizes the child DTM. The endpoint is persisted as a complete `MOVIE_PAIRED` savestate bound to that exact DTM.

The operation inserts one neutral guest-input epoch, not one neutral DTM record and not one neutral presented frame.

## Deferred work

- Checkpoint-to-checkpoint segment sources.
- Full TAS-tree publication for rewritten segments.
- SavorQt schedule inspection and insertion selection.
- Domain-specific recorders beyond the current Battle pipeline.

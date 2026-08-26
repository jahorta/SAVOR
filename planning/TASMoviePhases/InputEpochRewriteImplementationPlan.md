# Input Epoch Rewrite Implementation

## Annotation execution boundary

- Production annotation runs to authoritative movie end through
  `ExecutionContinueToMovieEnd` and has no foreground PADRead breakpoint.
- Its lossless capture profile samples `Movie::GetCurrentInputCount()` and the
  eight-byte guest `PADStatus` in the same routed CPU callback.
- Missing, dropped, malformed, regressed, overrun, or final-cursor-mismatched
  capture evidence fails annotation persistence closed.
- Active PADRead stopping exists only in the hidden diagnostic Full Phase at
  reserved program kind `100`.
- Rewrite remains an active, guest-acknowledged held-input protocol.

## Production identities

- Program kind 13: `tasmovie.annotate_input_epochs`
- Program kind 14: `tasmovie.rewrite_input_epochs`
- Stop point: `soa.tasmovie.point.input.PadReadReturned`, `0x801D6E7C`
- State artifact: `TAS_MOVIE_INPUT_EPOCH_SCHEDULE`
- Endpoint savestate type: `TAS_MOVIE_INPUT_EPOCH_REWRITE_ENDPOINT`

Both program kinds have independent immutable modules, Full Phase definitions, codecs, execution contracts, and production descriptors. Their workflow units are hidden and intended for composed graphs and E2E use.

## Runtime contracts

Annotation replays a complete boot DTM read-only to movie end. At every post-`PADRead` stop it records only the nondecreasing movie cursor and the actual port-0 `PADStatus` written by that completed guest call, converted to the canonical controller state. The cursor is DTM annotation provenance and never supplies the received input. List order supplies the epoch ordinal and the encoded list count supplies the epoch count. Cursor jumps and repeats are accepted; malformed cursor evidence returns a precise divergence.

Rewrite verifies the annotation's source hash and prefix, branches read-only playback to recording while paused, and acquires the port-0 input lease. Each output state is held across SI polling until a correlated `PadReadReturned` acknowledges that guest-input epoch. The phase inserts exactly one neutral state before the selected source epoch, then re-emits the entire source suffix.

The final source epoch is followed by deferred savestate capture and one observed neutral trailing poll before movie finalization.

## Persistence shape

AnalysisTasMovie stores immutable annotation and rewrite requests plus idempotent attempts keyed by execution job and terminal SHA. Records preserve workflow/step/job identity, source and schedule hashes, insertion ordinal, output artifact IDs, endpoint savestate ID, counts, worker provenance, and divergence diagnostics.

State stores schedules, rewritten DTMs, and endpoint savestates. A successful rewrite endpoint is complete, `MOVIE_PAIRED`, and references the exact rewritten DTM artifact.

Archive closure and rehydration include the four request/attempt streams and all referenced source, schedule, DTM, and savestate records.

## Failure and cleanup behavior

- Returned source, cursor, prefix, or delivery divergence is persisted as a failed domain attempt and failed execution job.
- Malformed or unpersisted worker results remain available to generic result recovery.
- Result staging cleanup is queued only after durable domain and execution finalization.
- Generated schedule, DTM, SAV, and applicable sidecars participate in staging cleanup.
- Replays with the same execution job and terminal SHA are idempotent.

## E2E acceptance

The `tasmovie_input_epoch_rewrite` scenario uses one worker and the standard complete root DTM:

1. Import and annotate the source DTM.
2. Find the final adjacent exact canonical B-only then A-only epochs.
3. Insert neutral immediately before the B epoch.
4. Annotate the child DTM through movie end.
5. Compare canonical input states against `source[0..B) + Neutral + source[B..end)`.
6. Verify the source bytes are unchanged, hashes differ, and the endpoint is paired with the exact child DTM.

The scenario reports insertion epoch/cursor, source and child counts, artifact IDs and hashes, endpoint savestate identity, and all three workflow IDs.

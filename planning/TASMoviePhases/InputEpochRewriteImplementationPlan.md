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

- Program kind 13: `tasmovie.annotate`
- Program kind 14: `tasmovie.revise`
- Stop point: `soa.tasmovie.point.input.PadReadReturned`, `0x801D6E7C`
- State artifact: `TAS_MOVIE_INPUT_EPOCH_SCHEDULE`
- Endpoint savestate type: `TAS_MOVIE_INPUT_EPOCH_REWRITE_ENDPOINT`

Both program kinds have independent immutable modules, Full Phase definitions, codecs, execution contracts, and production descriptors. Their workflow units are hidden and intended for composed graphs and E2E use.

## Runtime contracts

Annotation replays a complete boot DTM read-only to movie end. At every post-`PADRead` stop it records only the nondecreasing movie cursor and the actual port-0 `PADStatus` written by that completed guest call, converted to the canonical controller state. The cursor is DTM annotation provenance and never supplies the received input. List order supplies the epoch ordinal and the encoded list count supplies the epoch count. Cursor jumps and repeats are accepted; malformed cursor evidence returns a precise divergence.

Rewrite verifies that its annotation and semantic root-establishment inputs name the same immutable source DTM, replays the prefix, branches playback to recording while paused, and acquires the port-0 input lease. Each output state is held across SI polling until a correlated `PadReadReturned` acknowledges that guest-input epoch. The phase inserts exactly `neutral_epoch_count` neutral states before the selected source epoch, then re-emits the entire source suffix. Equal adjacent input states are coalesced into held runs without changing guest-observed epoch count.

Once the final source epoch has been sampled and guest-acknowledged, the phase observes the final cursor, captures the deferred endpoint savestate, and finalizes recording while Dolphin remains paused. It does not add a trailing guest-input epoch.

## Persistence shape

AnalysisTasMovie stores immutable annotation, semantic root-establishment, and rewrite authorities plus idempotent attempts keyed by execution job and terminal SHA. Annotation and root authorities identify their producer as `ANNOTATE`/`REVISE` and `ESTABLISH`/`REVISE`. Records preserve workflow/step/job identity, exact source and itinerary hashes, observed root PC and child movie cursor, insertion ordinal/count, output artifact IDs, endpoint savestate ID, worker provenance, and divergence diagnostics.

State stores schedules, rewritten DTMs, and endpoint savestates. A successful rewrite endpoint is complete, `MOVIE_PAIRED`, and references the exact rewritten DTM artifact.

Archive closure and rehydration include request/attempt streams, root authorities, chain parents, and all referenced source, schedule, itinerary, DTM, and savestate records.

## Failure and cleanup behavior

- Returned source, cursor, prefix, or delivery divergence is persisted as a failed domain attempt and failed execution job.
- Malformed or unpersisted worker results remain available to generic result recovery.
- Result staging cleanup is queued only after durable domain and execution finalization.
- Generated schedule, DTM, SAV, and applicable sidecars participate in staging cleanup.
- Replays with the same execution job and terminal SHA are idempotent.

## E2E acceptance

The `tasmovie_input_epoch_rewrite` scenario uses one worker and the standard complete root DTM:

1. Import, establish, and annotate the source DTM.
2. Find the final adjacent exact canonical B-only then A-only epochs.
3. Revise with exact count `1` immediately before the B epoch and consume its child authorities directly.
4. Chain a second revise with exact count `2` immediately before the shifted B epoch.
5. Compare the first child against `source[0..B) + Neutral + source[B..end)` and the chained child against `source[0..B) + Neutral x 3 + source[B..end)`.
6. Verify the source bytes are unchanged, hashes differ, each endpoint is paired with its exact child DTM, and no standalone child annotation or establishment workflow ran.

The scenario reports insertion epoch/cursor, source and child counts, artifact IDs and hashes, endpoint identities, and the source-establish, source-annotate, and two revise workflow IDs.

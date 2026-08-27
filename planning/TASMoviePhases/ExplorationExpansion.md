# TAS Movie Exploration Expansion

TAS exploration is represented by a durable workflow family above ordinary
workflow instances. A family owns reusable preparation branches and creates
ordinary child workflows for executable work.

## First Battle Exploration

Inputs are prepared annotation/root authorities, an inclusive RTC range, and a
maximum neutral guest-input delay. A range request materializes every exact
`(delay, RTC)` coordinate for delays `0..N`. Each delay uses one
`tasmovie.revise` child, whose child annotation and root authority feed its
validation-through-Battle workflow directly.

## Delay Exploration

The source is a qualified TAS route node. Its annotation and semantic root
establishment supply the DTM schedule, inherited boundary, and RTC. A range
request expands to the dense range `0..N`; an explicit target is one exact
delay coordinate and never implies lower-delay children. The remaining
structure is identical to First Battle Exploration.

## Placement and revision

The initial placement profile is `first_battle.final_dialog`. It selects the
last exact B-only to A-only transition in the input-epoch schedule and inserts
the requested number of neutral guest-input epochs before the B epoch.
`tasmovie.revise` performs the entire requested insertion in one execution.

## Persistence and recovery

Expansion and member identities live in the Execution database. Child work is
always a normal workflow graph instance with a deterministic launch key, so
restart replay cannot duplicate a branch. Failed or interrupted children park
only their branch; unrelated delays and RTC values continue. Normal workflow
retry reopens the child and the family coordinator observes its new current
result.

No revised-DTM cache or seed-level deduplication is implemented yet. A future
cache key should be based on captured seed plus TAS route node rather than RTC.

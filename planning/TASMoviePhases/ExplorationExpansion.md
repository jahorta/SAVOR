# TAS Movie Exploration Expansion

TAS exploration is represented by a durable workflow family above ordinary
workflow instances. A family owns reusable preparation branches and creates
ordinary child workflows for executable work.

## First Battle Exploration

Inputs are a root DTM artifact, an inclusive RTC range, and a maximum neutral
guest-input delay. Delay zero starts as soon as the source establishment is
available. Positive delays share one passive `tasmovie.annotate` child, then
each delay uses one `tasmovie.revise -> tasmovie.establish` child. Every
`(delay, RTC)` pair receives its own validation-through-Battle child workflow.

## Delay Exploration

The source is a qualified TAS route node. Its established root supplies the
DTM and inherited RTC. Delays are always the dense range `0..N`; lower delays
are admitted first. The remaining structure is identical to First Battle
Exploration.

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

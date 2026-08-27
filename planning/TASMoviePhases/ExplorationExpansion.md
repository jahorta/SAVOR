# TAS Movie Exploration Expansion

TAS preparation and exploration are authored as real workflow graphs. A graph
revision declares either the normal `WORKFLOW` execution shape or an
`EXPANSION` shape with a named expansion kind. Data edges transfer typed
references; control edges impose ordering without fabricating a data port.

## TAS Prepare Root

`TAS Prepare Root` runs `tasmovie.establish -> tasmovie.validate`, then uses a
control dependency to run `tasmovie.annotate` only after validation succeeds.
The imported root DTM is supplied to both establish and annotate. Its durable
result is the exact annotation/root-establishment authority pair for one DTM.

## First Battle Exploration

`First Battle Exploration` is an `EXPANSION/TAS_FIRST_BATTLE` graph whose
authored shape is `tasmovie.revise -> tasmovie.validate -> sterilize ->
SeedProbe/Battle Context -> Battle`. Inputs are a prepared annotation/root
authority pair, an inclusive RTC range, and a
maximum neutral guest-input delay. A range request materializes every exact
`(delay, RTC)` coordinate for delays `0..N`. Each delay uses one
`tasmovie.revise` child, whose child annotation and root authority feed its
validation-through-Battle branches. Delay zero bypasses revision and uses the
prepared root directly. Each positive delay is revised once and reused for all
requested RTC values.

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

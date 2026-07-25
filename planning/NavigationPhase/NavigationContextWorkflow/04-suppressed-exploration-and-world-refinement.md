# 04 - Navmesh Survey and World Refinement

## Status

Future implementation. The **Navmesh Survey** consumes one concrete Navigation Context export and its
matching savestate as the starting point for a runtime survey of one loaded area. Its goal is a complete,
runtime-verified per-area navmesh: stable places where the player can stand, passable connections,
collision boundaries, door portals, and the access conditions attached to those portals.

The Survey refines the static SAVOR-owned world rather than replacing its source geometry. Parsed MLD
ground, wall, trigger, and MovingObject data supplies candidate topology; isolated Dolphin workers test
that topology against the game. A deterministic reduction combines immutable spatial observations into a
new `NavigationWorldRefinement`.

Survey evidence is spatial. It records requested positions, resulting and settled positions, facing where
relevant, pass/block/fall/correction outcomes, collision boundaries, connectivity, and door metadata. It
does not infer a probe clock, VI-frame cost, movement schedule, or optimization model. Bounded execution
and retry limits are worker safeguards, not navmesh observations.

The draft Navigation Context phase and the concrete `a101b` export described below exist. The Navmesh
Survey itself is not implemented.

## Current First-Slice Inputs (`a101b`)

The common survey bootstrap is:

- savestate:
  `C:\savor\navigation_context_a201a_a101b_20260723_2100\navigation-context-verification\navigation-context-41.sav`
- adjacent Navigation Context export:
  `C:\savor\navigation_context_a201a_a101b_20260723_2100\navigation-context-verification\navigation-context-41.nctx`

Every first-slice worker reloads this same baseline. A survey anchor is not another savestate.

The established first-slice runtime controls are:

- encounter suppression writes one byte (`u8`) with value `0` at `0x8030b7ad`;
- trigger selection/commit is controlled at `0x80117e8c`;
- the original/enabled instruction is `0x480F86C5` (`bl 0x80210550`);
- the suppressed instruction is `0x48000018` (`b 0x80117ea4`); and
- `eventhook` is outside the first slice. Initial door-related scope is the path used by `motscpt`,
  `wallmot`, and `goscript`.

The executable-word toggle must use a paused, reversible patch path that verifies the expected original
word, performs required instruction-cache handling, reads the result back, and can restore the recorded
word. The first slice does not require a breakpoint, code cave, permanent hook, or generalized trigger
allowlist.

## Per-Area Boundary

Each loaded area is surveyed into its own file and refinement. An area-load transition is recorded as a
boundary or destination reference; it is not represented as a stateful positional anchor in the source
area. Positional anchors only need to replay inside the area whose common bootstrap is named by the
request.

## Evidence-Layer Separation

The workflow keeps four relevant classes distinct:

| Evidence class | Runtime modifications | Permitted use |
|---|---|---|
| Common survey bootstrap | None beyond the state already captured in the named `.sav` | Reload source for every anchor and survey job |
| Static content/world | None; parsed from disc content | Candidate geometry and authored metadata |
| Isolated live survey job | Encounter suppression, reversible trigger toggle, and recorded job-local BitVar overrides | Establish anchors and collect spatial observations |
| Survey output | Immutable anchor, portal, observation, coverage, and refinement artifacts | Per-area navmesh and later planning input |

No modified live state is promoted to a Navigation Context, prediction start, or clean validation input.
The common bootstrap is never overwritten. A failed or completed worker is disposable even though its
accepted observations are durable.

## Runtime Modification Rules

Encounter suppression and trigger suppression are independent operations.

### Encounter suppression

Each survey worker writes byte value `0` at `0x8030b7ad` before exploration and verifies the write. The runtime
modification record includes the address, original value, written value, runtime/disc compatibility,
application result, and cleanup result.

### Normal trigger suppression

Ordinary anchor movement, teleport settling, and navmesh probes run with `0x48000018` installed at
`0x80117e8c`. This bypasses the trigger-selection commit call while leaving the rest of the worker under
normal game execution.

### Door activation window

To interact with an intended door, the worker:

1. approaches and faces the door while triggers are suppressed;
2. pauses and restores `0x480F86C5` at `0x80117e8c`;
3. issues the intended interaction during a bounded runner-controlled execution window;
4. pauses and immediately reinstalls `0x48000018`;
5. verifies the patched-word readback; and
6. continues the door script with ordinary triggers suppressed.

The enable window is not intrinsically TBLID-selective. The worker therefore verifies the selected object
after the window and rejects a mismatched activation.

### BitVar-locked doors

The Survey distinguishes physical navmesh completeness from the initial gameplay availability of a door.
When an intended door is locked by a known BitVar, an isolated anchor-establishment job may change only
that bit to its unlocked value using read-modify-write, retry the door, and then discard the modified live
state.

The portal record retains:

- the door TBLID;
- `initially_locked`;
- the controlling BitVar and locked/unlocked polarity;
- the original value and temporary survey override; and
- the successful activation, crossing, and successor-anchor evidence.

The final navmesh contains the physical connection, while later gameplay-aware planning can respect its
recorded access condition.

## Survey Waves

The first implementation uses two explicit worker waves.

### Wave 1 - establish survey anchors

Anchor-establishment workers begin at the Navigation Context position in the common bootstrap and expand
through the loaded area. When a door separates regions, the worker uses the door activation window,
applies a recorded BitVar override when necessary, crosses the doorway, and proposes a position on stable
ground on the far side.

A door interaction succeeds only when:

1. the selected interaction object is the intended door;
2. any door-specific completion evidence indicates the open branch completed; and
3. the player physically crosses to the far side and settles at a usable position.

For the current runtime, mode-1 interaction selection leaves the selected object pointer at `0x8034744c`.
The worker validates that it is non-null/readable before using `[object + 8]` as the selected TBLID.
Generic activity at `0x80347408` is not sufficient identity evidence.

For `a101b` door TBLID `4101`:

- BitVar `2556` is the lock condition and must be cleared with a read-modify-write of word `0x80310c78`,
  mask `0x10000000`;
- BitVar `1555` is word `0x80310bfc`, mask `0x00080000`; it is set after the open/collision motion
  completes and is a door-specific completion witness; and
- physical crossing and far-side settling remain the final navmesh evidence.

Wrong TBLID, a locked/no-open result, or failure to cross produces no successor anchor.

### Anchor replay validation

Before publishing a proposed anchor, a worker validates the same operation later workers will use:

1. reload the common bootstrap;
2. reinstall the survey runtime controls;
3. teleport the player to the proposed position and optionally restore facing;
4. resume ordinary game updates so the game reconstructs ground and collision state; and
5. accept the anchor only if the resulting position settles near the request and remains usable.

The first slice deliberately does not serialize a full ground-selector record, persist worksheet or ground
pointers, or create a per-anchor savestate. A small correction may be canonicalized to the resulting
position only when it remains within settle tolerance and on the intended side of the door. A drop,
wrong-side correction, or snap-away rejects the candidate.

### Wave 2 - parallel spatial survey

Subsequent workers each:

1. load the same common bootstrap;
2. apply encounter and trigger suppression;
3. teleport to one verified positional anchor;
4. pass the same settle/usable-position check; and
5. probe an assigned position, surface, boundary, portal, or candidate cluster.

Workers do not share mutable game state or mutate a shared navmesh. They emit immutable observations for
deterministic reduction.

## Survey Anchor

A `NavigationSurveyAnchor` records:

- stable anchor identity and parent/reachability lineage;
- common bootstrap `.sav` and `.nctx` identities;
- area identity;
- requested and settled position;
- optional facing;
- teleport-and-settle validation evidence;
- door/portal reference when a door established reachability; the portal owns initial-lock and temporary
  BitVar-override metadata; and
- worker, runtime, disc, and runtime-modification identities.

Local teleport validity and proven reachability from the initial Navigation Context position remain
separate facts. Wave 1 supplies reachability; replay validation proves that later workers can reuse the
position.

## Probe Generation

`SavorNavigation` generates spatial candidates from:

- ground-triangle interiors and edges;
- projected wall intersections and near-boundary offsets;
- GRND/GOBJ overlaps and stacked surfaces;
- derived portals and uncertain graph links;
- doors and MovingObject boundaries;
- ramps, stairs, corners, and handoffs; and
- coverage gaps or contradictions from earlier observations.

Candidate generation may partition work by anchor, geometry region, surface, boundary, or approach
direction. It does not attach a survey clock or convert spatial samples into frame costs.

## Navigation Geometry Observation

Each immutable observation records:

- static world/graph and source-geometry identity;
- common bootstrap and survey-anchor identity;
- exact runtime-modification and job-local BitVar-override history;
- requested start, approach, and target positions;
- ordered resulting positions and facing where spatially relevant;
- observed contact or position correction when available;
- active area and attributable surface/resource identity when available from the live game;
- observed pass, block, fall, slide, warp, interruption, wrong-target, or settle-failure outcome;
- end position and coverage contribution; and
- runtime, worker, attempt, confidence, and diagnostics.

Ordered positions preserve spatial causality without turning the Survey into a timing model. Controller
realization, VI-frame costs, and temporal movement-response experiments belong to later work.

## Door and Portal Representation

A door that is initially locked does not produce a separate locked navmesh and unlocked navmesh merely
because its access flag differs. The Survey records the physical portal once and attaches the known access
constraint. The portal links its pre-door and far-side anchors and retains the evidence that proved the
connection.

General automatic-trigger boundary characterization, interactable activation-envelope measurement, and
`eventhook` behavior are deferred. The first slice controls triggers only as required to establish door
anchors safely.

## Building the Refinement

`nav.build_refinement` is deterministic over:

- static world identity;
- ordered anchor and observation artifact identities;
- runtime-modification and BitVar-override histories;
- refinement algorithm and parameters; and
- coordinate-policy version.

It emits a new immutable per-area refinement containing:

- validated passable and blocked local regions;
- refined adjacency and portal spans;
- door portals with initial-lock/access metadata;
- source-triangle provenance;
- tested, untested, contradictory, and unresolved coverage; and
- links to every supporting or contradicting observation.

New evidence creates a descendant refinement rather than editing an older result in place. No worker
silently rewrites extracted geometry.

## Failure Semantics

- Patch precondition or readback mismatch fails the job before it contributes geometry evidence.
- Partial patch application or cleanup failure quarantines the worker result.
- Encounter interruption under encounter suppression is a profile failure, not a passability result.
- Unexpected or wrong-TBLID trigger activation is an interruption and produces no door successor anchor.
- A BitVar override that changes more than its recorded bit invalidates the job.
- A teleport that does not settle near a usable position produces no anchor or passability claim.
- A door interaction that does not prove the intended TBLID, opening, crossing, and far-side settle
  produces no successor anchor.
- A worker failure preserves diagnostics but contributes no positive geometry claim beyond its last
  accepted observation.

## First-Slice Acceptance (`a101b`)

The first slice is successful when it can:

1. load `navigation-context-41.sav` and the adjacent `.nctx` as the common bootstrap;
2. apply encounter suppression and the reversible trigger-suppression instruction;
3. activate door `4101` only during a short enable window and verify its TBLID;
4. detect its initially locked condition, clear only BitVar `2556` in the disposable job, and record that
   override;
5. observe BitVar `1555`, cross the doorway, and settle on the far side;
6. replay the far-side position from the untouched common bootstrap by teleport and settle;
7. start parallel workers from the initial and far-side anchors; and
8. reduce their spatial observations into a per-area refinement without storing anchor savestates,
   serialized ground-selector records, or timing evidence.

These capabilities must be implemented in the Survey phase and worker path. A custom `SavorE2E` scenario
may exercise the slice end to end, but it is a validation harness rather than the only place the behavior
exists.

## Acceptance Rules

- The Survey begins from one explicitly named `.sav`/`.nctx` pair and never mutates that baseline.
- Every worker is isolated and every runtime write or executable patch is verified and auditable.
- The first-slice trigger toggle uses the exact recorded instruction words at `0x80117e8c`; generalized
  trigger-control research does not block door-anchor implementation.
- Anchor fan-out uses common-baseline teleport and settle validation, not per-anchor savestates.
- Area loads remain separate survey files.
- Initially locked doors remain present as physical portals with their BitVar access constraints.
- Persistent Survey evidence is spatial and contains no inferred probe timing.
- Parallel workers emit anchors and observations; deterministic reduction alone emits a refinement.
- Every refined fact links back to static geometry and runtime evidence.

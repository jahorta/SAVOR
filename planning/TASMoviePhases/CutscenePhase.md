# TAS Movie Cutscene Phase

## Purpose

`tasmovie.cutscene` is a recording phase for non-interactive cutscene traversal. It starts from a successfully validated movie-paired TAS tree, records to the next qualified TAS boundary, validates the child tree internally, and can feed that validated tree directly into another explicit Cutscene node.

## Dialogue handler

Dialogue advancement is an invocation handler selected by `DialogueAdvance`; it is not a separate program phase. The execution core retains sole foreground control.

- `TextRevealInputReady` at `0x8010D300` starts a trusted interruption.
- The handler publishes B-only and holds it until `PadReadReturned` at `0x801D6E7C` acknowledges guest consumption.
- On the same input lease, B is replaced directly with A without an intermediate neutral publication.
- A is held through the next `PadReadReturned`, then delivery completes to neutral and the parent execution resumes.
- `ChoiceInputReady` at `0x8010CFD4` fails with `CUTSCENE_CHOICE_UNSUPPORTED`. Choice branching is deferred and must not silently select an option.

The handler is reusable by future phases through the same invocation flag.

## Battle Results handler

Cutscene also enables the reusable `BattleResultsAdvance` interruption. If the
recorded route encounters Results `DescriptorReady` at `0x800E35F0`, the
handler advances the observed Results presentation through cleanup and resumes
Cutscene recording. Cutscenes that do not follow battles never trigger the
handler. No Battle Completion manifest or additional Cutscene input is
required.

Battle Results and Dialogue share one active trusted-interruption slot. Their
invocation routes are withdrawn while either handler owns control and restored
before the parent recording operation resumes.

## Endpoint selection

The phase ignores the source checkpoint itself and requires movie-input cursor advancement. The first eligible future endpoint wins:

1. Pre-battle `BeforeRandSeedSet` at `0x80101E48`.
2. Field fast preseed at `0x80101894` when `0x803475D4` is nonzero.
3. Field deferred preseed at `0x801018AC` when the fast point is not accepted.

## Persistence

The phase captures a deferred movie-paired SAV at the selected endpoint, records a neutral trailing poll, and finalizes the child DTM. Result handling appends the observed endpoint to the parent itinerary and publishes:

- a `TAS_MOVIE_CUTSCENE_ENDPOINT` movie-paired savestate;
- a child TAS tree with source context `CUTSCENE`;
- a durable AnalysisTasMovie cutscene request and attempt.

The normal workflow transition appends `tasmovie.validate_tree`. It does not sterilize the result.

The Cutscene unit does not publish downstream graph availability until that
internal validation completes. Chaining is explicit in the authored workflow;
the phase never launches itself recursively. Dialogue and Battle Results are
optional interruption handlers, so a successful Cutscene may activate either,
both, or neither.

## Optional seed-call diagnostics

Cutscene may opt into the code-owned progress library
`soa.progress.soa.seed_calls/1`. It passively observes
`RNG::srand_8025ecbc` at `0x8025ECBC` and records the seed argument, prior RNG
state, movie-input cursor, VI count, PC, and a bounded caller stack. The library
is disabled by default and is not enabled for SeedProbe or Battle phases. Zero
observed calls is a valid diagnostic result.

## Deferred work

- Dialogue-choice discovery and user-directed branch checkpoints.
- Additional non-dialogue cutscene interactions beyond Battle Results.
- Richer UI presentation and comparison of seed-call evidence.

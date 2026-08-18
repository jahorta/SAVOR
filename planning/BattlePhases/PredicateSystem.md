# Typed Predicate System

This document defines the current predicate authoring and execution model. It
is normative for the hard-cut implementation. There is no predecessor codec,
database adapter, runtime alias, or import path.

Predicate evaluation progress follows the canonical observation contract in
[`../ExecutionRuntime/17-semantic-routing-workset-capture-canonical-progress.md`](../ExecutionRuntime/17-semantic-routing-workset-capture-canonical-progress.md).
Typed derived observations follow
[`../ExecutionRuntime/18-derived-state-runtime.md`](../ExecutionRuntime/18-derived-state-runtime.md).
Capture samples never become predicate evidence.

## Canonical authored concepts

The system has three authored concepts and one resolved execution form.

### Predicate Definition

A Predicate Definition is a reusable, pure typed Boolean expression. A
definition revision contains:

- named, ordered typed witnesses;
- a finite expression graph;
- a Boolean root node; and
- exact imported reducer identities where specialized pure calculations are
  required.

Definitions contain no semantic hooks, guest addresses, concrete parameter
values, occurrence rules, reactions, aggregation rules, or evidence policy.
They perform no guest access and have no execution side effects.

The expression graph supports typed literals and witnesses, equality and
ordered comparisons, Boolean operators, checked arithmetic, and exact
registered reducers. The authoring backend validates arity, graph order,
operand and result types, reducer signatures, and the Boolean root before a
revision can be published.

### Predicate Execution Binding

A Predicate Execution Binding specializes one exact published definition
revision. The author supplies the concrete values which make that predicate
specific, such as `item_id=273`. Semantic Battle-state witnesses are planned
by the backend rather than selected by the user. Runtime source forms are:

- a concrete typed value;
- a backend-planned derived-state query captured at its registered hook and
  read later in the same Battle item;
- a field from the current routed hook receipt;
- a pinned read-only guest-memory read; or
- a baseline observation captured at one exact hook with `First` or `Latest`
  update policy.

Concrete values are binding content. For example, `item_id=273` is part of the
execution-binding revision. Changing it creates a different binding revision
and hash; it does not mutate the definition, Battle Plan, wave, or workflow
arguments.

Each source must exactly match its witness type and any registered action or
schema dependency. The user cannot override the source chosen for a semantic
Battle value. Execution Binding revisions are immutable after publication.

### Predicate Group

A Predicate Group is an ordered collection of atomic predicate memberships.
Each `PredicateGroupMemberV1` contains:

- one exact published Execution Binding revision;
- a sorted, unique, nonempty set of semantic hooks;
- one occurrence policy and any ordinal or compatible guard binding;
- one reaction;
- aggregation participation; and
- durable-evidence policy.

An Execution Binding revision may occur only once in a group. Multiple hooks
do not create separate predicates. They are mutually visible entry points into
one atomic member. `First`, `Every`, `Ordinal`, and `GuardOnce` operate over the
combined routed-hook stream in routed order.

For example, a cumulative-drop member may name both `EndTurn` and
`EndBattleVictory`. Those hooks are mutually exclusive for a normal Battle
turn, so `First` evaluates the same bound predicate at whichever terminal hook
is reached. There is no separate hook-use provenance object.

Group revisions reference only published Execution Bindings. Hooks, occurrence,
reaction, aggregation, and evidence policy belong only to group memberships;
parameters and witness sources do not.

### Predicate Execution Package

Coordination resolves one exact published group into a
`PredicateExecutionPackageV1`. The package contains the exact group, Execution
Bindings, Predicate Definitions, phase identity, semantic-hook contract, and
dependency closure needed by the worker. It is canonical, immutable, and
content-hashed.

Workers receive the package bytes and never query Authoring, Analysis, or
Execution databases. Database identifiers are lineage only; exact hashes and
typed identities are execution authority.

When a Battle turn has no selected Predicate Group, coordination and the worker
use the canonical runtime-only empty group and empty Execution Package. No
empty authored database row exists.

## Authoring lifecycle

Definitions, Execution Bindings, and Groups use the same revision discipline:

- the Authoring database transactionally generates one opaque, type-prefixed
  128-bit stable key when a logical object is created;
- callers never propose or edit a stable key;
- the parent row owns mutable `name` and `description` presentation metadata;
- metadata-only updates retain the stable key and every revision and content
  hash, including when the current revision is published;
- each revision owns a backend-computed semantic fingerprint over executable
  content only, excluding its stable key, revision number, name, and
  description;
- one stable identity may have at most one draft;
- saving updates that draft;
- editing a published revision creates the next draft revision;
- saving semantics already present in the same lineage returns the existing
  revision without rewriting its relational children;
- an explicit Duplicate operation creates a new stable identity and revision
  1, even when its semantic fingerprint matches the source;
- abandoning deletes only a draft; and
- published revisions are immutable.

Create and Duplicate commands carry a 128-bit creation-request key generated
once per editor intent. The Authoring database records the operation, request
key, canonical request hash, and returned identity in the same transaction.
An exact retry returns the original receipt without writes; reusing that key
with different content is an integrity conflict. Publication and metadata set
operations are likewise idempotent.

These identities have deliberately different jobs:

- the generated stable key identifies the logical authored object across
  revisions;
- name and description are current mutable presentation metadata;
- the semantic fingerprint detects executable-content equality within that
  logical lineage; and
- the exact immutable revision and runtime package hashes remain execution and
  provenance authority.

Group drafts may reference only published Execution Bindings. Battle Plans may
reference only published Groups.

The backend-owned `PredicateAuthoringCatalogV2` is built deterministically from
the phase hook contract, capability packs, and derived-state registry. In
addition to the exact typed identities required for validation, it owns the
friendly hook, query, reducer, value-recipe, argument, category, and semantic
input metadata used for authoring. The same catalog drives Qt filtering, draft
validation, publication, materialization, and package dependency resolution.
Qt does not hardcode action hashes, reducer identities, type compatibility, or
source availability.

The Qt authoring surface provides:

- a **Predicates** library that displays definitions with their named
  Execution Bindings beneath them;
- a guided Definition editor phrased as values, comparisons, and nested
  `All`/`Any`/`Not` conditions;
- a guided Execution Binding editor containing only the concrete values needed
  to specialize the Definition;
- a **Predicate Groups** library with ordered, policy-oriented predicate cards;
  and
- a published-group selector on each Battle Plan turn.

The three editors are intentionally connected as one authoring path:

1. **What must be true?** The Definition editor starts with a
   `left value -> comparison -> right value` condition and can grow into a
   Boolean tree. Domain choices such as *Last player turn position* and
   *Cumulative item drop count* compile through catalog recipes into the exact
   typed reducer graph.
2. **Which concrete values does it use?** Creating a Binding from a published
   Definition preselects that exact revision. Required Battle snapshots are
   planned automatically, while parameters such as `item_id` use typed value
   controls.
3. **When is it evaluated, and what happens on failure?** Creating or extending
   a Group from a published Binding uses friendly hook chips and policies such
   as *first matching hook*, *record and continue*, and *reject this job*.

The guided tree is an authoring-only representation. The backend compiles it
deterministically into the existing witnesses and expression nodes, infers all
types and the Boolean root, and can reconstruct a guided tree from any valid
stored Definition. The editable surface uses a compact semantic tree and one
selected-item detail pane: a left
game value, a compatible comparison, and a right game value, with `All`, `Any`,
and `Not` available as condition structure. Values are selected as Battle
values, fixed values, or calculations. Semantic inputs and recipe parameters
are generated by the backend; the Definition editor never asks users to name
or type them.

The collapsed read-only Technical Details panel shows generated inputs,
inferred types, reducer descriptions, and the compiled expression structure.
Opaque stable keys, database IDs, hashes, node indexes, and dependency
identities are absent from the UI. An unchanged reopened Definition retains
its original semantic body so presentation normalization alone cannot create a
revision.

Editors do not ask the user for stable keys. A new editor retains its
creation-request key across failed or retried asynchronous saves, adopts the
backend-returned object and revision references invisibly after the first
successful save, and creates a new request key only for an explicit Duplicate
intent. Save feedback distinguishes metadata, semantic, and no-op changes and
refers to the authored object by name.

Existing databases whose earlier `202608161400` migration name masks the
interim physical schema are repaired by the forward-only
`202608162300_authoring_predicate_identity_schema_repair.sql` migration. The
migration preserves non-predicate authoring and Battle Plans, clears interim
predicate rows and turn selections under the approved hard cut, and recreates
the current request-ledger, revision, child, index, and immutability schema.
Migration history is advanced only by the normal migration runner.

Raw JSON, canonical hashes, generated dependency identities, and package bytes
are not editable UI fields.

## Runtime semantics

### Evaluation hooks and automatic snapshot acquisition

The prepared Full Phase exposes an exact versioned semantic-hook contract.
Predicate Group hooks answer only *when should this predicate be evaluated?*
They do not select snapshot-capture breakpoints. The backend catalog separately
declares which hooks refresh each derived snapshot and the later hooks where
that snapshot is valid. Group publication and workset materialization prove
that:

- every member hook is present in that contract;
- every required snapshot can be captured before every selected evaluation
  hook;
- baseline capture hooks are valid;
- guard bindings are type- and hook-compatible;
- imported reducers and registered queries match exact identities; and
- all expression and source types lower successfully.

The derived-state service registers capture hooks independently of the Group
and refreshes snapshots passively. Predicate evaluation reads the authoritative
`LatestInItem` snapshot. Turn order can therefore be captured at `TurnIsReady`
and evaluated later at `EndTurn` or Victory; turn-entry state can be captured
at `TurnInputs` and evaluated at either terminal hook. Rewards are captured and
evaluated on the mutually exclusive terminal path. Missing, stale, malformed,
or failed source evidence is a job failure, never a false predicate or an
additional evaluation status.

### Occurrence and reactions

Occurrence policies are:

- `First`: evaluate on the first matching member hook;
- `Every`: evaluate on every matching member hook;
- `Ordinal`: evaluate on one combined-stream ordinal; and
- `GuardOnce`: evaluate once when a compatible typed guard becomes true.

An executed member returns exactly `Passed` or `Failed`. `RecordAndContinue`
records the result and permits execution to continue. `AbortOnFail` records the
failing result before returning a successful typed predicate-rejection Battle
outcome. A false condition is not an infrastructure failure.

Every participating evaluation increments total; only `Passed` increments
passed. A member whose hook or occurrence condition is never reached does not
count. Checks skipped after rejection do not count.

Evidence identifies the definition revision, Execution Binding revision,
Predicate Group revision, actual hook, observed values, and result. Detailed
evidence is durable when requested by the member or required by rejection or
failure. There is no synthetic per-hook-use identity.

## Battle ownership and materialization

A Battle Plan turn stores only an optional
`default_predicate_group_revision_id`. It stores no predicate parameter values,
witness sources, active-member list, or workflow overrides.

Wave materialization:

1. resolves the turn's exact published group, or the canonical empty group;
2. resolves every referenced Execution Binding and Predicate Definition;
3. validates the complete authored graph against the Battle authoring catalog
   and hook contract;
4. builds and hashes one immutable Execution Package; and
5. derives required query actions and derived-state blocks from that package.

All jobs in one wave/workset share the package, prepared phase, hook contract,
and observation bindings. Candidate savestates, concrete commands, SeedProbe
inputs, and fake-attack choices remain per-job input.

Analysis persistence records exact group, binding, definition, package, phase,
and hook-contract lineage needed for restart reconstruction. Historical Battle
counts and evidence remain data, but pre-cut authored lineage is intentionally
unset because it does not identify a current Group or Execution Package.

## First Battle authored example

The analogous first-Battle workflow authors two pure definitions:

1. **Players before enemies**
   - the backend captures `turn_order` at `TurnIsReady`;
   - reducers calculate `player_max_position` and `enemy_min_position`;
   - expression: `player_max_position < enemy_min_position`.

2. **Cumulative item drops**
   - the backend captures rewards on the reached terminal path and turn-entry
     state at `TurnInputs`;
   - reducers calculate `drop_count(snapshot, item_id)` and
     `current_turn(snapshot)`;
   - expression: `drop_count >= current_turn`.

The item-drop Execution Binding contains the concrete typed value
`item_id=273`. The First Battle Predicate Group has two members:

- the turn-order binding at `TurnIsReady`, `First`, `RecordAndContinue`; and
- the drop binding at `{EndTurn, EndBattleVictory}`, `First`, `AbortOnFail`.

Both participate in passed/total aggregation and emit durable evidence. A
normally observed turn evaluates the turn-order member and exactly one terminal
drop member.

## Hard-cut boundary

The current database contains clean Definition, Execution Binding, Group, and
authoring-request-ledger tables. The hard-cut migration clears every interim
Predicate Definition, Execution Binding, and Group row, drops the superseded
authored composition tables, and clears Battle-turn references that cannot
identify a current Group. Approved content is reauthored through the
backend-owned create APIs.

There is no predecessor runtime decoder, authoring adapter, parameter recovery,
alias, or import facility. The current types and database structure are the
only supported system.

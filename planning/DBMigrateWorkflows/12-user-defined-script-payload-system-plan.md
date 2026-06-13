# User-Defined Script and Payload System Plan

## Purpose

SAVOR currently runs a small set of compiled phase programs. Each program has a fixed `PhaseScript`, a program-specific binary payload format, and a C++ adapter path that materializes jobs from DB rows. This document captures a path toward arbitrary user-defined scripts that can be stored in the DB, recalled by workflow handlers, sent to workers with the required payload data, reconstructed worker-side, and executed through the existing VM model.

The intent is not to replace the current runner immediately. The safer path is to formalize the pieces that already exist, then move one simple built-in program through the new path as a compatibility slice.

## Current Architecture

The current runtime has these major boundaries:

- `ProgramKind` is the active program identity. The worker receives `MSG_SET_PROGRAM`, then reconstructs the compiled `PhaseScript` locally through `programs::build_main_program(program_kind)`.
- Job payloads are opaque program-specific byte arrays. The worker calls `programs::decode_payload_for(active_program_kind, payload, ctx)` to turn those bytes into `PSContext`.
- The VM itself is mostly generic. It runs a `PhaseScript` containing breakpoint lists and `PSOp` instructions against a `PSContext`.
- Results already use a generic `PSContextCodec` to encode numeric context values back to the parent process.
- DB workflow orchestration already has an adapter seam: a `ProgramKindDescriptor` owns job persistence, graph job persistence, runtime initialization, payload materialization, result mapping, result payload writing, and workflow transition behavior.

This means the core execution loop is already close to a bytecode VM. The less generic parts are script loading, payload materialization, and program identity.

## Key Existing Strengths

- The VM instruction model already supports labels, branches, key operands, breakpoint runs, memory reads, movie playback, savestate operations, predicate evaluation, derived buffers, and domain operations.
- `PSContext` already acts as the data boundary between payload decoding, VM execution, and result mapping.
- Context keys have stable numeric ids and names through `CtxRegistry`, with a registry hash that can become a compatibility check.
- Workflow steps already flow through descriptors and runtime adapters, so a user-defined script path can be introduced without replacing workflow orchestration.
- Worker affinity already considers savestate, program kind, and runtime bootstrap profile. This can become script revision and runtime profile affinity later.

## Current Constraints

- Workers can only reconstruct scripts from compiled `ProgramKind` values.
- Request payloads are program-specific binary formats, while result payloads use a generic context codec.
- `ProgramKind` currently mixes several concerns: script identity, payload decoder choice, result mapper choice, runtime affinity, and workflow step routing.
- Some VM operations are high-level domain capabilities, such as battle macro probing and battle turn input materialization. These are powerful, but should not be treated as unrestricted arbitrary script primitives.
- Authored workflow graph nodes point to typed domain specs. They do not yet point directly to script revisions, payload schemas, or declared script contracts.

## Target Model

The target system should separate four concepts:

1. Script revision
   Immutable DB-authored executable definition. It contains the script body, compatibility metadata, required capabilities, declared inputs, declared outputs, breakpoint requirements, and runtime requirements.

2. Payload schema
   The contract for the data needed by a script revision. It defines required context keys, optional context keys, value types, source bindings, default values, and validation rules.

3. Handler or planner
   The DB-side component that recognizes what information a workflow step needs, resolves references, materializes the payload context, and chooses the script revision to execute.

4. Worker execution package
   The transport envelope sent to a worker. It contains either a built-in program reference or a serialized script revision plus a generic encoded context payload.

## Proposed DB Concepts

Add immutable authoring records for scripts:

- `au_script`
  Logical script family: name, description, owner/source, active revision id.

- `au_script_revision`
  Immutable revision: script id, revision number, status, source format, source text or blob, compiled bytecode blob, script hash, context registry hash, opcode schema version, breakpoint registry hash, required capability mask, created timestamp.

- `au_script_input`
  Declared input contract: script revision id, key name, value type, required flag, default value, data kind, description.

- `au_script_output`
  Declared output contract: script revision id, key name, value type, data kind, artifact role, description.

- `au_script_capability`
  Fine-grained capability declarations: memory read, movie playback, savestate write, reboot core, derived battle buffer, predicate evaluation, battle macro probe, battle turn input materialization, visual debug support.

- `au_payload_schema`
  Optional named schema if multiple scripts share the same payload shape.

Execution can retain `exec_job.program_kind` initially, but should eventually add or derive:

- `script_ref_kind`
- `script_revision_id`
- `payload_schema_id`
- `script_hash`
- `runtime_profile_key`

In the interim, a `PK_UserScript` or equivalent built-in program kind can bridge the old and new worlds.

## Script Representation

Use a staged representation rather than executing source text directly.

Authoring source should be human-readable and stable. Good candidates:

- JSON or INI-like structured script format for the first version.
- Named ops with named context keys and named breakpoints.
- Labels as strings.
- Explicit declared capabilities.

Execution representation should be canonical and compact:

- Serialized `PhaseScript` bytecode.
- Numeric key ids resolved through `CtxRegistry`.
- Numeric breakpoint keys resolved through the breakpoint registry.
- Opcode schema version.
- Context registry hash and breakpoint registry hash.
- Script hash over the canonical representation.

The worker should execute only the canonical representation after validation.

## Payload Representation

Move request payloads toward generic `PSContext` encoding.

Current:

- Program-specific payload bytes go to the worker.
- Worker decodes program payload into `PSContext`.
- Result `PSContext` comes back through `PSContextCodec`.

Target:

- DB-side handler materializes a `PSContext` from domain rows, artifacts, workflow bindings, and defaults.
- Parent process sends a generic payload envelope containing payload schema id, script revision id, and encoded `PSContext`.
- Worker validates that required script inputs exist and value types match.
- Worker runs the script and returns encoded `PSContext`.

Program-specific encoders can remain temporarily, but their destination should become generic context materialization.

## Worker Protocol Evolution

Introduce a script-aware activation path while keeping the existing path:

- `MSG_SET_PROGRAM`
  Existing compiled built-in path. Keep for compatibility.

- `MSG_SET_SCRIPT` or extended `MSG_SET_PROGRAM`
  New path. Sends script revision metadata and canonical bytecode, or a script hash that the worker already has cached.

- `MSG_JOB`
  For built-ins, current payload bytes remain supported. For script revisions, payload bytes contain a generic context envelope.

Suggested execution package fields:

- package version
- execution mode: built-in program or DB script revision
- program kind or script revision id
- script hash
- payload schema id
- context registry hash
- opcode schema version
- encoded context payload

The worker should cache validated script revisions by script hash to avoid resending bytecode for every job.

## VM Changes Needed

The VM does not need a full rewrite, but it needs loader and verifier layers:

- `PhaseScriptSerializer`
  Encode and decode `PhaseScript` to a canonical binary or JSON form.

- `PhaseScriptLoader`
  Resolve named ops, named keys, named breakpoints, labels, and typed operands into `PSOp`.

- `PhaseScriptVerifier`
  Validate label targets, max op count, max branch depth or step budget, required inputs, allowed outputs, capability requirements, supported opcode schema, context registry hash, and breakpoint registry hash.

- `ScriptRuntimeProfile`
  Carries default timeout, derived buffer kind, bootstrap profile, savestate requirements, and visual-debug compatibility.

- `ScriptCapabilityPolicy`
  Allows or rejects ops before activation. Domain-heavy operations should require explicit capabilities.

The VM should remain the low-level executor. It should not be responsible for DB lookups or payload-source resolution.

## Capability Boundaries

Not all VM ops should be equally available to arbitrary scripts.

General-purpose primitive ops:

- labels and branches
- setting and adding context values
- timeout selection
- run until breakpoint
- record current breakpoint
- emit or return results
- simple memory reads through approved address keys

Capability-gated ops:

- movie playback and movie stop
- savestate load/save/reboot
- raw address reads
- predicate table evaluation
- derived buffer reads
- battle context extraction
- battle turn input materialization
- battle macro probe execution
- input tape playback

Domain operations should remain as reusable capabilities, not as hidden logic inside payload decoders.

## Workflow and Handler Model

Keep the descriptor model, but add a generic script descriptor path.

Current descriptor:

- finds jobs by step kind
- builds runtime init
- materializes program-specific `PSJob`
- maps result
- evaluates workflow transition

Future generic script descriptor:

- resolves the active script revision for a workflow step or authored node
- validates graph bindings against the script input contract
- materializes a generic context payload
- builds runtime init from the script runtime profile
- maps declared outputs to workflow step outputs or domain result rows
- invokes a transition handler by script output contract, result kind, or domain-specific plugin

This lets workflow nodes evolve from "unit kind maps to C++ program kind" toward "unit kind maps to script revision plus payload schema," while preserving current orchestration behavior.

## Migration Phases

### Phase 1: Serialize Current Built-Ins

- Add canonical serialization for `PhaseScript`.
- Round-trip all built-in scripts through serializer/deserializer in tests.
- Keep current compiled `build_main_program()` path as the source of truth.
- Add script metadata dump tooling for inspection.

Success criteria:

- Serialized/deserialized built-ins produce equivalent op lists and breakpoint lists.
- Existing tests and worker runs still use the current built-in path.

### Phase 2: Generic Context Payload Envelope

- Define a request-side generic `PSContext` envelope.
- Add worker support for generic context payloads while keeping current decoders.
- Add validation against declared required keys and value types.
- Convert one low-risk built-in adapter to materialize a generic context payload internally.

Good first candidate:

- SeedProbe, because its script and payload contract are small.

Success criteria:

- One built-in can execute from generic context payloads.
- Old program-specific payload path remains available.

### Phase 3: Script Activation Path

- Add `PK_UserScript` or `MSG_SET_SCRIPT`.
- Send serialized `PhaseScript` plus metadata to the worker.
- Worker validates and caches by script hash.
- Coordinator tracks loaded script hash/revision in worker affinity.

Success criteria:

- A built-in script serialized by the parent process can be activated and run worker-side without calling `build_main_program()` in the worker.

### Phase 4: DB Script Revisions

- Add authoring tables for script families and immutable revisions.
- Store the serialized form and source form.
- Add loader validation against context and breakpoint registries.
- Add UI/service hooks to create and activate revisions.

Success criteria:

- A DB-stored revision can be recalled, validated, activated on a worker, and executed.

### Phase 5: Workflow Graph Integration

- Allow workflow graph nodes to reference script revisions and payload schemas.
- Bind graph inputs to script input keys.
- Map script output declarations to workflow step outputs.
- Keep domain-specific result mappers where domain rows need to be updated.

Success criteria:

- A workflow step can schedule a DB-authored script without a new hard-coded `ProgramKindDescriptor`.

### Phase 6: Domain Capability Hardening

- Move domain-heavy VM operations behind explicit capability declarations.
- Add verifier checks and tests for rejected scripts.
- Add durable failure events for capability mismatch, registry mismatch, missing inputs, and unsupported opcodes.

Success criteria:

- Arbitrary scripts can fail safely before worker activation if they request unsupported capabilities.

## Testing Strategy

- Unit tests for `PhaseScript` serialization round trips.
- Unit tests for script verifier failures: missing labels, unsupported opcodes, missing input keys, bad key names, unsupported capabilities, registry hash mismatches.
- Codec tests for generic request payload envelopes and result context envelopes.
- Worker protocol tests for built-in program activation and script activation.
- One e2e compatibility test where a current built-in runs through the script revision path and produces equivalent output.
- DB migration/schema export validation for new authoring and execution tables.

## Open Questions

- Should authored script source be JSON, INI, or a small DSL?
- Should the DB store only source plus compiled bytecode, or also a normalized op table for queryability?
- Should arbitrary scripts be allowed to use raw address reads, or only address registry keys and address programs?
- Should `ProgramKind` remain the top-level worker affinity, or should script revision hash replace it for user-defined scripts?
- How should script revisions declare result mapping: generic workflow outputs only, or domain-specific mappers as plugins?
- How much of battle macro probing should become script-level ops versus a single capability-owned handler op?

## Recommended First Implementation Slice

Do not start with fully arbitrary authoring. Start by making the current system prove it can run a known script from a serialized representation.

1. Build `PhaseScriptSerializer`.
2. Round-trip all compiled built-in scripts in tests.
3. Add a generic request context envelope.
4. Convert SeedProbe to internally materialize generic context.
5. Add a script activation path that sends serialized SeedProbe bytecode to a worker.
6. Compare outputs against the old `PK_SeedProbe` path.

That slice exercises the VM, worker protocol, payload system, and descriptor seam without forcing UI authoring, DSL design, or domain plugin generalization too early.

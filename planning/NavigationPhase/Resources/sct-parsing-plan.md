# SCT Parsing Plan

## Purpose

This document plans the first implementation pass for parsing **Skies of Arcadia Legends SCT files** for SOASim's Navigation Phase.

The immediate goal is to parse SCT files into a form where we can:

- inspect sections,
- identify likely cutscene sections,
- identify likely trigger-related sections,
- preserve flag and control-flow information needed for downstream world and trigger modeling,
- and support later runtime validation against Dolphin-based execution. [#1] [#2]

This plan is specifically aligned to the **DBMigrate** branch architecture and the current Navigation Phase goals already documented in that branch. It also assumes the existing Python parser in `SALSA` is the best current behavioral reference, but that the C++ implementation for SOASim should intentionally differ in one major way:

- the SALSA parser aims for broad reconstruction and attempts to account for essentially all bytes,
- the SOASim parser should instead be **control-flow guided**, using `jmp` and `switch` instructions to follow reachable script paths and avoid parsing garbage or intentionally skipped regions.

## Scope

The initial implementation should support the following:

- read SCT bytes from the ISO through the existing Dolphin-oriented extraction direction, not from an external extractor pipeline. [#2]
- parse the SCT container and section structure,
- decode instructions well enough to build section-local and cross-section control flow,
- follow `jmp` and `switch` instructions to identify reachable instruction streams,
- preserve section metadata, labels, offsets, and relevant raw payloads,
- extract flag-related operations needed for trigger and cutscene heuristics,
- classify sections heuristically as likely cutscene, likely trigger, likely utility/system, or unknown,
- expose results in a C++ data model suitable for inspection, indexing, and later database-backed artifacts.

The initial implementation does **not** need to support:

- full script decompilation into high-level source,
- perfect semantic naming for every opcode,
- every edge case in unreachable garbage regions,
- full simulation of all script behavior,
- automatic proof that a section is a cutscene,
- full DB schema integration in the first parser pass.

## Confirmed constraints

### Confirmed from DBMigrate NavigationPhase docs

The existing DBMigrate planning docs already establish several requirements that directly shape SCT parsing:

1. `.SCT` scripts are part of the world and trigger modeling pipeline. [#1] [#2]
2. The planner needs trigger and cutscene modeling, including activation predicates, required flags, and resulting state transitions. [#1]
3. Cutscene detection should combine MLD trigger and flag combinations with script decoding and runtime heuristics. [#2]
4. Script section starts, jump targets, and player-position-set behavior are useful for transition cataloging. [#2]
5. The current file acquisition direction is to read required files from the ISO through Dolphin APIs. [#2]

### Confirmed from the current project direction

The user also established the following implementation constraints for this work:

1. We are working on the **DBMigrate** branch of `SOASim`.
2. The existing Python parser in `SALSA` is a useful reference but should **not** be ported mechanically.
3. The new C++ parser should be designed for simulator use, not archival reconstruction.
4. Reachability should be guided by `jmp` and `switch` instructions so we avoid treating all skipped bytes as meaningful script.
5. Flag information is important enough that the parser likely needs broad enough coverage to preserve trigger-related semantics.

## Design goals

The SCT parser should be designed around four goals.

### 1. Reachability-first parsing

The parser should treat SCT as executable control flow, not just as a byte container.

That means:

- discover section entrypoints,
- decode instructions linearly within a basic block,
- stop blocks on terminators or dynamic transfers,
- enqueue successor targets from `jmp` and `switch`,
- track visited instruction offsets,
- and distinguish reachable bytes from uninterpreted bytes.

This is the biggest intentional divergence from the SALSA behavior.

### 2. Preservation of evidence

Even when a region is not parsed as reachable code, we still need enough evidence to inspect the file later.

The parser should therefore preserve:

- section boundaries,
- raw bytes for unparsed regions,
- discovered labels / jump targets,
- opcode decode failures,
- unknown instructions,
- and references between sections.

### 3. Heuristic-friendly output

The immediate consumer is not a compiler backend. It is the Navigation Phase and related inspection tools.

The parser should therefore emit data that makes it easy to ask questions like:

- which sections read or test flags,
- which sections set flags,
- which sections branch on conditions,
- which sections call into likely event or cutscene-like behavior,
- which sections reposition the player,
- which sections are entered from trigger-controller contexts,
- which sections have large linear action sequences versus tiny predicate checks.

### 4. Incremental semantic growth

We should not block implementation on a perfect opcode catalog.

The parser should support:

- partially known opcode metadata,
- unknown opcode placeholders,
- late classification improvements,
- and sideband heuristics that can improve without redesigning the parser core.

## High-level architecture

The correct split is:

```text
ISO / Dolphin file access
  -> raw SCT bytes
  -> SCT container parser
  -> instruction decoder
  -> control-flow graph builder
  -> semantic annotation / heuristics
  -> engine-neutral script IR
  -> inspection tools / future DB artifacts / runtime validation
```

This separation matters because the parser should not be tied directly to:

- UI widgets,
- one-off debug printing,
- runtime breakpoint code,
- or any specific persistence format.

## Proposed module split

### `SctCore`

Pure C++ parsing and analysis logic.

Suggested responsibilities:

- byte reading,
- SCT container and section parsing,
- instruction decode,
- opcode metadata lookup,
- jump / switch target extraction,
- control-flow graph construction,
- flag-op annotation,
- section classification heuristics,
- diagnostics.

### `ScriptIR`

Engine-neutral in-memory representation.

Suggested responsibilities:

- sections,
- instructions,
- basic blocks,
- edges,
- section summaries,
- flag access summaries,
- cutscene / trigger heuristic evidence,
- unknown-region capture.

### `NavigationPhase integration`

Consumers of parsed SCT data.

Suggested responsibilities:

- connect script sections to MLD trigger/controller data,
- build `nav_script_index`-style artifacts, [#1]
- support inspection in tools,
- later integrate runtime breakpoint validation,
- later feed transition catalog building.

## Proposed data model

A reasonable first-pass engine-neutral representation is:

```cpp
struct SctSectionId {
    uint32_t index;
    std::string name;
};

struct SctInstruction {
    uint32_t offset;
    uint16_t opcode;
    std::vector<uint32_t> operands;
    uint32_t size_bytes;
    bool decode_ok;
};

struct SctBasicBlock {
    uint32_t start_offset;
    uint32_t end_offset;
    std::vector<uint32_t> instruction_offsets;
    std::vector<uint32_t> successor_offsets;
};

struct SctUnknownRegion {
    uint32_t start_offset;
    uint32_t end_offset;
    std::vector<uint8_t> raw_bytes;
    std::string reason;
};

struct FlagAccessSummary {
    std::vector<uint32_t> flags_read;
    std::vector<uint32_t> flags_written;
    std::vector<uint32_t> flags_tested;
};

struct SectionHeuristicEvidence {
    bool touches_flags = false;
    bool branches_on_flags = false;
    bool writes_flags = false;
    bool has_switch = false;
    bool has_long_linear_sequence = false;
    bool has_player_reposition = false;
    bool has_camera_or_timing_like_ops = false;
    bool likely_trigger = false;
    bool likely_cutscene = false;
    std::vector<std::string> notes;
};

struct SctSection {
    SctSectionId id;
    uint32_t start_offset;
    uint32_t end_offset;
    std::vector<SctInstruction> instructions;
    std::vector<SctBasicBlock> blocks;
    std::vector<SctUnknownRegion> unknown_regions;
    FlagAccessSummary flag_summary;
    SectionHeuristicEvidence heuristic_evidence;
};

struct SctFile {
    std::string source_path;
    std::vector<SctSection> sections;
};
```

This should stay independent of database rows and viewer state. DB integration can map this into artifacts later.

## Parsing strategy

## 1. File acquisition

The parser should accept raw bytes, but the project-level acquisition path should follow the current DBMigrate NavigationPhase direction:

1. open the ISO through Dolphin-related APIs,
2. walk the filesystem,
3. locate target `.SCT` files,
4. read bytes,
5. pass those bytes to the parser. [#2]

This keeps the parser testable while preserving the project's agreed runtime source of truth.

## 2. Container and section parsing

The first stage should identify:

- file header,
- section table,
- section names or identifiers if present,
- offsets and lengths,
- any external tables needed for switch targets or labels.

This stage should remain conservative. It should parse only clearly defined container structure and preserve raw slices for later interpretation.

## 3. Instruction decode

The decoder should use an opcode metadata table derived from the current understanding in SALSA, but represented in a C++-friendly way.

Each opcode definition should describe at minimum:

- opcode value,
- operand format,
- instruction size,
- whether it terminates a block,
- whether it performs an unconditional jump,
- whether it performs a conditional jump,
- whether it is a switch-like dispatch,
- whether it reads or writes flags,
- whether it is associated with movement, camera, timing, dialogue, or reposition semantics when known.

A reasonable representation would be:

```cpp
struct OpcodeInfo {
    uint16_t opcode;
    std::string mnemonic;
    uint8_t operand_count;
    bool is_terminator;
    bool is_unconditional_jump;
    bool is_conditional_branch;
    bool is_switch;
    bool may_read_flags;
    bool may_write_flags;
    bool may_reposition_player;
};
```

Unknown opcodes should still decode into placeholder instructions when possible, with raw operand bytes preserved.

## 4. Reachability-guided control-flow analysis

This is the core parser policy.

For each section:

1. Seed the worklist with the section entry offset.
2. Decode sequentially until:
   - end of section,
   - decode failure,
   - explicit terminator,
   - unconditional jump,
   - switch dispatch,
   - or entry into already visited instruction space.
3. When a conditional branch is encountered, enqueue both fallthrough and target.
4. When an unconditional `jmp` is encountered, enqueue only the target.
5. When a `switch` is encountered, enqueue every resolved target plus any documented default path.
6. Mark gaps between reachable blocks as unknown or unreachable regions rather than forcing linear decode through them.

This gives the project the behavior you asked for: use script control flow to avoid treating skipped bytes as meaningful code.

## 5. Unknown and garbage handling

The parser should deliberately distinguish several cases.

### Reachable but undecodable

This is likely one of:

- missing opcode metadata,
- malformed operand assumptions,
- or an incorrect target calculation.

These regions should be considered high-priority diagnostics.

### Unreachable between valid targets

This may be:

- literal data,
- dead code,
- padding,
- or intentionally skipped garbage.

These should be preserved as `unknown_regions`, not treated as parser failure.

### Section tail bytes after a terminator chain

These should also be preserved without assuming they are executable.

## 6. Switch handling

`switch` support is important enough to implement early.

The parser should:

- decode the instruction itself,
- locate the switch table or target list,
- validate that targets remain within legal section bounds,
- create CFG edges to each target,
- and annotate the section summary that it contains multi-way dispatch.

Because switch-like structures often create nontrivial control flow and can skip over data, they are essential to avoiding false linear parsing.

## Semantic annotation strategy

The parser should build semantic annotations in parallel with decode, but keep them lightweight.

### Flag access annotation

For each instruction with known flag semantics, record:

- flag ID or operand,
- read/write/test operation,
- owning section,
- instruction offset.

This is needed because trigger identification will likely depend on flag tests and writes. [#1] [#2]

### Control-role annotation

For each section, summarize whether it appears to be:

- predicate-heavy,
- transition-heavy,
- action-heavy,
- dispatch-heavy,
- or mostly utility.

### Runtime-interest annotation

Mark instructions or sections that are likely interesting for future Dolphin breakpoint validation, such as:

- flag writes,
- section entrypoints from trigger contexts,
- player reposition operations,
- transition-like calls.

## Heuristics for likely trigger sections

A section should be considered a stronger trigger candidate when it has several of the following:

- reads or tests flags early,
- contains short conditional gate logic,
- is entered from MLD trigger/controller binding,
- branches quickly to success/failure outcomes,
- writes a small number of state flags,
- transitions into another section or event sequence.

A section should be considered a weaker trigger candidate when it is mostly long linear action flow with little branching.

## Heuristics for likely cutscene sections

A section should be considered a stronger cutscene candidate when it has several of the following:

- long linear reachable instruction runs,
- timing or wait-like op patterns,
- camera-like or actor-control-like operations,
- dialogue/message-like operations,
- player reposition or scene-transition-like operations,
- a lower density of predicate gating compared with trigger sections,
- entry from a known trigger section,
- terminal state changes that resemble event completion.

The output should never claim certainty unless later runtime validation confirms it. For now these are evidence-based labels.

## Relationship to MLD parsing

The SCT parser should be designed with the expectation that its best classifications will improve when combined with MLD data.

That combined analysis path should eventually support:

- mapping MLD triggers to SCT section entrypoints,
- using MLD object flags to explain section gating,
- identifying cutscene starts from MLD trigger + SCT evidence,
- building transition catalog entries with script and world context together. [#1] [#2]

So while SCT parsing can be implemented independently, the data model should leave room for later linkage fields such as:

```cpp
struct ExternalSectionBinding {
    std::string mld_object_name;
    uint32_t trigger_id;
    uint32_t controller_id;
    uint32_t section_index;
};
```

## DBMigrate integration strategy

Because the project is on the **DBMigrate** branch, the parser output should be designed so it can later become a database-backed artifact without restructuring the parser.

The first pass does not need immediate DB writes, but it should produce stable structured output that could later populate something like:

- a script artifact blob,
- a section summary table,
- a flag-access index,
- or a `nav_script_index` artifact. [#1]

That means the parser should avoid embedding transient UI concerns and should prefer deterministic structured summaries.

## Validation strategy

The first implementation should be validated in layers.

### 1. Structural validation against SALSA

For a small corpus of SCT files:

- confirm section counts,
- confirm major section offsets,
- confirm opcode boundaries where known,
- confirm jump and switch targets where known,
- compare reachable decode against the Python reference where possible.

The expected result is not byte-for-byte parity, since our parsing philosophy is different.

### 2. Reachability validation

For representative sections:

- verify that control-flow-guided parsing avoids garbage regions,
- verify that known jump targets are discovered,
- verify that switch targets produce sensible CFG branches,
- verify that unreachable skipped bytes remain preserved but not misclassified as instructions.

### 3. Heuristic validation

For a growing labeled set of sections:

- compare likely trigger labels against known trigger-bound sections,
- compare likely cutscene labels against known cutscene sections,
- adjust evidence weights as needed.

### 4. Runtime validation later

Once Dolphin-side instrumentation is ready, validate selected sections by:

- breaking on section entry,
- observing flag reads/writes,
- observing player reposition behavior,
- correlating runtime behavior with static heuristic labels. [#2]

## Implementation phases

## Phase 1: Format and decode foundation

Goal: parse SCT structure and decode enough instructions to inspect sections.

Deliverables:

- raw SCT reader,
- section table parsing,
- opcode metadata table scaffold,
- instruction decode for core opcode set,
- structured diagnostics for unknown instructions.

## Phase 2: CFG-driven parsing

Goal: move from linear decode to reachability-guided parsing.

Deliverables:

- basic block construction,
- `jmp` target resolution,
- `switch` target resolution,
- visited-offset tracking,
- unknown / unreachable region capture,
- per-section CFG summaries.

## Phase 3: Semantic summaries

Goal: make parsed scripts useful to Navigation Phase.

Deliverables:

- flag access summaries,
- likely trigger heuristics,
- likely cutscene heuristics,
- section evidence summaries,
- export-ready script IR.

## Phase 4: Cross-linking and runtime follow-up

Goal: connect SCT parsing to the rest of Navigation Phase.

Deliverables:

- linkage points for MLD trigger/controller mapping,
- support for script indexing artifacts,
- runtime-interest markers for future Dolphin breakpoint validation,
- data export for inspection tooling.

## Testing corpus recommendations

The first regression corpus should include:

- a file with simple straight-line sections,
- a file with obvious conditional jumps,
- a file with switch-based control flow,
- a file known to participate in a cutscene,
- a file known to participate in trigger activation,
- a file with skipped or garbage regions that SALSA would attempt to reconstruct more aggressively.

## Open research items

The following should remain explicit open items during implementation:

1. The exact SCT container layout details that matter for C++ implementation.
2. The complete opcode catalog and operand semantics.
3. Which opcodes correspond to flag reads, writes, and tests.
4. Which opcodes correspond to player reposition, dialogue, waits, camera control, and scene transitions.
5. The exact shape of switch tables and any unusual branch encodings.
6. The best evidence weights for cutscene versus trigger heuristics.
7. Which sections are true entrypoints versus callable utility sections.

## Recommended implementation rule

Do not force parseability where the control flow does not justify it.

The parser should prefer:

- decode reachable instructions,
- preserve skipped bytes as unknown regions,
- record diagnostics instead of overcommitting,
- attach heuristic evidence rather than certainty,
- and leave room for later runtime confirmation.

That policy is the right fit for a simulator-oriented parser on DBMigrate.

## References

- [#1] SOASim NavigationPhase state and world model planning: `planning/NavigationPhase/02-state-and-world-model.md`  
  https://github.com/jahorta/SOASim/blob/DBMigrate/planning/NavigationPhase/02-state-and-world-model.md

- [#2] SOASim NavigationPhase open implementation questions: `planning/NavigationPhase/05-open-implementation-questions.md`  
  https://github.com/jahorta/SOASim/blob/DBMigrate/planning/NavigationPhase/05-open-implementation-questions.md

- [#3] SALSA repository, existing Python SCT parsing reference  
  https://github.com/jahorta/SALSA

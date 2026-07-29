#pragma once

#include "ProgramTypes.h"

#include <compare>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace savor::runtime::program {

struct ValueDefinition
{
    ProgramValueId id;
    TypeRef type;

    auto operator<=>(const ValueDefinition&) const = default;
};

enum class InstructionOpcode : std::uint16_t
{
    Constant,
    Copy,
    Select,
    CheckedConvert,
    RecordConstruct,
    RecordProject,
    RecordUpdate,
    OptionalConstruct,
    OptionalIsPresent,
    OptionalExtract,
    ListConstruct,
    ListAppend,
    ListIndex,
    ListSize,
    AddChecked,
    SubtractChecked,
    MultiplyChecked,
    DivideChecked,
    RemainderChecked,
    NegateChecked,
    Equal,
    NotEqual,
    Less,
    LessEqual,
    Greater,
    GreaterEqual,
    BooleanAnd,
    BooleanOr,
    BooleanNot,
    CallLocal,
    CallImported,
    CallReducer,
    AwaitAction,
    EmitRecord,
    PublishArtifact,
    EnterScope,
    ExitScope,
    DeferCompensation,
    PromoteResource,
};

enum class InstructionTargetKind : std::uint8_t
{
    None,
    LocalFunction,
    ImportedFunction,
    Action,
    Reducer,
    DeferredAction,
};

struct InstructionTarget
{
    InstructionTargetKind kind = InstructionTargetKind::None;
    ProgramFunctionId local_function;
    std::optional<ExactDependencyIdentity> dependency;
    std::string member_name;

    auto operator<=>(const InstructionTarget&) const = default;
};

struct Instruction
{
    ProgramInstructionId id;
    InstructionOpcode opcode = InstructionOpcode::Constant;
    ProgramSourceLocationId source_location;
    std::optional<ValueDefinition> result;
    std::vector<ProgramValueId> operands;
    std::optional<LiteralValue> literal;
    InstructionTarget target;
    std::string selector;
    std::uint64_t ordinal = 0;
    ProgramScopeId scope;

    auto operator<=>(const Instruction&) const = default;
};

struct BlockEdge
{
    ProgramBlockId target;
    std::vector<ProgramValueId> arguments;

    auto operator<=>(const BlockEdge&) const = default;
};

struct EnumSwitchCase
{
    std::int64_t enum_value = 0;
    BlockEdge edge;

    auto operator<=>(const EnumSwitchCase&) const = default;
};

enum class TerminatorKind : std::uint8_t
{
    Branch,
    ConditionalBranch,
    EnumSwitch,
    Return,
    StructuredFail,
};

struct StructuredFailure
{
    std::string code;
    std::string message;
    std::optional<ProgramValueId> details;

    auto operator<=>(const StructuredFailure&) const = default;
};

struct Terminator
{
    TerminatorKind kind = TerminatorKind::StructuredFail;
    ProgramSourceLocationId source_location;
    std::optional<ProgramValueId> condition_or_selector;
    std::vector<BlockEdge> edges;
    std::vector<EnumSwitchCase> enum_cases;
    std::optional<BlockEdge> default_edge;
    std::optional<ProgramValueId> return_value;
    std::optional<ProgramValueId> domain_outcome;
    std::optional<StructuredFailure> failure;

    auto operator<=>(const Terminator&) const = default;
};

struct BasicBlock
{
    ProgramBlockId id;
    std::vector<ValueDefinition> arguments;
    std::vector<Instruction> instructions;
    Terminator terminator;

    auto operator<=>(const BasicBlock&) const = default;
};

struct ProgramFunction
{
    ProgramFunctionId id;
    std::string name;
    std::vector<ValueDefinition> arguments;
    TypeRef output_type;
    std::optional<TypeRef> domain_outcome_type;
    ProgramBlockId entry_block;
    std::vector<BasicBlock> blocks;
    bool exported = false;

    auto operator<=>(const ProgramFunction&) const = default;
};

struct ProgramBudgets
{
    std::uint64_t maximum_instructions = 0;
    std::uint64_t maximum_calls = 0;
    std::uint64_t maximum_call_depth = 0;
    std::uint64_t maximum_action_requests = 0;
    std::uint64_t maximum_emissions = 0;
    std::uint64_t maximum_artifacts = 0;
    std::uint64_t maximum_values = 0;
    std::uint64_t maximum_value_bytes = 0;
    std::uint64_t maximum_trace_events = 0;

    auto operator<=>(const ProgramBudgets&) const = default;
};

enum class InvocationStatePolicy : std::uint8_t
{
    Boot,
    LoadArtifact,
    RestoreBaseline,
    ContinueSession,
};

enum class ExecutionIntent : std::uint8_t
{
    Live,
    Replay,
    VisualDebug,
};

struct ProgramPolicySet
{
    std::vector<InvocationStatePolicy> state_policies;
    std::vector<ExecutionIntent> execution_intents;
    bool permits_movie_playback = false;
    bool permits_movie_recording = false;
    bool permits_capture = false;
    bool permits_replay = false;
    bool permits_visual_debug = false;
    bool permits_state_replacement = false;
    bool permits_resource_promotion = false;

    auto operator<=>(const ProgramPolicySet&) const = default;
};

struct ProgramEntrypoint
{
    std::string name;
    ProgramFunctionId function;
    TypeRef input_type;
    TypeRef output_type;
    TypeRef domain_outcome_type;
    std::vector<SchemaIdentity> emission_schemas;
    std::vector<SchemaIdentity> artifact_schemas;
    std::vector<CapabilityPackIdentity> required_capability_packs;
    ProgramPolicySet accepted_policies;
    std::optional<ProgramBudgets> narrowed_budgets;

    auto operator<=>(const ProgramEntrypoint&) const = default;
};

struct SourceMapEntry
{
    ProgramSourceLocationId id;
    std::optional<ProgramFunctionId> function;
    std::optional<ProgramBlockId> block;
    std::optional<ProgramInstructionId> instruction;
    std::string source_name;
    std::string semantic_path;
    std::uint32_t line = 0;
    std::uint32_t column = 0;

    auto operator<=>(const SourceMapEntry&) const = default;
};

struct ProgramSourceMap
{
    std::uint32_t version = 1;
    std::vector<SourceMapEntry> entries;

    auto operator<=>(const ProgramSourceMap&) const = default;
};

} // namespace savor::runtime::program

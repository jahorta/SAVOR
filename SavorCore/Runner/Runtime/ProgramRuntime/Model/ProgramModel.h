#pragma once

#include "ProgramIr.h"

#include <compare>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace savor::runtime::program {

inline constexpr std::uint32_t kCanonicalIrVersionV1 = 1;

struct ProgramModule
{
    ModuleIdentity identity;
    std::uint32_t ir_version = kCanonicalIrVersionV1;
    std::vector<ProgramEntrypoint> entrypoints;
    std::vector<ProgramFunction> functions;
    std::vector<TypeSchemaDefinition> local_types;
    std::vector<ModuleImportIdentity> module_imports;
    std::vector<ExactDependencyIdentity> action_imports;
    std::vector<ExactDependencyIdentity> reducer_imports;
    std::vector<SchemaIdentity> type_imports;
    std::vector<CapabilityPackIdentity> required_capability_packs;
    ProgramPolicySet accepted_policies;
    ProgramBudgets budgets;
    ProgramSourceMap source_map;

    auto operator<=>(const ProgramModule&) const = default;
};

struct ProgramDependencyLock
{
    std::uint32_t ir_version = kCanonicalIrVersionV1;
    std::vector<ModuleImportIdentity> module_imports;
    std::vector<ExactDependencyIdentity> action_imports;
    std::vector<ExactDependencyIdentity> reducer_imports;
    std::vector<SchemaIdentity> type_imports;
    std::vector<CapabilityPackIdentity> capability_packs;

    auto operator<=>(const ProgramDependencyLock&) const = default;
};

struct RuntimeProfile
{
    std::string profile_id;
    std::string game_id;
    std::string disc_identity;
    std::string executable_identity;
    std::string backend;
    std::vector<CapabilityPackIdentity> capability_packs;

    auto operator<=>(const RuntimeProfile&) const = default;
};

struct InvocationStateRequest
{
    InvocationStatePolicy policy = InvocationStatePolicy::RestoreBaseline;
    std::string session_lineage;
    SessionId expected_session;
    WorksetEpoch expected_epoch;

    auto operator<=>(const InvocationStateRequest&) const = default;
};

struct InvocationExecutionPolicy
{
    ExecutionIntent intent = ExecutionIntent::Live;
    bool allow_movie_playback = false;
    bool allow_movie_recording = false;
    bool allow_input = false;
    bool allow_capture = false;
    bool record_trace = false;
    bool record_progress = true;

    auto operator<=>(const InvocationExecutionPolicy&) const = default;
};

struct ProvenanceEntry
{
    std::string key;
    std::string value;

    auto operator<=>(const ProvenanceEntry&) const = default;
};

struct ProgramProvenance
{
    std::string requesting_component;
    std::vector<ArtifactReferenceValue> source_artifacts;
    std::vector<ProvenanceEntry> attributes;

    auto operator<=>(const ProgramProvenance&) const = default;
};

struct ProgramInvocation
{
    InvocationId invocation_id;
    AttemptId attempt_id;
    ModuleIdentity module;
    std::string entrypoint;
    ProgramDependencyLock dependencies;
    RuntimeProfile runtime_profile;
    InvocationStateRequest state;
    InvocationExecutionPolicy execution;
    ProgramValueGraph input;
    ProgramBudgets limits;
    ProgramProvenance provenance;

    auto operator<=>(const ProgramInvocation&) const = default;
};

enum class ProgramInfrastructureStatus : std::uint8_t
{
    Completed,
    Rejected,
    Cancelled,
    TimedOut,
    BudgetExhausted,
    BackendFailed,
    ContractFailed,
};

enum class ProgramCleanupStatus : std::uint8_t
{
    Clean,
    CleanWithDiagnostics,
    Tainted,
};

enum class DiagnosticSeverity : std::uint8_t
{
    Information,
    Warning,
    Error,
};

struct ProgramDiagnostic
{
    DiagnosticSeverity severity = DiagnosticSeverity::Error;
    std::string code;
    std::string message;
    std::optional<ProgramSourceLocationId> source_location;
    std::vector<std::string> causal_chain;

    auto operator<=>(const ProgramDiagnostic&) const = default;
};

struct ProgramEmission
{
    ProgramEmissionSequence sequence;
    SchemaIdentity schema;
    ProgramValueGraph value;
    bool complete = true;

    auto operator<=>(const ProgramEmission&) const = default;
};

struct ProgramArtifact
{
    ProgramArtifactSequence sequence;
    ArtifactReferenceValue artifact;

    auto operator<=>(const ProgramArtifact&) const = default;
};

struct ProgramTraceEvent
{
    ProgramTraceSequence sequence;
    std::string kind;
    std::optional<ProgramSourceLocationId> source_location;
    WorksetEpoch epoch;
    std::vector<ProvenanceEntry> attributes;

    auto operator<=>(const ProgramTraceEvent&) const = default;
};

struct CleanupReceipt
{
    ProgramResourceHandleId resource;
    ProgramCleanupStatus status = ProgramCleanupStatus::Clean;
    std::string diagnostic;

    auto operator<=>(const CleanupReceipt&) const = default;
};

struct ProgramResult
{
    InvocationId invocation_id;
    AttemptId attempt_id;
    ModuleIdentity module;
    std::string entrypoint;
    ProgramDependencyLock resolved_dependencies;
    ProgramInfrastructureStatus infrastructure =
        ProgramInfrastructureStatus::ContractFailed;
    std::optional<ProgramValueGraph> domain_outcome;
    ProgramCleanupStatus cleanup = ProgramCleanupStatus::Clean;
    SessionDisposition session_disposition = SessionDisposition::Clean;
    std::optional<ProgramValueGraph> output;
    std::vector<ProgramEmission> emissions;
    std::vector<ProgramArtifact> artifacts;
    std::vector<ProgramDiagnostic> diagnostics;
    std::vector<ProgramTraceEvent> trace;
    std::vector<CleanupReceipt> cleanup_receipts;
    ProgramProvenance provenance;

    auto operator<=>(const ProgramResult&) const = default;
};

} // namespace savor::runtime::program

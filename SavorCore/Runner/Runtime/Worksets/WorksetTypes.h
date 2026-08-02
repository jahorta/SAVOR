#pragma once

#include "../IProgramRuntimePort.h"
#include "../FullPhase/FullPhaseProgram.h"
#include "../Services/State/StateTypes.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace savor::runtime {

inline constexpr std::size_t kMinimumWorksetTerminalReservationBytes = 1024;
// Leaves at least 1 MiB below WRMS v1's 64 MiB payload ceiling for the
// terminal envelope and correlation fields.
inline constexpr std::size_t kMaximumWorksetTerminalReservationBytes =
    63ull * 1024ull * 1024ull;

enum class RuntimeCatalogStatus : std::uint8_t
{
    Partial,
    CompleteExact,
};

struct RuntimeModuleManifestEntry
{
    ProgramModuleIdentity module;
    std::vector<std::string> entrypoints;
    std::string dependency_manifest_sha256;
    bool development_only = false;

    auto operator<=>(const RuntimeModuleManifestEntry&) const = default;
};

struct WorkerWorksetLimits
{
    std::uint32_t maximum_items_per_workset = 16;
    std::size_t maximum_encoded_workset_bytes = 32ull * 1024ull * 1024ull;
    std::uint32_t maximum_item_credits = 64;
    std::uint32_t maximum_active_and_staged_items = 32;
    std::uint32_t maximum_state_cache_entries = 16;
    std::size_t maximum_state_cache_bytes = 512ull * 1024ull * 1024ull;
    std::uint32_t finalizer_threads = 2;
    std::uint32_t maximum_pending_finalizers = 8;
    std::size_t maximum_pending_finalizer_bytes =
        256ull * 1024ull * 1024ull;
    std::uint32_t maximum_retained_terminals = 32;
    std::size_t maximum_retained_terminal_bytes =
        128ull * 1024ull * 1024ull;
    std::uint32_t progressive_start_concurrency = 2;

    auto operator<=>(const WorkerWorksetLimits&) const = default;
};

struct WorkerRuntimeManifest
{
    std::uint16_t wrms_protocol_version = 1;
    std::uint32_t program_module_format_version = 1;
    std::uint32_t program_invocation_format_version = 1;
    std::uint32_t program_result_format_version = 1;
    std::string runtime_profile_sha256;
    std::string dependency_manifest_sha256;
    RuntimeCatalogStatus catalog_status = RuntimeCatalogStatus::Partial;
    std::uint64_t catalog_generation = 1;
    std::string catalog_sha256;
    std::vector<RuntimeModuleManifestEntry> modules;
    WorkerWorksetLimits limits;

    auto operator<=>(const WorkerRuntimeManifest&) const = default;
};

enum class ProgramBaselineStateKind : std::uint8_t
{
    Boot,
    Artifact,
    CurrentSession,
};

enum class ProgramBaselineComponentPolicy : std::uint8_t
{
    ImmutableShared,
    ResetForEveryItem,
};

struct CurrentSessionBaselineGuard
{
    SessionId session_id;
    StateEpoch state_epoch;
    bool require_clean_idle = true;

    [[nodiscard]] explicit operator bool() const noexcept
    {
        return session_id && state_epoch && require_clean_idle;
    }

    auto operator<=>(const CurrentSessionBaselineGuard&) const = default;
};

struct ProgramBaselineArtifact
{
    std::filesystem::path state_path;
    std::string state_sha256;
    std::optional<std::filesystem::path> movie_path;
    std::string movie_sha256;
    ExternalMovieImportMode movie_mode = ExternalMovieImportMode::NoMovie;
    StateCompatibilityToken compatibility;
    StateLineage lineage;

    auto operator<=>(const ProgramBaselineArtifact&) const = default;
};

// One immutable, verifier-known part of the complete program starting state.
// Later phase slices may register providers for canonical component identities;
// WorkerRuntime never interprets component bytes as program IR.
struct ProgramBaselineComponent
{
    std::string canonical_id;
    std::uint32_t revision = 0;
    std::string schema_id;
    std::string content_sha256;
    ProgramBaselineComponentPolicy policy =
        ProgramBaselineComponentPolicy::ImmutableShared;
    std::vector<std::uint8_t> immutable_bytes;

    auto operator<=>(const ProgramBaselineComponent&) const = default;
};

struct ProgramBaselineDefinition
{
    ProgramBaselineStateKind state_kind = ProgramBaselineStateKind::Artifact;
    std::optional<ProgramBaselineArtifact> artifact;
    std::optional<CurrentSessionBaselineGuard> current_session;
    std::string lineage;
    std::vector<ProgramBaselineComponent> components;

    auto operator<=>(const ProgramBaselineDefinition&) const = default;
};

struct ProgramBaselineKey
{
    std::string sha256;

    [[nodiscard]] explicit operator bool() const noexcept
    {
        return sha256.size() == 64;
    }

    auto operator<=>(const ProgramBaselineKey&) const = default;
};

struct PreparedProgramBaselineReceipt
{
    ProgramBaselineKey key;
    SessionId session_id;
    StateEpoch state_epoch;
    std::string lineage;
    bool restored = false;

    [[nodiscard]] explicit operator bool() const noexcept
    {
        return static_cast<bool>(key) && session_id && state_epoch &&
            !lineage.empty();
    }

    auto operator<=>(const PreparedProgramBaselineReceipt&) const = default;
};

struct StateCacheKey
{
    ProgramBaselineKey baseline;
    std::string state_sha256;
    std::string lineage;
    StateCompatibilityToken compatibility;
    std::string movie_continuation_sha256;
    std::uint64_t session_generation = 0;

    auto operator<=>(const StateCacheKey&) const = default;
};

struct WorkerWorksetExecutionKey
{
    ProgramModuleIdentity module;
    std::string entrypoint;
    std::string verified_dependency_sha256;
    std::string runtime_profile_sha256;
    ProgramBaselineKey baseline;
    std::string movie_policy_sha256;
    std::string service_policy_sha256;
    std::string canonical_sha256;

    [[nodiscard]] explicit operator bool() const noexcept
    {
        return !module.canonical_id.empty() && !entrypoint.empty() &&
            static_cast<bool>(baseline) && canonical_sha256.size() == 64;
    }

    auto operator<=>(const WorkerWorksetExecutionKey&) const = default;
};

struct FullPhaseInvocationEnvelope
{
    ProgramInvocationId invocation_id;
    fullphase::FullPhaseProgramIdentity program;

    auto operator<=>(const FullPhaseInvocationEnvelope&) const = default;
};

struct ScalarProgramExecutionBinding
{
    ProgramExecutionId execution_id;
    AttemptId attempt_id;
    // Program-kind-specific scalar input. Full Phase invariants never appear
    // here; the worker resolves them from the compiled definition registry.
    std::vector<std::uint8_t> input_payload;

    auto operator<=>(const ScalarProgramExecutionBinding&) const = default;
};

struct WorksetItemCorrelation
{
    std::string durable_job_id;
    std::string claim_token;
    std::string parent_correlation;

    auto operator<=>(const WorksetItemCorrelation&) const = default;
};

struct WorksetItemTemplate
{
    WorkerWorksetItemId item_id;
    std::uint32_t ordinal = 0;
    ScalarProgramExecutionBinding execution;
    // Maximum encoded authoritative terminal bytes retained until the
    // coordinator acknowledges this exact item.
    std::size_t declared_terminal_bytes = 1024 * 1024;
    WorksetItemCorrelation correlation;

    auto operator<=>(const WorksetItemTemplate&) const = default;
};

struct WorkerWorksetDefinition
{
    WorkerWorksetId workset_id;
    FullPhaseInvocationEnvelope phase_invocation;
    WorkerWorksetExecutionKey execution_key;
    ProgramBaselineDefinition baseline;
    std::vector<WorksetItemTemplate> items;
    std::size_t encoded_size_bytes = 0;

    auto operator<=>(const WorkerWorksetDefinition&) const = default;
};

inline constexpr std::uint32_t
    kInitialWorksetCancellationSidecarVersionV1 = 1;

struct WorksetValidationResult;

struct InitialWorksetCancellationSidecarV1
{
    WorkerWorksetId workset_id;
    std::vector<WorkerWorksetItemId> item_ids;

    auto operator<=>(
        const InitialWorksetCancellationSidecarV1&) const = default;
};

enum class WorksetSubmissionDispositionV1 : std::uint8_t
{
    Accepted = 0,
    AlreadyAccepted,
};

struct SubmitWorksetResultV1
{
    WorkerWorksetId workset_id;
    std::uint32_t sidecar_version =
        kInitialWorksetCancellationSidecarVersionV1;
    std::uint32_t applied_item_count = 0;
    std::string applied_sidecar_sha256;
    WorksetSubmissionDispositionV1 disposition =
        WorksetSubmissionDispositionV1::Accepted;

    auto operator<=>(const SubmitWorksetResultV1&) const = default;
};

[[nodiscard]] std::string ComputeInitialWorksetCancellationSidecarSha256(
    const InitialWorksetCancellationSidecarV1& sidecar);

[[nodiscard]] WorksetValidationResult
ValidateInitialWorksetCancellationSidecar(
    const WorkerWorksetDefinition& definition,
    const InitialWorksetCancellationSidecarV1& sidecar);

enum class WorkerWorksetState : std::uint8_t
{
    Validating,
    Staged,
    PreparingBaseline,
    Running,
    Draining,
    Completed,
    Cancelled,
    Failed,
};

enum class WorkerWorksetItemState : std::uint8_t
{
    Pending,
    Active,
    Finalizing,
    TerminalReady,
    AwaitingAcknowledgement,
    Acknowledged,
    Unstarted,
};

struct WorkerItemTerminalCorrelation
{
    WorkerWorksetId workset_id;
    WorkerWorksetItemId item_id;
    std::uint32_t item_ordinal = 0;
    InvocationId invocation_id;
    AttemptId attempt_id;
    WorkerTerminalId terminal_id;
    WorkerTerminalOrder terminal_order;

    auto operator<=>(const WorkerItemTerminalCorrelation&) const = default;
};

// Exact child identity available as soon as an item is admitted. Host-output
// finalization must not depend on a terminal ID/order because those are
// reserved only after ordinary invocation execution completes.
struct WorkerItemExecutionCorrelation
{
    WorkerWorksetId workset_id;
    WorkerWorksetItemId item_id;
    std::uint32_t item_ordinal = 0;
    InvocationId invocation_id;
    AttemptId attempt_id;

    [[nodiscard]] explicit operator bool() const noexcept
    {
        return workset_id && item_id && invocation_id && attempt_id;
    }

    auto operator<=>(const WorkerItemExecutionCorrelation&) const = default;
};

[[nodiscard]] inline WorkerItemExecutionCorrelation
ExecutionCorrelation(
    const WorkerItemTerminalCorrelation& terminal) noexcept
{
    return {
        terminal.workset_id,
        terminal.item_id,
        terminal.item_ordinal,
        terminal.invocation_id,
        terminal.attempt_id};
}

struct WorksetValidationResult
{
    bool ok = false;
    RuntimeError error;

    [[nodiscard]] static WorksetValidationResult Success()
    {
        return {true, {}};
    }

    [[nodiscard]] static WorksetValidationResult Failure(
        WorkerRejectionCode code,
        std::string message)
    {
        return {false, {code, std::move(message)}};
    }
};

[[nodiscard]] ProgramBaselineKey ComputeProgramBaselineKey(
    const ProgramBaselineDefinition& definition);

[[nodiscard]] std::string ComputeWorkerWorksetExecutionKeyHash(
    const WorkerWorksetExecutionKey& key);

[[nodiscard]] std::string ComputeStateCacheKeyHash(
    const StateCacheKey& key);

[[nodiscard]] WorksetValidationResult ValidateWorkerWorksetDefinition(
    const WorkerWorksetDefinition& definition,
    const WorkerWorksetLimits& limits);

[[nodiscard]] std::string ComputeRuntimeCatalogHash(
    const std::vector<RuntimeModuleManifestEntry>& modules,
    RuntimeCatalogStatus status);

[[nodiscard]] WorksetValidationResult ValidateWorkerRuntimeManifest(
    const WorkerRuntimeManifest& manifest);

} // namespace savor::runtime

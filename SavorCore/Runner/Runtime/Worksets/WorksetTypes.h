#pragma once

#include "../IProgramRuntimePort.h"
#include "../DerivedState/DerivedStateTypes.h"
#include "../FullPhase/FullPhaseProgram.h"
#include "../Progress/ProgressTypes.h"
#include "../Services/Savestate/SavestateTypes.h"

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

struct WorkerWorksetLimits
{
    std::uint32_t maximum_items_per_workset = 16;
    std::size_t maximum_encoded_workset_bytes = 32ull * 1024ull * 1024ull;
    std::size_t maximum_capture_profile_bytes = 4ull * 1024ull * 1024ull;
    std::uint32_t maximum_item_credits = 64;
    std::uint32_t maximum_active_and_staged_items = 32;
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

// One immutable identity for the uniform worker runtime. This deliberately
// describes the static execution ABI as a single hash; it never advertises
// program kinds, modules, entrypoints, or optional capabilities.
struct WorkerRuntimeContractV1
{
    std::uint32_t contract_version = 1;
    std::uint16_t wrms_protocol_version = 2;
    std::uint32_t workset_wire_version = 4;
    std::uint32_t program_module_format_version = 1;
    std::uint32_t program_invocation_format_version = 1;
    std::uint32_t program_result_format_version = 1;
    std::string supported_game_id;
    std::string executable_identity;
    std::string address_map_revision;
    std::string emulator_bridge_revision;
    std::string build_identity;
    std::string static_runtime_abi_sha256;
    WorkerWorksetLimits limits;
    std::string canonical_sha256;

    [[nodiscard]] explicit operator bool() const noexcept
    {
        return contract_version == 1 && wrms_protocol_version == 2 &&
            workset_wire_version == 4 && !supported_game_id.empty() &&
            !executable_identity.empty() && !address_map_revision.empty() &&
            !emulator_bridge_revision.empty() && !build_identity.empty() &&
            static_runtime_abi_sha256.size() == 64 &&
            canonical_sha256.size() == 64;
    }

    auto operator<=>(const WorkerRuntimeContractV1&) const = default;
};

enum class ProgramBaselineArtifactKind : std::uint8_t
{
    Savestate,
    ReadOnlyMovie,
};

enum class ProgramBaselineComponentPolicy : std::uint8_t
{
    ImmutableShared,
    ResetForEveryItem,
};

struct ProgramBaselineArtifact
{
    ProgramBaselineArtifactKind kind =
        ProgramBaselineArtifactKind::Savestate;
    // ReadOnlyMovie is an explicit opt-in permitted only for TAS phases; it
    // is never their default. It is used only for a DTM-declared origin and
    // may name that DTM's exact startup savestate. Movie continuation from a
    // checkpoint uses Savestate with its exact DTM sidecar instead.
    // Savestate baselines require state_path; ReadOnlyMovie requires
    // movie_path.
    std::filesystem::path state_path;
    std::string state_sha256;
    std::optional<std::filesystem::path> movie_path;
    std::string movie_sha256;
    ArtifactCompatibilityToken compatibility;
    ArtifactLineage lineage;

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
    ProgramBaselineArtifact artifact;
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
    WorksetEpoch workset_epoch;
    std::string lineage;
    bool state_established = false;

    [[nodiscard]] explicit operator bool() const noexcept
    {
        return static_cast<bool>(key) && session_id && workset_epoch &&
            !lineage.empty();
    }

    auto operator<=>(const PreparedProgramBaselineReceipt&) const = default;
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
    std::string program_package_sha256;
    std::string common_input_sha256;
    std::string derived_state_binding_sha256;
    std::string capture_binding_sha256;
    std::string progress_plan_sha256;
    std::string canonical_sha256;

    [[nodiscard]] explicit operator bool() const noexcept
    {
        return !module.canonical_id.empty() && !entrypoint.empty() &&
            static_cast<bool>(baseline) &&
            program_package_sha256.size() == 64 &&
            common_input_sha256.size() == 64 &&
            derived_state_binding_sha256.size() == 64 &&
            capture_binding_sha256.size() == 64 &&
            progress_plan_sha256.size() == 64 &&
            canonical_sha256.size() == 64;
    }

    auto operator<=>(const WorkerWorksetExecutionKey&) const = default;
};

enum class CaptureProfileStorageV1 : std::uint8_t
{
    Inline = 1,
    ContentAddressedSidecar = 2,
};

struct WorksetCaptureBindingV1
{
    std::uint32_t version = 1;
    CaptureProfileStorageV1 storage = CaptureProfileStorageV1::Inline;
    std::string profile_json;
    std::filesystem::path profile_sidecar_path;
    std::string profile_sha256;
    std::string expected_module_sha256;
    std::string resolved_observation_sha256;
    std::filesystem::path output_directory;
    std::string content_sha256;

    [[nodiscard]] explicit operator bool() const noexcept
    {
        return version == 1 && profile_sha256.size() == 64 &&
            resolved_observation_sha256.size() == 64 &&
            content_sha256.size() == 64 && !output_directory.empty();
    }

    auto operator<=>(const WorksetCaptureBindingV1&) const = default;
};

struct FullPhaseInvocationEnvelope
{
    ProgramInvocationId invocation_id;
    fullphase::FullPhaseProgramPackage program_package;
    fullphase::FullPhaseCommonInput common_input;

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
    derived::WorksetDerivedStateBindingV1 derived_state;
    std::optional<WorksetCaptureBindingV1> capture;
    progress::ProgressPlanV1 progress_plan;
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
    Admitted = 0,
    AlreadyAdmitted,
};

struct SubmitWorksetResultV1
{
    WorkerWorksetId workset_id;
    std::uint32_t sidecar_version =
        kInitialWorksetCancellationSidecarVersionV1;
    std::uint32_t applied_item_count = 0;
    std::string applied_sidecar_sha256;
    WorksetSubmissionDispositionV1 disposition =
        WorksetSubmissionDispositionV1::Admitted;

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
    Admitted,
    Initializing,
    Ready,
    Running,
    ResettingItem,
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

[[nodiscard]] std::string ComputeWorksetCaptureBindingHashV1(
    const WorksetCaptureBindingV1& binding);

[[nodiscard]] std::string EmptyWorksetCaptureBindingHashV1();

[[nodiscard]] WorksetValidationResult ValidateWorkerWorksetDefinition(
    const WorkerWorksetDefinition& definition,
    const WorkerWorksetLimits& limits);

[[nodiscard]] std::string ComputeWorkerRuntimeContractHashV1(
    const WorkerRuntimeContractV1& contract);

[[nodiscard]] WorkerRuntimeContractV1
BuildProductionWorkerRuntimeContractV1();

[[nodiscard]] WorksetValidationResult ValidateWorkerRuntimeContractV1(
    const WorkerRuntimeContractV1& contract);

} // namespace savor::runtime

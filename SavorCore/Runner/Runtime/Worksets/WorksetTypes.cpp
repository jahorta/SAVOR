#include "WorksetTypes.h"

#include "Runner/Runtime/ProgramRuntime/Capabilities/SourceCapabilityPacks.h"
#include "Runner/Runtime/DerivedState/DerivedStateRegistry.h"
#include "Runner/Runtime/StopPoints/StopPointRouter.h"
#include "Utils/Hash.h"

#include <algorithm>
#include <array>
#include <limits>
#include <set>
#include <sstream>
#include <tuple>

namespace savor::runtime {
namespace {

void AppendField(std::string& output, std::string_view value)
{
    output.append(std::to_string(value.size()));
    output.push_back(':');
    output.append(value);
    output.push_back('|');
}

template <typename Value>
void AppendNumber(std::string& output, Value value)
{
    AppendField(output, std::to_string(value));
}

void AppendCompatibility(
    std::string& output,
    const ArtifactCompatibilityToken& value)
{
    AppendField(output, value.game_id);
    AppendField(output, value.iso_sha256);
    AppendField(output, value.emulator_build);
    AppendField(output, value.runtime_revision);
}

bool CompleteSha256(std::string_view value)
{
    if (value.size() != 64)
        return false;
    return std::all_of(
        value.begin(),
        value.end(),
        [](char ch)
        {
            return (ch >= '0' && ch <= '9') ||
                (ch >= 'a' && ch <= 'f');
        });
}

bool ValidWorksetLimits(const WorkerWorksetLimits& limits)
{
    return limits.maximum_items_per_workset != 0 &&
        limits.maximum_encoded_workset_bytes != 0 &&
        limits.maximum_capture_profile_bytes != 0 &&
        limits.maximum_item_credits != 0 &&
        limits.maximum_active_and_staged_items != 0 &&
        limits.finalizer_threads != 0 &&
        limits.maximum_pending_finalizers != 0 &&
        limits.maximum_pending_finalizer_bytes != 0 &&
        limits.maximum_retained_terminals != 0 &&
        limits.maximum_retained_terminal_bytes >=
            kMinimumWorksetTerminalReservationBytes &&
        limits.progressive_start_concurrency != 0 &&
        limits.maximum_items_per_workset <=
            limits.maximum_active_and_staged_items &&
        limits.maximum_active_and_staged_items <=
            limits.maximum_item_credits;
}

std::string ComputeStaticRuntimeAbiHash()
{
    std::vector<std::string> identities;
    const auto catalog =
        program::capabilities::BuildSourceCapabilityPackCatalog();
    identities.reserve(
        catalog.schemas.size() + catalog.actions.size() +
        catalog.reducers.size() + catalog.manifests.size());
    for (const auto& schema : catalog.schemas)
    {
        identities.push_back(
            "schema:" + schema.identity.canonical_id + ":" +
            std::to_string(schema.identity.version) + ":" +
            schema.identity.schema_hash.ToHex());
    }
    for (const auto& action : catalog.actions)
    {
        identities.push_back(
            "action:" + action.identity.canonical_id + ":" +
            std::to_string(action.identity.version) + ":" +
            action.identity.signature_hash.ToHex());
    }
    for (const auto& reducer : catalog.reducers)
    {
        identities.push_back(
            "reducer:" + reducer.identity.canonical_id + ":" +
            std::to_string(reducer.identity.version) + ":" +
            reducer.identity.signature_hash.ToHex());
    }
    for (const auto& manifest : catalog.manifests)
    {
        identities.push_back(
            "pack:" + manifest.identity.canonical_id + ":" +
            std::to_string(manifest.identity.version) + ":" +
            manifest.identity.manifest_hash.ToHex());
    }
    const auto& phases = fullphase::ProductionRegistry();
    for (const auto& phase : phases.identities())
    {
        const fullphase::FullPhaseWorksetPolicy policy =
            phases.Find(phase.program_kind)->workset_policy();
        identities.push_back(
            "handler:" + std::to_string(phase.program_kind) + ":" +
            std::to_string(phase.program_version) + ":" +
            phase.canonical_id + ":" +
            std::to_string(phase.contract_revision) + ":" +
            phase.canonical_sha256 + ":items:" +
            std::to_string(policy.minimum_item_count) + ":" +
            std::to_string(policy.maximum_item_count));
    }
    identities.push_back(
        "progress-registry:" +
        progress::ProductionProgressRegistry().canonical_sha256());
    identities.push_back(
        "derived-state-registry:" +
        derived::ProductionDerivedStateRegistry().canonical_sha256());
    for (const auto& observer : CanonicalStopCpuObserverDefinitions())
    {
        identities.push_back(
            "stop-cpu-observer:" +
            std::to_string(CanonicalStopCpuObserverId(observer.key)) + ":" +
            std::string(observer.stable_name));
    }
    std::sort(identities.begin(), identities.end());
    std::string canonical;
    AppendField(canonical, "savor.worker.static-runtime-abi/v1");
    for (const auto& identity : identities)
        AppendField(canonical, identity);
    return hash::sha256(canonical.data(), canonical.size());
}

} // namespace

ProgramBaselineKey ComputeProgramBaselineKey(
    const ProgramBaselineDefinition& definition)
{
    std::string canonical;
    AppendNumber(
        canonical,
        static_cast<std::uint32_t>(definition.artifact.kind));
    AppendField(canonical, definition.lineage);
    const ProgramBaselineArtifact& artifact = definition.artifact;
    AppendField(canonical, artifact.state_sha256);
    AppendNumber(canonical, artifact.movie_path ? 1 : 0);
    AppendField(canonical, artifact.movie_sha256);
    AppendCompatibility(canonical, artifact.compatibility);
    AppendField(canonical, artifact.lineage.edge);
    AppendField(canonical, artifact.lineage.producer);
    AppendNumber(canonical, definition.components.size());
    for (const ProgramBaselineComponent& component : definition.components)
    {
        AppendField(canonical, component.canonical_id);
        AppendNumber(canonical, component.revision);
        AppendField(canonical, component.schema_id);
        AppendField(canonical, component.content_sha256);
        AppendNumber(
            canonical,
            static_cast<std::uint32_t>(component.policy));
    }
    return {hash::sha256(canonical.data(), canonical.size())};
}

std::string ComputeWorkerWorksetExecutionKeyHash(
    const WorkerWorksetExecutionKey& key)
{
    std::string canonical;
    AppendField(canonical, key.module.canonical_id);
    AppendNumber(canonical, key.module.revision);
    AppendField(canonical, key.module.canonical_hash);
    AppendField(canonical, key.entrypoint);
    AppendField(canonical, key.verified_dependency_sha256);
    AppendField(canonical, key.runtime_profile_sha256);
    AppendField(canonical, key.baseline.sha256);
    AppendField(canonical, key.movie_policy_sha256);
    AppendField(canonical, key.service_policy_sha256);
    AppendField(canonical, key.program_package_sha256);
    AppendField(canonical, key.common_input_sha256);
    AppendField(canonical, key.derived_state_binding_sha256);
    AppendField(canonical, key.capture_binding_sha256);
    AppendField(canonical, key.progress_plan_sha256);
    return hash::sha256(canonical.data(), canonical.size());
}

std::string ComputeWorksetCaptureBindingHashV1(
    const WorksetCaptureBindingV1& binding)
{
    std::string canonical;
    AppendField(canonical, "savor.workset.capture-binding/v1");
    AppendNumber(canonical, binding.version);
    AppendField(canonical, binding.profile_sha256);
    AppendField(canonical, binding.expected_module_sha256);
    AppendField(canonical, binding.resolved_observation_sha256);
    return hash::sha256(canonical.data(), canonical.size());
}

std::string EmptyWorksetCaptureBindingHashV1()
{
    constexpr std::string_view canonical =
        "savor.workset.capture-binding/none/v1";
    return hash::sha256(canonical.data(), canonical.size());
}

std::string ComputeInitialWorksetCancellationSidecarSha256(
    const InitialWorksetCancellationSidecarV1& sidecar)
{
    std::string canonical;
    AppendNumber(
        canonical,
        kInitialWorksetCancellationSidecarVersionV1);
    AppendNumber(canonical, sidecar.workset_id.value());
    AppendNumber(canonical, sidecar.item_ids.size());
    for (const WorkerWorksetItemId item_id : sidecar.item_ids)
        AppendNumber(canonical, item_id.value());
    return hash::sha256(canonical.data(), canonical.size());
}

WorksetValidationResult ValidateInitialWorksetCancellationSidecar(
    const WorkerWorksetDefinition& definition,
    const InitialWorksetCancellationSidecarV1& sidecar)
{
    if (!sidecar.workset_id ||
        sidecar.workset_id != definition.workset_id)
    {
        return WorksetValidationResult::Failure(
            WorkerRejectionCode::InvalidArgument,
            "Initial cancellation sidecar does not identify the submitted workset");
    }
    std::uint64_t previous = 0;
    for (const WorkerWorksetItemId item_id : sidecar.item_ids)
    {
        if (!item_id || item_id.value() <= previous)
        {
            return WorksetValidationResult::Failure(
                WorkerRejectionCode::InvalidArgument,
                "Initial cancellation sidecar item IDs must be unique and sorted");
        }
        const bool member = std::ranges::any_of(
            definition.items,
            [&](const WorksetItemTemplate& item)
            {
                return item.item_id == item_id;
            });
        if (!member)
        {
            return WorksetValidationResult::Failure(
                WorkerRejectionCode::InvalidArgument,
                "Initial cancellation sidecar contains an item outside the workset");
        }
        previous = item_id.value();
    }
    return WorksetValidationResult::Success();
}

WorksetValidationResult ValidateWorkerWorksetDefinition(
    const WorkerWorksetDefinition& definition,
    const WorkerWorksetLimits& limits)
{
    if (!definition.workset_id)
    {
        return WorksetValidationResult::Failure(
            WorkerRejectionCode::InvalidArgument,
            "WorkerWorkset requires a nonzero identity");
    }
    const auto& phase_invocation = definition.phase_invocation;
    const auto& package = phase_invocation.program_package;
    const auto& common_input = phase_invocation.common_input;
    if (!phase_invocation.invocation_id || !package || !common_input)
    {
        return WorksetValidationResult::Failure(
            WorkerRejectionCode::InvalidArgument,
            "WorkerWorkset requires a complete Full Phase package and common input");
    }
    if (fullphase::ComputeFullPhaseProgramPackageHash(package) !=
            package.canonical_sha256 ||
        hash::sha256(
            common_input.payload.data(),
            common_input.payload.size()) != common_input.content_sha256)
    {
        return WorksetValidationResult::Failure(
            WorkerRejectionCode::InvalidArgument,
            "WorkerWorkset Full Phase package or common-input hash is not canonical");
    }
    if (!CompleteSha256(package.identity.canonical_sha256) ||
        !CompleteSha256(package.canonical_sha256) ||
        !CompleteSha256(common_input.content_sha256) ||
        package.module_closure.size() > 64)
    {
        return WorksetValidationResult::Failure(
            WorkerRejectionCode::InvalidArgument,
            "WorkerWorkset Full Phase package identity or module count is invalid");
    }
    bool contains_root = false;
    std::set<std::tuple<std::string, std::uint32_t, std::string>> modules;
    for (const EncodedModuleEnvelope& module : package.module_closure)
    {
        if (module.identity.canonical_id.empty() ||
            module.identity.revision == 0 ||
            !CompleteSha256(module.identity.canonical_hash) ||
            module.format_version != 1 || module.payload.empty() ||
            !modules.emplace(
                module.identity.canonical_id,
                module.identity.revision,
                module.identity.canonical_hash).second)
        {
            return WorksetValidationResult::Failure(
                WorkerRejectionCode::InvalidArgument,
                "WorkerWorkset Full Phase module closure is invalid");
        }
        contains_root = contains_root ||
            module.identity == package.runtime_contract.module;
    }
    if (!contains_root)
    {
        return WorksetValidationResult::Failure(
            WorkerRejectionCode::InvalidArgument,
            "WorkerWorkset Full Phase module closure omits its root module");
    }
    if (definition.items.empty() ||
        definition.items.size() > limits.maximum_items_per_workset)
    {
        return WorksetValidationResult::Failure(
            WorkerRejectionCode::InvalidArgument,
            "WorkerWorkset item count exceeds its negotiated bounds");
    }
    if (definition.encoded_size_bytes >
        limits.maximum_encoded_workset_bytes)
    {
        return WorksetValidationResult::Failure(
            WorkerRejectionCode::InvalidArgument,
            "WorkerWorkset encoded bytes exceed the negotiated bound");
    }
    const ProgramBaselineKey baseline =
        ComputeProgramBaselineKey(definition.baseline);
    if (!baseline || baseline != definition.execution_key.baseline)
    {
        return WorksetValidationResult::Failure(
            WorkerRejectionCode::InvalidArgument,
            "WorkerWorkset baseline identity does not match its execution key");
    }
    if (!definition.execution_key)
    {
        return WorksetValidationResult::Failure(
            WorkerRejectionCode::InvalidArgument,
            "WorkerWorkset execution key is incomplete");
    }
    if (ComputeWorkerWorksetExecutionKeyHash(definition.execution_key) !=
        definition.execution_key.canonical_sha256)
    {
        return WorksetValidationResult::Failure(
            WorkerRejectionCode::InvalidArgument,
            "WorkerWorkset execution key hash is not canonical");
    }
    std::string derived_error;
    if (!derived::ValidateWorksetDerivedStateBindingV1(
            definition.derived_state, &derived_error))
    {
        return WorksetValidationResult::Failure(
            WorkerRejectionCode::InvalidArgument,
            derived_error.empty()
                ? "WorkerWorkset derived-state binding is invalid"
                : std::move(derived_error));
    }
    const auto& derived_registry =
        derived::ProductionDerivedStateRegistry();
    for (const auto& block : definition.derived_state.blocks)
    {
        const auto* descriptor =
            derived_registry.FindBlock(block.identity.canonical_id);
        if (!descriptor || descriptor->identity != block.identity ||
            block.configuration.size() >
                descriptor->maximum_configuration_bytes)
        {
            return WorksetValidationResult::Failure(
                WorkerRejectionCode::InvalidArgument,
                "WorkerWorkset derived-state block is unavailable or mismatched");
        }
    }
    const std::string capture_binding_sha256 = definition.capture
        ? definition.capture->content_sha256
        : EmptyWorksetCaptureBindingHashV1();
    if (definition.execution_key.derived_state_binding_sha256 !=
            definition.derived_state.content_sha256 ||
        definition.execution_key.capture_binding_sha256 !=
            capture_binding_sha256 ||
        definition.execution_key.progress_plan_sha256 !=
            definition.progress_plan.content_sha256)
    {
        return WorksetValidationResult::Failure(
            WorkerRejectionCode::InvalidArgument,
            "WorkerWorkset observation hashes do not match its execution key");
    }
    const progress::ProgressValidationResult progress_validation =
        progress::ValidateProgressPlanV1(definition.progress_plan);
    if (!progress_validation)
    {
        return WorksetValidationResult::Failure(
            WorkerRejectionCode::InvalidArgument,
            progress_validation.message.empty()
                ? "WorkerWorkset progress plan is invalid"
                : progress_validation.message);
    }
    if (definition.capture)
    {
        const WorksetCaptureBindingV1& capture = *definition.capture;
        if (!capture ||
            !CompleteSha256(capture.profile_sha256) ||
            !CompleteSha256(capture.expected_module_sha256) ||
            !CompleteSha256(capture.resolved_observation_sha256) ||
            ComputeWorksetCaptureBindingHashV1(capture) !=
                capture.content_sha256)
        {
            return WorksetValidationResult::Failure(
                WorkerRejectionCode::InvalidArgument,
                "WorkerWorkset capture binding identity is invalid");
        }
        switch (capture.storage)
        {
        case CaptureProfileStorageV1::Inline:
            if (capture.profile_json.empty() ||
                !capture.profile_sidecar_path.empty() ||
                hash::sha256(
                    capture.profile_json.data(),
                    capture.profile_json.size()) !=
                    capture.profile_sha256)
            {
                return WorksetValidationResult::Failure(
                    WorkerRejectionCode::InvalidArgument,
                    "Inline capture profile content is not canonical");
            }
            break;
        case CaptureProfileStorageV1::ContentAddressedSidecar:
            if (!capture.profile_json.empty() ||
                capture.profile_sidecar_path.empty())
            {
                return WorksetValidationResult::Failure(
                    WorkerRejectionCode::InvalidArgument,
                    "Capture sidecar binding is incomplete");
            }
            break;
        default:
            return WorksetValidationResult::Failure(
                WorkerRejectionCode::InvalidArgument,
                "Capture profile storage kind is invalid");
        }
    }
    const auto* phase = fullphase::ProductionRegistry().Find(
        package.identity.program_kind);
    if (phase == nullptr)
    {
        return WorksetValidationResult::Failure(
            WorkerRejectionCode::Unsupported,
            "WorkerWorkset Full Phase kind has no local production handler");
    }
    const fullphase::FullPhaseWorksetPolicy workset_policy =
        phase->workset_policy();
    if (!workset_policy.accepts(definition.items.size()))
    {
        return WorksetValidationResult::Failure(
            WorkerRejectionCode::InvalidArgument,
            "WorkerWorkset item count violates its Full Phase program-kind policy");
    }
    const auto& contract = package.runtime_contract;
    if (definition.execution_key.module != contract.module ||
        definition.execution_key.entrypoint != contract.entrypoint ||
        definition.execution_key.verified_dependency_sha256 !=
            contract.verified_dependency_sha256 ||
        definition.execution_key.runtime_profile_sha256 !=
            contract.runtime_profile_sha256 ||
        definition.execution_key.movie_policy_sha256 !=
            contract.movie_policy_sha256 ||
        definition.execution_key.service_policy_sha256 !=
            contract.service_policy_sha256 ||
        definition.execution_key.program_package_sha256 !=
            package.canonical_sha256 ||
        definition.execution_key.common_input_sha256 !=
            common_input.content_sha256 ||
        definition.baseline.lineage != contract.baseline_lineage)
    {
        return WorksetValidationResult::Failure(
            WorkerRejectionCode::InvalidArgument,
            "WorkerWorkset execution or baseline invariants disagree with its Full Phase definition");
    }

    if (definition.baseline.lineage.empty())
    {
        return WorksetValidationResult::Failure(
            WorkerRejectionCode::InvalidArgument,
            "WorkerWorkset baseline requires explicit lineage");
    }
    const ProgramBaselineArtifact& artifact = definition.baseline.artifact;
    if (!artifact.compatibility.Complete() ||
        artifact.lineage.edge.empty() || artifact.lineage.producer.empty())
    {
        return WorksetValidationResult::Failure(
            WorkerRejectionCode::InvalidArgument,
            "Program baseline artifact compatibility or lineage is incomplete");
    }
    const bool has_state = !artifact.state_path.empty();
    const bool has_state_hash = !artifact.state_sha256.empty();
    const bool has_movie = artifact.movie_path.has_value();
    const bool has_movie_hash = !artifact.movie_sha256.empty();
    switch (artifact.kind)
    {
    case ProgramBaselineArtifactKind::Savestate: {
        if (!has_state || !CompleteSha256(artifact.state_sha256) ||
            has_movie != has_movie_hash)
        {
            return WorksetValidationResult::Failure(
                WorkerRejectionCode::InvalidArgument,
                "Savestate baseline requires an exact state and a complete optional DTM sidecar");
        }
        if (has_movie)
        {
            const std::filesystem::path expected_movie =
                std::filesystem::path(artifact.state_path.string() + ".dtm");
            if (*artifact.movie_path != expected_movie ||
                !CompleteSha256(artifact.movie_sha256))
            {
                return WorksetValidationResult::Failure(
                    WorkerRejectionCode::InvalidArgument,
                    "Savestate baseline DTM must be the exact same-name sidecar");
            }
        }
        break;
    }
    case ProgramBaselineArtifactKind::ReadOnlyMovie:
        if (!has_movie || !CompleteSha256(artifact.movie_sha256) ||
            has_state != has_state_hash ||
            (has_state && !CompleteSha256(artifact.state_sha256)))
        {
            return WorksetValidationResult::Failure(
                WorkerRejectionCode::InvalidArgument,
                "Read-only-movie baseline requires an exact DTM and a complete optional startup savestate");
        }
        break;
    default:
        return WorksetValidationResult::Failure(
            WorkerRejectionCode::InvalidArgument,
            "Program baseline artifact kind is invalid");
    }
    std::set<std::pair<std::string, std::uint32_t>> component_ids;
    for (const ProgramBaselineComponent& component :
         definition.baseline.components)
    {
        if (component.canonical_id.empty() ||
            component.revision == 0 ||
            component.schema_id.empty() ||
            !CompleteSha256(component.content_sha256) ||
            !component_ids
                 .emplace(component.canonical_id, component.revision)
                 .second)
        {
            return WorksetValidationResult::Failure(
                WorkerRejectionCode::InvalidArgument,
                "Program baseline component identity, schema, content hash, "
                "or uniqueness is invalid");
        }
    }

    std::set<std::uint64_t> item_ids;
    std::set<std::uint64_t> execution_ids;
    std::size_t aggregate_terminal_bytes = 0;
    for (std::size_t index = 0; index < definition.items.size(); ++index)
    {
        const WorksetItemTemplate& item = definition.items[index];
        if (!item.item_id || item.ordinal != index ||
            !item.execution.execution_id ||
            !item.execution.attempt_id ||
            item.execution.input_payload.empty())
        {
            return WorksetValidationResult::Failure(
                WorkerRejectionCode::InvalidArgument,
                "WorkerWorkset item identity, order, or scalar execution binding is invalid");
        }
        if (!item_ids.insert(item.item_id.value()).second ||
            !execution_ids
                 .insert(item.execution.execution_id.value())
                 .second)
        {
            return WorksetValidationResult::Failure(
                WorkerRejectionCode::InvalidArgument,
                "WorkerWorkset contains duplicate item or execution identities");
        }
        if (item.declared_terminal_bytes <
                kMinimumWorksetTerminalReservationBytes ||
            item.declared_terminal_bytes >
                std::min(
                    limits.maximum_retained_terminal_bytes,
                    kMaximumWorksetTerminalReservationBytes) ||
            aggregate_terminal_bytes >
                limits.maximum_retained_terminal_bytes -
                    item.declared_terminal_bytes)
        {
            return WorksetValidationResult::Failure(
                WorkerRejectionCode::CapacityExceeded,
                "WorkerWorkset item terminal-byte reservation exceeds its negotiated bound");
        }
        aggregate_terminal_bytes += item.declared_terminal_bytes;
    }
    return WorksetValidationResult::Success();
}

std::string ComputeWorkerRuntimeContractHashV1(
    const WorkerRuntimeContractV1& contract)
{
    std::string canonical;
    AppendField(canonical, "savor.worker.runtime-contract/v1");
    AppendNumber(canonical, contract.contract_version);
    AppendNumber(canonical, contract.wrms_protocol_version);
    AppendNumber(canonical, contract.workset_wire_version);
    AppendNumber(canonical, contract.program_module_format_version);
    AppendNumber(canonical, contract.program_invocation_format_version);
    AppendNumber(canonical, contract.program_result_format_version);
    AppendField(canonical, contract.supported_game_id);
    AppendField(canonical, contract.executable_identity);
    AppendField(canonical, contract.address_map_revision);
    AppendField(canonical, contract.emulator_bridge_revision);
    AppendField(canonical, contract.build_identity);
    AppendField(canonical, contract.static_runtime_abi_sha256);
    const auto& limits = contract.limits;
    AppendNumber(canonical, limits.maximum_items_per_workset);
    AppendNumber(canonical, limits.maximum_encoded_workset_bytes);
    AppendNumber(canonical, limits.maximum_capture_profile_bytes);
    AppendNumber(canonical, limits.maximum_item_credits);
    AppendNumber(canonical, limits.maximum_active_and_staged_items);
    AppendNumber(canonical, limits.finalizer_threads);
    AppendNumber(canonical, limits.maximum_pending_finalizers);
    AppendNumber(canonical, limits.maximum_pending_finalizer_bytes);
    AppendNumber(canonical, limits.maximum_retained_terminals);
    AppendNumber(canonical, limits.maximum_retained_terminal_bytes);
    AppendNumber(canonical, limits.progressive_start_concurrency);
    return hash::sha256(canonical.data(), canonical.size());
}

WorkerRuntimeContractV1 BuildProductionWorkerRuntimeContractV1()
{
    WorkerRuntimeContractV1 contract{
        .supported_game_id = std::string(
            program::capabilities::kSupportedGameId),
        .executable_identity = std::string(
            program::capabilities::kSupportedExecutableIdentity),
        .address_map_revision = std::string(
            program::capabilities::kSupportedAddressMapRevision),
        .emulator_bridge_revision = "dolphin-2506a/savor-bridge-v1",
        .build_identity = "SavorWorker homogeneous-runtime/v1",
        .static_runtime_abi_sha256 = ComputeStaticRuntimeAbiHash(),
    };
    contract.canonical_sha256 =
        ComputeWorkerRuntimeContractHashV1(contract);
    return contract;
}

WorksetValidationResult ValidateWorkerRuntimeContractV1(
    const WorkerRuntimeContractV1& contract)
{
    if (!contract || contract.program_module_format_version != 1 ||
        contract.program_invocation_format_version != 1 ||
        contract.program_result_format_version != 1 ||
        !CompleteSha256(contract.static_runtime_abi_sha256) ||
        !ValidWorksetLimits(contract.limits))
    {
        return WorksetValidationResult::Failure(
            WorkerRejectionCode::InvalidArgument,
            "Worker runtime contract is incomplete or contradictory");
    }
    if (ComputeWorkerRuntimeContractHashV1(contract) !=
        contract.canonical_sha256)
    {
        return WorksetValidationResult::Failure(
            WorkerRejectionCode::InvalidArgument,
            "Worker runtime contract hash is not canonical");
    }
    return WorksetValidationResult::Success();
}

} // namespace savor::runtime

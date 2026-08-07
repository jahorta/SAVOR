#include "WorksetTypes.h"

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
    if (!definition.phase_invocation.invocation_id ||
        !definition.phase_invocation.program)
    {
        return WorksetValidationResult::Failure(
            WorkerRejectionCode::InvalidArgument,
            "WorkerWorkset requires one complete Full Phase invocation identity");
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
    const auto* phase = fullphase::ProductionRegistry().Find(
        definition.phase_invocation.program);
    if (phase == nullptr)
    {
        const auto* local = fullphase::ProductionRegistry().Find(
            definition.phase_invocation.program.program_kind);
        if (local != nullptr)
        {
            const auto& received =
                definition.phase_invocation.program;
            const auto& available = local->identity();
            return WorksetValidationResult::Failure(
                WorkerRejectionCode::Unsupported,
                "WorkerWorkset Full Phase identity disagrees with the "
                "local production definition: received=" +
                    received.canonical_id + "@" +
                    std::to_string(received.contract_revision) + "#" +
                    received.canonical_sha256 + ", local=" +
                    available.canonical_id + "@" +
                    std::to_string(available.contract_revision) + "#" +
                    available.canonical_sha256);
        }
        return WorksetValidationResult::Failure(
            WorkerRejectionCode::Unsupported,
            "WorkerWorkset Full Phase identity is not in the local production registry");
    }
    const auto& contract = phase->runtime_contract();
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

std::string ComputeRuntimeCatalogHash(
    const std::vector<RuntimeModuleManifestEntry>& modules,
    RuntimeCatalogStatus status)
{
    std::vector<RuntimeModuleManifestEntry> ordered = modules;
    std::sort(
        ordered.begin(),
        ordered.end(),
        [](const auto& lhs, const auto& rhs)
        {
            if (lhs.module.canonical_id != rhs.module.canonical_id)
                return lhs.module.canonical_id < rhs.module.canonical_id;
            if (lhs.module.revision != rhs.module.revision)
                return lhs.module.revision < rhs.module.revision;
            return lhs.module.canonical_hash < rhs.module.canonical_hash;
        });

    std::string canonical;
    AppendNumber(canonical, static_cast<std::uint32_t>(status));
    for (const RuntimeModuleManifestEntry& entry : ordered)
    {
        AppendField(canonical, entry.module.canonical_id);
        AppendNumber(canonical, entry.module.revision);
        AppendField(canonical, entry.module.canonical_hash);
        AppendField(canonical, entry.dependency_manifest_sha256);
        AppendNumber(canonical, entry.development_only ? 1 : 0);
        std::vector<std::string> entrypoints =
            entry.entrypoints;
        std::sort(entrypoints.begin(), entrypoints.end());
        for (const std::string& entrypoint : entrypoints)
            AppendField(canonical, entrypoint);
    }
    return hash::sha256(canonical.data(), canonical.size());
}

WorksetValidationResult ValidateWorkerRuntimeManifest(
    const WorkerRuntimeManifest& manifest)
{
    if (manifest.wrms_protocol_version != 1 ||
        manifest.program_module_format_version != 1 ||
        manifest.program_invocation_format_version != 1 ||
        manifest.program_result_format_version != 1 ||
        manifest.catalog_generation == 0 ||
        !CompleteSha256(manifest.runtime_profile_sha256) ||
        !CompleteSha256(manifest.dependency_manifest_sha256) ||
        !CompleteSha256(manifest.catalog_sha256))
    {
        return WorksetValidationResult::Failure(
            WorkerRejectionCode::InvalidArgument,
            "Worker runtime manifest version, generation, or hashes are incomplete");
    }

    std::set<std::tuple<std::string, std::uint32_t, std::string>>
        module_ids;
    for (const RuntimeModuleManifestEntry& module :
         manifest.modules)
    {
        if (module.module.canonical_id.empty() ||
            module.module.revision == 0 ||
            !CompleteSha256(module.module.canonical_hash) ||
            module.entrypoints.empty() ||
            !CompleteSha256(module.dependency_manifest_sha256) ||
            !module_ids
                 .emplace(
                     module.module.canonical_id,
                     module.module.revision,
                     module.module.canonical_hash)
                 .second)
        {
            return WorksetValidationResult::Failure(
                WorkerRejectionCode::InvalidArgument,
                "Worker runtime manifest module identity or dependency hash is invalid");
        }
        std::set<std::string> entrypoints;
        for (const std::string& entrypoint :
             module.entrypoints)
        {
            if (entrypoint.empty() ||
                !entrypoints.insert(entrypoint).second)
            {
                return WorksetValidationResult::Failure(
                    WorkerRejectionCode::InvalidArgument,
                    "Worker runtime manifest contains an empty or duplicate entrypoint");
            }
        }
    }
    if (ComputeRuntimeCatalogHash(
            manifest.modules,
            manifest.catalog_status) !=
        manifest.catalog_sha256)
    {
        return WorksetValidationResult::Failure(
            WorkerRejectionCode::WorksetCatalogMismatch,
            "Worker runtime manifest catalog hash is not canonical");
    }

    const WorkerWorksetLimits& limits = manifest.limits;
    if (limits.maximum_items_per_workset == 0 ||
        limits.maximum_encoded_workset_bytes == 0 ||
        limits.maximum_item_credits == 0 ||
        limits.maximum_active_and_staged_items == 0 ||
        limits.finalizer_threads == 0 ||
        limits.maximum_pending_finalizers == 0 ||
        limits.maximum_pending_finalizer_bytes == 0 ||
        limits.maximum_retained_terminals == 0 ||
        limits.maximum_retained_terminal_bytes <
            kMinimumWorksetTerminalReservationBytes ||
        limits.progressive_start_concurrency == 0 ||
        limits.maximum_items_per_workset >
            limits.maximum_active_and_staged_items ||
        limits.maximum_active_and_staged_items >
            limits.maximum_item_credits)
    {
        return WorksetValidationResult::Failure(
            WorkerRejectionCode::InvalidArgument,
            "Worker runtime manifest contains zero or contradictory negotiated limits");
    }
    return WorksetValidationResult::Success();
}

} // namespace savor::runtime

#include "FullPhaseProgram.h"

#include "../../../Phases/Programs/SeedProbe/SeedProbeModule.h"
#include "../../../Phases/Programs/TasMovieValidation/TasMovieValidationModule.h"
#include "../../../Phases/Programs/TasMovieInputEpoch/TasMovieInputEpochModule.h"
#include "../../../Phases/Programs/BattleContext/BattleContextModule.h"
#include "../../../Phases/Programs/BattleCompletion/BattleCompletionModule.h"
#include "../../../Phases/Programs/BattleRecord/BattleRecordModule.h"
#include "../../../Phases/Programs/BattleRecord/BattleReplayModule.h"
#include "../../../Phases/Programs/BattleSingleTurn/BattleSingleTurnModule.h"
#include "Utils/Hash.h"

#include <algorithm>
#include <string_view>
#include <tuple>

namespace savor::runtime::fullphase {
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

void AppendModuleIdentity(
    std::string& output,
    const ProgramModuleIdentity& identity)
{
    AppendField(output, identity.canonical_id);
    AppendNumber(output, identity.revision);
    AppendField(output, identity.canonical_hash);
}

void AppendBudgets(
    std::string& output,
    const program::ProgramBudgets& value)
{
    AppendNumber(output, value.maximum_instructions);
    AppendNumber(output, value.maximum_calls);
    AppendNumber(output, value.maximum_call_depth);
    AppendNumber(output, value.maximum_action_requests);
    AppendNumber(output, value.maximum_emissions);
    AppendNumber(output, value.maximum_artifacts);
    AppendNumber(output, value.maximum_values);
    AppendNumber(output, value.maximum_value_bytes);
    AppendNumber(output, value.maximum_trace_events);
}

} // namespace

std::string ComputeFullPhaseProgramPackageHash(
    const FullPhaseProgramPackage& package)
{
    std::string canonical;
    AppendField(canonical, "savor.full_phase.package/v1");
    AppendNumber(canonical, package.identity.program_kind);
    AppendNumber(canonical, package.identity.program_version);
    AppendField(canonical, package.identity.canonical_id);
    AppendNumber(canonical, package.identity.contract_revision);
    AppendField(canonical, package.identity.canonical_sha256);

    const FullPhaseRuntimeContract& runtime = package.runtime_contract;
    AppendModuleIdentity(canonical, runtime.module);
    AppendField(canonical, runtime.entrypoint);
    AppendField(canonical, runtime.dependency_lock_sha256);
    AppendField(canonical, runtime.verified_dependency_sha256);
    AppendField(canonical, runtime.runtime_profile_sha256);
    AppendNumber(canonical, static_cast<std::uint8_t>(runtime.state_policy));
    AppendNumber(canonical, static_cast<std::uint8_t>(runtime.execution.intent));
    AppendNumber(canonical, runtime.execution.allow_movie_playback ? 1 : 0);
    AppendNumber(canonical, runtime.execution.allow_movie_recording ? 1 : 0);
    AppendNumber(canonical, runtime.execution.allow_input ? 1 : 0);
    AppendNumber(canonical, runtime.execution.allow_capture ? 1 : 0);
    AppendNumber(canonical, runtime.execution.record_trace ? 1 : 0);
    AppendBudgets(canonical, runtime.limits);
    AppendField(canonical, runtime.baseline_lineage);
    AppendField(canonical, runtime.movie_policy_sha256);
    AppendField(canonical, runtime.service_policy_sha256);

    AppendNumber(canonical, package.module_closure.size());
    for (const EncodedModuleEnvelope& module : package.module_closure)
    {
        AppendModuleIdentity(canonical, module.identity);
        AppendNumber(canonical, module.format_version);
        AppendNumber(canonical, module.development_only ? 1 : 0);
        AppendField(
            canonical,
            hash::sha256(module.payload.data(), module.payload.size()));
    }
    return hash::sha256(canonical.data(), canonical.size());
}

FullPhaseProgramPackage BuildFullPhaseProgramPackage(
    const IFullPhaseProgramDefinition& definition)
{
    FullPhaseProgramPackage package{
        .identity = definition.identity(),
        .runtime_contract = definition.runtime_contract(),
        .module_closure = {definition.module_envelope()},
    };
    package.canonical_sha256 =
        ComputeFullPhaseProgramPackageHash(package);
    return package;
}

FullPhaseCommonInput MakeFullPhaseCommonInput(
    std::string schema_id,
    std::uint32_t schema_version,
    std::vector<std::uint8_t> payload)
{
    FullPhaseCommonInput input{
        .schema_id = std::move(schema_id),
        .schema_version = schema_version,
        .payload = std::move(payload),
    };
    input.content_sha256 =
        hash::sha256(input.payload.data(), input.payload.size());
    return input;
}

std::optional<program::ProgramInvocation>
IFullPhaseProgramDefinition::BuildResolvedExecution(
    const FullPhaseProgramPackage& package,
    std::span<const std::uint8_t> common_input_payload,
    std::span<const std::uint8_t> item_input_payload,
    ProgramExecutionId execution_id,
    AttemptId attempt_id,
    std::string* diagnostic) const
{
    if (package.identity != identity() ||
        package.runtime_contract != runtime_contract() ||
        package.module_closure.size() != 1 ||
        package.module_closure.front() != module_envelope())
    {
        if (diagnostic)
            *diagnostic =
                "Full Phase workset package does not match the fixed kind definition";
        return std::nullopt;
    }
    if (!common_input_payload.empty())
    {
        if (diagnostic)
            *diagnostic =
                "This Full Phase kind requires an empty common input";
        return std::nullopt;
    }
    return BuildResolvedExecution(
        item_input_payload,
        execution_id,
        attempt_id,
        diagnostic);
}

bool FullPhaseProgramRegistry::Register(
    std::shared_ptr<const IFullPhaseProgramDefinition> definition)
{
    if (!definition || !definition->identity() ||
        !definition->workset_policy().valid())
        return false;
    if (Find(definition->identity().program_kind) != nullptr)
        return false;
    definitions_.push_back(std::move(definition));
    return true;
}

const IFullPhaseProgramDefinition* FullPhaseProgramRegistry::Find(
    std::int32_t program_kind) const noexcept
{
    const auto found = std::ranges::find_if(
        definitions_,
        [&](const auto& definition)
        {
            return definition->identity().program_kind == program_kind;
        });
    return found == definitions_.end() ? nullptr : found->get();
}

const IFullPhaseProgramDefinition* FullPhaseProgramRegistry::Find(
    const FullPhaseProgramIdentity& identity) const noexcept
{
    const auto* definition = Find(identity.program_kind);
    return definition != nullptr && definition->identity() == identity
        ? definition
        : nullptr;
}

std::vector<FullPhaseProgramIdentity>
FullPhaseProgramRegistry::identities() const
{
    std::vector<FullPhaseProgramIdentity> result;
    result.reserve(definitions_.size());
    for (const auto& definition : definitions_)
        result.push_back(definition->identity());
    return result;
}

const FullPhaseProgramRegistry& ProductionRegistry()
{
    static const FullPhaseProgramRegistry registry = []
    {
        FullPhaseProgramRegistry value;
        (void)value.Register(
            battlecontext::BattleContextFullPhaseDefinitionV1());
        (void)value.Register(
            battlecompletion::BattleCompletionFullPhaseDefinitionV1());
        (void)value.Register(
            battlerecord::BattleRecordKindHandlerV1());
        (void)value.Register(
            battlereplay::BattleReplayKindHandlerV1());
        (void)value.Register(
            battlesingleturn::BattleSingleTurnKindHandlerV1());
        (void)value.Register(
            seedprobe::SeedProbeFullPhaseDefinitionV2());
        (void)value.Register(
            tasmovie::TasMovieValidationFullPhaseDefinitionV1());
        (void)value.Register(
            tasmovie::TasMovieCheckpointSterilizationFullPhaseDefinitionV1());
        (void)value.Register(
            tasmovie::inputepoch::AnnotationFullPhaseDefinitionV1());
        (void)value.Register(
            tasmovie::inputepoch::RewriteFullPhaseDefinitionV1());
        return value;
    }();
    return registry;
}

} // namespace savor::runtime::fullphase

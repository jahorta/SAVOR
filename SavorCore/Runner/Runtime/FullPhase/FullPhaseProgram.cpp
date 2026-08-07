#include "FullPhaseProgram.h"

#include "../../../Phases/Programs/SeedProbe/SeedProbeModule.h"
#include "../../../Phases/Programs/TasMovieValidation/TasMovieValidationModule.h"

#include <algorithm>

namespace savor::runtime::fullphase {

bool FullPhaseProgramRegistry::Register(
    std::shared_ptr<const IFullPhaseProgramDefinition> definition)
{
    if (!definition || !definition->identity())
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
            seedprobe::SeedProbeFullPhaseDefinitionV2());
        (void)value.Register(
            tasmovie::TasMovieValidationFullPhaseDefinitionV1());
        return value;
    }();
    return registry;
}

} // namespace savor::runtime::fullphase

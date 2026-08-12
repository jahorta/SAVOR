#include "WorksetDerivedStateBinding.h"

#include "../../../SavorCore/Runner/Runtime/DerivedState/DerivedStateRegistry.h"
#include "../../../SavorCore/Runner/Runtime/ProgramRuntime/Codec/ProgramCodecV1.h"
#include "../../../SavorCore/Runner/Runtime/ProgramRuntime/Capabilities/SourceCapabilityPacks.h"
#include "../../../SavorCore/Runner/Runtime/Worksets/WorksetWireCodec.h"

#include <set>
#include <algorithm>

namespace savor::db::execution::programdb {
namespace {

bool Fail(std::string message, std::string* error_out)
{
    if (error_out)
        *error_out = std::move(message);
    return false;
}

} // namespace

bool ResolveWorksetDerivedStateBindingV1(
    std::span<const std::string> default_block_ids,
    const savor::runtime::fullphase::FullPhaseProgramPackage& package,
    ResolvedWorksetDerivedStateBindingV1* output,
    std::string* error_out)
{
    using namespace savor::runtime;
    if (!output || !package)
        return Fail("Derived-state workset resolver input is incomplete", error_out);

    std::vector<std::string> sorted_defaults(
        default_block_ids.begin(), default_block_ids.end());
    std::ranges::sort(sorted_defaults);
    if (std::ranges::adjacent_find(sorted_defaults) !=
        sorted_defaults.end())
    {
        return Fail(
            "Program-kind derived-state defaults contain a duplicate block ID",
            error_out);
    }
    std::set<std::string> required(
        sorted_defaults.begin(), sorted_defaults.end());
    const auto& registry = derived::ProductionDerivedStateRegistry();
    const auto action_catalog =
        program::capabilities::BuildSourceCapabilityPackCatalog();
    for (const auto& encoded : package.module_closure)
    {
        const auto decoded = program::DecodeProgramModuleV1(encoded.payload);
        if (!decoded)
        {
            return Fail(
                decoded.status.message.empty()
                    ? "Derived-state dependency scan could not decode the prepared module closure"
                    : decoded.status.message,
                error_out);
        }
        for (const auto& action : decoded.value->action_imports)
        {
            const auto descriptor = std::ranges::find(
                action_catalog.actions,
                action,
                &program::ActionDescriptor::identity);
            if (descriptor != action_catalog.actions.end() &&
                !descriptor->required_derived_state_block_id.empty())
            {
                const auto* block = registry.FindBlock(
                    descriptor->required_derived_state_block_id);
                if (!block ||
                    registry.FindBlockForQueryAction(action) != block)
                    return Fail(
                        "Derived-state query action does not match its exact static block descriptor",
                        error_out);
                required.insert(block->identity.canonical_id);
            }
        }
    }

    std::vector<std::string> block_ids(required.begin(), required.end());
    std::string binding_error;
    auto binding = derived::ResolveWorksetDerivedStateBindingV1(
        block_ids, &binding_error);
    if (!derived::ValidateWorksetDerivedStateBindingV1(
            binding, &binding_error))
    {
        return Fail(
            binding_error.empty()
                ? "Derived-state workset binding could not be resolved"
                : std::move(binding_error),
            error_out);
    }

    ResolvedWorksetDerivedStateBindingV1 candidate;
    candidate.binding = std::move(binding);
    candidate.binding_sha256 = candidate.binding.content_sha256;
    const auto encoded = EncodeWorksetDerivedStateBindingV1(
        candidate.binding, candidate.encoded_binding);
    if (!encoded)
        return Fail(encoded.message, error_out);
    *output = std::move(candidate);
    if (error_out)
        error_out->clear();
    return true;
}

} // namespace savor::db::execution::programdb

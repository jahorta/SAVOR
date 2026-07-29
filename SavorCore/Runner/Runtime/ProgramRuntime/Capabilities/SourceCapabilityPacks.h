#pragma once

#include "Runner/Runtime/ProgramRuntime/Registry/CapabilityPackRegistry.h"

#include <string_view>
#include <vector>

namespace savor::runtime::program::capabilities {

inline constexpr std::string_view kSupportedGameId = "GEAE8P";
inline constexpr std::string_view kSupportedExecutableIdentity =
    "soal-usa.GEAE8E";
inline constexpr std::string_view kSupportedAddressMapRevision =
    "savor.builtin-soal-usa-addresses/1";

struct SourceCapabilityPackCatalog
{
    std::vector<TypeSchemaDefinition> schemas;
    std::vector<ActionDescriptor> actions;
    std::vector<ReducerDescriptor> reducers;
    std::vector<CapabilityPackManifest> manifests;
};

[[nodiscard]] RuntimeCompatibility SupportedSoaUsaCompatibility();

[[nodiscard]] CapabilityPackIdentity FieldPackIdentity();
[[nodiscard]] CapabilityPackIdentity BattlePackIdentity();
[[nodiscard]] CapabilityPackIdentity NavigationPackIdentity();

[[nodiscard]] ExactDependencyIdentity BattleCaptureContextActionIdentity();
[[nodiscard]] ExactDependencyIdentity NavigationCaptureContextActionIdentity();

[[nodiscard]] SourceCapabilityPackCatalog BuildSourceCapabilityPackCatalog();

// The registries must refer to each other in the normal order:
// TypeSchemaRegistry -> ActionRegistry -> CapabilityPackRegistry. Definitions
// are registered as schema/action batches before one atomic manifest batch.
[[nodiscard]] RegistryResult RegisterSourceCapabilityPacks(
    TypeSchemaRegistry& schemas,
    ActionRegistry& actions,
    CapabilityPackRegistry& packs);

} // namespace savor::runtime::program::capabilities

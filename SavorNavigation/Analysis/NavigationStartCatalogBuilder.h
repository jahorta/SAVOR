#pragma once

#include "../Model/NavigationAreaIdentity.h"
#include "../Model/NavigationScriptModel.h"

namespace spice::sct {
struct SctParseResult;
}

namespace savor::navigation {

// Parser-facing implementation boundary. SPICE types are consumed here and
// converted immediately into the SAVOR-owned catalog exposed by the model.
class NavigationStartCatalogBuilder final {
public:
    [[nodiscard]] NavigationStartCatalog build(
        const spice::sct::SctParseResult& parseResult,
        const NavigationAreaIdentity& areaIdentity) const;
};

} // namespace savor::navigation

#pragma once

#include "CompositionSupport.h"
#include "SemanticObservationComposition.h"

namespace savor::runtime::program::composition {

struct InputDeliveryLoweringResult
{
    ProgramValueId stop;
    ProgramValueId receipt;
};

[[nodiscard]] std::optional<InputDeliveryLoweringResult>
LowerSynchronizedFrameDelivery(
    detail::ModuleFragmentBuilder& builder,
    ProgramFunction& function,
    BasicBlock& block,
    ProgramValueId frame,
    std::span<const SemanticPointReference> endpoints,
    std::string selector,
    bool fail_on_movie_end = true);

} // namespace savor::runtime::program::composition

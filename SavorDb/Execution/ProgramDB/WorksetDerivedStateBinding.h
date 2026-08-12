#pragma once

#include "../../../SavorCore/Runner/Runtime/FullPhase/FullPhaseProgram.h"
#include "../../../SavorCore/Runner/Runtime/Worksets/WorksetTypes.h"

#include <span>
#include <string>
#include <vector>

namespace savor::db::execution::programdb {

struct ResolvedWorksetDerivedStateBindingV1
{
    savor::runtime::derived::WorksetDerivedStateBindingV1 binding;
    std::vector<std::uint8_t> encoded_binding;
    std::string binding_sha256;
};

[[nodiscard]] bool ResolveWorksetDerivedStateBindingV1(
    std::span<const std::string> default_block_ids,
    const savor::runtime::fullphase::FullPhaseProgramPackage& package,
    ResolvedWorksetDerivedStateBindingV1* output,
    std::string* error_out = nullptr);

} // namespace savor::db::execution::programdb

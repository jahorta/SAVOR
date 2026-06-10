#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "SavorDbRuntime.h"

namespace savorqt::db {

inline std::string ResolveProgramKindName(int program_kind, std::string fallback)
{
    auto* registry = savorqt::SavorDbRuntime::instance().programKindRegistry();
    if (registry != nullptr) {
        const auto* descriptor = registry->Find(static_cast<std::int32_t>(program_kind));
        if (descriptor != nullptr && !descriptor->program_name.empty()) {
            return descriptor->program_name;
        }
    }

    return fallback;
}

inline std::string ResolveProgramKindName(
    const std::optional<int>& program_kind,
    std::string none_label = "(none)",
    std::string unknown_prefix = "kind ")
{
    if (!program_kind.has_value()) {
        return none_label;
    }

    return ResolveProgramKindName(
        *program_kind,
        unknown_prefix + std::to_string(*program_kind));
}

} // namespace savorqt::db

#include "BattlePredictorResourceBundle.h"

#include <algorithm>
#include <cctype>

namespace savor::predict {
namespace {

unsigned char lowercase_ascii(unsigned char value) noexcept {
    if (value >= static_cast<unsigned char>('A')
        && value <= static_cast<unsigned char>('Z')) {
        return static_cast<unsigned char>(
            value - static_cast<unsigned char>('A')
            + static_cast<unsigned char>('a'));
    }
    return value;
}

} // namespace

bool AsciiCaseInsensitiveLess::operator()(
    std::string_view lhs,
    std::string_view rhs) const noexcept {
    const auto common = std::min(lhs.size(), rhs.size());
    for (std::size_t index = 0; index < common; ++index) {
        const auto left = lowercase_ascii(
            static_cast<unsigned char>(lhs[index]));
        const auto right = lowercase_ascii(
            static_cast<unsigned char>(rhs[index]));
        if (left != right) {
            return left < right;
        }
    }
    return lhs.size() < rhs.size();
}

const BattlePredictorResourceTemplate*
BattlePredictorResourceBundle::find_resource(
    std::string_view resource_stem) const noexcept {
    const auto found = resource_templates.find(resource_stem);
    return found == resource_templates.end() ? nullptr : &found->second;
}

const char* battle_predictor_resource_input_status_name(
    BattlePredictorResourceInputStatus status) {
    switch (status) {
    case BattlePredictorResourceInputStatus::Ready:
        return "Ready";
    case BattlePredictorResourceInputStatus::MissingInput:
        return "MissingInput";
    case BattlePredictorResourceInputStatus::Ambiguous:
        return "Ambiguous";
    default:
        return "MissingInput";
    }
}

const char* battle_predictor_resource_provider_kind_name(
    BattlePredictorResourceProviderKind kind) {
    switch (kind) {
    case BattlePredictorResourceProviderKind::DirectSpice:
        return "direct_spice";
    case BattlePredictorResourceProviderKind::LegacyStdJsonDirectMld:
        return "legacy_std_json_direct_mld";
    default:
        return "direct_spice";
    }
}

} // namespace savor::predict

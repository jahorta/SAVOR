#pragma once

#include <string>
#include <string_view>

namespace simcore::db::events {

inline bool ValidateEventTypeFormat(
    std::string_view event_type,
    int event_version,
    std::string* error_out = nullptr) {
    if (event_version <= 0) {
        if (error_out) *error_out = "event_version must be > 0";
        return false;
    }

    if (event_type.empty()) {
        if (error_out) *error_out = "event_type is required";
        return false;
    }

    const std::string suffix = ".v" + std::to_string(event_version);
    if (event_type.size() <= suffix.size() ||
        event_type.substr(event_type.size() - suffix.size()) != suffix) {
        if (error_out) *error_out = "event_type must end with .v<event_version>";
        return false;
    }

    const auto prefix = event_type.substr(0, event_type.size() - suffix.size());
    const auto first_dot = prefix.find('.');
    if (first_dot == std::string_view::npos || first_dot == 0 || first_dot + 1 >= prefix.size()) {
        if (error_out) *error_out = "event_type must match <Domain>.<EventName>.v<version>";
        return false;
    }

    if (prefix.find('.', first_dot + 1) != std::string_view::npos) {
        if (error_out) *error_out = "event_type must contain exactly one dot before version suffix";
        return false;
    }

    return true;
}

} // namespace simcore::db::events

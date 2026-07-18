#pragma once

#include <string>
#include <string_view>

namespace savor::predict {

// Converts the profile builder's private text DSL into the only public profile schema.
std::string build_capture_profile_json(std::string_view builder_text);
std::string pin_capture_profile_module_hash(
    std::string_view profile_json,
    std::string_view module_sha256);

} // namespace savor::predict

#pragma once

#include <filesystem>
#include <iosfwd>
#include <string>

namespace savor::predict {

std::string build_first_battle_capture_profile_ini();
std::string build_first_battle_predictor_validation_profile_ini();
int write_first_battle_capture_profile(
    const std::filesystem::path& output_path,
    std::ostream& out,
    std::ostream& err);
int write_first_battle_predictor_validation_profile(
    const std::filesystem::path& output_path,
    std::ostream& out,
    std::ostream& err);

} // namespace savor::predict

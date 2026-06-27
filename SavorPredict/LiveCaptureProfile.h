#pragma once

#include <filesystem>
#include <iosfwd>
#include <string>

namespace savor::predict {

std::string build_first_battle_capture_profile_ini();
std::string build_first_battle_predictor_validation_profile_ini();
std::string build_first_battle_turn_order_validation_profile_ini();
std::string build_first_battle_field6_watch_profile_ini();
std::string build_first_battle_action_view_resource_profile_ini();
std::string build_first_battle_action_view_selector_coverage_profile_ini();
std::string build_first_battle_thread_list_profile_ini();
int write_first_battle_capture_profile(
    const std::filesystem::path& output_path,
    std::ostream& out,
    std::ostream& err);
int write_first_battle_predictor_validation_profile(
    const std::filesystem::path& output_path,
    std::ostream& out,
    std::ostream& err);
int write_first_battle_turn_order_validation_profile(
    const std::filesystem::path& output_path,
    std::ostream& out,
    std::ostream& err);
int write_first_battle_field6_watch_profile(
    const std::filesystem::path& output_path,
    std::ostream& out,
    std::ostream& err);
int write_first_battle_action_view_resource_profile(
    const std::filesystem::path& output_path,
    std::ostream& out,
    std::ostream& err);
int write_first_battle_action_view_selector_coverage_profile(
    const std::filesystem::path& output_path,
    std::ostream& out,
    std::ostream& err);
int write_first_battle_thread_list_profile(
    const std::filesystem::path& output_path,
    std::ostream& out,
    std::ostream& err);

} // namespace savor::predict

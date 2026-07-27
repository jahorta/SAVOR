#pragma once

#include <cstdint>
#include <filesystem>
#include <iosfwd>
#include <string>

namespace savor::predict {

enum class Mode1State6ProgressActivation {
    QueuedState,
    CounterFollowup,
};

std::string build_first_battle_capture_profile_ini();
std::string build_first_battle_predictor_validation_profile_ini();
std::string build_first_battle_turn_order_validation_profile_ini();
std::string build_first_battle_field6_watch_profile_ini();
std::string build_first_battle_view_eligibility_profile_ini();
std::string build_first_battle_view_placement_cache_profile_ini();
std::string build_first_battle_view_placement_frame_thread_profile_ini(
    std::uint32_t thread_list_max_nodes = 128);
std::string build_first_battle_view_placement_semantic_hooks_profile_ini(
    std::uint32_t thread_list_max_nodes = 128);
std::string build_first_battle_predictor_live_comparison_profile_ini(
    std::uint32_t thread_list_max_nodes = 128);
std::string build_first_battle_movement_destination_stop_profile_ini(
    std::uint32_t thread_list_max_nodes = 128);
std::string build_first_battle_action_view_service_lifecycle_profile_ini(
    std::uint32_t thread_list_max_nodes = 128);
std::string build_first_battle_visual_publication_order_profile_ini(
    std::uint32_t thread_list_max_nodes = 128);
std::string build_first_battle_pc_worker_selector_lifetime_profile_ini(
    std::uint32_t thread_list_max_nodes = 128);
std::string build_first_battle_queued_instruction_param_profile_ini();
std::string build_first_battle_mode1_pathing_lifetime_profile_ini(
    std::uint32_t thread_list_max_nodes = 128);
std::string build_first_battle_mode1_state6_progress_profile_ini(
    std::uint32_t thread_list_max_nodes = 128,
    Mode1State6ProgressActivation activation =
        Mode1State6ProgressActivation::QueuedState);
std::string build_first_battle_action_view_pathing_loop_profile_ini();
std::string build_first_battle_thread_pathing_timing_profile_ini(
    std::uint32_t thread_list_max_nodes = 128);
std::string build_battle_thread_producer_profile_ini(
    std::uint32_t thread_list_max_nodes = 128);
std::string build_first_battle_action_view_resource_profile_ini();
std::string build_first_battle_action_view_selector_coverage_profile_ini();
std::string build_first_battle_thread_list_profile_ini();
std::string build_first_battle_pre_handler_frame_pathing_profile_ini();
std::string build_first_battle_float_motion_profile_ini();
std::string build_first_battle_move_increment_read_watch_profile_ini();
std::string build_first_battle_probe_layer_validation_profile_ini();
std::string build_first_battle_action_motion_invocation_profile_ini(
    std::uint32_t thread_list_max_nodes = 128);
std::string build_first_battle_direct_reset_thread_position_profile_ini(
    std::uint32_t thread_list_max_nodes = 128);
std::string build_first_battle_direct_transition_producer_profile_ini(
    std::uint32_t thread_list_max_nodes = 128);
std::string build_first_battle_state17_lifecycle_profile_ini(
    std::uint32_t thread_list_max_nodes = 128);
std::string build_first_battle_direct_transition_input_audit_profile_ini(
    std::uint32_t thread_list_max_nodes = 128);
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
int write_first_battle_view_eligibility_profile(
    const std::filesystem::path& output_path,
    std::ostream& out,
    std::ostream& err);
int write_first_battle_view_placement_cache_profile(
    const std::filesystem::path& output_path,
    std::ostream& out,
    std::ostream& err);
int write_first_battle_view_placement_frame_thread_profile(
    const std::filesystem::path& output_path,
    std::ostream& out,
    std::ostream& err,
    std::uint32_t thread_list_max_nodes = 128);
int write_first_battle_view_placement_semantic_hooks_profile(
    const std::filesystem::path& output_path,
    std::ostream& out,
    std::ostream& err,
    std::uint32_t thread_list_max_nodes = 128);
int write_first_battle_predictor_live_comparison_profile(
    const std::filesystem::path& output_path,
    std::ostream& out,
    std::ostream& err,
    std::uint32_t thread_list_max_nodes = 128);
int write_first_battle_movement_destination_stop_profile(
    const std::filesystem::path& output_path,
    std::ostream& out,
    std::ostream& err,
    std::uint32_t thread_list_max_nodes = 128);
int write_first_battle_action_view_service_lifecycle_profile(
    const std::filesystem::path& output_path,
    std::ostream& out,
    std::ostream& err,
    std::uint32_t thread_list_max_nodes = 128);
int write_first_battle_visual_publication_order_profile(
    const std::filesystem::path& output_path,
    std::ostream& out,
    std::ostream& err,
    std::uint32_t thread_list_max_nodes = 128);
int write_first_battle_pc_worker_selector_lifetime_profile(
    const std::filesystem::path& output_path,
    std::ostream& out,
    std::ostream& err,
    std::uint32_t thread_list_max_nodes = 128);
int write_first_battle_queued_instruction_param_profile(
    const std::filesystem::path& output_path,
    std::ostream& out,
    std::ostream& err);
int write_first_battle_mode1_pathing_lifetime_profile(
    const std::filesystem::path& output_path,
    std::ostream& out,
    std::ostream& err,
    std::uint32_t thread_list_max_nodes = 128);
int write_first_battle_mode1_state6_progress_profile(
    const std::filesystem::path& output_path,
    std::ostream& out,
    std::ostream& err,
    std::uint32_t thread_list_max_nodes = 128,
    Mode1State6ProgressActivation activation =
        Mode1State6ProgressActivation::QueuedState);
int write_first_battle_action_view_pathing_loop_profile(
    const std::filesystem::path& output_path,
    std::ostream& out,
    std::ostream& err);
int write_first_battle_thread_pathing_timing_profile(
    const std::filesystem::path& output_path,
    std::ostream& out,
    std::ostream& err,
    std::uint32_t thread_list_max_nodes = 128);
int write_battle_thread_producer_profile(
    const std::filesystem::path& output_path,
    std::ostream& out,
    std::ostream& err,
    std::uint32_t thread_list_max_nodes = 128);
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
int write_first_battle_pre_handler_frame_pathing_profile(
    const std::filesystem::path& output_path,
    std::ostream& out,
    std::ostream& err);
int write_first_battle_float_motion_profile(
    const std::filesystem::path& output_path,
    std::ostream& out,
    std::ostream& err);
int write_first_battle_move_increment_read_watch_profile(
    const std::filesystem::path& output_path,
    std::ostream& out,
    std::ostream& err);
int write_first_battle_probe_layer_validation_profile(
    const std::filesystem::path& output_path,
    std::ostream& out,
    std::ostream& err);
int write_first_battle_action_motion_invocation_profile(
    const std::filesystem::path& output_path,
    std::ostream& out,
    std::ostream& err,
    std::uint32_t thread_list_max_nodes = 128);
int write_first_battle_direct_reset_thread_position_profile(
    const std::filesystem::path& output_path,
    std::ostream& out,
    std::ostream& err,
    std::uint32_t thread_list_max_nodes = 128);
int write_first_battle_direct_transition_producer_profile(
    const std::filesystem::path& output_path,
    std::ostream& out,
    std::ostream& err,
    std::uint32_t thread_list_max_nodes = 128);
int write_first_battle_state17_lifecycle_profile(
    const std::filesystem::path& output_path,
    std::ostream& out,
    std::ostream& err,
    std::uint32_t thread_list_max_nodes = 128);
int write_first_battle_direct_transition_input_audit_profile(
    const std::filesystem::path& output_path,
    std::ostream& out,
    std::ostream& err,
    std::uint32_t thread_list_max_nodes = 128);

} // namespace savor::predict

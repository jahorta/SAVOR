#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "../Common/Types/UtcTimestamp.h"

namespace savor::db {

struct UiProjectionSubscription {
    std::string projector_name;
    std::string source_context;
    std::string source_outbox_table;
    std::int64_t last_outbox_id = 0;
    std::string last_event_id;
    types::UtcTimePoint updated_at_utc{};
    std::string status = "ACTIVE";
    std::string last_error;
};

struct UiProjectionSubscriptionBatchAudit {
    std::string projector_name;
    std::string source_context;
    std::string source_outbox_table;
    std::int64_t from_outbox_id = 0;
    std::int64_t to_outbox_id = 0;
    std::int64_t processed_count = 0;
    std::int64_t failed_count = 0;
    types::UtcTimePoint recorded_at_utc{};
};

struct UiReadSeedProbeRunListCursor {
    std::int64_t requested_at_utc = 0;
    std::int64_t probe_run_id = 0;
};

struct UiReadSeedProbeRunListQuery {
    std::optional<UiReadSeedProbeRunListCursor> before;
    std::optional<UiReadSeedProbeRunListCursor> after;
    int limit = 50;
    std::string search;
    bool only_completed = false;
};

struct UiSeedProbeRunSummary {
    std::int64_t probe_run_id = 0;
    std::int64_t probe_set_id = 0;
    std::int64_t entry_savestate_id = 0;
    std::int64_t seed_probe_spec_id = 0;
    int codec_version = 0;
    std::string status;
    std::optional<std::uint32_t> neutral_seed_value;
    int grid_count = 0;
    int unique_count = 0;
    std::int64_t requested_at_utc = 0;
    std::optional<std::int64_t> completed_at_utc;
};

struct UiSeedProbeRunPage {
    std::vector<UiSeedProbeRunSummary> items;
    std::optional<UiReadSeedProbeRunListCursor> next;
    std::optional<UiReadSeedProbeRunListCursor> prev;
};

struct UiSeedProbeDeltaPoint {
    std::int64_t delta_point_id = 0;
    std::int64_t probe_run_id = 0;
    std::string source_family;
    int axis_x = 0;
    int axis_y = 0;
    std::uint32_t seed_value = 0;
    std::int32_t seed_delta = 0;
};

struct UiSeedProbeUniqueValue {
    std::int64_t unique_value_id = 0;
    std::int64_t probe_run_id = 0;
    std::uint32_t seed_value = 0;
    std::int32_t seed_delta = 0;
    int main_x = 0;
    int main_y = 0;
    int cstick_x = 0;
    int cstick_y = 0;
    int trigger_x = 0;
    int trigger_y = 0;
};

struct UiReadListCursor {
    std::int64_t primary = 0;
    std::int64_t secondary = 0;
};

template <typename T>
struct UiReadPage {
    std::vector<T> items;
    std::optional<UiReadListCursor> next;
    std::optional<UiReadListCursor> prev;
};

struct UiReadJobListQuery {
    std::optional<UiReadListCursor> before;
    std::optional<UiReadListCursor> after;
    int limit = 50;
    std::vector<std::string> states;
    std::optional<int> program_kind;
    std::optional<std::int64_t> job_set_id;
};

struct UiProgramKind {
    int id = 0;
    std::string name;
};

struct UiJobSummary {
    std::int64_t job_id = 0;
    std::int64_t job_set_id = 0;
    int program_kind = 0;
    std::string state;
    int priority = 0;
    int attempts = 0;
    int max_attempts = 0;
    std::int64_t queued_at_utc = 0;
    std::optional<std::int64_t> started_at_utc;
    std::optional<std::int64_t> ended_at_utc;
    std::string error_code;
    std::string error_text;
    std::string result_processing_state;
    int result_processing_attempts = 0;
    int result_processing_failures = 0;
    std::string result_processing_error_code;
    std::string result_processing_error_text;
    std::optional<std::uint64_t> last_progress_attempt_id;
    std::optional<std::uint64_t> last_progress_ordinal;
    std::string last_progress_text;
    std::optional<std::int64_t> last_progress_at_utc;
};

struct UiCanonicalJobProgress {
    std::int64_t job_id = 0;
    std::uint64_t attempt_id = 0;
    std::uint64_t ordinal = 0;
    std::uint64_t workset_id = 0;
    std::uint64_t item_id = 0;
    std::uint64_t invocation_id = 0;
    std::string library_id;
    std::uint32_t library_revision = 0;
    std::string progress_point_id;
    std::optional<std::uint64_t> routed_sequence;
    std::optional<std::uint64_t> sample_snapshot_id;
    std::optional<std::uint64_t> trigger_epoch;
    std::string schema_id;
    std::uint32_t schema_revision = 0;
    std::string schema_sha256;
    std::vector<std::uint8_t> typed_payload;
    std::string display_text;
    std::int64_t recorded_at_utc = 0;
};

struct UiJobStateCounts {
    std::int64_t total = 0;
    std::int64_t queued = 0;
    std::int64_t claimed = 0;
    std::int64_t running = 0;
    std::int64_t execution_finished = 0;
    std::int64_t failed = 0;
    std::int64_t succeeded = 0;
    std::int64_t canceled = 0;
    std::int64_t superseded = 0;
    std::int64_t other = 0;
};

struct UiJobDetail {
    UiJobSummary summary;
    std::string fingerprint;
    std::optional<std::string> claimed_by_token;
    std::optional<std::int64_t> lease_expires_at_utc;
};

struct UiJobArtifact {
    std::int64_t artifact_id = 0;
    std::string role_kind;
    std::string filename;
    std::uint64_t size_bytes = 0;
    std::string artifact_kind;
    std::int64_t created_at_utc = 0;
};

struct UiJobSetSummary {
    std::int64_t job_set_id = 0;
    int program_kind = 0;
    std::int64_t created_at_utc = 0;
    std::int64_t total_jobs = 0;
    std::int64_t completed_jobs = 0;
    std::int64_t succeeded_jobs = 0;
    std::int64_t failed_jobs = 0;
    std::int64_t canceled_jobs = 0;
};

struct UiJobSetDetail {
    UiJobSetSummary summary;
    std::vector<UiJobSummary> jobs;
    bool hierarchy_projection_available = false;
};

struct UiReadJobSetListQuery {
    std::optional<UiReadListCursor> before;
    std::optional<UiReadListCursor> after;
    int limit = 50;
    std::optional<int> program_kind;
};

struct UiArtifactSummary {
    std::int64_t artifact_id = 0;
    std::string sha256;
    std::uint64_t size_bytes = 0;
    std::string artifact_kind;
    std::string filename;
    std::int64_t created_at_utc = 0;
};

struct UiSavestateSummary {
    std::int64_t savestate_id = 0;
    std::int64_t artifact_id = 0;
    std::string savestate_type;
    std::string note;
    bool is_complete = false;
    std::string playback_state;
    std::optional<std::int64_t> dtm_artifact_id;
    std::string sha256;
    std::uint64_t size_bytes = 0;
    std::string filename;
    std::int64_t created_at_utc = 0;
};

struct UiTasMovieRootSummary {
    std::int64_t tas_movie_root_id = 0;
    std::int64_t source_dtm_artifact_id = 0;
    std::int64_t dtm_artifact_id = 0;
    std::uint32_t rtc_value = 0;
    std::int64_t itinerary_artifact_id = 0;
    std::uint32_t required_final_breakpoint_pc = 0;
    std::int64_t checkpoint_savestate_id = 0;
    std::string source_context_kind;
    std::int64_t source_context_id = 0;
    std::int64_t created_at_utc = 0;
};

struct UiTasMovieTreeSummary {
    std::int64_t tas_movie_tree_id = 0;
    std::int64_t tas_movie_root_id = 0;
    std::optional<std::int64_t> parent_tas_movie_tree_id;
    std::int64_t dtm_artifact_id = 0;
    std::int64_t itinerary_artifact_id = 0;
    std::uint32_t required_final_breakpoint_pc = 0;
    std::int64_t checkpoint_savestate_id = 0;
    std::string source_context_kind;
    std::int64_t source_context_id = 0;
    std::int64_t created_at_utc = 0;
};

struct UiTasMovieValidationAttemptSummary {
    std::int64_t validation_attempt_id = 0;
    std::int64_t validation_request_id = 0;
    std::int64_t source_job_id = 0;
    std::string outcome;
    std::string failure_reason;
    std::optional<std::uint32_t> expected_pc;
    std::optional<std::uint64_t> expected_input_count;
    std::uint32_t actual_pc = 0;
    std::uint64_t actual_input_count = 0;
    std::optional<std::int64_t> last_known_good_savestate_id;
    std::optional<std::int64_t> produced_tas_movie_root_id;
    std::string worker_id;
    std::int64_t recorded_at_utc = 0;
};

struct UiTasMovieValidationRequestSummary {
    std::int64_t validation_request_id = 0;
    std::int64_t workflow_instance_id = 0;
    std::int64_t workflow_step_id = 0;
    std::string step_kind;
    std::string operation;
    std::string source_kind;
    std::int64_t source_ref_id = 0;
    std::int64_t source_dtm_artifact_id = 0;
    std::string source_dtm_sha256;
    std::optional<std::uint32_t> rtc_value;
    std::string effective_dtm_sha256;
    std::optional<std::int64_t> itinerary_artifact_id;
    std::string itinerary_sha256;
    std::uint32_t required_final_breakpoint_pc = 0;
    std::optional<std::int64_t> latest_validation_attempt_id;
    std::string latest_outcome;
    std::string latest_failure_reason;
    std::optional<std::uint32_t> latest_actual_pc;
    std::optional<std::uint64_t> latest_actual_input_count;
    std::optional<std::int64_t> produced_tas_movie_root_id;
    std::int64_t created_at_utc = 0;
};

struct UiTasMovieSterilizationAttemptSummary {
    std::int64_t sterilization_attempt_id = 0;
    std::int64_t sterilization_request_id = 0;
    std::int64_t source_job_id = 0;
    std::string candidate_savestate_sha256;
    std::int64_t produced_savestate_id = 0;
    std::string worker_id;
    std::int64_t recorded_at_utc = 0;
};

struct UiTasMovieSterilizationRequestSummary {
    std::int64_t sterilization_request_id = 0;
    std::int64_t workflow_instance_id = 0;
    std::int64_t workflow_step_id = 0;
    std::int64_t source_savestate_id = 0;
    std::int64_t source_savestate_artifact_id = 0;
    std::string source_savestate_sha256;
    std::int64_t source_dtm_artifact_id = 0;
    std::string source_dtm_sha256;
    std::optional<std::int64_t> reused_savestate_id;
    std::optional<std::int64_t> latest_sterilization_attempt_id;
    std::optional<std::int64_t> latest_produced_savestate_id;
    std::string latest_candidate_savestate_sha256;
    std::int64_t created_at_utc = 0;
};

struct UiBattleContextSummary {
    std::int64_t context_probe_id = 0;
    std::optional<std::int64_t> wave_id;
    std::int64_t source_savestate_id = 0;
    std::optional<std::int64_t> exec_job_id;
    std::string probe_status;
    std::optional<int> context_version;
    std::optional<std::int64_t> context_artifact_id;
    std::optional<std::uint32_t> entry_pc;
    std::optional<std::int64_t> recorded_at_utc;
    std::int64_t created_at_utc = 0;
};

struct UiReadArtifactListQuery {
    std::optional<UiReadListCursor> before;
    std::optional<UiReadListCursor> after;
    int limit = 50;
    std::string search;
    std::string extension;
};

struct UiArchiveCatalogListQuery {
    std::string search;
    std::string source_scope_kind;
    std::string checksum_status;
    bool workflow_packages_only = true;
    std::optional<std::int64_t> created_from_utc;
    std::optional<std::int64_t> created_to_utc;
};

struct UiArchiveCatalogRow {
    std::int64_t archive_package_id = 0;
    std::string source_context;
    std::int64_t source_root_job_set_id = 0;
    std::string source_scope_kind;
    std::int64_t source_workflow_count = 0;
    std::string selection_summary;
    std::string archive_name;
    std::string archive_notes;
    std::int64_t created_at_utc = 0;
    int schema_version = 0;
    int event_catalog_version = 0;
    std::int64_t time_range_start_utc = 0;
    std::int64_t time_range_end_utc = 0;
    std::string checksum_status;
};

struct UiArchiveRehydrateRequestRow {
    std::int64_t rehydrate_request_id = 0;
    std::int64_t archive_package_id = 0;
    std::string status;
    std::string target_namespace;
    std::int64_t requested_at_utc = 0;
    std::optional<std::int64_t> completed_at_utc;
    std::string error_text;
};

struct UiWorkflowInstanceListQuery {
    std::optional<UiReadListCursor> before;
    std::optional<UiReadListCursor> after;
    int limit = 50;
    std::string state;
    std::string display_state;
    std::string workflow_kind;
    bool battle_final_victory_only = false;
    bool battle_final_victory_absent_only = false;
};

struct UiWorkflowInstanceSummary {
    std::int64_t workflow_instance_id = 0;
    std::string workflow_kind;
    std::string state;
    std::string display_state;
    std::string root_scope_kind;
    std::optional<std::int64_t> root_scope_id;
    std::string created_by;
    std::int64_t blocked_step_count = 0;
    std::int64_t failed_step_count = 0;
    std::int64_t created_at_utc = 0;
    std::optional<std::int64_t> started_at_utc;
    std::optional<std::int64_t> completed_at_utc;
    std::string failure_code;
    std::string failure_text;
    int battle_advancement_rank = 0;
    std::int64_t battle_desired_outcome_count = 0;
    std::int64_t battle_final_victory_count = 0;
    std::int64_t battle_selected_count = 0;
};

struct UiWorkflowDisplayStateCounts {
    std::int64_t total = 0;
    std::int64_t running = 0;
    std::int64_t queued = 0;
    std::int64_t waiting = 0;
    std::int64_t completed = 0;
    std::int64_t failed = 0;
    std::int64_t canceled = 0;
    std::int64_t terminal = 0;
    std::int64_t other = 0;
};

struct UiWorkflowStepSummary {
    std::int64_t workflow_step_id = 0;
    std::int64_t workflow_instance_id = 0;
    std::optional<std::int64_t> workflow_unit_activation_id;
    std::string step_key;
    std::string step_kind;
    std::string state;
    std::string blocked_reason;
    std::optional<std::int64_t> job_set_id;
    std::int64_t job_count = 0;
    std::int64_t job_completed_count = 0;
    std::int64_t job_failed_count = 0;
    int priority = 0;
    int attempts = 0;
    int max_attempts = 1;
    std::optional<std::int64_t> ready_at_utc;
    std::optional<std::int64_t> started_at_utc;
    std::optional<std::int64_t> completed_at_utc;
    std::optional<std::int64_t> failed_at_utc;
    std::int64_t created_at_utc = 0;
    int battle_advancement_rank = 0;
    std::int64_t battle_desired_outcome_count = 0;
    std::int64_t battle_final_victory_count = 0;
    std::int64_t battle_selected_count = 0;
};

struct UiWorkflowUnitActivationSummary {
    std::int64_t workflow_unit_activation_id = 0;
    std::int64_t workflow_instance_id = 0;
    std::optional<std::int64_t> parent_workflow_unit_activation_id;
    std::string activation_key;
    std::string graph_node_key;
    std::string unit_kind;
    std::string display_name;
    std::string state;
    std::string activation_params_json;
    std::string authored_ref_kind;
    std::optional<std::int64_t> authored_ref_id;
    std::string failure_code;
    std::string failure_text;
    std::int64_t created_at_utc = 0;
    std::optional<std::int64_t> ready_at_utc;
    std::optional<std::int64_t> started_at_utc;
    std::optional<std::int64_t> completed_at_utc;
    std::optional<std::int64_t> failed_at_utc;
};

struct UiWorkflowEdgeSummary {
    std::int64_t workflow_edge_id = 0;
    std::int64_t workflow_instance_id = 0;
    std::int64_t from_step_id = 0;
    std::int64_t to_step_id = 0;
    std::string condition_kind;
    std::string condition_value;
    std::int64_t created_at_utc = 0;
};

struct UiWorkflowUnitActivationEdgeSummary {
    std::int64_t workflow_unit_activation_edge_id = 0;
    std::int64_t workflow_instance_id = 0;
    std::int64_t from_workflow_unit_activation_id = 0;
    std::int64_t to_workflow_unit_activation_id = 0;
    std::string output_key;
    std::string input_key;
    std::string condition_kind;
    std::string condition_value;
    std::int64_t created_at_utc = 0;
};

struct UiWorkflowAlertSummary {
    std::int64_t workflow_alert_id = 0;
    std::int64_t workflow_instance_id = 0;
    std::optional<std::int64_t> workflow_step_id;
    std::string alert_kind;
    std::string alert_code;
    std::string message;
    bool is_active = false;
    std::int64_t first_seen_at_utc = 0;
    std::int64_t last_seen_at_utc = 0;
    std::optional<std::int64_t> cleared_at_utc;
};

struct UiWorkflowDetail {
    UiWorkflowInstanceSummary instance;
    std::vector<UiWorkflowUnitActivationSummary> unit_activations;
    std::vector<UiWorkflowUnitActivationEdgeSummary> unit_activation_edges;
    std::vector<UiWorkflowStepSummary> steps;
    std::vector<UiWorkflowEdgeSummary> edges;
    std::vector<UiWorkflowAlertSummary> alerts;
};

struct UiBattleGroupListQuery {
    std::optional<UiReadListCursor> before;
    std::optional<UiReadListCursor> after;
    int limit = 50;
    bool child_selected_only = false;
    bool final_victory_only = false;
};

struct UiBattleGroupSummary {
    std::int64_t battle_set_id = 0;
    std::string name;
    std::string status;
    std::int64_t created_at_utc = 0;
    std::optional<std::int64_t> completed_at_utc;
    std::int64_t wave_count = 0;
    std::int64_t job_count = 0;
    std::int64_t selected_count = 0;
    std::int64_t desired_outcome_count = 0;
    std::int64_t final_victory_count = 0;
    std::int64_t failed_count = 0;
    std::int64_t manual_followup_count = 0;
    int advancement_rank = 0;
};

struct UiBattleWaveSummary {
    std::int64_t wave_id = 0;
    std::int64_t battle_set_id = 0;
    std::optional<std::int64_t> parent_wave_id;
    std::optional<std::int64_t> parent_turn_job_id;
    int turn_index = 0;
    std::string status;
    std::int64_t created_at_utc = 0;
    std::optional<std::int64_t> completed_at_utc;
    std::int64_t job_count = 0;
    std::int64_t selected_count = 0;
    std::int64_t desired_outcome_count = 0;
    std::int64_t final_victory_count = 0;
    std::int64_t failed_count = 0;
    int advancement_rank = 0;
};

struct UiBattleAdvancementDecisionSummary {
    std::int64_t battle_advancement_decision_id = 0;
    std::int64_t battle_advancement_pool_id = 0;
    std::int64_t turn_job_id = 0;
    std::string decision_kind;
    std::string decision_reason;
    std::int64_t created_at_utc = 0;
};

struct UiBattleManualFollowupSummary {
    std::int64_t turn_job_id = 0;
    std::string manual_followup_status;
    std::optional<std::int64_t> recorded_dtm_artifact_id;
    std::string note;
    std::int64_t updated_at_utc = 0;
};

struct UiBattleTurnJobSummary {
    std::int64_t turn_job_id = 0;
    std::optional<std::int64_t> exec_job_id;
    std::int64_t wave_id = 0;
    std::int64_t battle_set_id = 0;
    int turn_index = 0;
    std::string job_state;
    int fake_attacks_this_turn = 0;
    int fake_attacks_used_before = 0;
    std::optional<std::int64_t> rng_seed;
    std::optional<std::int64_t> delta_vi;
    std::optional<int> pred_passed;
    std::optional<int> pred_total;
    std::optional<int> battle_outcome;
    bool has_desired_outcome = false;
    bool has_final_victory_outcome = false;
    bool selected_for_advancement = false;
    std::string advancement_decision_kind;
    int advancement_rank = 0;
    std::optional<std::int64_t> started_at_utc;
    std::optional<std::int64_t> ended_at_utc;
    std::optional<UiBattleAdvancementDecisionSummary> advancement_decision;
    std::optional<UiBattleManualFollowupSummary> manual_followup;
};

struct UiBattleTurnJobDetail {
    UiBattleTurnJobSummary summary;
    std::optional<UiBattleGroupSummary> group;
    std::optional<UiBattleWaveSummary> wave;
    std::vector<UiJobArtifact> artifacts;
};

struct UiBattleTurnJobReplicationRow {
    std::int64_t turn_job_id = 0;
    std::int64_t battle_set_id = 0;
    std::int64_t wave_id = 0;
    std::optional<std::int64_t> parent_wave_id;
    std::optional<std::int64_t> parent_turn_job_id;
    std::optional<std::int64_t> exec_job_id;
    std::optional<std::int64_t> source_savestate_id;
    std::optional<std::int64_t> seed_candidate_id;
    std::optional<std::int64_t> authored_plan_id;
    std::optional<int> authored_turn_index;
    std::optional<std::string> resolved_turn_commands_blob;
    std::optional<std::string> resolved_turn_variant_key;
    int fake_attacks_used_before = 0;
    int fake_attacks_this_turn = 0;
    std::optional<std::int64_t> output_savestate_id;
    std::optional<std::int64_t> input_trace_artifact_id;
};

struct IUiReadDb {
    virtual ~IUiReadDb() = default;

    virtual std::vector<UiProgramKind> ListProgramKinds() const = 0;

    virtual UiReadPage<UiJobSummary> ListJobs(
        const UiReadJobListQuery& query) const = 0;

    virtual UiJobStateCounts CountJobsByState(
        const UiReadJobListQuery& query) const = 0;

    virtual std::optional<UiJobSummary> GetJobSummary(
        std::int64_t job_id) const = 0;

    virtual std::optional<UiJobDetail> GetJobDetail(
        std::int64_t job_id) const = 0;

    virtual std::vector<UiJobArtifact> ListJobArtifacts(
        std::int64_t job_id) const = 0;

    virtual std::vector<UiCanonicalJobProgress> ListJobProgress(
        std::int64_t job_id,
        int limit = 128) const = 0;

    virtual UiReadPage<UiJobSetSummary> ListJobSets(
        const UiReadJobSetListQuery& query) const = 0;

    virtual std::optional<UiJobSetDetail> GetJobSetDetail(
        std::int64_t job_set_id,
        int jobs_limit) const = 0;

    virtual UiReadPage<UiArtifactSummary> ListArtifacts(
        const UiReadArtifactListQuery& query) const = 0;

    virtual std::vector<UiSavestateSummary> ListSavestates(
        std::string_view playback_state, bool complete_only, std::string_view search, int limit) const = 0;
    virtual std::vector<UiTasMovieRootSummary> ListTasMovieRoots(int limit) const = 0;
    virtual std::vector<UiTasMovieTreeSummary> ListTasMovieTrees(int limit) const = 0;
    virtual std::vector<UiTasMovieValidationRequestSummary> ListTasMovieValidationRequests(int limit) const = 0;
    virtual std::vector<UiTasMovieValidationAttemptSummary> ListTasMovieValidationAttempts(
        std::optional<std::int64_t> request_id, int limit) const = 0;
    virtual std::vector<UiTasMovieSterilizationRequestSummary> ListTasMovieSterilizationRequests(int limit) const = 0;
    virtual std::vector<UiTasMovieSterilizationAttemptSummary> ListTasMovieSterilizationAttempts(
        std::optional<std::int64_t> request_id, int limit) const = 0;
    virtual std::vector<UiBattleContextSummary> ListBattleContexts(bool complete_only, int limit) const = 0;

    virtual std::vector<UiArchiveCatalogRow> ListArchiveCatalog(
        const UiArchiveCatalogListQuery& query) const = 0;

    virtual std::vector<UiArchiveRehydrateRequestRow> ListArchiveRehydrateRequests(
        std::int64_t archive_package_id) const = 0;

    virtual bool UpsertArtifactSummary(
        const UiArtifactSummary& summary,
        std::string* error_out = nullptr) = 0;

    virtual UiReadPage<UiWorkflowInstanceSummary> ListWorkflowInstances(
        const UiWorkflowInstanceListQuery& query) const = 0;

    virtual UiWorkflowDisplayStateCounts CountWorkflowDisplayStates() const = 0;

    virtual std::optional<UiWorkflowDetail> GetWorkflowDetail(
        std::int64_t workflow_instance_id) const = 0;

    virtual UiReadPage<UiBattleGroupSummary> ListBattleGroups(
        const UiBattleGroupListQuery& query) const = 0;

    virtual std::vector<UiBattleWaveSummary> ListBattleWaves(
        std::int64_t battle_set_id) const = 0;

    virtual std::vector<UiBattleTurnJobSummary> ListBattleTurnJobsForWaves(
        const std::vector<std::int64_t>& wave_ids,
        bool final_victory_only = false) const = 0;

    virtual std::optional<UiBattleTurnJobDetail> GetBattleTurnJobDetail(
        std::int64_t turn_job_id) const = 0;

    virtual std::optional<UiBattleTurnJobReplicationRow> GetBattleTurnJobReplication(
        std::int64_t turn_job_id) const = 0;

    virtual std::vector<UiBattleTurnJobReplicationRow> ListBattleTurnJobReplicationChain(
        std::int64_t turn_job_id) const = 0;

    virtual UiSeedProbeRunPage ListSeedProbeRuns(
        const UiReadSeedProbeRunListQuery& query) const = 0;

    virtual std::optional<UiSeedProbeRunSummary> GetSeedProbeRunSummary(
        std::int64_t probe_run_id) const = 0;

    virtual std::vector<UiSeedProbeDeltaPoint> ListSeedProbeDeltaPoints(
        std::int64_t probe_run_id) const = 0;

    virtual std::vector<UiSeedProbeUniqueValue> ListSeedProbeUniqueValues(
        std::int64_t probe_run_id) const = 0;

    virtual bool UpsertSeedProbeRunSummary(
        const UiSeedProbeRunSummary& summary,
        std::string* error_out = nullptr) = 0;

    virtual bool ReplaceSeedProbeDeltaPoints(
        std::int64_t probe_run_id,
        const std::vector<UiSeedProbeDeltaPoint>& points,
        std::string* error_out = nullptr) = 0;

    virtual bool ReplaceSeedProbeUniqueValues(
        std::int64_t probe_run_id,
        const std::vector<UiSeedProbeUniqueValue>& values,
        std::string* error_out = nullptr) = 0;

    // Gets a subscription row by exact composite key.
    virtual std::optional<UiProjectionSubscription> GetProjectionSubscription(
        const std::string& projector_name,
        const std::string& source_context,
        const std::string& source_outbox_table) const = 0;

    // Lists subscriptions for a source stream across all projectors.
    virtual std::vector<UiProjectionSubscription> ListProjectionSubscriptions(
        const std::string& source_context,
        const std::string& source_outbox_table) const = 0;

    // Returns MIN(last_outbox_id) for ACTIVE subscriptions on a source stream.
    virtual std::optional<std::int64_t> ComputeSafeFloorOutboxId(
        const std::string& source_context,
        const std::string& source_outbox_table) const = 0;

    // Creates a subscription row if one does not already exist and returns the row.
    virtual std::optional<UiProjectionSubscription> GetOrCreateProjectionSubscription(
        const UiProjectionSubscription& subscription) = 0;

    // Advances the subscription cursor and moves status to ACTIVE while clearing any error.
    virtual bool AdvanceProjectionSubscriptionCursor(
        const std::string& projector_name,
        const std::string& source_context,
        const std::string& source_outbox_table,
        std::int64_t last_outbox_id,
        const std::string& last_event_id,
        types::UtcTimePoint updated_at_utc,
        const std::optional<UiProjectionSubscriptionBatchAudit>& batch_audit) = 0;

    // Sets a subscription into ERROR state with a reason.
    virtual bool SetProjectionSubscriptionError(
        const std::string& projector_name,
        const std::string& source_context,
        const std::string& source_outbox_table,
        const std::string& last_error,
        types::UtcTimePoint updated_at_utc) = 0;

    // Pauses a subscription for operations.
    virtual bool PauseProjectionSubscription(
        const std::string& projector_name,
        const std::string& source_context,
        const std::string& source_outbox_table,
        types::UtcTimePoint updated_at_utc,
        const std::string& reason) = 0;

    // Resumes a paused or errored subscription.
    virtual bool ResumeProjectionSubscription(
        const std::string& projector_name,
        const std::string& source_context,
        const std::string& source_outbox_table,
        types::UtcTimePoint updated_at_utc) = 0;
};

} // namespace savor::db

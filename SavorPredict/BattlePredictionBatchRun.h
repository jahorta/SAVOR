#pragma once

#include "BattlePredictionDbInput.h"

#include <cstdint>
#include <filesystem>
#include <iosfwd>
#include <optional>
#include <string>
#include <vector>

namespace savor::predict {

enum class BattlePredictionBatchInvocationStatus {
    NotRun,
    Ok,
    Error,
};

enum class BattlePredictionBatchPredictionStatus {
    NotRun,
    Complete,
    Partial,
};

// The CLI owns default resolution and job-list parsing. The core receives the
// explicit source exec-job IDs and exact run root that should be represented by
// the durable artifacts.
struct BattlePredictionBatchRunOptions {
    std::vector<long long> source_exec_job_ids;
    std::filesystem::path db_root;
    std::string profile_name;
    std::optional<std::string> scenario_name;
    std::optional<BattleSourceSelection> source_selection;
    std::optional<BattleEncounterIdentity> expected_encounter;
    std::filesystem::path action_view_std_json_dir;
    std::filesystem::path run_root;
    std::string run_name;

    // This remains false unless the caller explicitly opts into degraded seed
    // provenance. There is no implicit fallback in batch preflight.
    bool allow_seed_candidate_fallback = false;
    bool allow_profile_overrides = false;
    bool emit_causal_diagnostics = false;
    bool require_complete = false;
    bool preflight_only = false;

    // Reproducibility fields supplied by the front end when available.
    std::string git_commit;
    std::filesystem::path executable_path;
};

struct BattlePredictionBatchJobSummary {
    long long source_exec_job_id = 0;
    BattlePredictionBatchInvocationStatus invocation_status =
        BattlePredictionBatchInvocationStatus::NotRun;
    BattlePredictionBatchPredictionStatus prediction_status =
        BattlePredictionBatchPredictionStatus::NotRun;
    std::optional<BattlePredictionDbInputMetadata> input_metadata;
    std::optional<BattlePredictionOutcome> prediction_outcome;
    std::optional<std::uint32_t> final_rng_seed;
    std::optional<int> total_draws_consumed;
    std::optional<int> single_job_exit_code;
    bool prediction_json_valid = false;
    std::filesystem::path prediction_path;
    std::filesystem::path stderr_path;
    std::vector<std::string> partial_reasons;
    std::vector<std::string> diagnostics;
};

struct BattlePredictionBatchDatabaseIdentity {
    std::uintmax_t analysis_db_size = 0;
    std::uintmax_t state_db_size = 0;
    std::string analysis_db_sha256;
    std::string state_db_sha256;
    std::string used_database_fingerprint;
    bool snapshot_manifest_present = false;
    bool snapshot_manifest_verified = false;
    std::string snapshot_manifest_sha256;
    std::string snapshot_database_fingerprint;
};

struct BattlePredictionBatchRunResult {
    BattlePredictionBatchRunOptions options;
    std::filesystem::path request_path;
    std::filesystem::path manifest_path;
    std::filesystem::path summary_csv_path;
    std::filesystem::path summary_text_path;
    std::vector<BattlePredictionBatchJobSummary> jobs;
    BattlePredictionBatchDatabaseIdentity database_identity;
    std::string started_at_utc;
    std::string completed_at_utc;
    bool preflight_succeeded = false;
    bool artifacts_valid = false;
    bool success = false;
    int recommended_exit_code = 1;
    std::vector<std::string> errors;
};

const char* battle_prediction_batch_invocation_status_name(
    BattlePredictionBatchInvocationStatus status);

const char* battle_prediction_batch_prediction_status_name(
    BattlePredictionBatchPredictionStatus status);

std::vector<std::string> validate_battle_prediction_batch_run_options(
    const BattlePredictionBatchRunOptions& options);

BattlePredictionBatchRunResult run_battle_prediction_batch(
    const BattlePredictionBatchRunOptions& options,
    std::ostream& progress,
    std::ostream& err);

} // namespace savor::predict

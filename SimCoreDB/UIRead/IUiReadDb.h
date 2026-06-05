#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "../Common/Types/UtcTimestamp.h"

namespace simcore::db {

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
    std::optional<std::int64_t> neutral_seed_value;
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
    std::int64_t seed_value = 0;
    std::int64_t seed_delta = 0;
};

struct UiSeedProbeUniqueValue {
    std::int64_t unique_value_id = 0;
    std::int64_t probe_run_id = 0;
    std::int64_t seed_value = 0;
    std::int64_t seed_delta = 0;
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

struct UiReadArtifactListQuery {
    std::optional<UiReadListCursor> before;
    std::optional<UiReadListCursor> after;
    int limit = 50;
    std::string search;
    std::string extension;
};

struct UiWorkflowInstanceListQuery {
    std::optional<UiReadListCursor> before;
    std::optional<UiReadListCursor> after;
    int limit = 50;
    std::string state;
    std::string workflow_kind;
};

struct UiWorkflowInstanceSummary {
    std::int64_t workflow_instance_id = 0;
    std::string workflow_kind;
    std::string state;
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
};

struct UiWorkflowStepSummary {
    std::int64_t workflow_step_id = 0;
    std::int64_t workflow_instance_id = 0;
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
    std::vector<UiWorkflowStepSummary> steps;
    std::vector<UiWorkflowEdgeSummary> edges;
    std::vector<UiWorkflowAlertSummary> alerts;
};

struct IUiReadDb {
    virtual ~IUiReadDb() = default;

    virtual std::vector<UiProgramKind> ListProgramKinds() const = 0;

    virtual UiReadPage<UiJobSummary> ListJobs(
        const UiReadJobListQuery& query) const = 0;

    virtual std::optional<UiJobSummary> GetJobSummary(
        std::int64_t job_id) const = 0;

    virtual std::optional<UiJobDetail> GetJobDetail(
        std::int64_t job_id) const = 0;

    virtual std::vector<UiJobArtifact> ListJobArtifacts(
        std::int64_t job_id) const = 0;

    virtual UiReadPage<UiJobSetSummary> ListJobSets(
        const UiReadJobSetListQuery& query) const = 0;

    virtual std::optional<UiJobSetDetail> GetJobSetDetail(
        std::int64_t job_set_id,
        int jobs_limit) const = 0;

    virtual UiReadPage<UiArtifactSummary> ListArtifacts(
        const UiReadArtifactListQuery& query) const = 0;

    virtual bool UpsertArtifactSummary(
        const UiArtifactSummary& summary,
        std::string* error_out = nullptr) = 0;

    virtual UiReadPage<UiWorkflowInstanceSummary> ListWorkflowInstances(
        const UiWorkflowInstanceListQuery& query) const = 0;

    virtual std::optional<UiWorkflowDetail> GetWorkflowDetail(
        std::int64_t workflow_instance_id) const = 0;

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

} // namespace simcore::db

#pragma once

#include <sqlite3.h>

#include "IUiReadDb.h"

namespace simcore::db {

class SqliteUiReadDb final : public IUiReadDb {
public:
    explicit SqliteUiReadDb(sqlite3* db);

    std::vector<UiProgramKind> ListProgramKinds() const override;
    UiReadPage<UiJobSummary> ListJobs(
        const UiReadJobListQuery& query) const override;
    std::optional<UiJobSummary> GetJobSummary(
        std::int64_t job_id) const override;
    std::optional<UiJobDetail> GetJobDetail(
        std::int64_t job_id) const override;
    std::vector<UiJobArtifact> ListJobArtifacts(
        std::int64_t job_id) const override;
    UiReadPage<UiJobSetSummary> ListJobSets(
        const UiReadJobSetListQuery& query) const override;
    std::optional<UiJobSetDetail> GetJobSetDetail(
        std::int64_t job_set_id,
        int jobs_limit) const override;
    UiReadPage<UiArtifactSummary> ListArtifacts(
        const UiReadArtifactListQuery& query) const override;
    bool UpsertArtifactSummary(
        const UiArtifactSummary& summary,
        std::string* error_out = nullptr) override;
    UiReadPage<UiWorkflowInstanceSummary> ListWorkflowInstances(
        const UiWorkflowInstanceListQuery& query) const override;
    std::optional<UiWorkflowDetail> GetWorkflowDetail(
        std::int64_t workflow_instance_id) const override;

    UiSeedProbeRunPage ListSeedProbeRuns(
        const UiReadSeedProbeRunListQuery& query) const override;
    std::optional<UiSeedProbeRunSummary> GetSeedProbeRunSummary(
        std::int64_t probe_run_id) const override;
    std::vector<UiSeedProbeDeltaPoint> ListSeedProbeDeltaPoints(
        std::int64_t probe_run_id) const override;
    std::vector<UiSeedProbeUniqueValue> ListSeedProbeUniqueValues(
        std::int64_t probe_run_id) const override;
    bool UpsertSeedProbeRunSummary(
        const UiSeedProbeRunSummary& summary,
        std::string* error_out = nullptr) override;
    bool ReplaceSeedProbeDeltaPoints(
        std::int64_t probe_run_id,
        const std::vector<UiSeedProbeDeltaPoint>& points,
        std::string* error_out = nullptr) override;
    bool ReplaceSeedProbeUniqueValues(
        std::int64_t probe_run_id,
        const std::vector<UiSeedProbeUniqueValue>& values,
        std::string* error_out = nullptr) override;

    std::optional<UiProjectionSubscription> GetProjectionSubscription(
        const std::string& projector_name,
        const std::string& source_context,
        const std::string& source_outbox_table) const override;
    std::vector<UiProjectionSubscription> ListProjectionSubscriptions(
        const std::string& source_context,
        const std::string& source_outbox_table) const override;
    std::optional<std::int64_t> ComputeSafeFloorOutboxId(
        const std::string& source_context,
        const std::string& source_outbox_table) const override;
    std::optional<UiProjectionSubscription> GetOrCreateProjectionSubscription(
        const UiProjectionSubscription& subscription) override;
    bool AdvanceProjectionSubscriptionCursor(
        const std::string& projector_name,
        const std::string& source_context,
        const std::string& source_outbox_table,
        std::int64_t last_outbox_id,
        const std::string& last_event_id,
        types::UtcTimePoint updated_at_utc,
        const std::optional<UiProjectionSubscriptionBatchAudit>& batch_audit) override;
    bool SetProjectionSubscriptionError(
        const std::string& projector_name,
        const std::string& source_context,
        const std::string& source_outbox_table,
        const std::string& last_error,
        types::UtcTimePoint updated_at_utc) override;
    bool PauseProjectionSubscription(
        const std::string& projector_name,
        const std::string& source_context,
        const std::string& source_outbox_table,
        types::UtcTimePoint updated_at_utc,
        const std::string& reason) override;
    bool ResumeProjectionSubscription(
        const std::string& projector_name,
        const std::string& source_context,
        const std::string& source_outbox_table,
        types::UtcTimePoint updated_at_utc) override;

private:
    sqlite3* db_ = nullptr;
};

} // namespace simcore::db

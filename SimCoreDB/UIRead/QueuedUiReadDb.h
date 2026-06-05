#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "../Common/QueuedDb.h"
#include "IUiReadDb.h"

namespace simcore::db {

class QueuedUiReadDb final : public IUiReadDb, private core::QueuedDbExecutor {
public:
    explicit QueuedUiReadDb(
        IUiReadDb* inner,
        core::QueuedDbConfig config = {});
    ~QueuedUiReadDb() override;

    QueuedUiReadDb(const QueuedUiReadDb&) = delete;
    QueuedUiReadDb& operator=(const QueuedUiReadDb&) = delete;

    bool Start(std::string* error_out = nullptr);
    void Stop();
    [[nodiscard]] bool IsRunning() const;
    [[nodiscard]] core::QueuedDbTelemetrySnapshot GetTelemetrySnapshot() const;

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
    template <typename Result, typename Fn>
    Result ExecuteRead(Fn&& fn, Result fallback, std::string* error_out = nullptr) const;

    template <typename Result, typename Fn>
    Result ExecuteWrite(Fn&& fn, Result fallback, std::string* error_out = nullptr) const;

    IUiReadDb* inner_ = nullptr;
    core::QueuedDbConfig config_{};
    mutable std::mutex sqlite_call_mtx_;
    std::unique_ptr<core::QueuedDbLane> read_lane_;
    std::unique_ptr<core::QueuedDbLane> write_lane_;
};

} // namespace simcore::db

#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

#include <sqlite3.h>

#include "../Common/Events/EventPayloadResolvers.h"

namespace savor::db::analysis {

class SqliteSeedProbePayloadRowResolver final : public events::ISeedProbePayloadRowResolver {
public:
    explicit SqliteSeedProbePayloadRowResolver(sqlite3* db);

    std::optional<events::AnalysisSeedProbeSetCreatedPayloadView> ResolveSeedProbeSetCreated(
        std::string_view payload_ref_kind,
        std::int64_t payload_ref_id) const override;
    std::optional<events::AnalysisSeedProbeRunRequestedPayloadView> ResolveSeedProbeRunRequested(
        std::string_view payload_ref_kind,
        std::int64_t payload_ref_id) const override;
    std::optional<events::AnalysisSeedProbeNeutralSeedRecordedPayloadView> ResolveSeedProbeNeutralSeedRecorded(
        std::string_view payload_ref_kind,
        std::int64_t payload_ref_id) const override;
    std::optional<events::AnalysisSeedProbeGridSeedRecordedPayloadView> ResolveSeedProbeGridSeedRecorded(
        std::string_view payload_ref_kind,
        std::int64_t payload_ref_id) const override;
    std::optional<events::AnalysisSeedProbeUniqueSeedRecordedPayloadView> ResolveSeedProbeUniqueSeedRecorded(
        std::string_view payload_ref_kind,
        std::int64_t payload_ref_id) const override;
    std::optional<events::AnalysisSeedProbeEncounterProjectionRecordedPayloadView> ResolveSeedProbeEncounterProjectionRecorded(
        std::string_view payload_ref_kind,
        std::int64_t payload_ref_id) const override;
    std::optional<events::AnalysisSeedProbeRunCompletedPayloadView> ResolveSeedProbeRunCompleted(
        std::string_view payload_ref_kind,
        std::int64_t payload_ref_id) const override;

private:
    sqlite3* db_ = nullptr;
};

class SqliteBattlePayloadRowResolver final : public events::IAnalysisBattlePayloadRowResolver {
public:
    explicit SqliteBattlePayloadRowResolver(sqlite3* db);

    std::optional<events::AnalysisBattleSetCreatedPayloadView> ResolveBattleSetCreated(
        std::string_view payload_ref_kind,
        std::int64_t payload_ref_id) const override;
    std::optional<events::AnalysisBattleSeedCandidateAddedPayloadView> ResolveBattleSeedCandidateAdded(
        std::string_view payload_ref_kind,
        std::int64_t payload_ref_id) const override;
    std::optional<events::AnalysisBattleTurnWaveCreatedPayloadView> ResolveBattleTurnWaveCreated(
        std::string_view payload_ref_kind,
        std::int64_t payload_ref_id) const override;
    std::optional<events::AnalysisBattleTurnJobRecordedPayloadView> ResolveBattleTurnJobRecorded(
        std::string_view payload_ref_kind,
        std::int64_t payload_ref_id) const override;
    std::optional<events::AnalysisBattleTurnJobResultUpdatedPayloadView> ResolveBattleTurnJobResultUpdated(
        std::string_view payload_ref_kind,
        std::int64_t payload_ref_id) const override;
    std::optional<events::AnalysisBattleTurnWaveStatusUpdatedPayloadView> ResolveBattleTurnWaveStatusUpdated(
        std::string_view payload_ref_kind,
        std::int64_t payload_ref_id) const override;
    std::optional<events::AnalysisBattleBattleSetStatusUpdatedPayloadView> ResolveBattleSetStatusUpdated(
        std::string_view payload_ref_kind,
        std::int64_t payload_ref_id) const override;
    std::optional<events::AnalysisBattleBattleAdvancementPoolCreatedPayloadView> ResolveBattleBattleAdvancementPoolCreated(
        std::string_view payload_ref_kind,
        std::int64_t payload_ref_id) const override;
    std::optional<events::AnalysisBattleBattleAdvancementDecisionRecordedPayloadView> ResolveBattleBattleAdvancementDecisionRecorded(
        std::string_view payload_ref_kind,
        std::int64_t payload_ref_id) const override;
    std::optional<events::AnalysisBattleManualFollowupUpdatedPayloadView> ResolveBattleManualFollowupUpdated(
        std::string_view payload_ref_kind,
        std::int64_t payload_ref_id) const override;

private:
    sqlite3* db_ = nullptr;
};

class SqliteAnalysisSpinePayloadRowResolver final : public events::IAnalysisSpinePayloadRowResolver {
public:
    explicit SqliteAnalysisSpinePayloadRowResolver(sqlite3* db);

    std::optional<events::AnalysisSpineRunCreatedPayloadView> ResolveSpineRunCreated(
        std::string_view payload_ref_kind,
        std::int64_t payload_ref_id) const override;
    std::optional<events::AnalysisSpineStateRefRegisteredPayloadView> ResolveSpineStateRefRegistered(
        std::string_view payload_ref_kind,
        std::int64_t payload_ref_id) const override;
    std::optional<events::AnalysisSpineLineageEdgeAddedPayloadView> ResolveSpineLineageEdgeAdded(
        std::string_view payload_ref_kind,
        std::int64_t payload_ref_id) const override;
    std::optional<events::AnalysisSpineArtifactLinkedPayloadView> ResolveSpineArtifactLinked(
        std::string_view payload_ref_kind,
        std::int64_t payload_ref_id) const override;

private:
    sqlite3* db_ = nullptr;
};

} // namespace savor::db::analysis

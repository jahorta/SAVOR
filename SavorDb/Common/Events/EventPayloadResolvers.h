#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

#include "EventPayloadViews.h"

namespace savor::db::events {

// Contracts only: concrete database implementations should live in their owning bounded context modules.
struct ISeedProbePayloadRowResolver {
    virtual ~ISeedProbePayloadRowResolver() = default;

    virtual std::optional<AnalysisSeedProbeSetCreatedPayloadView> ResolveSeedProbeSetCreated(
        std::string_view payload_ref_kind,
        std::int64_t payload_ref_id) const = 0;

    virtual std::optional<AnalysisSeedProbeRunRequestedPayloadView> ResolveSeedProbeRunRequested(
        std::string_view payload_ref_kind,
        std::int64_t payload_ref_id) const = 0;

    virtual std::optional<AnalysisSeedProbeNeutralSeedRecordedPayloadView> ResolveSeedProbeNeutralSeedRecorded(
        std::string_view payload_ref_kind,
        std::int64_t payload_ref_id) const = 0;

    virtual std::optional<AnalysisSeedProbeGridSeedRecordedPayloadView> ResolveSeedProbeGridSeedRecorded(
        std::string_view payload_ref_kind,
        std::int64_t payload_ref_id) const = 0;

    virtual std::optional<AnalysisSeedProbeUniqueSeedRecordedPayloadView> ResolveSeedProbeUniqueSeedRecorded(
        std::string_view payload_ref_kind,
        std::int64_t payload_ref_id) const = 0;

    virtual std::optional<AnalysisSeedProbeEncounterProjectionRecordedPayloadView> ResolveSeedProbeEncounterProjectionRecorded(
        std::string_view payload_ref_kind,
        std::int64_t payload_ref_id) const = 0;

    virtual std::optional<AnalysisSeedProbeRunCompletedPayloadView> ResolveSeedProbeRunCompleted(
        std::string_view payload_ref_kind,
        std::int64_t payload_ref_id) const = 0;
};

struct IAnalysisBattlePayloadRowResolver {
    virtual ~IAnalysisBattlePayloadRowResolver() = default;

    virtual std::optional<AnalysisBattleSetCreatedPayloadView> ResolveBattleSetCreated(
        std::string_view payload_ref_kind,
        std::int64_t payload_ref_id) const = 0;

    virtual std::optional<AnalysisBattleSeedCandidateAddedPayloadView> ResolveBattleSeedCandidateAdded(
        std::string_view payload_ref_kind,
        std::int64_t payload_ref_id) const = 0;

    virtual std::optional<AnalysisBattleTurnWaveCreatedPayloadView> ResolveBattleTurnWaveCreated(
        std::string_view payload_ref_kind,
        std::int64_t payload_ref_id) const = 0;

    virtual std::optional<AnalysisBattleTurnJobRecordedPayloadView> ResolveBattleTurnJobRecorded(
        std::string_view payload_ref_kind,
        std::int64_t payload_ref_id) const = 0;

    virtual std::optional<AnalysisBattleTurnJobResultUpdatedPayloadView> ResolveBattleTurnJobResultUpdated(
        std::string_view payload_ref_kind,
        std::int64_t payload_ref_id) const = 0;

    virtual std::optional<AnalysisBattleTurnWaveStatusUpdatedPayloadView> ResolveBattleTurnWaveStatusUpdated(
        std::string_view payload_ref_kind,
        std::int64_t payload_ref_id) const = 0;

    virtual std::optional<AnalysisBattleBattleSetStatusUpdatedPayloadView> ResolveBattleSetStatusUpdated(
        std::string_view payload_ref_kind,
        std::int64_t payload_ref_id) const = 0;

    virtual std::optional<AnalysisBattleBattleAdvancementPoolCreatedPayloadView> ResolveBattleBattleAdvancementPoolCreated(
        std::string_view payload_ref_kind,
        std::int64_t payload_ref_id) const = 0;

    virtual std::optional<AnalysisBattleBattleAdvancementDecisionRecordedPayloadView> ResolveBattleBattleAdvancementDecisionRecorded(
        std::string_view payload_ref_kind,
        std::int64_t payload_ref_id) const = 0;

    virtual std::optional<AnalysisBattleManualFollowupUpdatedPayloadView> ResolveBattleManualFollowupUpdated(
        std::string_view payload_ref_kind,
        std::int64_t payload_ref_id) const = 0;

    virtual std::optional<AnalysisBattleCompletionPayloadView> ResolveBattleCompletion(
        std::string_view payload_ref_kind,
        std::int64_t payload_ref_id) const = 0;

    virtual std::optional<AnalysisBattleResultsPayloadView> ResolveBattleResults(
        std::string_view payload_ref_kind,
        std::int64_t payload_ref_id) const = 0;
};

struct IAnalysisSpinePayloadRowResolver {
    virtual ~IAnalysisSpinePayloadRowResolver() = default;

    virtual std::optional<AnalysisSpineRunCreatedPayloadView> ResolveSpineRunCreated(
        std::string_view payload_ref_kind,
        std::int64_t payload_ref_id) const = 0;

    virtual std::optional<AnalysisSpineStateRefRegisteredPayloadView> ResolveSpineStateRefRegistered(
        std::string_view payload_ref_kind,
        std::int64_t payload_ref_id) const = 0;

    virtual std::optional<AnalysisSpineLineageEdgeAddedPayloadView> ResolveSpineLineageEdgeAdded(
        std::string_view payload_ref_kind,
        std::int64_t payload_ref_id) const = 0;

    virtual std::optional<AnalysisSpineArtifactLinkedPayloadView> ResolveSpineArtifactLinked(
        std::string_view payload_ref_kind,
        std::int64_t payload_ref_id) const = 0;
};

} // namespace savor::db::events

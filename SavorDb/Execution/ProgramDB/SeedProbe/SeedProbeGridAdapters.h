#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "../ProgramKindDescriptor.h"
#include "../../../Analysis/IAnalysisDb.h"
#include "../../../Authoring/IAuthoringDb.h"
#include "../../IExecutionDb.h"
#include "../../../../SavorCore/Runner/IPC/Wire.h"

namespace savor::db::execution::programdb::seedprobe {

enum class SeedProbeWorkflowPhase : std::uint32_t {
    Neutral = 1,
    Grid = 2,
    Unique = 3,
};

struct SeedProbeGridBlueprintConfig {
    std::int64_t probe_id = 0;
    std::int32_t program_version = 1;
    std::uint32_t run_ms = 0;
    std::uint32_t vi_stall_ms = 0;
};

struct SeedProbeGridSpec {
    int samples_per_axis = 5;
    std::uint8_t min_value = 0;
    std::uint8_t max_value = 255;
    bool cap_trigger_top = true;
    bool ignore_trigger_minmax = false;
};

struct GridFanoutEntry {
    std::int64_t domain_ref_id = 0;
    savor::GCInputFrame frame{};
    std::string frame_hex;
    std::string family;
    JobPersistenceRecord persistence{};
};

class SeedProbeGridJobPersistenceAdapter final : public IJobPersistenceAdapter {
public:
    SeedProbeGridJobPersistenceAdapter(
        savor::db::IExecutionDb* execution_db,
        savor::db::IAnalysisDb* analysis_db,
        savor::db::IAuthoringDb* authoring_db,
        SeedProbeGridBlueprintConfig blueprint,
        SeedProbeGridSpec grid);

    WorkflowStepScheduleResult EncodeForQueueing(const WorkflowStepScheduleContext& context) const override;
    std::int64_t DecodeDomainRefId(const JobPersistenceRecord& persisted) const override;

    const std::vector<GridFanoutEntry>& Fanout() const;

private:
    static std::string FingerprintFor(const SeedProbeGridBlueprintConfig& blueprint, std::int64_t probe_run_id, const std::string& frame_hex, const char* family, std::int64_t grid_ref);
    std::vector<GridFanoutEntry> BuildFanout(
        const SeedProbeGridSpec& grid,
        const SeedProbeGridBlueprintConfig& blueprint) const;
    SeedProbeGridBlueprintConfig ResolveBlueprintForRun(std::int64_t probe_run_id) const;
    SeedProbeGridSpec ResolveGridSpecForRun(std::int64_t probe_run_id) const;

    savor::db::IExecutionDb* execution_db_ = nullptr;
    savor::db::IAnalysisDb* analysis_db_ = nullptr;
    savor::db::IAuthoringDb* authoring_db_ = nullptr;
    SeedProbeGridBlueprintConfig blueprint_{};
    SeedProbeGridSpec grid_{};
    std::vector<GridFanoutEntry> fanout_{};
};

struct RuntimeInitSeedProbeContext {
    std::int64_t savestate_id = 0;
    std::string bootstrap_profile;
};

class SeedProbeRuntimeInitAdapter final : public IRuntimeInitAdapter {
public:
    SeedProbeRuntimeInitAdapter(savor::db::IExecutionDb* execution_db, const savor::db::IAnalysisDb* analysis_db);

    RuntimeInitRequest BuildRuntimeInit(std::int64_t job_id) const override;
    std::optional<savor::PSJob> MaterializePsJob(std::int64_t job_id, const RuntimeInitRequest& request) const override;

private:
    savor::db::IExecutionDb* execution_db_ = nullptr;
    const savor::db::IAnalysisDb* analysis_db_ = nullptr;
};

struct GridResultContext {
    SeedProbeWorkflowPhase phase = SeedProbeWorkflowPhase::Neutral;
    std::int64_t probe_result_id = 0;
    std::int64_t input_frame_id = 0;
    std::int64_t neutral_seed = 0;
    std::uint32_t observed_seed = 0;
    std::string frame_hex;
    std::string source_family;
    std::string correlation_id;
    std::string causation_id;
    std::int64_t expected_delta = 0;
};

class SeedProbeGridResultMapper final : public IResultMapper {
public:
    SeedProbeGridResultMapper(savor::db::IExecutionDb* execution_db, savor::db::IAnalysisDb* analysis_db);

    std::string BuildResultIniFromPrResult(std::int64_t job_id, const savor::PRResult& result) const override;
    ResultMapPayload MapPrimaryResult(std::int64_t job_id, const std::string& result_ini) const override;
    std::optional<ResultArtifactRef> MapPrimaryArtifact(std::int64_t job_id) const override;

    static bool ShouldRequeueOnFailure(SeedProbeWorkflowPhase phase);

private:
    std::optional<GridResultContext> ResolveContextFromJob(std::int64_t job_id) const;
    static std::optional<savor::GCInputFrame> ParseFrame(const std::string& frame_hex);
    static std::string FamilyLabel(savor::ElementFamily family);
    static std::string NormalizeFamilyLabel(const std::string& family);
    static std::int64_t AxisXYId(const savor::GCInputFrame& frame, const std::string& source_family);

    savor::db::IExecutionDb* execution_db_ = nullptr;
    savor::db::IAnalysisDb* analysis_db_ = nullptr;
};

ProgramKindDescriptor BuildSeedProbeGridDescriptor(
    savor::db::IExecutionDb* execution_db,
    savor::db::IAnalysisDb* analysis_db,
    SeedProbeGridBlueprintConfig blueprint,
    SeedProbeGridSpec grid,
    savor::db::IAuthoringDb* authoring_db = nullptr);

} // namespace savor::db::execution::programdb::seedprobe

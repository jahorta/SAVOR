#pragma once

#include "../ProgramKindDescriptor.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>

namespace savor::db {
struct IAnalysisDb;
struct IExecutionDb;
struct IStateDb;
}

namespace savor::db::execution::programdb::seedprobe {

struct SeedProbeExecutionAdapters {
    std::shared_ptr<IWorksetReconstructionAdapter> reconstruction;
    std::shared_ptr<IProgramResultHandler> result_handler;
};

[[nodiscard]] std::optional<std::int64_t>
FindSurveyNeutralSeedProbeResultId(
    savor::db::IExecutionDb* execution_db,
    savor::db::IAnalysisDb* analysis_db,
    std::int64_t probe_run_id);

[[nodiscard]] SeedProbeExecutionAdapters
BuildSeedProbeExecutionAdapters(
    savor::db::IExecutionDb* execution_db,
    savor::db::IStateDb* state_db,
    savor::db::IAnalysisDb* analysis_db,
    std::filesystem::path working_dir_root);

} // namespace savor::db::execution::programdb::seedprobe

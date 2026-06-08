#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

#include "../ProgramKindDescriptor.h"
#include "../../../Analysis/IAnalysisDb.h"
#include "../../../Authoring/IAuthoringDb.h"
#include "../../../Execution/IExecutionDb.h"
#include "../../../State/IStateDb.h"

namespace simcore::db::execution::programdb::tasmovie {

struct TasMovieBlueprintConfig {
    std::int64_t base_dtm_artifact_id = 0;
    std::optional<std::int64_t> tas_spec_id;
    std::int64_t rtc_low = 0;
    std::int64_t rtc_high = 0;
    int priority = 0;
    std::uint32_t run_ms = 0;
    std::uint32_t vi_stall_ms = 2000;
    bool progress_enable = false;
    std::uint8_t headroom_x10 = 15;
    std::optional<std::int64_t> bind_seed_probe_run_id;
};

struct TasMoviePhaseRegistrationConfig {
    TasMovieBlueprintConfig blueprint;
    simcore::db::IAuthoringDb* authoring_db = nullptr;
    std::filesystem::path working_dir_root;
    std::string next_step_key = "Done";
};

struct TasMovieResultsIni {
    int w_err = 0;
    std::uint32_t dw_err = 0;
    std::uint32_t run_ms_used = 0;
    std::uint32_t vi_start = 0;
    std::uint32_t vi_end = 0;
    std::string savestate_path;

    static TasMovieResultsIni FromIniText(const std::string& text);
    std::string ToIniText() const;
};

ProgramKindDescriptor BuildTasMovieDescriptor(
    simcore::db::IExecutionDb* execution_db,
    simcore::db::IStateDb* state_db,
    simcore::db::IAnalysisDb* analysis_db,
    TasMoviePhaseRegistrationConfig config);

} // namespace simcore::db::execution::programdb::tasmovie

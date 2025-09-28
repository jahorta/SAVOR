// SimCore/DB/ProgramDB/SeedProbeDBCodec.h
#pragma once
#include <optional>
#include <string>
#include "IProgramDBCodec.h"
#include "../DBCore/DbResult.h"
#include "../../Runner/Script/PhaseScriptVM.h"

struct SeedProbeDBCodec final : IProgramDBCodec {
    simcore::db::DbResult<int64_t> encode_job_into_db(int64_t job_set_id, const std::string& blueprint_ini) override;
    simcore::db::DbResult<simcore::PSJob> decode_job_from_db(int64_t job_id) override;
    simcore::db::DbResult<void> encode_progress_into_db(int64_t job_id, const std::string& progress_line) override;
    simcore::db::DbResult<void> encode_results_into_db(int64_t job_id, const std::string& results_ini, bool success) override;
    simcore::db::DbResult<std::string> decode_progress_from_db(std::optional<int64_t> job_id, std::optional<int64_t> job_set_id) override;
    simcore::db::DbResult<std::string> decode_results_from_db(std::optional<int64_t> job_id, std::optional<int64_t> job_set_id) override;
    simcore::db::DbResult<std::optional<int64_t>> get_required_savestate_id(int64_t job_id) override;
    simcore::db::DbResult<simcore::PSInit>        build_psinit_for_job(int64_t job_id) override;
    simcore::db::DbResult<std::string>            build_results_ini_from_prresult(int64_t job_id, const simcore::PRResult& r) override;
};

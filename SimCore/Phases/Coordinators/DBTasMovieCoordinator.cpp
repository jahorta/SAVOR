#include "DBTasMovieCoordinator.h"
#include "../../DB/Scheduling/JobSetsRepo.h"
#include "../../DB/ProgramDB/TasMovieDBCodec.h"
#include "../../Utils/IniDoc.h"
#include "../../Runner/IPC/Wire.h"

using simcore::db::DbResult;

namespace simcore {
    namespace phase {

        DbResult<int64_t> TasMovieCoordinator::SetupJobSet()
        {
            auto js = simcore::db::JobSetsRepo::Create("TasMovie", PK_TasMovie,
                std::nullopt, std::optional<std::string>("TasMovie"),
                std::nullopt, std::nullopt, std::nullopt);
            if (!js.ok) return DbResult<int64_t>::Err(js.error);
            return DbResult<int64_t>::Ok(js.value);
        }

        DbResult<void> TasMovieCoordinator::QueueJob(int64_t job_set_id, const TasJobSpec& job)
        {
            TasMovieDBCodec codec;
            db::codec::tas::BlueprintIni ini;
            ini.base_dtm_artifact_id = job.base_dtm_id;
            ini.rtc_low = job.new_rtc_min;
            ini.rtc_high = job.new_rtc_max;
            ini.priority = job.priority;
            ini.run_ms = job.run_ms;
            ini.vi_stall_ms = job.vi_stall_ms;
            ini.progress_enable = job.progress_enable;
            auto enq = codec.encode_job_into_db(job_set_id, ini.to_string());
            if (!enq.ok) return DbResult<void>::Err(enq.error);
            return DbResult<void>::Ok();
        }

        DbResult<std::string> TasMovieCoordinator::PollProgress_JobSet(int64_t job_set_id)
        {
            TasMovieDBCodec codec;
            return codec.decode_progress_from_db(std::nullopt, job_set_id);
        }

        DbResult<std::string> TasMovieCoordinator::RetrieveResults_JobSet(int64_t job_set_id)
        {
            TasMovieDBCodec codec;
            return codec.decode_results_from_db(std::nullopt, job_set_id);
        }

    }
} // namespace

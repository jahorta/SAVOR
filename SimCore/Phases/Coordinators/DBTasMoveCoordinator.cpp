#include "DBTasMovieCoordinator.h"
#include "../../DB/Scheduling/JobSetsRepo.h"
#include "../../DB/ProgramDB/TasMovieDBCodec.h"
#include "../../IniDoc.h"
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

        DbResult<void> TasMovieCoordinator::QueueJobs(int64_t job_set_id, const std::vector<TasJobSpec>& jobs)
        {
            TasMovieDBCodec codec;
            for (auto& j : jobs) {
                IniKV kv;
                kv.add("dtm_path", j.dtm_path);
                kv.add("save_dir", j.save_dir);
                if (j.new_rtc) kv.add("new_rtc", std::to_string(j.new_rtc));
                if (j.priority) kv.add("priority", std::to_string(j.priority));
                if (j.run_ms) kv.add("run_ms", std::to_string(j.run_ms));
                if (j.vi_stall_ms) kv.add("vi_stall_ms", std::to_string(j.vi_stall_ms));
                if (!j.progress_enable) kv.add("progress_enable", "0");
                if (j.save_on_fail) kv.add("save_on_fail", "1");
                auto enq = codec.encode_job_into_db(job_set_id, kv.to_string_sorted());
                if (!enq.ok) return DbResult<void>::Err(enq.error);
            }
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

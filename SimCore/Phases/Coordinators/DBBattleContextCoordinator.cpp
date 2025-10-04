#include "DBBattleContextCoordinator.h"
#include "../../DB/Scheduling/JobSetsRepo.h"
#include "../../DB/ProgramDB/BattleContextDBCodec.h"
#include "../../Utils/IniDoc.h"
#include "../../Runner/IPC/Wire.h"

using simcore::db::DbResult;

namespace simcore {
    namespace phase {

        DbResult<int64_t> BattleContextCoordinator::SetupJobSet()
        {
            auto js = simcore::db::JobSetsRepo::Create("BattleContextProbe", PK_BattleContextProbe,
                std::nullopt, std::optional<std::string>("BattleContext"),
                std::nullopt, std::nullopt, std::nullopt);
            if (!js.ok) return DbResult<int64_t>::Err(js.error);
            return DbResult<int64_t>::Ok(js.value);
        }

        DbResult<int64_t> BattleContextCoordinator::QueueProbe(int64_t job_set_id, const BCOptions& opt)
        {
            BattleContextDBCodec codec;
            IniKV kv;
            kv.add("run_ms", std::to_string(opt.run_ms));
            kv.add("vi_stall_ms", std::to_string(opt.vi_stall_ms));
            if (opt.priority) kv.add("priority", std::to_string(opt.priority));
            kv.add("savestate_id", std::to_string(opt.savestate_id));
            auto enq = codec.encode_job_into_db(job_set_id, kv.to_string_sorted());
            if (!enq.ok) return DbResult<int64_t>::Err(enq.error);
            return DbResult<int64_t>::Ok(enq.value);
        }

        DbResult<std::string> BattleContextCoordinator::PollProgress_JobSet(int64_t job_set_id)
        {
            BattleContextDBCodec codec;
            return codec.decode_progress_from_db(std::nullopt, job_set_id);
        }

        DbResult<std::string> BattleContextCoordinator::RetrieveResults_JobSet(int64_t job_set_id)
        {
            BattleContextDBCodec codec;
            return codec.decode_results_from_db(std::nullopt, job_set_id);
        }

    }
} // namespace

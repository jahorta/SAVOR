#include "DBSeedProbeCoordinator.h"
#include "../../DB/Scheduling/JobSetsRepo.h"
#include "../../DB/ProgramDB/SeedProbeDBCodec.h"
#include "../../Utils/IniDoc.h"
#include "../../Runner/IPC/Wire.h"

using simcore::db::DbResult;

namespace simcore {
    namespace phase {

        DbResult<int64_t> SeedProbeCoordinator::SetupJobSet()
        {
            auto js = simcore::db::JobSetsRepo::Create("SeedProbe", PK_SeedProbe,
                std::nullopt, std::optional<std::string>("SeedProbe"),
                std::nullopt, std::nullopt, std::nullopt);
            if (!js.ok) return DbResult<int64_t>::Err(js.error);
            return DbResult<int64_t>::Ok(js.value);
        }

        DbResult<void> SeedProbeCoordinator::QueueBatches(int64_t job_set_id, const std::vector<SeedProbeBatch>& batches)
        {
            SeedProbeDBCodec codec;
            for (auto& b : batches) {
                IniKV kv;
                kv.add("probe_id", std::to_string(b.probe_id));
                if (b.run_ms) kv.add("run_ms", std::to_string(b.run_ms));
                if (b.vi_stall_ms) kv.add("vi_stall_ms", std::to_string(b.vi_stall_ms));
                if (b.is_neutral) kv.add("is_neutral", "1");
                if (b.is_grid) kv.add("is_grid", "1");
                if (b.is_unique) kv.add("is_unique", "1");
                if (!b.inputs_hex.empty()) kv.set_list("inputs", b.inputs_hex);
                if (!b.expected_deltas_i32.empty()) kv.set_list("expected_deltas", b.expected_deltas_i32);
                if (!b.expected_tags.empty()) kv.set_list("expected_tags", b.expected_tags);
                auto enq = codec.encode_job_into_db(job_set_id, kv.to_string_sorted());
                if (!enq.ok) return DbResult<void>::Err(enq.error);
            }
            return DbResult<void>::Ok();
        }

        DbResult<std::string> SeedProbeCoordinator::PollProgress_JobSet(int64_t job_set_id)
        {
            SeedProbeDBCodec codec;
            return codec.decode_progress_from_db(std::nullopt, job_set_id);
        }

        DbResult<std::string> SeedProbeCoordinator::RetrieveResults_JobSet(int64_t job_set_id)
        {
            SeedProbeDBCodec codec;
            return codec.decode_results_from_db(std::nullopt, job_set_id);
        }

    }
} // namespace

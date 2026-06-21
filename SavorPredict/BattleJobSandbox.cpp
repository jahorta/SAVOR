#include "BattleJobSandbox.h"

#include "DbRootCopy.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <ostream>
#include <utility>

namespace savor::predict {

int prepare_battle_job_sandbox(
    const BattleJobRunOptions& options,
    BattleJobSandboxResult* result_out,
    std::ostream& out,
    std::ostream& err) {
    if (result_out == nullptr) {
        err << "Internal error: sandbox result output is null.\n";
        return 1;
    }
    std::error_code ec;
    const auto run_root = options.run_root;
    if (run_root.empty()) {
        err << "run-battle-job requires a run root.\n";
        return 2;
    }
    std::filesystem::create_directories(run_root, ec);
    if (ec) {
        err << "Failed to create run root " << run_root.string() << ": " << ec.message() << "\n";
        return 1;
    }

    const auto sandbox_db_root = run_root / "db";
    if (std::filesystem::exists(sandbox_db_root, ec)) {
        err << "Sandbox DB already exists: " << sandbox_db_root.string() << "\n";
        err << "Choose a fresh --run-root so the real analysis DB is never mutated in place.\n";
        return 1;
    }

    result_out->run_root = run_root;
    result_out->db_root = sandbox_db_root;
    result_out->sandbox_mode = options.sandbox_mode;

    if (options.sandbox_mode == savor::dbutils::SandboxMode::FullCopy) {
        if (const int rc = savor::dbutils::CopyDbRootFull(
                {
                    .source_root = options.db_root,
                    .dest_root = sandbox_db_root,
                    .overwrite = false,
                },
                out,
                err);
            rc != 0) {
            return rc;
        }
        return 0;
    }

    savor::dbutils::BattleSingleTurnJobSubsetResult subset;
    if (const int rc = savor::dbutils::HydrateBattleSingleTurnJobSubset(
            {
                .source_root = options.db_root,
                .target_root = sandbox_db_root,
                .artifact_root = run_root / "source-artifacts",
                .selector = {
                    .turn_job_id = options.turn_job_id.has_value()
                        ? std::optional<std::int64_t>(*options.turn_job_id)
                        : std::nullopt,
                    .exec_job_id = options.exec_job_id.has_value()
                        ? std::optional<std::int64_t>(*options.exec_job_id)
                        : std::nullopt,
                },
                .overwrite_target = false,
            },
            &subset,
            out,
            err);
        rc != 0) {
        result_out->table_counts = std::move(subset.table_counts);
        result_out->copied_artifacts = std::move(subset.copied_artifacts);
        result_out->validation_errors = std::move(subset.validation_errors);
        return rc;
    }

    result_out->table_counts = std::move(subset.table_counts);
    result_out->copied_artifacts = std::move(subset.copied_artifacts);
    result_out->validation_errors = std::move(subset.validation_errors);
    return 0;
}

} // namespace savor::predict

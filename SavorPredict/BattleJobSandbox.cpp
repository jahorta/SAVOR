#include "BattleJobSandbox.h"

#include "DbCopy.h"

#include <filesystem>
#include <ostream>

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

    PrepareDbOptions copy_options;
    copy_options.source = options.db_root;
    copy_options.dest = sandbox_db_root;
    copy_options.overwrite = false;
    if (const int rc = run_prepare_db(copy_options, out, err); rc != 0) {
        return rc;
    }

    result_out->run_root = run_root;
    result_out->db_root = sandbox_db_root;
    return 0;
}

} // namespace savor::predict

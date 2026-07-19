#pragma once

#include <cstdint>
#include <cstddef>

#include "../Script/PhaseScriptProgram.h"
#include "PRTypes.h"
#include "WorkerBootPlan.h"

namespace savor {

class ParallelPhaseScriptRunner {
public:
    ParallelPhaseScriptRunner(size_t workers);
    ~ParallelPhaseScriptRunner();

    bool start(const BootPlan& boot);

    uint64_t submit(const PSJob& job);
    bool try_get_result(PRResult& out);
    PRStatus status() const;
    void stop();

    bool set_program(uint8_t init_kind, uint8_t main_kind, const PSInit& init);
    bool run_init_once();
    bool activate_main();

    void increment_epoch();
    void reset_job_ids();
    uint32_t worker_count();
    bool try_get_progress(size_t worker_id, PRProgress& out) const;
};

} // namespace savor

#pragma once

namespace savor::runner::parallel::savordb {

struct CoordinatorIntegrationConfig {
    bool workflow_enabled = true;
    bool strict_smoke_terminal_on_failure = false;
};

} // namespace savor::runner::parallel::savordb

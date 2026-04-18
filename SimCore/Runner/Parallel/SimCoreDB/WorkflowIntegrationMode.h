#pragma once

namespace simcore::runner::parallel::simcoredb {

struct CoordinatorIntegrationConfig {
    bool workflow_enabled = true;
    bool strict_smoke_terminal_on_failure = false;
};

} // namespace simcore::runner::parallel::simcoredb

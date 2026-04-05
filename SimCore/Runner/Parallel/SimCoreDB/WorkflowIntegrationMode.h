#pragma once

namespace simcore::runner::parallel::simcoredb {

enum class CoordinatorIntegrationMode {
    LegacyCoordinator = 0,
    SimCoreDbWorkflow = 1,
};

struct CoordinatorIntegrationConfig {
    CoordinatorIntegrationMode mode = CoordinatorIntegrationMode::LegacyCoordinator;
    bool dual_write_observe = true;
};

} // namespace simcore::runner::parallel::simcoredb

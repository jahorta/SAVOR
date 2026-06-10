#pragma once

#include <string>

#include "Execution/Jobs/JobEventOrchestration.h"

class NullJobEventCommandService final : public savor::db::execution::jobs::IJobEventCommandService {
public:
    bool AppendLifecycleEvent(const savor::db::execution::jobs::JobLifecycleEventCommand&, std::string*) override {
        return true;
    }
};

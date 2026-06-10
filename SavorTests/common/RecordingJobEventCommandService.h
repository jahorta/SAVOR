#pragma once

#include <vector>

#include "Execution/Jobs/JobEventOrchestration.h"

class RecordingJobEventCommandService final : public savor::db::execution::jobs::IJobEventCommandService {
public:
    bool AppendLifecycleEvent(const savor::db::execution::jobs::JobLifecycleEventCommand& command, std::string*) override {
        calls.push_back(command);
        return true;
    }

    std::vector<savor::db::execution::jobs::JobLifecycleEventCommand> calls;
};

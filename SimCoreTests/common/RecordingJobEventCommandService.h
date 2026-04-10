#pragma once

#include <vector>

#include "Execution/Jobs/JobEventOrchestration.h"

class RecordingJobEventCommandService final : public simcore::db::execution::jobs::IJobEventCommandService {
public:
    bool AppendLifecycleEvent(const simcore::db::execution::jobs::JobLifecycleEventCommand& command, std::string*) override {
        calls.push_back(command);
        return true;
    }

    std::vector<simcore::db::execution::jobs::JobLifecycleEventCommand> calls;
};


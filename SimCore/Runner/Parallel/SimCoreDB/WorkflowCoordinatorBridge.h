#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_set>

namespace simcore::runner::parallel::simcoredb {

struct TerminalJobSetSignal {
    std::int64_t workflow_instance_id = 0;
    std::int64_t workflow_step_id = 0;
    std::int64_t job_set_id = 0;
    std::string terminal_state;
};

class WorkflowCoordinatorBridge {
public:
    using MaterializationCallback = std::function<void(std::int64_t workflow_step_id, std::int64_t job_set_id)>;
    using TerminalCallback = std::function<void(const TerminalJobSetSignal& signal)>;

    void SetMaterializationCallback(MaterializationCallback callback);
    void SetTerminalCallback(TerminalCallback callback);

    void NotifyMaterialized(std::int64_t workflow_step_id, std::int64_t job_set_id) const;
    bool NotifyTerminal(const TerminalJobSetSignal& signal);

private:
    static std::string BuildTerminalDedupKey(const TerminalJobSetSignal& signal);

    MaterializationCallback materialization_callback_;
    TerminalCallback terminal_callback_;
    std::unordered_set<std::string> seen_terminal_signals_;
};

} // namespace simcore::runner::parallel::simcoredb

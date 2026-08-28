#pragma once

#include "IVisualMessageBackendPort.h"

#include <string>
#include <string_view>
#include <thread>

namespace savor::runtime {

struct SessionVisualMessageOptions
{
    bool show_current_phase = false;
};

class SessionVisualMessageService final
{
public:
    SessionVisualMessageService(
        IVisualMessageBackendPort& backend,
        SessionVisualMessageOptions options);

    [[nodiscard]] BackendResult SetCurrentPhase(std::string_view display_name);
    [[nodiscard]] BackendResult SetIdle();

private:
    [[nodiscard]] BackendResult ReplaceCurrentPhase(std::string message);
    [[nodiscard]] bool OnOwnerThread() const noexcept;

    IVisualMessageBackendPort& backend_;
    SessionVisualMessageOptions options_;
    std::thread::id owner_thread_;
    std::string current_phase_message_;
};

} // namespace savor::runtime

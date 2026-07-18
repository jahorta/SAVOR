#pragma once

#include <cstddef>
#include <cstdint>

namespace savor::probe {

struct NativeMemcheckEvent {
    std::uint64_t value = 0;
    std::uint32_t address = 0;
    bool write = false;
    std::size_t size = 0;
    std::uint32_t pc = 0;
};

template <typename ForeignAction, typename ProbeAction>
bool dispatch_memcheck_action_once(
    const NativeMemcheckEvent& event,
    ForeignAction&& foreign_action,
    ProbeAction&& probe_action)
{
    const bool foreign_control = foreign_action(event);
    const bool probe_control = probe_action(event);
    return foreign_control || probe_control;
}

} // namespace savor::probe

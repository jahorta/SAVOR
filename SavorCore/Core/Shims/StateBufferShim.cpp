#include "StateBufferShim.h"

// Do NOT include Core/State.h here - its declaration (const&) doesn't match the library ABI.

// Forward-declare the ABI that actually exists in DolphinLib (non-const UniqueBuffer&).
namespace State {
    void LoadFromBuffer(Core::System& system, Common::UniqueBuffer<u8>& buffer);
}

void SAVOR_LoadFromBufferShim(Core::System& system,
    const Common::UniqueBuffer<u8>& buffer)
{
    // Drop const to call the real exported function.
    auto& nc = const_cast<Common::UniqueBuffer<u8>&>(buffer);
    State::LoadFromBuffer(system, nc);
}

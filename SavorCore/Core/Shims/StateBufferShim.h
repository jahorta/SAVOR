#pragma once
#include "Common/CommonTypes.h"
#include "Common/Buffer.h"

namespace Core { class System; }

// Public shim you can call from DolphinWrapper (const in, forwards to non-const ABI)
void SAVOR_LoadFromBufferShim(Core::System& system,
    const Common::UniqueBuffer<u8>& buffer);

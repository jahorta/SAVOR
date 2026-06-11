// Runner/Script/ContextKeys/KeyIds.h
#pragma once
#include <cstdint>
#include <string_view>

namespace savor::context::key {

    using KeyId = uint16_t;

    // Reserved ID ranges (adjust/extend as you add modules)
    inline constexpr KeyId CORE_MIN = 0x0000, CORE_MAX = 0x00FF;
    inline constexpr KeyId SEED_MIN = 0x0100, SEED_MAX = 0x01FF;
    inline constexpr KeyId TAS_MIN = 0x0200, TAS_MAX = 0x02FF;
    inline constexpr KeyId BATTLE_MIN = 0x0300, BATTLE_MAX = 0x03FF;
    inline constexpr KeyId TAS_FRAME_DETECTOR_MIN = 0x0400, TAS_FRAME_DETECTOR_MAX = 0x04FF;

    struct KeyPair {
        KeyId id;
        std::string_view name;
    };

} // namespace savor::context::key

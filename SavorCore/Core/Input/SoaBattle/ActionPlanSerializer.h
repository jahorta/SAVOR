#pragma once
#include <cstdint>
#include <vector>
#include <span>
#include "ActionTypes.h"
#include "../../../Runner/IPC/Wire.h"
#include "../../../Utils/Hash.h"

namespace soa::battle::actions {

    // Encode a full BattlePath into a single payload buffer:
    // Layout: u32 turn_count; repeat turn_count { u32 action_count; action_count * WireActionPlan }
    // Encode a terminal BattlePath (vector<TurnPlan>) into a compact buffer.
    void encode_battle_plan_to_buffer(const BattlePath& path, std::vector<std::uint8_t>& out);

    // Decode a BattlePath from a buffer. Returns false if malformed.
    bool decode_battle_plan_from_buffer(std::span<const std::uint8_t> buf, actions::BattlePath& out);

    inline static std::string fingerprint_battle_plan(const BattlePath& bp) {
        std::vector<uint8_t> buf; buf.reserve(8 + bp.size() * 16);
        encode_battle_plan_to_buffer(bp, buf);
        return hash::sha256(buf.data(), buf.size());
    }

} // namespace savor::programs::battle

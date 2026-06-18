#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ActionTypes.h"
#include "../../../Utils/Hash.h"

namespace soa::battle::actions {

    void encode_battle_turn_commands_to_buffer(
        const BattleTurnCommandSet& commands,
        std::vector<std::uint8_t>& out);

    bool decode_battle_turn_commands_from_buffer(
        std::span<const std::uint8_t> buf,
        BattleTurnCommandSet& out);

    std::string encode_battle_turn_commands_hex(const BattleTurnCommandSet& commands);

    std::optional<BattleTurnCommandSet> decode_battle_turn_commands_hex(std::string_view hex);

    void encode_battle_execution_script_to_buffer(
        const BattleExecutionScript& script,
        std::vector<std::uint8_t>& out);

    bool decode_battle_execution_script_from_buffer(
        std::span<const std::uint8_t> buf,
        BattleExecutionScript& out);

    inline std::string fingerprint_battle_execution_script(const BattleExecutionScript& script) {
        std::vector<std::uint8_t> buf;
        buf.reserve(8 + script.size() * 16);
        encode_battle_execution_script_to_buffer(script, buf);
        return hash::sha256(buf.data(), buf.size());
    }

} // namespace soa::battle::actions

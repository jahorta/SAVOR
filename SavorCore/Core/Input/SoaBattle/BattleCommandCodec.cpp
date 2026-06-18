#include "BattleCommandCodec.h"

#include <cstring>
#include <utility>

#include "../../../Utils/Hex.h"

namespace soa::battle::actions {

    static int hex_nibble(char c) {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
        if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
        return -1;
    }

    static std::optional<std::vector<std::uint8_t>> strict_hex_to_bytes(std::string_view hex) {
        std::string normalized;
        normalized.reserve(hex.size());
        for (char c : hex) {
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
                continue;
            }
            normalized.push_back(c);
        }
        if (normalized.rfind("0x", 0) == 0 || normalized.rfind("0X", 0) == 0) {
            normalized.erase(0, 2);
        }
        if (normalized.size() % 2 != 0) {
            return std::nullopt;
        }

        std::vector<std::uint8_t> out;
        out.reserve(normalized.size() / 2);
        for (std::size_t i = 0; i < normalized.size(); i += 2) {
            const int hi = hex_nibble(normalized[i]);
            const int lo = hex_nibble(normalized[i + 1]);
            if (hi < 0 || lo < 0) {
                return std::nullopt;
            }
            out.push_back(static_cast<std::uint8_t>((hi << 4) | lo));
        }
        return out;
    }

    static inline void u32_le(std::vector<std::uint8_t>& buf, std::uint32_t v) {
        buf.push_back(static_cast<std::uint8_t>(v & 0xFF));
        buf.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
        buf.push_back(static_cast<std::uint8_t>((v >> 16) & 0xFF));
        buf.push_back(static_cast<std::uint8_t>((v >> 24) & 0xFF));
    }

    static bool read_u32_le(const std::uint8_t*& cur, const std::uint8_t* end, std::uint32_t& v) {
        if (end - cur < 4) return false;
        std::memcpy(&v, cur, 4);
        cur += 4;
        return true;
    }

    void encode_battle_turn_commands_to_buffer(
        const BattleTurnCommandSet& commands,
        std::vector<std::uint8_t>& out) {
        out.clear();
        u32_le(out, static_cast<std::uint32_t>(commands.size()));
        for (const auto& command : commands) {
            BattleCommand::to_wire(command, out);
        }
    }

    bool decode_battle_turn_commands_from_buffer(
        std::span<const std::uint8_t> buf,
        BattleTurnCommandSet& out) {
        out.clear();
        const auto* cur = buf.data();
        const auto* end = buf.data() + buf.size();

        std::uint32_t command_count = 0;
        if (!read_u32_le(cur, end, command_count)) return false;

        out.reserve(command_count);
        for (std::uint32_t i = 0; i < command_count; ++i) {
            BattleCommand command{};
            if (!BattleCommand::from_wire(cur, end, command)) return false;
            out.push_back(command);
        }
        return cur == end;
    }

    std::string encode_battle_turn_commands_hex(const BattleTurnCommandSet& commands) {
        std::vector<std::uint8_t> bytes;
        encode_battle_turn_commands_to_buffer(commands, bytes);
        return bytes_to_hex(bytes.data(), bytes.size());
    }

    std::optional<BattleTurnCommandSet> decode_battle_turn_commands_hex(std::string_view hex) {
        const auto bytes = strict_hex_to_bytes(hex);
        if (!bytes.has_value()) {
            return std::nullopt;
        }
        BattleTurnCommandSet commands;
        if (!decode_battle_turn_commands_from_buffer(std::span<const std::uint8_t>(bytes->data(), bytes->size()), commands)) {
            return std::nullopt;
        }
        return commands;
    }

    void encode_battle_execution_script_to_buffer(
        const BattleExecutionScript& script,
        std::vector<std::uint8_t>& out) {
        out.clear();
        u32_le(out, static_cast<std::uint32_t>(script.size()));
        for (const auto& turn : script) {
            u32_le(out, static_cast<std::uint32_t>(turn.fake_attack_count));
            u32_le(out, static_cast<std::uint32_t>(turn.commands.size()));
            for (const auto& command : turn.commands) {
                BattleCommand::to_wire(command, out);
            }
        }
    }

    bool decode_battle_execution_script_from_buffer(
        std::span<const std::uint8_t> buf,
        BattleExecutionScript& out) {
        out.clear();
        const auto* cur = buf.data();
        const auto* end = buf.data() + buf.size();

        std::uint32_t turn_count = 0;
        if (!read_u32_le(cur, end, turn_count)) return false;

        out.reserve(turn_count);
        for (std::uint32_t t = 0; t < turn_count; ++t) {
            std::uint32_t fake_attacks = 0;
            std::uint32_t command_count = 0;
            if (!read_u32_le(cur, end, fake_attacks)) return false;
            if (!read_u32_le(cur, end, command_count)) return false;

            BattleTurnExecutionSpec turn;
            turn.fake_attack_count = fake_attacks;
            turn.commands.reserve(command_count);
            for (std::uint32_t i = 0; i < command_count; ++i) {
                BattleCommand command{};
                if (!BattleCommand::from_wire(cur, end, command)) return false;
                turn.commands.push_back(command);
            }
            out.push_back(std::move(turn));
        }
        return cur == end;
    }

} // namespace soa::battle::actions

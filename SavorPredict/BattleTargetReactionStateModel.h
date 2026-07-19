#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>

namespace savor::predict {

enum class BattleTargetReactionStatus {
    Matched,
    Provisional,
    MissingInput,
    Unsupported,
};

enum class BattleTargetReactionActionKind {
    BasicAttack,
    Unknown,
};

struct BattleTargetReactionInput {
    BattleTargetReactionActionKind action_kind =
        BattleTargetReactionActionKind::Unknown;
    int origin_slot = -1;
    int target_slot = -1;
    std::optional<int> hit_check;
    std::optional<int> pending_damage;
    std::optional<int> target_hp_before_flush;
    std::optional<bool> target_dead;
    std::optional<bool> counter_accepted;
    std::optional<std::array<std::int16_t, 3>> result_shorts;
};

struct BattleTargetReactionResult {
    BattleTargetReactionStatus status =
        BattleTargetReactionStatus::MissingInput;
    int origin_slot = -1;
    int target_slot = -1;
    int queued_result = -1;
    std::uint8_t queued_counter_byte = 0;
    int pending_damage = 0;
    int target_hp_before_flush = 0;
    std::array<std::int16_t, 3> result_shorts{0, -1, -1};
    std::uint32_t reaction_flags_0x50 = 0;
    std::uint32_t selector_flags_0xf0 = 0;
    std::uint32_t selector_flags_0xf4 = 0;
    bool self_target_suppressed = false;
    std::string provenance;
};

BattleTargetReactionResult model_battle_target_reaction(
    const BattleTargetReactionInput& input);

const char* battle_target_reaction_status_name(
    BattleTargetReactionStatus status);

} // namespace savor::predict

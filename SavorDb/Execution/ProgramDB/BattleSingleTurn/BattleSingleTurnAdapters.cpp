#include "BattleSingleTurnAdapters.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "../../IExecutionDb.h"
#include "../../Jobs/JobEventOrchestration.h"
#include "../../../Analysis/IAnalysisDb.h"
#include "../../../Authoring/IAuthoringDb.h"
#include "../../../Common/Types/UtcTimestamp.h"
#include "../../../State/IStateDb.h"
#include "../../../../SavorCore/Core/Input/BattleInputTraceBlob.h"
#include "../../../../SavorCore/Core/Input/InputPlan.h"
#include "../../../../SavorCore/Core/Input/SoaBattle/ActionTypes.h"
#include "../../../../SavorCore/Core/Input/SoaBattle/BattleCommandCodec.h"
#include "../../../../SavorCore/Core/Input/SoaBattle/PlanWriter.h"
#include "../../../../SavorCore/Core/Memory/Soa/Battle/BattleContextCodec.h"
#include "../../../../SavorCore/Phases/Programs/BattleRunner/BattleOutcome.h"
#include "../../../../SavorCore/Phases/Programs/BattleTurnRunner/BattleTurnRunnerPayload.h"
#include "../../../../SavorCore/Runner/IPC/Wire.h"
#include "../../../../SavorCore/Runner/Parallel/PRTypes.h"
#include "../../../../SavorCore/Runner/Script/CtxRegistry.h"
#include "../../../../SavorCore/Utils/Base64.h"
#include "../../../../SavorCore/Utils/Hash.h"
#include "../../../../SavorCore/Utils/IniDoc.h"

namespace savor::db::execution::programdb::battle {
namespace {

constexpr const char* kBattleSingleTurnStepKind = "battle.single_turn";
constexpr const char* kWaveRefKind = "analysis_battle.turn_wave";
constexpr const char* kTurnJobRefKind = "analysis_battle.turn_job";
constexpr const char* kJobSection = "BattleSingleTurn.Job";
constexpr const char* kResultsSection = "BattleSingleTurn.Results";
constexpr std::int32_t kProgramVersion = phase::battle::turnrunner::PayloadVersion;

struct JobIni {
    std::int64_t wave_id = 0;
    std::int64_t plan_id = 0;
    std::int64_t seed_candidate_id = 0;
    std::int64_t savestate_id = 0;
    int turn_index = 1;
    int fake_attacks_used_before = 0;
    int fake_attacks_this_turn = 0;
    std::string action_key;
    std::string resolved_turn_commands_blob;
    std::string resolved_turn_variant_key;
    std::string capture_profile_path;
    std::optional<std::uint32_t> run_ms_override;
    std::optional<std::uint32_t> override_start_rng_seed;

    void set_section(IniDoc& ini) const {
        ini.set(kJobSection, "wave_id", std::to_string(wave_id));
        ini.set(kJobSection, "plan_id", std::to_string(plan_id));
        ini.set(kJobSection, "seed_candidate_id", std::to_string(seed_candidate_id));
        ini.set(kJobSection, "savestate_id", std::to_string(savestate_id));
        ini.set(kJobSection, "turn_index", std::to_string(turn_index));
        ini.set(kJobSection, "fake_attacks_used_before", std::to_string(fake_attacks_used_before));
        ini.set(kJobSection, "fake_attacks_this_turn", std::to_string(fake_attacks_this_turn));
        ini.set(kJobSection, "action_key", action_key);
        ini.set(kJobSection, "resolved_turn_commands_blob", resolved_turn_commands_blob);
        ini.set(kJobSection, "resolved_turn_variant_key", resolved_turn_variant_key);
        if (!capture_profile_path.empty()) {
            ini.set(kJobSection, "capture_profile_path", capture_profile_path);
        }
        if (run_ms_override.has_value()) {
            ini.set(kJobSection, "run_ms_override", std::to_string(*run_ms_override));
        }
        if (override_start_rng_seed.has_value()) {
            ini.set(kJobSection, "override_start_rng_seed", std::to_string(*override_start_rng_seed));
        }
        // Compatibility for queued jobs created before the resolved-command terminology.
        ini.set(kJobSection, "concrete_turn_plan_hex", resolved_turn_commands_blob);
        ini.set(kJobSection, "target_variant_key", resolved_turn_variant_key);
    }

    static JobIni parse(const std::string& text) {
        const auto ini = IniDoc::parse(text);
        JobIni out{};
        out.wave_id = ini.get_i64(kJobSection, "wave_id", 0);
        out.plan_id = ini.get_i64(kJobSection, "plan_id", 0);
        out.seed_candidate_id = ini.get_i64(kJobSection, "seed_candidate_id", 0);
        out.savestate_id = ini.get_i64(kJobSection, "savestate_id", 0);
        out.turn_index = static_cast<int>(ini.get_i64(kJobSection, "turn_index", 1));
        out.fake_attacks_used_before = static_cast<int>(ini.get_i64(kJobSection, "fake_attacks_used_before", 0));
        out.fake_attacks_this_turn = static_cast<int>(ini.get_i64(kJobSection, "fake_attacks_this_turn", 0));
        out.action_key = ini.get(kJobSection, "action_key", "");
        out.resolved_turn_commands_blob = ini.get(kJobSection, "resolved_turn_commands_blob", "");
        if (out.resolved_turn_commands_blob.empty()) {
            out.resolved_turn_commands_blob = ini.get(kJobSection, "concrete_turn_plan_hex", "");
        }
        out.resolved_turn_variant_key = ini.get(kJobSection, "resolved_turn_variant_key", "");
        if (out.resolved_turn_variant_key.empty()) {
            out.resolved_turn_variant_key = ini.get(kJobSection, "target_variant_key", "");
        }
        out.capture_profile_path = ini.get(kJobSection, "capture_profile_path", "");
        if (ini.has(kJobSection, "run_ms_override")) {
            out.run_ms_override = ini.get_u32(kJobSection, "run_ms_override", 0);
        }
        if (ini.has(kJobSection, "override_start_rng_seed")) {
            out.override_start_rng_seed = ini.get_u32(kJobSection, "override_start_rng_seed", 0);
        }
        return out;
    }
};

struct ResultsIni {
    std::uint32_t w_err = 0;
    std::uint32_t dw_err = 0;
    std::uint32_t vi_start = 0;
    std::uint32_t vi_end = 0;
    std::uint32_t rng_seed = 0;
    std::uint32_t battle_outcome = static_cast<std::uint32_t>(savor::battle::Outcome::Unknown);
    std::uint32_t plan_materialize_err = 0;
    std::uint32_t fake_attacks_used = 0;
    std::uint32_t pred_passed = 0;
    std::uint32_t pred_total = 0;
    std::uint32_t pred_abort_run = 0;
    std::uint32_t macro_failure_code = 0;
    std::uint32_t macro_step_count = 0;
    std::uint32_t macro_last_step_index = 0;
    std::uint32_t macro_last_expected_bp = 0;
    std::uint32_t macro_last_hit_bp = 0;
    std::uint32_t macro_last_hit_pc = 0;
    std::uint32_t rng_override_enabled = 0;
    std::uint32_t rng_override_seed = 0;
    std::uint32_t rng_original_seed = 0;
    std::uint32_t rng_applied_seed = 0;
    std::uint32_t memwrite_status = 0;
    std::uint32_t memwrite_readback = 0;
    std::int64_t applied_input_artifact_id = 0;
    std::int64_t input_trace_artifact_id = 0;
    std::int64_t capture_artifact_id = 0;
    std::string input_trace_blob;
    std::string capture_output_path;
    std::string savestate_path;
    std::int64_t output_savestate_id = 0;
    std::string context_blob_base64;
    int context_version = 0;

    std::string to_ini() const {
        IniDoc ini;
        ini.set(kResultsSection, "w_err", std::to_string(w_err));
        ini.set(kResultsSection, "dw_err", std::to_string(dw_err));
        ini.set(kResultsSection, "vi_start", std::to_string(vi_start));
        ini.set(kResultsSection, "vi_end", std::to_string(vi_end));
        ini.set(kResultsSection, "rng_seed", std::to_string(rng_seed));
        ini.set(kResultsSection, "battle_outcome", std::to_string(battle_outcome));
        ini.set(kResultsSection, "plan_materialize_err", std::to_string(plan_materialize_err));
        ini.set(kResultsSection, "fake_attacks_used", std::to_string(fake_attacks_used));
        ini.set(kResultsSection, "pred_passed", std::to_string(pred_passed));
        ini.set(kResultsSection, "pred_total", std::to_string(pred_total));
        ini.set(kResultsSection, "pred_abort_run", std::to_string(pred_abort_run));
        ini.set(kResultsSection, "macro_failure_code", std::to_string(macro_failure_code));
        ini.set(kResultsSection, "macro_step_count", std::to_string(macro_step_count));
        ini.set(kResultsSection, "macro_last_step_index", std::to_string(macro_last_step_index));
        ini.set(kResultsSection, "macro_last_expected_bp", std::to_string(macro_last_expected_bp));
        ini.set(kResultsSection, "macro_last_hit_bp", std::to_string(macro_last_hit_bp));
        ini.set(kResultsSection, "macro_last_hit_pc", std::to_string(macro_last_hit_pc));
        ini.set(kResultsSection, "rng_override_enabled", std::to_string(rng_override_enabled));
        ini.set(kResultsSection, "rng_override_seed", std::to_string(rng_override_seed));
        ini.set(kResultsSection, "rng_original_seed", std::to_string(rng_original_seed));
        ini.set(kResultsSection, "rng_applied_seed", std::to_string(rng_applied_seed));
        ini.set(kResultsSection, "memwrite_status", std::to_string(memwrite_status));
        ini.set(kResultsSection, "memwrite_readback", std::to_string(memwrite_readback));
        ini.set(kResultsSection, "applied_input_artifact_id", std::to_string(applied_input_artifact_id));
        ini.set(kResultsSection, "input_trace_artifact_id", std::to_string(input_trace_artifact_id));
        ini.set(kResultsSection, "capture_artifact_id", std::to_string(capture_artifact_id));
        ini.set(kResultsSection, "input_trace_blob", input_trace_blob);
        ini.set(kResultsSection, "applied_input_tape_text", input_trace_blob);
        if (!capture_output_path.empty()) {
            ini.set(kResultsSection, "capture_output_path", capture_output_path);
        }
        ini.set(kResultsSection, "savestate_path", savestate_path);
        ini.set(kResultsSection, "output_savestate_id", std::to_string(output_savestate_id));
        if (!context_blob_base64.empty()) {
            ini.set(kResultsSection, "context_blob_base64", context_blob_base64);
            ini.set(kResultsSection, "context_version", std::to_string(context_version));
        }
        return ini.to_string_sorted();
    }

    static ResultsIni parse(const std::string& text) {
        const auto ini = IniDoc::parse(text);
        ResultsIni out{};
        out.w_err = ini.get_u32(kResultsSection, "w_err", 0);
        out.dw_err = ini.get_u32(kResultsSection, "dw_err", 0);
        out.vi_start = ini.get_u32(kResultsSection, "vi_start", 0);
        out.vi_end = ini.get_u32(kResultsSection, "vi_end", 0);
        out.rng_seed = ini.get_u32(kResultsSection, "rng_seed", 0);
        out.battle_outcome = ini.get_u32(kResultsSection, "battle_outcome", out.battle_outcome);
        out.plan_materialize_err = ini.get_u32(kResultsSection, "plan_materialize_err", 0);
        out.fake_attacks_used = ini.get_u32(kResultsSection, "fake_attacks_used", 0);
        out.pred_passed = ini.get_u32(kResultsSection, "pred_passed", 0);
        out.pred_total = ini.get_u32(kResultsSection, "pred_total", 0);
        out.pred_abort_run = ini.get_u32(kResultsSection, "pred_abort_run", 0);
        out.macro_failure_code = ini.get_u32(kResultsSection, "macro_failure_code", 0);
        out.macro_step_count = ini.get_u32(kResultsSection, "macro_step_count", 0);
        out.macro_last_step_index = ini.get_u32(kResultsSection, "macro_last_step_index", 0);
        out.macro_last_expected_bp = ini.get_u32(kResultsSection, "macro_last_expected_bp", 0);
        out.macro_last_hit_bp = ini.get_u32(kResultsSection, "macro_last_hit_bp", 0);
        out.macro_last_hit_pc = ini.get_u32(kResultsSection, "macro_last_hit_pc", 0);
        out.rng_override_enabled = ini.get_u32(kResultsSection, "rng_override_enabled", 0);
        out.rng_override_seed = ini.get_u32(kResultsSection, "rng_override_seed", 0);
        out.rng_original_seed = ini.get_u32(kResultsSection, "rng_original_seed", 0);
        out.rng_applied_seed = ini.get_u32(kResultsSection, "rng_applied_seed", 0);
        out.memwrite_status = ini.get_u32(kResultsSection, "memwrite_status", 0);
        out.memwrite_readback = ini.get_u32(kResultsSection, "memwrite_readback", 0);
        out.applied_input_artifact_id = ini.get_i64(kResultsSection, "applied_input_artifact_id", 0);
        out.input_trace_artifact_id = ini.get_i64(kResultsSection, "input_trace_artifact_id", 0);
        out.capture_artifact_id = ini.get_i64(kResultsSection, "capture_artifact_id", 0);
        out.input_trace_blob = ini.get(kResultsSection, "input_trace_blob", "");
        if (out.input_trace_blob.empty()) {
            out.input_trace_blob = ini.get(kResultsSection, "applied_input_tape_text", "");
        }
        out.capture_output_path = ini.get(kResultsSection, "capture_output_path", "");
        out.savestate_path = ini.get(kResultsSection, "savestate_path", "");
        out.output_savestate_id = ini.get_i64(kResultsSection, "output_savestate_id", 0);
        out.context_blob_base64 = ini.get(kResultsSection, "context_blob_base64", "");
        out.context_version = static_cast<int>(ini.get_i64(kResultsSection, "context_version", 0));
        return out;
    }
};

std::filesystem::path WorkingRoot(const std::filesystem::path& configured) {
    if (!configured.empty()) {
        return configured;
    }
    return std::filesystem::temp_directory_path() / "savordb-battle-single-turn";
}

std::int64_t FileSize(const std::filesystem::path& path) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    return ec ? 0 : static_cast<std::int64_t>(size);
}

std::uint32_t ClampU32(std::int64_t value) {
    if (value <= 0) {
        return 0;
    }
    if (value > static_cast<std::int64_t>(std::numeric_limits<std::uint32_t>::max())) {
        return std::numeric_limits<std::uint32_t>::max();
    }
    return static_cast<std::uint32_t>(value);
}

std::uint8_t ClampByte(int value) {
    return static_cast<std::uint8_t>(std::clamp(value, 0, 255));
}

std::string BytesToHex(const std::vector<std::uint8_t>& bytes) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (const auto byte : bytes) {
        out.push_back(kHex[(byte >> 4) & 0x0f]);
        out.push_back(kHex[byte & 0x0f]);
    }
    return out;
}

int HexNibble(char ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return 10 + ch - 'a';
    if (ch >= 'A' && ch <= 'F') return 10 + ch - 'A';
    return -1;
}

std::optional<std::vector<std::uint8_t>> HexToBytes(const std::string& hex) {
    if (hex.size() % 2 != 0) {
        return std::nullopt;
    }
    std::vector<std::uint8_t> out;
    out.reserve(hex.size() / 2);
    for (std::size_t i = 0; i < hex.size(); i += 2) {
        const int hi = HexNibble(hex[i]);
        const int lo = HexNibble(hex[i + 1]);
        if (hi < 0 || lo < 0) {
            return std::nullopt;
        }
        out.push_back(static_cast<std::uint8_t>((hi << 4) | lo));
    }
    return out;
}

std::string ActionKey(const savor::db::BattlePlanTurnSnapshot& turn) {
    std::ostringstream raw;
    for (const auto& action : turn.actions) {
        const auto& preset = action.action_preset;
        raw << action.actor_slot << ':'
            << action.action_preset_id << ':'
            << static_cast<int>(preset.macro) << ':'
            << static_cast<int>(preset.target_kind) << ':'
            << preset.target_mask_bits.value_or(-1) << ':'
            << preset.target_single_slot.value_or(-1) << ':'
            << preset.target_same_as_actor_slot.value_or(-1) << ':'
            << preset.target_expr_ini.value_or("") << ':'
            << preset.item_id.value_or(-1) << '|';
    }
    const auto text = raw.str();
    return hash::sha256(text.data(), text.size());
}

const savor::db::BattlePlanTurnSnapshot* FindTurn(
    const savor::db::BattlePlanSnapshot& plan,
    int turn_index) {
    auto it = std::find_if(plan.turns.begin(), plan.turns.end(), [turn_index](const auto& turn) {
        return turn.turn_index == turn_index;
    });
    return it == plan.turns.end() ? nullptr : &*it;
}

soa::battle::actions::TurnPlan BuildTurnPlan(
    const savor::db::BattlePlanTurnSnapshot& turn,
    int fake_attacks_this_turn) {
    soa::battle::actions::TurnPlan out{};
    out.fake_attack_count = static_cast<std::uint32_t>(std::max(0, fake_attacks_this_turn));
    for (const auto& action : turn.actions) {
        const auto& preset = action.action_preset;
        soa::battle::actions::BattleCommand ap{};
        ap.actor_slot = static_cast<std::uint8_t>(std::clamp(action.actor_slot, 0, 255));
        ap.macro = preset.macro;
        if (preset.target_kind == savor::db::BattlePlanTargetKind::SingleEnemy
            && preset.target_single_slot.has_value()) {
            ap.params.target_slot = static_cast<std::uint8_t>(std::clamp(*preset.target_single_slot, -1, 255));
        } else {
            ap.params.target_slot = 0xFF;
        }
        if (preset.item_id.has_value()) {
            ap.params.item_id = static_cast<std::uint16_t>(std::clamp(*preset.item_id, 0, 0xFFFF));
        }
        out.commands.push_back(ap);
    }
    return out;
}

bool ActionNeedsTarget(soa::battle::actions::BattleAction action) {
    return action == soa::battle::actions::BattleAction::Attack
        || action == soa::battle::actions::BattleAction::UseItem;
}

bool IsAliveEnemySlot(const soa::battle::ctx::BattleContext& context, int slot) {
    return slot >= 4
        && slot <= 11
        && context.slots_[slot].present == 1
        && context.slots_[slot].is_alive == 1;
}

std::vector<int> PresentEnemySlots(const soa::battle::ctx::BattleContext& context) {
    std::vector<int> slots;
    for (int slot = 4; slot <= 11; ++slot) {
        if (IsAliveEnemySlot(context, slot)) {
            slots.push_back(slot);
        }
    }
    return slots;
}

std::vector<int> EnemySlotsFromMask(
    const soa::battle::ctx::BattleContext& context,
    int mask_bits) {
    std::vector<int> slots;
    for (int slot = 4; slot <= 11; ++slot) {
        if ((mask_bits & (1 << slot)) != 0 && IsAliveEnemySlot(context, slot)) {
            slots.push_back(slot);
        }
    }
    return slots;
}

std::vector<int> EnemySlotsByKind(
    const soa::battle::ctx::BattleContext& context,
    const std::string& target_expr_ini) {
    if (target_expr_ini.empty()) {
        return {};
    }
    const auto ini = IniDoc::parse(target_expr_ini);
    if (ini.get("target", "kind", "") != "ByEnemyKind") {
        return {};
    }
    const int enemy_kind_id = static_cast<int>(ini.get_i64("target", "enemy_kind_id", -1));
    const std::string quantifier = ini.get("target", "quantifier", "Any");
    if (enemy_kind_id < 0 || (quantifier != "Any" && quantifier != "First")) {
        return {};
    }

    std::vector<int> slots;
    for (int slot = 4; slot <= 11; ++slot) {
        if (IsAliveEnemySlot(context, slot) && static_cast<int>(context.slots_[slot].id) == enemy_kind_id) {
            slots.push_back(slot);
            if (quantifier == "First") {
                break;
            }
        }
    }
    return slots;
}

std::vector<int> TargetDomain(
    const soa::battle::ctx::BattleContext& context,
    const savor::db::BattlePlanActionSnapshot& action) {
    const auto& preset = action.action_preset;
    if (preset.target_expr_ini.has_value() && !preset.target_expr_ini->empty()) {
        return EnemySlotsByKind(context, *preset.target_expr_ini);
    }
    switch (preset.target_kind) {
    case savor::db::BattlePlanTargetKind::SingleEnemy: {
        const int slot = preset.target_single_slot.value_or(-1);
        return IsAliveEnemySlot(context, slot)
            ? std::vector<int>{ slot }
            : std::vector<int>{};
    }
    case savor::db::BattlePlanTargetKind::MultipleEnemies: {
        const int mask_bits = preset.target_mask_bits.value_or(0);
        return EnemySlotsFromMask(context, mask_bits);
    }
    case savor::db::BattlePlanTargetKind::AnyEnemy:
        return PresentEnemySlots(context);
    case savor::db::BattlePlanTargetKind::SameAsOtherPC:
        return {};
    default:
        return {};
    }
}

std::optional<int> SameAsActorSlot(const savor::db::BattlePlanActionSnapshot& action) {
    const auto& preset = action.action_preset;
    if (preset.target_kind != savor::db::BattlePlanTargetKind::SameAsOtherPC) {
        return std::nullopt;
    }
    const int actor = preset.target_same_as_actor_slot.value_or(-1);
    return actor >= 0 && actor <= 3 ? std::optional<int>(actor) : std::nullopt;
}

std::optional<int> ResolveAssignedTargetForActor(
    int actor_slot,
    const std::map<int, int>& direct_target_by_actor,
    const std::map<int, int>& same_as_by_actor,
    std::vector<int>& visiting) {
    if (const auto direct_it = direct_target_by_actor.find(actor_slot); direct_it != direct_target_by_actor.end()) {
        return direct_it->second;
    }
    if (std::find(visiting.begin(), visiting.end(), actor_slot) != visiting.end()) {
        return std::nullopt;
    }
    const auto same_it = same_as_by_actor.find(actor_slot);
    if (same_it == same_as_by_actor.end()) {
        return std::nullopt;
    }
    visiting.push_back(actor_slot);
    const auto resolved = ResolveAssignedTargetForActor(same_it->second, direct_target_by_actor, same_as_by_actor, visiting);
    visiting.pop_back();
    return resolved;
}

std::optional<soa::battle::actions::TurnPlan> DecodeTurnPlanSpecHex(
    const std::string& hex,
    int fake_attacks_this_turn) {
    const auto commands = soa::battle::actions::decode_battle_turn_commands_hex(hex);
    if (!commands.has_value()) {
        return std::nullopt;
    }
    soa::battle::actions::TurnPlan plan{};
    plan.fake_attack_count = static_cast<std::uint32_t>(std::max(0, fake_attacks_this_turn));
    plan.commands = *commands;
    return plan;
}

std::vector<soa::battle::actions::TurnPlanSpec> CompileConcreteTurnSpecs(
    const soa::battle::ctx::BattleContext& context,
    const savor::db::BattlePlanTurnSnapshot& turn) {
    struct PlannedAction {
        savor::db::BattlePlanActionSnapshot source;
        soa::battle::actions::BattleCommand base{};
        bool needs_target = false;
        std::vector<int> domain;
        std::optional<int> same_as_actor;
    };

    std::vector<PlannedAction> planned;
    planned.reserve(turn.actions.size());
    for (const auto& action : turn.actions) {
        const auto& preset = action.action_preset;
        PlannedAction item{};
        item.source = action;
        item.base.actor_slot = static_cast<std::uint8_t>(std::clamp(action.actor_slot, 0, 255));
        item.base.macro = preset.macro;
        item.base.params.target_slot = 0xFF;
        if (preset.item_id.has_value()) {
            item.base.params.item_id = static_cast<std::uint16_t>(std::clamp(*preset.item_id, 0, 0xFFFF));
        }
        item.needs_target = ActionNeedsTarget(item.base.macro);
        if (item.needs_target) {
            item.same_as_actor = SameAsActorSlot(action);
            if (!item.same_as_actor.has_value()) {
                item.domain = TargetDomain(context, action);
                if (item.domain.empty()) {
                    return {};
                }
            }
        }
        planned.push_back(std::move(item));
    }

    std::vector<soa::battle::actions::TurnPlanSpec> specs;
    std::vector<soa::battle::actions::BattleCommand> current(turn.actions.size());
    std::map<int, int> direct_target_by_actor;

    const auto emit_if_resolved = [&]() -> std::optional<soa::battle::actions::TurnPlanSpec> {
        std::map<int, int> same_as_by_actor;
        for (const auto& item : planned) {
            if (item.same_as_actor.has_value()) {
                same_as_by_actor[item.source.actor_slot] = *item.same_as_actor;
            }
        }
        soa::battle::actions::TurnPlanSpec spec;
        spec.reserve(planned.size());
        for (std::size_t index = 0; index < planned.size(); ++index) {
            auto action = current[index];
            if (planned[index].needs_target && planned[index].same_as_actor.has_value()) {
                std::vector<int> visiting;
                const auto resolved = ResolveAssignedTargetForActor(
                    planned[index].source.actor_slot,
                    direct_target_by_actor,
                    same_as_by_actor,
                    visiting);
                if (!resolved.has_value()) {
                    return std::nullopt;
                }
                action.params.target_slot = static_cast<std::uint8_t>(*resolved);
            }
            spec.push_back(action);
        }
        return spec;
    };

    const auto recurse = [&](const auto& self, std::size_t index) -> void {
        if (index >= planned.size()) {
            if (auto spec = emit_if_resolved(); spec.has_value()) {
                specs.push_back(std::move(*spec));
            }
            return;
        }
        const auto& item = planned[index];
        current[index] = item.base;
        if (!item.needs_target || item.same_as_actor.has_value()) {
            self(self, index + 1);
            return;
        }
        for (const int target_slot : item.domain) {
            current[index] = item.base;
            current[index].params.target_slot = static_cast<std::uint8_t>(target_slot);
            direct_target_by_actor[item.source.actor_slot] = target_slot;
            self(self, index + 1);
            direct_target_by_actor.erase(item.source.actor_slot);
        }
    };
    recurse(recurse, 0);
    return specs;
}

std::vector<savor::pred::Spec> BuildPredicates(
    const savor::db::PredicateSetSnapshot* predicate_set,
    savor::db::IAuthoringDb* authoring_db) {
    std::vector<savor::pred::Spec> out;
    if (predicate_set == nullptr) {
        return out;
    }
    const auto to_addr_key = [](std::int64_t value) -> std::optional<addr::AddrKey> {
        if (value < 0 || value > static_cast<std::int64_t>(std::numeric_limits<std::uint16_t>::max())) {
            return std::nullopt;
        }
        const auto key = static_cast<addr::AddrKey>(static_cast<std::uint16_t>(value));
        return addr::AddrRegistry::exists(key) ? std::optional<addr::AddrKey>(key) : std::nullopt;
    };
    std::uint16_t ordinal = 0;
    for (const auto& pred : predicate_set->predicates) {
        savor::pred::Spec spec{};
        spec.id = ordinal++;
        spec.required_bps.reserve(pred.required_breakpoint_ids.size());
        for (const auto bp_key : pred.required_breakpoint_ids) {
            if (bp_key != 0) {
                spec.required_bps.push_back(static_cast<std::uint16_t>(bp_key));
            }
        }
        spec.required_bp = spec.required_bps.empty()
            ? pred.breakpoint_id
            : static_cast<BPKey>(spec.required_bps.front());
        spec.width = static_cast<std::uint8_t>(pred.width);
        spec.cmp = pred.cmp_op;
        spec.flags = static_cast<std::uint32_t>(pred.flag_mask.value_or(0));
        spec.lhs_addr = static_cast<std::uint32_t>(pred.lhs_value);
        spec.rhs_value = static_cast<std::uint64_t>(pred.rhs_value);
        for (const auto bp_key : pred.baseline_breakpoint_ids) {
            spec.baseline_bps.push_back(static_cast<std::uint16_t>(bp_key));
        }
        spec.turn_mask = static_cast<std::uint32_t>(pred.value_mask.value_or(0xFFFFFFFF));
        if (spec.has_flag(savor::pred::PredFlag::LhsIsKey)) {
            spec.lhs_key = to_addr_key(pred.lhs_value);
        }
        if (spec.has_flag(savor::pred::PredFlag::RhsIsKey)) {
            spec.rhs_key = to_addr_key(pred.rhs_value);
        }
        if (pred.abort_on_fail) {
            spec.set_flag(savor::pred::PredFlag::AbortOnFail);
        }
        spec.name = pred.name.empty() ? "unnamed" : pred.name;
        spec.desc = pred.name;
        if (authoring_db != nullptr && pred.lhs_address_program_id.has_value()) {
            if (const auto program = authoring_db->GetAddressProgram(*pred.lhs_address_program_id); program.has_value()) {
                spec.lhs_prog = program->prog_bytes;
                spec.lhs_prog_desc = program->description;
                spec.set_flag(savor::pred::PredFlag::LhsIsProg);
            }
        }
        if (authoring_db != nullptr && pred.rhs_address_program_id.has_value()) {
            if (const auto program = authoring_db->GetAddressProgram(*pred.rhs_address_program_id); program.has_value()) {
                spec.rhs_prog = program->prog_bytes;
                spec.rhs_prog_desc = program->description;
                spec.set_flag(savor::pred::PredFlag::RhsIsProg);
            }
        }
        out.push_back(std::move(spec));
    }
    return out;
}

savor::GCInputFrame InitialFrameFromUniqueSeed(const savor::db::SeedProbeUniqueSeedRow& seed) {
    savor::GCInputFrame frame{};
    frame.main_x = ClampByte(seed.main_x);
    frame.main_y = ClampByte(seed.main_y);
    frame.c_x = ClampByte(seed.cstick_x);
    frame.c_y = ClampByte(seed.cstick_y);
    frame.trig_l = ClampByte(seed.trigger_x);
    frame.trig_r = ClampByte(seed.trigger_y);
    return frame;
}

savor::GCInputFrame InitialFrameFromInputSetFrame(const savor::db::AnalysisInputSetFrameRow& seed) {
    savor::GCInputFrame frame{};
    frame.main_x = ClampByte(seed.main_x);
    frame.main_y = ClampByte(seed.main_y);
    frame.c_x = ClampByte(seed.cstick_x);
    frame.c_y = ClampByte(seed.cstick_y);
    frame.trig_l = ClampByte(seed.trigger_x);
    frame.trig_r = ClampByte(seed.trigger_y);
    return frame;
}

std::string BuildInputIni(const JobIni& job) {
    IniDoc ini;
    job.set_section(ini);
    return ini.to_string_sorted();
}

std::string FingerprintFor(const JobIni& job) {
    const auto ini = BuildInputIni(job);
    return "PK=" + std::to_string(savor::PK_BattleSingleTurnRunner)
        + ";PV=" + std::to_string(kProgramVersion)
        + ";phase=battle.single_turn;sha=" + hash::sha256(ini.data(), ini.size());
}

bool AppendJobCompleted(
    savor::db::IExecutionDb* execution_db,
    std::int64_t job_id,
    const char* terminal_state,
    std::vector<std::string>* lines) {
    std::ostringstream event;
    event << "[battle-single-turn-job-terminal-state] job=" << job_id
          << " terminal_state=" << terminal_state;
    if (execution_db == nullptr || execution_db->JobCommandService() == nullptr || job_id <= 0) {
        event << " ok=false error=execution_db_unavailable";
        if (lines) lines->push_back(event.str());
        return false;
    }
    std::string error;
    const bool ok = execution_db->JobCommandService()->AppendLifecycleEvent(
        {
            .kind = savor::db::execution::jobs::JobLifecycleEventKind::JobCompleted,
            .job_id = job_id,
            .terminal_state = std::string(terminal_state),
            .requested_by = "battle_single_turn_result_mapper",
        },
        &error);
    event << " ok=" << (ok ? "true" : "false");
    if (!ok) event << " error=" << error;
    if (lines) lines->push_back(event.str());
    return ok;
}

bool IsContinuationOutcome(savor::battle::Outcome outcome) {
    return outcome == savor::battle::Outcome::ReachedNextTurn
        || outcome == savor::battle::Outcome::Victory;
}

bool IsVictory(savor::battle::Outcome outcome) {
    return outcome == savor::battle::Outcome::Victory;
}

bool IsBadMaterialize(const ResultsIni& results) {
    return results.battle_outcome == static_cast<std::uint32_t>(savor::battle::Outcome::PlanMaterializeFailure);
}

class BattleSingleTurnJobPersistenceAdapter final : public IJobPersistenceAdapter {
public:
    BattleSingleTurnJobPersistenceAdapter(
        savor::db::IExecutionDb* execution_db,
        savor::db::IAnalysisDb* analysis_db,
        savor::db::IAuthoringDb* authoring_db)
        : execution_db_(execution_db)
        , analysis_db_(analysis_db)
        , authoring_db_(authoring_db) {
    }

    WorkflowStepScheduleResult EncodeForQueueing(const WorkflowStepScheduleContext& context) const override {
        WorkflowStepScheduleResult scheduled{};
        const std::int64_t domain_ref_id = context.domain_ref_id;
        scheduled.persistence.program_ref_kind = kWaveRefKind;
        scheduled.persistence.program_ref_id = domain_ref_id;
        scheduled.persistence.program_version = kProgramVersion;
        scheduled.persistence.fingerprint = "battle.single_turn.wave." + std::to_string(domain_ref_id);

        if (execution_db_ == nullptr || analysis_db_ == nullptr || authoring_db_ == nullptr || domain_ref_id <= 0) {
            scheduled.event_lines.push_back("[battle-single-turn-enqueue] ok=false error=db_unavailable");
            return scheduled;
        }
        const auto wave = analysis_db_->GetBattleTurnWave(domain_ref_id);
        if (!wave.has_value()) {
            scheduled.event_lines.push_back("[battle-single-turn-enqueue] ok=false error=wave_not_found");
            return scheduled;
        }
        const auto battle_set = analysis_db_->GetBattleSet(wave->battle_set_id);
        const auto run_spec = battle_set.has_value() ? authoring_db_->GetBattleRunSpec(battle_set->battle_run_spec_id) : std::nullopt;
        const auto settings = battle_set.has_value() ? authoring_db_->GetExplorerSettings(battle_set->explorer_settings_id) : std::nullopt;
        if (!battle_set.has_value() || !run_spec.has_value() || !settings.has_value() || !settings->default_plan_id.has_value()) {
            scheduled.event_lines.push_back("[battle-single-turn-enqueue] ok=false error=authoring_context_missing");
            return scheduled;
        }
        const auto plan = authoring_db_->GetBattlePlan(*settings->default_plan_id);
        if (!plan.has_value()) {
            scheduled.event_lines.push_back("[battle-single-turn-enqueue] ok=false error=plan_missing");
            return scheduled;
        }
        const auto* turn = FindTurn(*plan, wave->turn_index);
        if (turn == nullptr) {
            scheduled.event_lines.push_back("[battle-single-turn-enqueue] ok=false error=turn_missing");
            return scheduled;
        }
        std::int64_t source_savestate_id = battle_set->entry_savestate_id;
        int fake_used_before = 0;
        std::optional<std::int64_t> parent_exec_job_id;
        std::optional<std::string> context_blob;
        if (wave->parent_turn_job_id.has_value()) {
            const auto parent_jobs = analysis_db_->ListBattleTurnJobsForBattleTurn(battle_set->battle_set_id, wave->turn_index - 1);
            auto parent_it = std::find_if(parent_jobs.begin(), parent_jobs.end(), [&](const auto& row) {
                return row.turn_job_id == *wave->parent_turn_job_id;
            });
            if (parent_it == parent_jobs.end() || !parent_it->output_savestate_id.has_value()) {
                scheduled.event_lines.push_back("[battle-single-turn-enqueue] ok=false error=parent_turn_job_missing_output");
                return scheduled;
            }
            source_savestate_id = *parent_it->output_savestate_id;
            fake_used_before = parent_it->fake_attacks_used_before + parent_it->fake_attacks_this_turn;
            parent_exec_job_id = parent_it->exec_job_id;
            if (!parent_it->result_context_blob_base64.has_value() || parent_it->result_context_blob_base64->empty()) {
                scheduled.event_lines.push_back("[battle-single-turn-enqueue] ok=false error=parent_turn_job_context_missing");
                return scheduled;
            }
            const auto decoded = savor::utils::Base64Decode(*parent_it->result_context_blob_base64);
            if (!decoded.has_value() || decoded->empty()) {
                scheduled.event_lines.push_back("[battle-single-turn-enqueue] ok=false error=parent_turn_job_context_base64_decode_failed");
                return scheduled;
            }
            context_blob = *decoded;
        } else {
            const auto context_probe = wave->context_probe_id.has_value()
                ? analysis_db_->GetBattleContextProbe(*wave->context_probe_id)
                : analysis_db_->GetLatestBattleContextForWave(wave->wave_id);
            if (!context_probe.has_value() || !context_probe->context_blob.has_value() || context_probe->context_blob->empty()) {
                scheduled.event_lines.push_back("[battle-single-turn-enqueue] ok=false error=context_probe_missing");
                return scheduled;
            }
            context_blob = *context_probe->context_blob;
        }

        soa::battle::ctx::BattleContext battle_context{};
        if (!context_blob.has_value() || !soa::battle::ctx::codec::decode(*context_blob, battle_context)) {
            scheduled.event_lines.push_back("[battle-single-turn-enqueue] ok=false error=context_decode_failed");
            return scheduled;
        }
        const auto concrete_specs = CompileConcreteTurnSpecs(battle_context, *turn);
        if (concrete_specs.empty()) {
            scheduled.event_lines.push_back("[battle-single-turn-enqueue] ok=false error=no_valid_target_variants");
            return scheduled;
        }

        const int max_fake = std::max(battle_set->launch_fake_attack_min, battle_set->launch_fake_attack_max);
        const int min_fake = std::min(battle_set->launch_fake_attack_min, battle_set->launch_fake_attack_max);
        if (fake_used_before > max_fake) {
            scheduled.event_lines.push_back("[battle-single-turn-enqueue] ok=false error=fake_budget_exhausted");
            return scheduled;
        }
        const int remaining = max_fake - fake_used_before;
        const int needed_to_reach_min = std::max(0, min_fake - fake_used_before);
        const int fake_option_count = std::max(0, remaining - needed_to_reach_min + 1);
        const int expected_total = fake_option_count * static_cast<int>(concrete_specs.size());

        std::string error;
        std::int64_t job_set_id = 0;
        if (!execution_db_->CreateJobSet(
                {
                    .program_kind = static_cast<std::int32_t>(savor::PK_BattleSingleTurnRunner),
                    .purpose = "Battle Single Turn",
                    .created_by = "battle.single_turn.adapters",
                    .created_at_utc = savor::db::types::UtcNow().time_since_epoch().count(),
                    .expected_total = expected_total,
                    .domain_ref_kind = std::string(kWaveRefKind),
                    .domain_ref_id = wave->wave_id,
                    .meta_note = "battle_set=" + std::to_string(battle_set->battle_set_id)
                        + ";turn=" + std::to_string(wave->turn_index),
                },
                &job_set_id,
                &error)
            || job_set_id <= 0) {
            scheduled.event_lines.push_back("[battle-single-turn-enqueue] ok=false error=" + error);
            return scheduled;
        }
        scheduled.root_job_set_id = job_set_id;

        int jobs_enqueued = 0;
        for (std::size_t variant_index = 0; variant_index < concrete_specs.size(); ++variant_index) {
            const auto concrete_hex = soa::battle::actions::encode_battle_turn_commands_hex(concrete_specs[variant_index]);
            const auto variant_key = hash::sha256(concrete_hex.data(), concrete_hex.size());
            for (int fake = needed_to_reach_min; fake <= remaining; ++fake) {
                JobIni job_ini{};
                job_ini.wave_id = wave->wave_id;
                job_ini.plan_id = plan->plan_id;
                job_ini.seed_candidate_id = wave->seed_candidate_id;
                job_ini.savestate_id = source_savestate_id;
                job_ini.turn_index = wave->turn_index;
                job_ini.fake_attacks_used_before = fake_used_before;
                job_ini.fake_attacks_this_turn = fake;
                job_ini.action_key = ActionKey(*turn) + ":" + variant_key;
                job_ini.resolved_turn_commands_blob = concrete_hex;
                job_ini.resolved_turn_variant_key = variant_key;

                std::int64_t turn_job_id = 0;
                const auto now = savor::db::types::UtcNow();
                if (!analysis_db_->RecordBattleTurnJob(
                        {
                            .wave_id = wave->wave_id,
                            .plan_id = plan->plan_id,
                            .source_savestate_id = source_savestate_id,
                            .seed_candidate_id = wave->seed_candidate_id,
                            .authored_plan_id = plan->plan_id,
                            .authored_turn_index = wave->turn_index,
                            .resolved_turn_commands_blob = concrete_hex,
                            .resolved_turn_variant_key = variant_key,
                            .fake_attacks_this_turn = fake,
                            .fake_attacks_used_before = fake_used_before,
                            .job_state = savor::db::BattleTurnJobState::Queued,
                            .started_at_utc = now,
                            .recorded_at_utc = now,
                            .correlation_id = "battle-set-" + std::to_string(battle_set->battle_set_id),
                            .causation_id = "wave-" + std::to_string(wave->wave_id),
                        },
                        &turn_job_id,
                        &error)
                    || turn_job_id <= 0) {
                    scheduled.event_lines.push_back("[battle-single-turn-enqueue] warning=turn_job_record_failed error=" + error);
                    continue;
                }

                std::int64_t exec_job_id = 0;
                if (execution_db_->EnqueueJob(
                        {
                            .job_set_id = job_set_id,
                            .parent_job_id = parent_exec_job_id,
                            .program_kind = static_cast<std::int32_t>(savor::PK_BattleSingleTurnRunner),
                            .program_version = kProgramVersion,
                            .program_ref_kind = kTurnJobRefKind,
                            .program_ref_id = turn_job_id,
                            .savestate_id = source_savestate_id,
                            .fingerprint = FingerprintFor(job_ini),
                            .priority = context.step_priority,
                            .max_attempts = 1,
                            .input_ini = BuildInputIni(job_ini),
                            .pending_until_workflow_materialized = true,
                        },
                        &exec_job_id,
                        &error)
                    && exec_job_id > 0) {
                    (void)analysis_db_->SetBattleTurnJobExecJobId(turn_job_id, exec_job_id, &error);
                    ++jobs_enqueued;
                } else {
                    scheduled.event_lines.push_back("[battle-single-turn-enqueue] warning=enqueue_failed turn_job="
                        + std::to_string(turn_job_id) + " error=" + error);
                }
            }
        }

        (void)analysis_db_->UpdateBattleTurnWaveStatus(
            wave->wave_id,
            savor::db::BattleTurnWaveStatus::Running,
            std::nullopt,
            nullptr);
        scheduled.event_lines.push_back("[battle-single-turn-enqueue] ok=true wave="
            + std::to_string(wave->wave_id)
            + " target_variants=" + std::to_string(concrete_specs.size())
            + " jobs_enqueued=" + std::to_string(jobs_enqueued));
        return scheduled;
    }

    std::int64_t DecodeDomainRefId(const JobPersistenceRecord& persisted) const override {
        return persisted.program_ref_id;
    }

private:
    savor::db::IExecutionDb* execution_db_ = nullptr;
    savor::db::IAnalysisDb* analysis_db_ = nullptr;
    savor::db::IAuthoringDb* authoring_db_ = nullptr;
};

class BattleSingleTurnRuntimeInitAdapter final : public IRuntimeInitAdapter {
public:
    BattleSingleTurnRuntimeInitAdapter(
        savor::db::IExecutionDb* execution_db,
        savor::db::IAnalysisDb* analysis_db,
        savor::db::IAuthoringDb* authoring_db,
        std::filesystem::path working_dir_root)
        : execution_db_(execution_db)
        , analysis_db_(analysis_db)
        , authoring_db_(authoring_db)
        , working_dir_root_(std::move(working_dir_root)) {
    }

    RuntimeInitRequest BuildRuntimeInit(std::int64_t job_id) const override {
        RuntimeInitRequest request{};
        request.bootstrap_profile = "battle.single_turn";
        request.savestate_ref_kind = "state_savestate";
        request.derived_buffer_type = savor::DBuf::DK_Battle;
        request.default_timeout_ms = 10000;
        if (execution_db_ == nullptr) {
            return request;
        }
        const auto job = execution_db_->GetJob(job_id);
        if (!job.has_value()) {
            return request;
        }
        if (job->savestate_id.has_value()) {
            request.savestate_ref_id = *job->savestate_id;
        }
        const auto job_ini = JobIni::parse(job->input_ini);
        if (job_ini.run_ms_override.has_value() && *job_ini.run_ms_override > 0) {
            request.default_timeout_ms = static_cast<int>(std::min<std::uint32_t>(
                *job_ini.run_ms_override,
                static_cast<std::uint32_t>(std::numeric_limits<int>::max())));
        }
        if (analysis_db_ != nullptr && authoring_db_ != nullptr && job_ini.wave_id > 0) {
            if (const auto wave = analysis_db_->GetBattleTurnWave(job_ini.wave_id); wave.has_value()) {
                if (const auto battle_set = analysis_db_->GetBattleSet(wave->battle_set_id); battle_set.has_value()) {
                    if (!job_ini.run_ms_override.has_value()) {
                        if (const auto spec = authoring_db_->GetBattleRunSpec(battle_set->battle_run_spec_id);
                            spec.has_value() && spec->run_ms > 0) {
                            request.default_timeout_ms = spec->run_ms;
                        }
                    }
                }
            }
        }
        return request;
    }

    std::optional<savor::PSJob> MaterializePsJob(std::int64_t job_id, const RuntimeInitRequest&) const override {
        if (execution_db_ == nullptr || analysis_db_ == nullptr || authoring_db_ == nullptr) {
            return std::nullopt;
        }
        const auto exec_job = execution_db_->GetJob(job_id);
        if (!exec_job.has_value() || exec_job->program_ref_kind != kTurnJobRefKind) {
            return std::nullopt;
        }
        const auto job_ini = JobIni::parse(exec_job->input_ini);
        const auto wave = analysis_db_->GetBattleTurnWave(job_ini.wave_id);
        if (!wave.has_value()) {
            return std::nullopt;
        }
        const auto battle_set = analysis_db_->GetBattleSet(wave->battle_set_id);
        const auto run_spec = battle_set.has_value() ? authoring_db_->GetBattleRunSpec(battle_set->battle_run_spec_id) : std::nullopt;
        const auto settings = battle_set.has_value() ? authoring_db_->GetExplorerSettings(battle_set->explorer_settings_id) : std::nullopt;
        const auto plan = authoring_db_->GetBattlePlan(job_ini.plan_id);
        if (!battle_set.has_value() || !run_spec.has_value() || !settings.has_value() || !plan.has_value()) {
            return std::nullopt;
        }
        const auto* turn = FindTurn(*plan, job_ini.turn_index);
        if (turn == nullptr) {
            return std::nullopt;
        }

        std::optional<savor::db::PredicateSetSnapshot> predicate_set;
        if (settings->default_predicate_set_id.has_value()) {
            predicate_set = authoring_db_->GetPredicateSet(*settings->default_predicate_set_id);
        }

        phase::battle::turnrunner::EncodeSpec spec{};
        spec.run_ms = job_ini.run_ms_override.value_or(ClampU32(run_spec->run_ms));
        spec.vi_stall_ms = ClampU32(run_spec->vi_stall_ms);
        spec.current_turn = static_cast<std::uint32_t>(std::max(1, job_ini.turn_index));
        spec.max_turn = static_cast<std::uint32_t>(std::max(plan->num_turns, job_ini.turn_index));
        if (!job_ini.resolved_turn_commands_blob.empty()) {
            spec.turn_plan = DecodeTurnPlanSpecHex(
                job_ini.resolved_turn_commands_blob,
                job_ini.fake_attacks_this_turn).value_or(BuildTurnPlan(*turn, job_ini.fake_attacks_this_turn));
        } else {
            spec.turn_plan = BuildTurnPlan(*turn, job_ini.fake_attacks_this_turn);
        }
        spec.predicates = BuildPredicates(predicate_set.has_value() ? &*predicate_set : nullptr, authoring_db_);
        spec.fake_attack_budget_max = static_cast<std::uint32_t>(std::max(battle_set->launch_fake_attack_min, battle_set->launch_fake_attack_max));
        spec.fake_attacks_used_before_turn = static_cast<std::uint32_t>(std::max(0, job_ini.fake_attacks_used_before));

        if (job_ini.turn_index == 1) {
            if (const auto candidate = analysis_db_->GetBattleSeedCandidate(job_ini.seed_candidate_id);
                candidate.has_value() && candidate->source_unique_seed_id.has_value()) {
                if (const auto unique = analysis_db_->GetSeedProbeUniqueSeed(*candidate->source_unique_seed_id); unique.has_value()) {
                    spec.has_initial_input = true;
                    spec.initial = InitialFrameFromUniqueSeed(*unique);
                }
            } else if (candidate.has_value() && candidate->source_input_frame_id.has_value()) {
                if (const auto frame = analysis_db_->GetAnalysisInputFrame(*candidate->source_input_frame_id); frame.has_value()) {
                    spec.has_initial_input = true;
                    spec.initial = InitialFrameFromInputSetFrame(*frame);
                }
            }
        }

        const auto out_dir = WorkingRoot(working_dir_root_) / ("job-" + std::to_string(job_id));
        std::filesystem::create_directories(out_dir);
        spec.output_savestate_path = (out_dir / "battle_single_turn_output.sav").string();
        spec.capture_profile_path = job_ini.capture_profile_path;
        if (!spec.capture_profile_path.empty()) {
            spec.capture_output_path = (out_dir / "battle_checkpoint_capture.jsonl").string();
        }
        spec.override_start_rng_seed = job_ini.override_start_rng_seed;

        savor::PSJob ps_job{};
        if (!phase::battle::turnrunner::encode_payload(spec, ps_job.payload)) {
            return std::nullopt;
        }
        return ps_job;
    }

private:
    savor::db::IExecutionDb* execution_db_ = nullptr;
    savor::db::IAnalysisDb* analysis_db_ = nullptr;
    savor::db::IAuthoringDb* authoring_db_ = nullptr;
    std::filesystem::path working_dir_root_;
};

class BattleSingleTurnResultMapper final : public IResultMapper {
public:
    BattleSingleTurnResultMapper(
        savor::db::IExecutionDb* execution_db,
        savor::db::IStateDb* state_db,
        savor::db::IAnalysisDb* analysis_db,
        std::filesystem::path working_dir_root)
        : execution_db_(execution_db)
        , state_db_(state_db)
        , analysis_db_(analysis_db)
        , working_dir_root_(std::move(working_dir_root)) {
    }

    std::string BuildResultIniFromPrResult(std::int64_t, const savor::PRResult& result) const override {
        ResultsIni out{};
        out.w_err = result.ps.w_err;
        if (out.w_err == 0) {
            result.ps.ctx.get(savor::context::key::core::DW_RUN_OUTCOME_CODE, out.dw_err);
        }
        result.ps.ctx.get(savor::context::key::core::VI_FIRST, out.vi_start);
        result.ps.ctx.get(savor::context::key::core::VI_LAST, out.vi_end);
        result.ps.ctx.get(savor::context::key::seed::RNG_SEED, out.rng_seed);
        result.ps.ctx.get(savor::context::key::battle::BATTLE_OUTCOME, out.battle_outcome);
        result.ps.ctx.get(savor::context::key::battle::PLAN_MATERIALIZE_ERR, out.plan_materialize_err);
        std::uint32_t before = 0;
        std::uint32_t cur = 0;
        result.ps.ctx.get(savor::context::key::battle::FAKE_ATTACK_USED_BEFORE, before);
        result.ps.ctx.get(savor::context::key::battle::FAKE_ATTACK_COUNT_THIS_TURN, cur);
        out.fake_attacks_used = before + cur;
        result.ps.ctx.get(savor::context::key::core::PRED_PASSED, out.pred_passed);
        result.ps.ctx.get(savor::context::key::core::PRED_TOTAL, out.pred_total);
        result.ps.ctx.get(savor::context::key::core::PRED_ABORT_RUN, out.pred_abort_run);
        result.ps.ctx.get(savor::context::key::battle::MACRO_FAILURE_CODE, out.macro_failure_code);
        result.ps.ctx.get(savor::context::key::battle::MACRO_STEP_COUNT, out.macro_step_count);
        result.ps.ctx.get(savor::context::key::battle::MACRO_LAST_STEP_INDEX, out.macro_last_step_index);
        result.ps.ctx.get(savor::context::key::battle::MACRO_LAST_EXPECTED_BP, out.macro_last_expected_bp);
        result.ps.ctx.get(savor::context::key::battle::MACRO_LAST_HIT_BP, out.macro_last_hit_bp);
        result.ps.ctx.get(savor::context::key::battle::MACRO_LAST_HIT_PC, out.macro_last_hit_pc);
        result.ps.ctx.get(savor::context::key::battle::RNG_OVERRIDE_ENABLED, out.rng_override_enabled);
        result.ps.ctx.get(savor::context::key::battle::RNG_OVERRIDE_SEED, out.rng_override_seed);
        result.ps.ctx.get(savor::context::key::battle::RNG_ORIGINAL_SEED, out.rng_original_seed);
        result.ps.ctx.get(savor::context::key::battle::RNG_APPLIED_SEED, out.rng_applied_seed);
        result.ps.ctx.get(savor::context::key::core::MEMWRITE_STATUS, out.memwrite_status);
        result.ps.ctx.get(savor::context::key::core::MEMWRITE_READBACK, out.memwrite_readback);
        std::string turn_blob;
        result.ps.ctx.get(savor::context::key::battle::APPLIED_INPUTPLAN_TURN_BLOB, turn_blob);
        if (!turn_blob.empty()) {
            std::vector<savor::inputtrace::BattleTurnInputTrace> traces;
            if (savor::inputtrace::decode_turn_input_traces(turn_blob, traces) && !traces.empty()) {
                out.input_trace_blob = turn_blob;
            }
        }
        result.ps.ctx.get(savor::context::key::core::LAST_SAVESTATE_PATH, out.savestate_path);
        result.ps.ctx.get(savor::context::key::core::CAPTURE_OUTPUT_PATH, out.capture_output_path);
        std::string context_blob;
        if (result.ps.ctx.get(savor::context::key::battle::CTX_BLOB, context_blob) && !context_blob.empty()) {
            out.context_blob_base64 = savor::utils::Base64Encode(context_blob);
            out.context_version = soa::battle::ctx::codec::ver;
        }
        return out.to_ini();
    }

    ResultMapPayload MapPrimaryResult(std::int64_t job_id, const std::string& result_ini) const override {
        ResultMapPayload payload{};
        payload.result_kind = "analysisbattle.turn_job";
        if (execution_db_ == nullptr || analysis_db_ == nullptr) {
            AppendJobCompleted(execution_db_, job_id, "FAILED", &payload.event_lines);
            return payload;
        }
        const auto exec_job = execution_db_->GetJob(job_id);
        const auto turn_job = analysis_db_->GetBattleTurnJobForExecJob(job_id);
        if (!exec_job.has_value() || !turn_job.has_value()) {
            AppendJobCompleted(execution_db_, job_id, "FAILED", &payload.event_lines);
            payload.result_kind = "analysisbattle.turn_job.context_missing";
            return payload;
        }
        auto parsed = ResultsIni::parse(result_ini);
        const auto parsed_outcome = static_cast<savor::battle::Outcome>(parsed.battle_outcome);
        const bool requires_result_context = parsed_outcome == savor::battle::Outcome::ReachedNextTurn;
        const bool failed = parsed.w_err != 0
            || parsed.dw_err != 0
            || IsBadMaterialize(parsed)
            || parsed_outcome == savor::battle::Outcome::Unknown
            || (requires_result_context && parsed.context_blob_base64.empty());

        std::optional<std::int64_t> output_savestate_id;
        if (!failed && IsContinuationOutcome(parsed_outcome)) {
            output_savestate_id = StoreOutputSavestate(job_id, parsed, payload.event_lines);
            if (!output_savestate_id.has_value()) {
                AppendJobCompleted(execution_db_, job_id, "FAILED", &payload.event_lines);
                return payload;
            }
            parsed.output_savestate_id = *output_savestate_id;
        }

        std::optional<std::int64_t> input_trace_artifact_id;
        if (!parsed.input_trace_blob.empty()) {
            input_trace_artifact_id = StoreInputTrace(job_id, parsed.input_trace_blob, payload.event_lines);
            if (input_trace_artifact_id.has_value()) {
                parsed.input_trace_artifact_id = *input_trace_artifact_id;
                parsed.applied_input_artifact_id = *input_trace_artifact_id;
            }
        }
        if (!parsed.capture_output_path.empty()) {
            if (const auto capture_artifact_id = StoreCaptureArtifact(job_id, parsed.capture_output_path, payload.event_lines);
                capture_artifact_id.has_value()) {
                parsed.capture_artifact_id = *capture_artifact_id;
            }
        }

        savor::db::RecordBattleTurnJobCommand update{};
        update.exec_job_id = job_id;
        update.plan_id = turn_job->plan_id;
        update.wave_id = turn_job->wave_id;
        update.fake_attacks_this_turn = turn_job->fake_attacks_this_turn;
        update.fake_attacks_used_before = turn_job->fake_attacks_used_before;
        update.job_state = failed ? savor::db::BattleTurnJobState::Failed : savor::db::BattleTurnJobState::Succeeded;
        update.ended_at_utc = savor::db::types::UtcNow();
        update.has_results = true;
        update.vi_start = static_cast<int>(parsed.vi_start);
        update.vi_end = static_cast<int>(parsed.vi_end);
        update.delta_vi = static_cast<int>(parsed.vi_end >= parsed.vi_start ? parsed.vi_end - parsed.vi_start : 0);
        update.rng_seed = static_cast<std::int64_t>(parsed.rng_seed);
        update.battle_outcome = parsed_outcome;
        update.plan_materialize_err = static_cast<int>(parsed.plan_materialize_err);
        update.pred_passed = static_cast<int>(parsed.pred_passed);
        update.pred_total = static_cast<int>(parsed.pred_total);
        update.pred_abort_run = static_cast<int>(parsed.pred_abort_run);
        update.output_savestate_id = output_savestate_id;
        update.applied_input_artifact_id = input_trace_artifact_id;
        update.input_trace_artifact_id = input_trace_artifact_id;
        if (!failed && !parsed.context_blob_base64.empty()) {
            update.result_context_blob_base64 = parsed.context_blob_base64;
            update.result_context_version = parsed.context_version > 0
                ? std::optional<int>(parsed.context_version)
                : std::nullopt;
        }
        update.recorded_at_utc = savor::db::types::UtcNow();
        std::string error;
        if (!analysis_db_->UpdateBattleTurnJobResult(update, &error)) {
            payload.event_lines.push_back("[battle-single-turn-result] warning=turn_job_update_failed error=" + error);
        }

        payload.event_lines.push_back("[battle-single-turn-result-diagnostics] job=" + std::to_string(job_id)
            + " turn_job=" + std::to_string(turn_job->turn_job_id)
            + " w_err=" + std::to_string(parsed.w_err)
            + " dw_err=" + std::string(savor::RunToBpOutcomeToString(parsed.dw_err))
            + "(" + std::to_string(parsed.dw_err) + ")"
            + " battle_outcome=" + savor::battle::get_outcome_string(parsed_outcome)
            + "(" + std::to_string(parsed.battle_outcome) + ")"
            + " vi_start=" + std::to_string(parsed.vi_start)
            + " vi_end=" + std::to_string(parsed.vi_end)
            + " pred_passed=" + std::to_string(parsed.pred_passed)
            + " pred_total=" + std::to_string(parsed.pred_total)
            + " pred_abort_run=" + std::to_string(parsed.pred_abort_run)
            + " plan_materialize_err=" + std::to_string(parsed.plan_materialize_err)
            + " macro_failure=" + std::to_string(parsed.macro_failure_code)
            + " macro_step_count=" + std::to_string(parsed.macro_step_count)
            + " macro_last_step=" + std::to_string(parsed.macro_last_step_index)
            + " macro_expected_bp=" + std::to_string(parsed.macro_last_expected_bp)
            + " macro_hit_bp=" + std::to_string(parsed.macro_last_hit_bp)
            + " macro_hit_pc=0x" + [&]() {
                std::ostringstream pc;
                pc << std::hex << std::uppercase << std::setw(8) << std::setfill('0') << parsed.macro_last_hit_pc;
                return pc.str();
            }());
        AppendJobCompleted(execution_db_, job_id, failed ? "FAILED" : "SUCCEEDED", &payload.event_lines);
        payload.result_ref_id = turn_job->turn_job_id;
        payload.event_lines.push_back("[battle-single-turn-result] job=" + std::to_string(job_id)
            + " turn_job=" + std::to_string(turn_job->turn_job_id)
            + " outcome=" + std::to_string(parsed.battle_outcome)
            + " rng_seed=" + std::to_string(parsed.rng_seed));
        return payload;
    }

    std::optional<ResultArtifactRef> MapPrimaryArtifact(std::int64_t) const override {
        return std::nullopt;
    }

private:
    std::optional<std::int64_t> StoreCaptureArtifact(
        std::int64_t job_id,
        const std::string& capture_output_path,
        std::vector<std::string>& lines) const {
        if (state_db_ == nullptr || capture_output_path.empty()) {
            return std::nullopt;
        }
        const std::filesystem::path path(capture_output_path);
        if (!std::filesystem::exists(path)) {
            lines.push_back("[battle-single-turn-result] capture_missing path=" + capture_output_path);
            return std::nullopt;
        }

        const auto now = savor::db::types::UtcNow();
        std::string error;
        std::int64_t artifact_id = 0;
        if (!state_db_->StoreArtifact(
                {
                    .sha256 = hash::sha256_of_file(path.string()),
                    .size_bytes = FileSize(path),
                    .filename = std::filesystem::absolute(path).string(),
                    .file_ext = path.extension().string(),
                    .artifact_kind = "OTHER",
                    .created_at_utc = now,
                    .correlation_id = "battle-job-" + std::to_string(job_id),
                    .causation_id = "job-" + std::to_string(job_id),
                },
                &artifact_id,
                &error)
            || artifact_id <= 0) {
            lines.push_back("[battle-single-turn-result] capture_store_failed error=" + error);
            return std::nullopt;
        }
        lines.push_back("[battle-single-turn-result] capture_artifact_id=" + std::to_string(artifact_id)
            + " path=" + std::filesystem::absolute(path).string());
        return artifact_id;
    }

    std::optional<std::int64_t> StoreOutputSavestate(
        std::int64_t job_id,
        const ResultsIni& parsed,
        std::vector<std::string>& lines) const {
        if (state_db_ == nullptr || parsed.savestate_path.empty()) {
            return std::nullopt;
        }
        const std::filesystem::path path(parsed.savestate_path);
        if (!std::filesystem::exists(path)) {
            lines.push_back("[battle-single-turn-result] savestate_missing path=" + parsed.savestate_path);
            return std::nullopt;
        }
        const auto now = savor::db::types::UtcNow();
        std::string error;
        std::int64_t artifact_id = 0;
        if (!state_db_->StoreArtifact(
                {
                    .sha256 = hash::sha256_of_file(path.string()),
                    .size_bytes = FileSize(path),
                    .filename = std::filesystem::absolute(path).string(),
                    .file_ext = path.extension().string(),
                    .artifact_kind = "SAV",
                    .created_at_utc = now,
                    .correlation_id = "battle-job-" + std::to_string(job_id),
                    .causation_id = "job-" + std::to_string(job_id),
                },
                &artifact_id,
                &error)
            || artifact_id <= 0) {
            lines.push_back("[battle-single-turn-result] artifact_store_failed error=" + error);
            return std::nullopt;
        }
        std::int64_t savestate_id = 0;
        if (!state_db_->CreateSavestate(
                {
                    .artifact_id = artifact_id,
                    .savestate_type = "BATTLE",
                    .note = "BattleSingleTurnRunner produced savestate",
                    .is_complete = true,
                    .created_at_utc = now,
                    .correlation_id = "battle-job-" + std::to_string(job_id),
                    .causation_id = "artifact-" + std::to_string(artifact_id),
                },
                &savestate_id,
                &error)
            || savestate_id <= 0) {
            lines.push_back("[battle-single-turn-result] savestate_create_failed error=" + error);
            return std::nullopt;
        }
        return savestate_id;
    }

    std::optional<std::int64_t> StoreInputTrace(
        std::int64_t job_id,
        const std::string& blob,
        std::vector<std::string>& lines) const {
        if (state_db_ == nullptr || blob.empty()) {
            return std::nullopt;
        }
        const auto root = WorkingRoot(working_dir_root_) / ("job-" + std::to_string(job_id));
        std::filesystem::create_directories(root);
        const auto path = root / "battle_input_trace.aitb";
        {
            std::ofstream out(path, std::ios::binary | std::ios::trunc);
            out.write(blob.data(), static_cast<std::streamsize>(blob.size()));
            if (!out.good()) {
                lines.push_back("[battle-single-turn-result] input_trace_write_failed");
                return std::nullopt;
            }
        }
        const auto now = savor::db::types::UtcNow();
        std::string error;
        std::int64_t artifact_id = 0;
        if (!state_db_->StoreArtifact(
                {
                    .sha256 = hash::sha256_of_file(path.string()),
                    .size_bytes = FileSize(path),
                    .filename = std::filesystem::absolute(path).string(),
                    .file_ext = ".aitb",
                    .artifact_kind = "OTHER",
                    .created_at_utc = now,
                    .correlation_id = "battle-job-" + std::to_string(job_id),
                    .causation_id = "job-" + std::to_string(job_id),
                },
                &artifact_id,
                &error)
            || artifact_id <= 0) {
            lines.push_back("[battle-single-turn-result] input_trace_store_failed error=" + error);
            return std::nullopt;
        }
        return artifact_id;
    }

    savor::db::IExecutionDb* execution_db_ = nullptr;
    savor::db::IStateDb* state_db_ = nullptr;
    savor::db::IAnalysisDb* analysis_db_ = nullptr;
    std::filesystem::path working_dir_root_;
};

struct Survivor {
    savor::db::BattleTurnJobSnapshot job;
    savor::db::BattleTurnWaveSnapshot wave;
};

class BattleSingleTurnTransitionHandler final : public IWorkflowTransitionHandler {
public:
    BattleSingleTurnTransitionHandler(
        savor::db::IAnalysisDb* analysis_db,
        savor::db::IAuthoringDb* authoring_db)
        : analysis_db_(analysis_db)
        , authoring_db_(authoring_db) {
    }

    WorkflowTransitionDecision EvaluateTransition(const WorkflowTransitionContext& context) const override {
        WorkflowTransitionDecision decision{};
        if (analysis_db_ == nullptr || authoring_db_ == nullptr) {
            decision.blocked_reason = "db_unavailable";
            return decision;
        }
        if (!context.input_ref_id.has_value()
            || (context.input_ref_kind.has_value() && *context.input_ref_kind != kWaveRefKind)) {
            decision.blocked_reason = "wave_ref_missing";
            return decision;
        }
        const auto current_wave = analysis_db_->GetBattleTurnWave(*context.input_ref_id);
        if (!current_wave.has_value()) {
            decision.blocked_reason = "wave_not_found";
            return decision;
        }
        const auto battle_set = analysis_db_->GetBattleSet(current_wave->battle_set_id);
        const auto run_spec = battle_set.has_value() ? authoring_db_->GetBattleRunSpec(battle_set->battle_run_spec_id) : std::nullopt;
        const auto settings = battle_set.has_value() ? authoring_db_->GetExplorerSettings(battle_set->explorer_settings_id) : std::nullopt;
        if (!battle_set.has_value() || !run_spec.has_value() || !settings.has_value() || !settings->default_plan_id.has_value()) {
            decision.blocked_reason = "authoring_context_missing";
            return decision;
        }

        const auto now = savor::db::types::UtcNow();
        (void)analysis_db_->UpdateBattleTurnWaveStatus(current_wave->wave_id, savor::db::BattleTurnWaveStatus::Completed, now, nullptr);

        const auto waves = analysis_db_->ListBattleTurnWaves(current_wave->battle_set_id);
        for (const auto& wave : waves) {
            if (wave.turn_index == current_wave->turn_index
                && wave.status != savor::db::BattleTurnWaveStatus::Completed
                && wave.status != savor::db::BattleTurnWaveStatus::NoSurvivors
                && wave.status != savor::db::BattleTurnWaveStatus::Selected) {
                decision.should_advance = false;
                return decision;
            }
        }
        std::int64_t pool_id = 0;
        std::string error;
        if (!analysis_db_->EnsureBattleAdvancementPool(
                {
                    .battle_set_id = current_wave->battle_set_id,
                    .turn_index = current_wave->turn_index,
                    .pool_name = "turn-rng-seed-survivors",
                    .criterion_kind = savor::db::BattleAdvancementCriterionKind::BestFakeAttacksByRngSeed,
                    .created_at_utc = now,
                    .correlation_id = "battle-set-" + std::to_string(current_wave->battle_set_id),
                    .causation_id = "wave-" + std::to_string(current_wave->wave_id),
                },
                &pool_id,
                &error)
            || pool_id <= 0) {
            decision.blocked_reason = "battle_advancement_pool_failed:" + error;
            return decision;
        }
        if (!analysis_db_->ListBattleAdvancementDecisionsForPool(pool_id).empty()) {
            decision.should_advance = false;
            return decision;
        }

        const auto turn_jobs = analysis_db_->ListBattleTurnJobsForBattleTurn(current_wave->battle_set_id, current_wave->turn_index);
        std::map<std::int64_t, savor::db::BattleTurnWaveSnapshot> wave_by_id;
        for (const auto& wave : waves) {
            wave_by_id.emplace(wave.wave_id, wave);
        }

        std::map<std::int64_t, Survivor> best_by_rng;
        std::vector<Survivor> all_survivors;
        for (const auto& job : turn_jobs) {
            if (job.job_state != savor::db::BattleTurnJobState::Succeeded) {
                continue;
            }
            if (!job.rng_seed.has_value() || !job.output_savestate_id.has_value() || !job.battle_outcome.has_value()) {
                continue;
            }
            if (!IsContinuationOutcome(*job.battle_outcome)) {
                continue;
            }
            auto wave_it = wave_by_id.find(job.wave_id);
            if (wave_it == wave_by_id.end()) {
                continue;
            }
            Survivor survivor{ .job = job, .wave = wave_it->second };
            all_survivors.push_back(survivor);
            auto best_it = best_by_rng.find(*job.rng_seed);
            const int fake_used = job.fake_attacks_used_before + job.fake_attacks_this_turn;
            int preds_passed = 0;
            if (job.pred_passed.has_value()) {
                preds_passed = *job.pred_passed;
            }
            if (best_it == best_by_rng.end()) {
                best_by_rng.emplace(*job.rng_seed, survivor);
            } else {
                const int best_fake = best_it->second.job.fake_attacks_used_before + best_it->second.job.fake_attacks_this_turn;
				const int best_preds_passed = best_it->second.job.pred_passed.has_value() ? *best_it->second.job.pred_passed : 0;
                bool is_better = preds_passed > best_preds_passed || (preds_passed == best_preds_passed && fake_used < best_fake);
                if (is_better) {
                    best_it->second = survivor;
				}

            }
        }

        if (all_survivors.empty()) {
            if (battle_set->status != savor::db::BattleSetStatus::Victory) {
                (void)analysis_db_->UpdateBattleSetStatus(current_wave->battle_set_id, savor::db::BattleSetStatus::NoSurvivors, now, nullptr);
            }
            decision.should_advance = true;
            return decision;
        }

        auto victory_is_better = [](const Survivor& candidate, const Survivor& best) {
            const int candidate_preds = candidate.job.pred_passed.has_value() ? *candidate.job.pred_passed : 0;
            const int best_preds = best.job.pred_passed.has_value() ? *best.job.pred_passed : 0;
            if (candidate_preds != best_preds) {
                return candidate_preds > best_preds;
            }
            const int candidate_fake = candidate.job.fake_attacks_used_before + candidate.job.fake_attacks_this_turn;
            const int best_fake = best.job.fake_attacks_used_before + best.job.fake_attacks_this_turn;
            if (candidate_fake != best_fake) {
                return candidate_fake < best_fake;
            }
            if (candidate.job.rng_seed.has_value() != best.job.rng_seed.has_value()) {
                return candidate.job.rng_seed.has_value();
            }
            if (candidate.job.rng_seed.has_value() && best.job.rng_seed.has_value()
                && *candidate.job.rng_seed != *best.job.rng_seed) {
                return *candidate.job.rng_seed < *best.job.rng_seed;
            }
            return candidate.job.turn_job_id < best.job.turn_job_id;
        };

        std::map<std::int64_t, Survivor> best_victory_by_rng;
        for (const auto& survivor : all_survivors) {
            if (!survivor.job.battle_outcome.has_value() || !IsVictory(*survivor.job.battle_outcome)) {
                continue;
            }
            if (!survivor.job.rng_seed.has_value()) {
                continue;
            }
            auto best_it = best_victory_by_rng.find(*survivor.job.rng_seed);
            if (best_it == best_victory_by_rng.end()) {
                best_victory_by_rng.emplace(*survivor.job.rng_seed, survivor);
            } else if (victory_is_better(survivor, best_it->second)) {
                best_it->second = survivor;
            }
        }

        std::vector<std::int64_t> selected_job_ids;
        if (!best_victory_by_rng.empty()) {
            for (const auto& [rng, survivor] : best_victory_by_rng) {
                (void)rng;
                selected_job_ids.push_back(survivor.job.turn_job_id);
            }
        } else {
            for (const auto& [rng, survivor] : best_by_rng) {
                (void)rng;
                selected_job_ids.push_back(survivor.job.turn_job_id);
            }
        }
        std::sort(selected_job_ids.begin(), selected_job_ids.end());

        for (const auto& survivor : all_survivors) {
            const bool selected_for_advancement = std::binary_search(selected_job_ids.begin(), selected_job_ids.end(), survivor.job.turn_job_id);
            (void)analysis_db_->RecordBattleAdvancementDecision(
                {
                    .battle_advancement_pool_id = pool_id,
                    .turn_job_id = survivor.job.turn_job_id,
                    .decision_kind = selected_for_advancement ? savor::db::BattleAdvancementDecisionKind::Selected : savor::db::BattleAdvancementDecisionKind::NotSelected,
                    .decision_reason = selected_for_advancement ? std::optional<std::string>("best_for_rng_seed") : std::optional<std::string>("rng_seed_duplicate"),
                    .created_at_utc = now,
                    .correlation_id = "battle-set-" + std::to_string(current_wave->battle_set_id),
                    .causation_id = "battle-advancement-pool-" + std::to_string(pool_id),
                },
                nullptr,
                nullptr);
        }

        if (!best_victory_by_rng.empty()) {
            (void)analysis_db_->UpdateBattleSetStatus(current_wave->battle_set_id, savor::db::BattleSetStatus::Victory, now, nullptr);
            decision.should_advance = true;
            return decision;
        }
        if (!run_spec->auto_wave_trigger_enable) {
            decision.should_advance = true;
            return decision;
        }

        const auto plan = authoring_db_->GetBattlePlan(*settings->default_plan_id);
        if (!plan.has_value() || FindTurn(*plan, current_wave->turn_index + 1) == nullptr) {
            (void)analysis_db_->UpdateBattleSetStatus(
                current_wave->battle_set_id,
                battle_set->status == savor::db::BattleSetStatus::Victory
                    ? savor::db::BattleSetStatus::Victory
                    : savor::db::BattleSetStatus::Completed,
                now,
                nullptr);
            decision.should_advance = true;
            return decision;
        }

        for (const auto& [rng, selected] : best_by_rng) {
            (void)rng;
            if (selected.job.battle_outcome.has_value() && IsVictory(*selected.job.battle_outcome)) {
                continue;
            }
            std::int64_t next_wave_id = 0;
            if (!analysis_db_->CreateBattleTurnWave(
                    {
                        .battle_set_id = current_wave->battle_set_id,
                        .turn_index = current_wave->turn_index + 1,
                        .parent_wave_id = selected.wave.wave_id,
                        .parent_turn_job_id = selected.job.turn_job_id,
                        .seed_candidate_id = selected.wave.seed_candidate_id,
                        .battle_advancement_pool_id = pool_id,
                        .status = savor::db::BattleTurnWaveStatus::Ready,
                        .created_at_utc = now,
                        .correlation_id = "battle-set-" + std::to_string(current_wave->battle_set_id),
                        .causation_id = "turn-job-" + std::to_string(selected.job.turn_job_id),
                    },
                    &next_wave_id,
                    &error)
                || next_wave_id <= 0) {
                continue;
            }
            WorkflowTransitionDecision::DynamicStep step{};
            step.step_key = "BattleTurn/t" + std::to_string(current_wave->turn_index + 1)
                + "/w" + std::to_string(next_wave_id);
            step.step_kind = kBattleSingleTurnStepKind;
            step.input_ref_kind = kWaveRefKind;
            step.input_ref_id = next_wave_id;
            step.priority = 0;
            step.max_attempts = 1;
            decision.spawn_steps.push_back(std::move(step));
        }
        decision.should_advance = true;
        return decision;
    }

private:
    savor::db::IAnalysisDb* analysis_db_ = nullptr;
    savor::db::IAuthoringDb* authoring_db_ = nullptr;
};

} // namespace

ProgramKindDescriptor BuildBattleSingleTurnDescriptor(
    savor::db::IExecutionDb* execution_db,
    savor::db::IStateDb* state_db,
    savor::db::IAnalysisDb* analysis_db,
    BattleSingleTurnPhaseRegistrationConfig config) {
    ProgramKindDescriptor descriptor{};
    descriptor.program_kind = savor::PK_BattleSingleTurnRunner;
    descriptor.program_name = "BattleSingleTurnRunner";
    descriptor.job_persistence = std::make_shared<BattleSingleTurnJobPersistenceAdapter>(
        execution_db,
        analysis_db,
        config.authoring_db);
    descriptor.runtime_init = std::make_shared<BattleSingleTurnRuntimeInitAdapter>(
        execution_db,
        analysis_db,
        config.authoring_db,
        config.working_dir_root);
    descriptor.result_mapper = std::make_shared<BattleSingleTurnResultMapper>(
        execution_db,
        state_db,
        analysis_db,
        config.working_dir_root);
    descriptor.workflow_transition = std::make_shared<BattleSingleTurnTransitionHandler>(
        analysis_db,
        config.authoring_db);
    descriptor.supports_workflow_orchestration = true;
    descriptor.allow_mixed_success_failed_transition = true;
    return descriptor;
}

} // namespace savor::db::execution::programdb::battle

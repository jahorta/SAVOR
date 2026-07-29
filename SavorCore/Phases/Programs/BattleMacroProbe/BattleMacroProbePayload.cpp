#include "BattleMacroProbePayload.h"

#include "../../../Runner/IPC/Wire.h"
#include "../../../Runner/Script/CtxRegistry.h"
#include "../../../Runner/Script/ScriptProgress.h"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <sstream>

namespace phase::battle::macroprobe {
namespace {

void put_u32(std::vector<std::uint8_t>& b, std::uint32_t v) {
    b.push_back(static_cast<std::uint8_t>(v));
    b.push_back(static_cast<std::uint8_t>(v >> 8));
    b.push_back(static_cast<std::uint8_t>(v >> 16));
    b.push_back(static_cast<std::uint8_t>(v >> 24));
}

bool get_u32(const std::uint8_t*& p, const std::uint8_t* e, std::uint32_t& v) {
    if (p + 4 > e) return false;
    v = static_cast<std::uint32_t>(p[0])
        | (static_cast<std::uint32_t>(p[1]) << 8)
        | (static_cast<std::uint32_t>(p[2]) << 16)
        | (static_cast<std::uint32_t>(p[3]) << 24);
    p += 4;
    return true;
}

std::string TrimAscii(std::string_view value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())) != 0) {
        value.remove_prefix(1);
    }
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())) != 0) {
        value.remove_suffix(1);
    }
    return std::string(value);
}

std::string LowerAscii(std::string value) {
    std::transform(
        value.begin(),
        value.end(),
        value.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

bool ParseTargetSlot(std::string_view value, std::uint32_t* out) {
    const std::string trimmed = TrimAscii(value);
    if (trimmed.empty()) return false;
    std::uint32_t parsed = 0;
    const char* first = trimmed.data();
    const char* last = trimmed.data() + trimmed.size();
    const auto result = std::from_chars(first, last, parsed);
    if (result.ec != std::errc{} || result.ptr != last) return false;
    if (parsed < 4 || parsed > 11) return false;
    if (out) *out = parsed;
    return true;
}

} // namespace

const char* MacroModeName(MacroMode mode) {
    switch (mode) {
    case MacroMode::Attack: return "attack";
    case MacroMode::Focus: return "focus";
    case MacroMode::Block: return "block";
    default: return "unknown";
    }
}

const char* FailureCodeName(FailureCode code) {
    switch (code) {
    case FailureCode::Ok: return "ok";
    case FailureCode::InvalidMode: return "invalid_mode";
    case FailureCode::InvalidTarget: return "invalid_target";
    case FailureCode::NoSteps: return "no_steps";
    case FailureCode::HostFailure: return "host_failure";
    case FailureCode::UnexpectedBreakpoint: return "unexpected_breakpoint";
    case FailureCode::BattleContextUnavailable: return "battle_context_unavailable";
    case FailureCode::MemoryReadFailed: return "memory_read_failed";
    case FailureCode::CaptureOnlyHitLimit: return "capture_only_hit_limit";
    default: return "unknown";
    }
}

bool TryParseMacroMode(std::string_view value, MacroMode* out) {
    if (value == "attack") {
        if (out) *out = MacroMode::Attack;
        return true;
    }
    if (value == "focus") {
        if (out) *out = MacroMode::Focus;
        return true;
    }
    if (value == "block" || value == "defend") {
        if (out) *out = MacroMode::Block;
        return true;
    }
    return false;
}

std::string FormatCommandPlanSpec(const std::vector<MacroCommand>& commands) {
    std::ostringstream out;
    for (size_t i = 0; i < commands.size(); ++i) {
        if (i > 0) out << ',';
        out << MacroModeName(commands[i].mode);
        if (commands[i].mode == MacroMode::Attack) {
            out << ':' << commands[i].target_slot;
        }
    }
    return out.str();
}

bool ParseCommandPlanSpec(std::string_view spec, std::vector<MacroCommand>* out, std::string* error_out) {
    std::vector<MacroCommand> commands;
    size_t start = 0;
    while (start <= spec.size()) {
        const size_t comma = spec.find(',', start);
        const size_t end = comma == std::string_view::npos ? spec.size() : comma;
        std::string token = TrimAscii(spec.substr(start, end - start));
        if (token.empty()) {
            if (error_out) *error_out = "empty command in --battle-plan";
            return false;
        }

        std::string mode_text = token;
        std::string target_text;
        const size_t colon = token.find(':');
        if (colon != std::string::npos) {
            mode_text = token.substr(0, colon);
            target_text = token.substr(colon + 1);
        }
        mode_text = LowerAscii(TrimAscii(mode_text));

        MacroMode mode = MacroMode::Attack;
        if (!TryParseMacroMode(mode_text, &mode)) {
            if (error_out) *error_out = "unknown command in --battle-plan: " + mode_text;
            return false;
        }

        MacroCommand command{.mode = mode, .target_slot = 4};
        if (!target_text.empty()) {
            if (mode != MacroMode::Attack) {
                if (error_out) *error_out = "only attack accepts a target slot in --battle-plan";
                return false;
            }
            if (!ParseTargetSlot(target_text, &command.target_slot)) {
                if (error_out) *error_out = "attack target slot must be between 4 and 11 in --battle-plan";
                return false;
            }
        } else if (colon != std::string::npos) {
            if (error_out) *error_out = "missing attack target slot in --battle-plan";
            return false;
        }

        commands.push_back(command);
        if (comma == std::string_view::npos) break;
        start = comma + 1;
    }

    if (commands.empty()) {
        if (error_out) *error_out = "--battle-plan must contain at least one command";
        return false;
    }

    if (out) *out = std::move(commands);
    return true;
}

std::string SerializeCommandPlan(const std::vector<MacroCommand>& commands) {
    return FormatCommandPlanSpec(commands);
}

bool DeserializeCommandPlan(std::string_view blob, std::vector<MacroCommand>* out, std::string* error_out) {
    return ParseCommandPlanSpec(blob, out, error_out);
}

bool encode_payload(const EncodeSpec& spec, std::vector<std::uint8_t>& out) {
    out.clear();
    std::vector<MacroCommand> commands = spec.commands;
    if (commands.empty()) {
        commands.push_back(MacroCommand{.mode = spec.mode, .target_slot = spec.target_slot});
    }

    out.push_back(savor::PK_BattleMacroProbe);
    put_u32(out, PayloadVersion);
    put_u32(out, static_cast<std::uint32_t>(commands.size()));
    for (const auto& command : commands) {
        put_u32(out, static_cast<std::uint32_t>(command.mode));
        put_u32(out, command.target_slot);
    }
    put_u32(out, spec.transition_neutral_frames);
    put_u32(out, spec.fake_attack_count);
    put_u32(out, static_cast<std::uint32_t>(spec.fake_attack_pattern.memory_gate_mode));
    put_u32(out, spec.fake_attack_pattern.target_neutral_before_b_frames);
    put_u32(out, spec.fake_attack_pattern.input_neutral_after_b_frames);
    put_u32(out, spec.use_mixed_fake_attack_patterns ? 1u : 0u);
    put_u32(out, static_cast<std::uint32_t>(spec.first_fake_attack_pattern.memory_gate_mode));
    put_u32(out, spec.first_fake_attack_pattern.target_neutral_before_b_frames);
    put_u32(out, spec.first_fake_attack_pattern.input_neutral_after_b_frames);
    put_u32(out, static_cast<std::uint32_t>(spec.repeat_fake_attack_pattern.memory_gate_mode));
    put_u32(out, spec.repeat_fake_attack_pattern.target_neutral_before_b_frames);
    put_u32(out, spec.repeat_fake_attack_pattern.input_neutral_after_b_frames);
    put_u32(out, spec.use_final_fake_attack_pattern ? 1u : 0u);
    put_u32(out, static_cast<std::uint32_t>(spec.final_fake_attack_pattern.memory_gate_mode));
    put_u32(out, spec.final_fake_attack_pattern.target_neutral_before_b_frames);
    put_u32(out, spec.final_fake_attack_pattern.input_neutral_after_b_frames);
    return true;
}

bool decode_payload(const std::vector<std::uint8_t>& in, savor::PSContext& out_ctx) {
    if (in.size() < 1 + 4 * 6) return false;

    const std::uint8_t* p = in.data();
    const std::uint8_t* e = p + in.size();
    const std::uint8_t tag = *p++;
    if (tag != savor::PK_BattleMacroProbe) return false;

    std::uint32_t version = 0;
    std::uint32_t command_count = 0;
    std::uint32_t transition_neutral_frames = 0;
    std::uint32_t fake_attack_count = 0;
    FakeAttackPattern fake_attack_pattern{};
    bool use_mixed_fake_attack_patterns = false;
    FakeAttackPattern first_fake_attack_pattern{};
    FakeAttackPattern repeat_fake_attack_pattern{};
    bool use_final_fake_attack_pattern = false;
    FakeAttackPattern final_fake_attack_pattern{};
    const auto valid_gate_mode = [](FakeAttackMemoryGateMode mode) {
        return mode == FakeAttackMemoryGateMode::TargetSide
            || mode == FakeAttackMemoryGateMode::InputSide
            || mode == FakeAttackMemoryGateMode::Both;
    };
    const auto get_pattern = [&](FakeAttackPattern* pattern) {
        std::uint32_t raw_gate_mode = 0;
        if (!get_u32(p, e, raw_gate_mode)) return false;
        if (!get_u32(p, e, pattern->target_neutral_before_b_frames)) return false;
        if (!get_u32(p, e, pattern->input_neutral_after_b_frames)) return false;
        pattern->memory_gate_mode = static_cast<FakeAttackMemoryGateMode>(raw_gate_mode);
        return valid_gate_mode(pattern->memory_gate_mode);
    };
    if (!get_u32(p, e, version) || version != PayloadVersion) return false;
    if (!get_u32(p, e, command_count)) return false;
    if (command_count == 0 || command_count > 16) return false;
    std::vector<MacroCommand> commands;
    commands.reserve(command_count);
    for (std::uint32_t i = 0; i < command_count; ++i) {
        std::uint32_t mode = 0;
        std::uint32_t target_slot = 0;
        if (!get_u32(p, e, mode)) return false;
        if (!get_u32(p, e, target_slot)) return false;
        MacroMode parsed_mode = static_cast<MacroMode>(mode);
        if (parsed_mode != MacroMode::Attack && parsed_mode != MacroMode::Focus && parsed_mode != MacroMode::Block) {
            return false;
        }
        if (target_slot < 4 || target_slot > 11) return false;
        commands.push_back(MacroCommand{.mode = parsed_mode, .target_slot = target_slot});
    }
    if (!get_u32(p, e, transition_neutral_frames)) return false;
    if (!get_u32(p, e, fake_attack_count)) return false;
    if (!get_pattern(&fake_attack_pattern)) return false;
    first_fake_attack_pattern = fake_attack_pattern;
    repeat_fake_attack_pattern = fake_attack_pattern;
    final_fake_attack_pattern = fake_attack_pattern;
    std::uint32_t raw_use_mixed = 0;
    if (!get_u32(p, e, raw_use_mixed)) return false;
    use_mixed_fake_attack_patterns = raw_use_mixed != 0;
    if (!get_pattern(&first_fake_attack_pattern)) return false;
    if (!get_pattern(&repeat_fake_attack_pattern)) return false;
    final_fake_attack_pattern = repeat_fake_attack_pattern;
    std::uint32_t raw_use_final = 0;
    if (!get_u32(p, e, raw_use_final)) return false;
    use_final_fake_attack_pattern = raw_use_final != 0;
    if (!get_pattern(&final_fake_attack_pattern)) return false;
    if (p != e) return false;

    out_ctx[savor::context::key::battle::MACRO_MODE] = static_cast<std::uint32_t>(commands.front().mode);
    out_ctx[savor::context::key::battle::MACRO_TARGET_SLOT] = commands.front().target_slot;
    out_ctx[savor::context::key::battle::MACRO_PLAN_BLOB] = SerializeCommandPlan(commands);
    out_ctx[savor::context::key::battle::MACRO_TRANSITION_NEUTRAL_FRAMES] = transition_neutral_frames;
    out_ctx[savor::context::key::battle::FAKE_ATTACK_COUNT_THIS_TURN] = fake_attack_count;
    out_ctx[savor::context::key::battle::MACRO_FAKE_MEMORY_GATE_MODE] =
        static_cast<std::uint32_t>(fake_attack_pattern.memory_gate_mode);
    out_ctx[savor::context::key::battle::MACRO_FAKE_TARGET_NEUTRAL_FRAMES] =
        fake_attack_pattern.target_neutral_before_b_frames;
    out_ctx[savor::context::key::battle::MACRO_FAKE_INPUT_NEUTRAL_FRAMES] =
        fake_attack_pattern.input_neutral_after_b_frames;
    out_ctx[savor::context::key::battle::MACRO_FAKE_USE_MIXED_PATTERNS] =
        use_mixed_fake_attack_patterns ? 1u : 0u;
    out_ctx[savor::context::key::battle::MACRO_FAKE_FIRST_MEMORY_GATE_MODE] =
        static_cast<std::uint32_t>(first_fake_attack_pattern.memory_gate_mode);
    out_ctx[savor::context::key::battle::MACRO_FAKE_FIRST_TARGET_NEUTRAL_FRAMES] =
        first_fake_attack_pattern.target_neutral_before_b_frames;
    out_ctx[savor::context::key::battle::MACRO_FAKE_FIRST_INPUT_NEUTRAL_FRAMES] =
        first_fake_attack_pattern.input_neutral_after_b_frames;
    out_ctx[savor::context::key::battle::MACRO_FAKE_USE_FINAL_PATTERN] =
        use_final_fake_attack_pattern ? 1u : 0u;
    out_ctx[savor::context::key::battle::MACRO_FAKE_FINAL_MEMORY_GATE_MODE] =
        static_cast<std::uint32_t>(final_fake_attack_pattern.memory_gate_mode);
    out_ctx[savor::context::key::battle::MACRO_FAKE_FINAL_TARGET_NEUTRAL_FRAMES] =
        final_fake_attack_pattern.target_neutral_before_b_frames;
    out_ctx[savor::context::key::battle::MACRO_FAKE_FINAL_INPUT_NEUTRAL_FRAMES] =
        final_fake_attack_pattern.input_neutral_after_b_frames;
    savor::progress::ProgressDeets progress{};
    progress.set_flag(CoreProgressFlags::BattleProgress);
    progress.set_flag(CoreProgressFlags::DontRecordHeartbeat);
    out_ctx[savor::context::key::core::PROGRESS_CORE_FLAGS] = progress.flags;
    out_ctx[savor::context::key::battle::MACRO_RESULT] = 1u;
    out_ctx[savor::context::key::battle::MACRO_FAILURE_CODE] = static_cast<std::uint32_t>(FailureCode::NoSteps);
    out_ctx[savor::context::key::battle::MACRO_STEP_COUNT] = 0u;
    out_ctx[savor::context::key::battle::MACRO_MEMORY_ADDR] = 0u;
    out_ctx[savor::context::key::battle::MACRO_MEMORY_BASELINE] = 0u;
    out_ctx[savor::context::key::battle::MACRO_MEMORY_LATEST] = 0u;
    out_ctx[savor::context::key::battle::MACRO_MEMORY_CHANGED] = 0u;
    out_ctx[savor::context::key::battle::MACRO_MEMORY_POLL_COUNT] = 0u;
    out_ctx[savor::context::key::battle::MACRO_MEMORY_GATE_COUNT] = 0u;
    out_ctx[savor::context::key::battle::MACRO_MEMORY_FIRST_BASELINE] = 0u;
    out_ctx[savor::context::key::battle::MACRO_MEMORY_FIRST_LATEST] = 0u;
    out_ctx[savor::context::key::battle::MACRO_MEMORY_FIRST_CHANGED] = 0u;
    out_ctx[savor::context::key::battle::MACRO_MEMORY_FIRST_POLL_COUNT] = 0u;
    out_ctx[savor::context::key::battle::MACRO_MEMORY_REPEAT_BASELINE] = 0u;
    out_ctx[savor::context::key::battle::MACRO_MEMORY_REPEAT_LATEST] = 0u;
    out_ctx[savor::context::key::battle::MACRO_MEMORY_REPEAT_CHANGED] = 0u;
    out_ctx[savor::context::key::battle::MACRO_MEMORY_REPEAT_POLL_COUNT] = 0u;
    out_ctx[savor::context::key::battle::MACRO_MEMORY_REPEAT2_BASELINE] = 0u;
    out_ctx[savor::context::key::battle::MACRO_MEMORY_REPEAT2_LATEST] = 0u;
    out_ctx[savor::context::key::battle::MACRO_MEMORY_REPEAT2_CHANGED] = 0u;
    out_ctx[savor::context::key::battle::MACRO_MEMORY_REPEAT2_POLL_COUNT] = 0u;
    return true;
}

} // namespace phase::battle::macroprobe

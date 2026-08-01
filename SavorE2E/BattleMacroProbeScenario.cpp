#include "BattleMacroProbeScenario.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <thread>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

#include "Common/DbService.h"
#include "DurableLogFile.h"
#include "Execution/DBWorkflowWorkerCoordinator.h"
#include "Phases/Programs/BattleMacroProbe/BattleMacroProbePayload.h"
#include "Runner/IPC/Wire.h"
#include "Runner/Parallel/PRTypes.h"
#include "Worker/ProcessWorker.h"
#include "Worker/TSQueue.h"
#include "Worker/WorkerCapabilityPreflight.h"

namespace savor::e2e {
namespace {

using Clock = std::chrono::steady_clock;
constexpr std::uint32_t kWorkerHostOperationTimeoutMs = 60'000;
constexpr std::uint32_t kWorkerLivenessProbeTimeoutMs = 2'000;
constexpr auto kWorkerLivenessProbeInterval =
    std::chrono::seconds(5);
constexpr std::uint32_t kWorkerLivenessFailureThreshold = 2;

template <typename DrainProgress>
bool WaitForWorkerResult(
    savor::ProcessWorker& worker,
    TSQueue<savor::PRResult>& results,
    std::chrono::milliseconds poll_interval,
    DrainProgress&& drain_progress,
    savor::PRResult* result_out,
    std::string* error_out) {
    if (result_out == nullptr) {
        if (error_out != nullptr) {
            *error_out = "worker result output is unavailable";
        }
        return false;
    }

    auto next_probe = Clock::now() + kWorkerLivenessProbeInterval;
    std::uint32_t consecutive_probe_failures = 0;
    for (;;) {
        drain_progress();
        if (results.try_pop(*result_out)) {
            return true;
        }
        if (!worker.is_running()) {
            if (error_out != nullptr) {
                *error_out = "worker exited before returning a result";
            }
            return false;
        }

        const auto now = Clock::now();
        if (now >= next_probe) {
            savor::wrms::CommandResultPayload probe_result;
            if (worker.probe_liveness(
                    &probe_result,
                    kWorkerLivenessProbeTimeoutMs)) {
                consecutive_probe_failures = 0;
            } else {
                ++consecutive_probe_failures;
                if (consecutive_probe_failures
                    >= kWorkerLivenessFailureThreshold) {
                    if (error_out != nullptr) {
                        *error_out =
                            "worker stopped responding to control-plane "
                            "liveness probes: "
                            + worker.last_error();
                    }
                    return false;
                }
            }
            next_probe = Clock::now() + kWorkerLivenessProbeInterval;
        }
        std::this_thread::sleep_for(
            std::max(poll_interval, std::chrono::milliseconds(1)));
    }
}

std::filesystem::path ResolveWorkspaceRoot(const CliOptions& options) {
    return options.workspace_root.value_or(std::filesystem::temp_directory_path() / "savor-e2e-default");
}

std::filesystem::path ResolveWorkerRoot(const CliOptions& options) {
    return options.worker_dir_root.value_or(ResolveWorkspaceRoot(options) / ".workers");
}

std::string FormatHexPc(std::uint32_t value) {
    std::ostringstream out;
    out << "0x" << std::hex << std::uppercase << value << std::dec;
    return out.str();
}

std::string MacroResultLine(const savor::PRResult& result) {
    std::uint32_t dw_err = 0;
    std::uint32_t macro_result = 1;
    std::uint32_t macro_failure = 0;
    std::uint32_t step_count = 0;
    std::uint32_t last_step = 0;
    std::uint32_t expected_bp = 0;
    std::uint32_t hit_bp = 0;
    std::uint32_t hit_pc = 0;
    std::uint32_t memory_addr = 0;
    std::uint32_t memory_baseline = 0;
    std::uint32_t memory_latest = 0;
    std::uint32_t memory_changed = 0;
    std::uint32_t memory_poll_count = 0;
    result.ps.ctx.get(savor::context::key::core::DW_RUN_OUTCOME_CODE, dw_err);
    result.ps.ctx.get(savor::context::key::battle::MACRO_RESULT, macro_result);
    result.ps.ctx.get(savor::context::key::battle::MACRO_FAILURE_CODE, macro_failure);
    result.ps.ctx.get(savor::context::key::battle::MACRO_STEP_COUNT, step_count);
    result.ps.ctx.get(savor::context::key::battle::MACRO_LAST_STEP_INDEX, last_step);
    result.ps.ctx.get(savor::context::key::battle::MACRO_LAST_EXPECTED_BP, expected_bp);
    result.ps.ctx.get(savor::context::key::battle::MACRO_LAST_HIT_BP, hit_bp);
    result.ps.ctx.get(savor::context::key::battle::MACRO_LAST_HIT_PC, hit_pc);
    result.ps.ctx.get(savor::context::key::battle::MACRO_MEMORY_ADDR, memory_addr);
    result.ps.ctx.get(savor::context::key::battle::MACRO_MEMORY_BASELINE, memory_baseline);
    result.ps.ctx.get(savor::context::key::battle::MACRO_MEMORY_LATEST, memory_latest);
    result.ps.ctx.get(savor::context::key::battle::MACRO_MEMORY_CHANGED, memory_changed);
    result.ps.ctx.get(savor::context::key::battle::MACRO_MEMORY_POLL_COUNT, memory_poll_count);

    std::ostringstream line;
    line << "[battle-macro-probe-result]"
         << " job=" << result.job_id
         << " worker=" << result.worker_id
         << " ok=" << (result.ps.ok ? "true" : "false")
         << " w_err=" << savor::WErrToString(result.ps.w_err) << "(" << static_cast<int>(result.ps.w_err) << ")"
         << " dw_err=" << dw_err
         << " macro_result=" << macro_result
         << " failure=" << macro_failure
         << " failure_name=" << phase::battle::macroprobe::FailureCodeName(static_cast<phase::battle::macroprobe::FailureCode>(macro_failure))
         << " step_count=" << step_count
         << " last_step=" << last_step
         << " expected_bp=" << expected_bp
         << " hit_bp=" << hit_bp
         << " hit_pc=" << FormatHexPc(hit_pc)
         << " memory_addr=" << FormatHexPc(memory_addr)
         << " memory_before=" << FormatHexPc(memory_baseline)
         << " memory_after=" << FormatHexPc(memory_latest)
         << " memory_changed=" << memory_changed
         << " memory_polls=" << memory_poll_count;
    return line.str();
}

std::string EscapeLogValue(std::string_view value) {
    std::string out;
    out.reserve(value.size());
    for (const char c : value) {
        switch (c) {
        case '\\': out += "\\\\"; break;
        case '"': out += "\\\""; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default: out.push_back(c); break;
        }
    }
    return out;
}

std::string MacroProgressLine(const savor::PRProgress& progress) {
    std::ostringstream line;
    line << "[battle-macro-probe-progress]"
         << " job=" << progress.job_id
         << " worker=" << progress.worker_id
         << " text=\"" << EscapeLogValue(progress.text) << "\"";
    return line.str();
}

const char* SweepGateModeName(phase::battle::macroprobe::FakeAttackMemoryGateMode mode) {
    switch (mode) {
    case phase::battle::macroprobe::FakeAttackMemoryGateMode::TargetSide: return "target";
    case phase::battle::macroprobe::FakeAttackMemoryGateMode::InputSide: return "input";
    case phase::battle::macroprobe::FakeAttackMemoryGateMode::Both: return "both";
    default: return "unknown";
    }
}

struct FakeAttackSweepCandidate {
    std::uint32_t index{0};
    phase::battle::macroprobe::FakeAttackPattern pattern{};

    std::uint32_t frame_cost() const {
        return pattern.target_neutral_before_b_frames + pattern.input_neutral_after_b_frames;
    }

    std::string id() const {
        std::ostringstream out;
        out << SweepGateModeName(pattern.memory_gate_mode)
            << "-tn" << pattern.target_neutral_before_b_frames
            << "-in" << pattern.input_neutral_after_b_frames;
        return out.str();
    }
};

struct FakeAttackSweepTrial {
    bool worker_ok{false};
    bool macro_ok{false};
    bool first_ok{false};
    bool repeat1_ok{false};
    bool repeat2_ok{false};
    std::uint32_t first_before{0};
    std::uint32_t first_after{0};
    std::uint32_t repeat1_before{0};
    std::uint32_t repeat1_after{0};
    std::uint32_t repeat2_before{0};
    std::uint32_t repeat2_after{0};
    std::uint32_t first_polls{0};
    std::uint32_t repeat1_polls{0};
    std::uint32_t repeat2_polls{0};
    std::uint32_t failure_code{0};
};

struct FakeAttackSweepSummary {
    FakeAttackSweepCandidate candidate{};
    std::uint32_t trials{0};
    std::uint32_t failures{0};
    std::uint32_t first_successes{0};
    std::uint32_t repeat1_successes{0};
    std::uint32_t repeat2_successes{0};
    std::uint64_t total_polls{0};
    std::uint32_t max_polls{0};

    bool first_reliable() const { return trials != 0 && first_successes == trials; }
    bool repeat1_reliable() const { return trials != 0 && repeat1_successes == trials; }
    bool repeat2_reliable() const { return trials != 0 && repeat2_successes == trials; }
    bool repeat_reliable() const { return repeat1_reliable() && repeat2_reliable(); }
    bool reliable() const { return trials != 0 && failures == 0 && first_reliable() && repeat_reliable(); }
    double avg_polls() const {
        return trials == 0 ? 0.0 : static_cast<double>(total_polls) / static_cast<double>(trials * 3u);
    }
};

std::vector<FakeAttackSweepCandidate> BuildFakeAttackSweepCandidates(
    std::uint32_t min_target_neutral,
    std::uint32_t max_target_neutral,
    std::uint32_t min_input_neutral,
    std::uint32_t max_input_neutral) {
    std::vector<FakeAttackSweepCandidate> candidates;
    const auto modes = {
        phase::battle::macroprobe::FakeAttackMemoryGateMode::TargetSide,
        phase::battle::macroprobe::FakeAttackMemoryGateMode::InputSide,
        phase::battle::macroprobe::FakeAttackMemoryGateMode::Both,
    };
    for (std::uint32_t target_neutral = min_target_neutral; target_neutral <= max_target_neutral; ++target_neutral) {
        for (std::uint32_t input_neutral = min_input_neutral; input_neutral <= max_input_neutral; ++input_neutral) {
            for (const auto mode : modes) {
                candidates.push_back(FakeAttackSweepCandidate{
                    .index = static_cast<std::uint32_t>(candidates.size()),
                    .pattern = phase::battle::macroprobe::FakeAttackPattern{
                        .memory_gate_mode = mode,
                        .target_neutral_before_b_frames = target_neutral,
                        .input_neutral_after_b_frames = input_neutral,
                    },
                });
            }
        }
    }
    return candidates;
}

bool IsBetterSweepRecommendation(const FakeAttackSweepSummary& lhs, const FakeAttackSweepSummary& rhs) {
    if (lhs.candidate.frame_cost() != rhs.candidate.frame_cost()) {
        return lhs.candidate.frame_cost() < rhs.candidate.frame_cost();
    }
    if (lhs.max_polls != rhs.max_polls) {
        return lhs.max_polls < rhs.max_polls;
    }
    if (lhs.avg_polls() != rhs.avg_polls()) {
        return lhs.avg_polls() < rhs.avg_polls();
    }
    return lhs.candidate.index < rhs.candidate.index;
}

bool IsSweepRecommendationEligible(const FakeAttackSweepSummary& summary) {
    return summary.candidate.pattern.memory_gate_mode != phase::battle::macroprobe::FakeAttackMemoryGateMode::Both;
}


std::string JsonEscape(std::string_view value) {
    std::string out;
    out.reserve(value.size() + 8);
    for (const char c : value) {
        switch (c) {
        case '\\': out += "\\\\"; break;
        case '"': out += "\\\""; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default: out.push_back(c); break;
        }
    }
    return out;
}

bool IsInteractiveStdin() {
#ifdef _WIN32
    return _isatty(_fileno(stdin)) != 0;
#else
    return isatty(fileno(stdin)) != 0;
#endif
}

bool ReadPromptLine(const std::string& prompt, std::string* out, std::string* error_out) {
    std::cout << prompt;
    std::cout.flush();
    if (!std::getline(std::cin, *out)) {
        if (error_out) *error_out = "battle macro probe input was closed";
        return false;
    }
    return true;
}

std::string TrimPromptValue(std::string value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())) != 0) {
        value.erase(value.begin());
    }
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())) != 0) {
        value.pop_back();
    }
    if (value.size() >= 2
        && ((value.front() == '"' && value.back() == '"') || (value.front() == '\'' && value.back() == '\''))) {
        value = value.substr(1, value.size() - 2);
    }
    return value;
}

bool IsCancelInput(const std::string& value) {
    const auto trimmed = TrimPromptValue(value);
    return trimmed == "q" || trimmed == "Q" || trimmed == "cancel";
}

bool IsBackInput(const std::string& value) {
    const auto trimmed = TrimPromptValue(value);
    return trimmed == "b" || trimmed == "B" || trimmed == "back";
}

enum class PromptResult {
    Done,
    Back,
    Cancel,
};

PromptResult PromptCommand(int character_index, phase::battle::macroprobe::MacroCommand* command, std::string* error_out) {
    for (;;) {
        std::string line;
        if (!ReadPromptLine("Character " + std::to_string(character_index) + " command [block/focus/attack, b back, q cancel]: ", &line, error_out)) {
            return PromptResult::Cancel;
        }
        if (IsCancelInput(line)) {
            if (error_out) *error_out = "battle macro probe cancelled";
            return PromptResult::Cancel;
        }
        if (IsBackInput(line)) {
            return PromptResult::Back;
        }
        std::vector<phase::battle::macroprobe::MacroCommand> parsed_command;
        if (!phase::battle::macroprobe::ParseCommandPlanSpec(line, &parsed_command, nullptr) || parsed_command.size() != 1) {
            std::cout << "Enter block, focus, attack, b, or q.\n";
            continue;
        }
        *command = parsed_command.front();
        if (command->mode != phase::battle::macroprobe::MacroMode::Attack) {
            return PromptResult::Done;
        }

        for (;;) {
            std::string target_line;
            if (!ReadPromptLine("Attack target slot [4..11, b back, q cancel]: ", &target_line, error_out)) {
                return PromptResult::Cancel;
            }
            if (IsCancelInput(target_line)) {
                if (error_out) *error_out = "battle macro probe cancelled";
                return PromptResult::Cancel;
            }
            if (IsBackInput(target_line)) {
                break;
            }
            std::vector<phase::battle::macroprobe::MacroCommand> parsed;
            std::string parse_error;
            if (phase::battle::macroprobe::ParseCommandPlanSpec("attack:" + target_line, &parsed, &parse_error)) {
                *command = parsed.front();
                return PromptResult::Done;
            }
            std::cout << "Enter a target slot from 4 through 11, b, or q.\n";
        }
    }
}

PromptResult PromptBattleMacroPlan(std::vector<phase::battle::macroprobe::MacroCommand>* commands, std::string* error_out) {
    if (!IsInteractiveStdin()) {
        if (error_out) *error_out = "battle_macro_probe requires --battle-plan or --battle-macro when stdin is not interactive";
        return PromptResult::Cancel;
    }

    std::cout << "Battle Macro Probe Plan\n";
    std::vector<phase::battle::macroprobe::MacroCommand> plan;
    plan.resize(2);
    int step = 0;
    while (step < 2) {
        const auto result = PromptCommand(step + 1, &plan[step], error_out);
        if (result == PromptResult::Cancel) return PromptResult::Cancel;
        if (result == PromptResult::Back) {
            if (step == 0) return PromptResult::Back;
            --step;
            continue;
        }
        ++step;
    }

    *commands = std::move(plan);
    return PromptResult::Done;
}

PromptResult PromptBattleMacroSavestatePath(
    const std::filesystem::path& default_path,
    std::filesystem::path* savestate_path,
    std::string* error_out) {
    if (!IsInteractiveStdin()) {
        if (error_out) *error_out = "battle_macro_probe requires --savestate-file when stdin is not interactive";
        return PromptResult::Cancel;
    }

    for (;;) {
        std::string line;
        if (!ReadPromptLine("Savestate path [Enter current, b back, q cancel]\n  current: "
            + default_path.string() + "\n> ", &line, error_out)) {
            return PromptResult::Cancel;
        }
        line = TrimPromptValue(std::move(line));
        if (IsCancelInput(line)) {
            if (error_out) *error_out = "battle macro probe cancelled";
            return PromptResult::Cancel;
        }
        if (IsBackInput(line)) {
            std::cout << "Already at the first prompt.\n";
            continue;
        }

        const auto candidate = line.empty() ? default_path : std::filesystem::path(line);
        if (candidate.empty()) {
            std::cout << "Enter a savestate path, or q.\n";
            continue;
        }
        if (!std::filesystem::exists(candidate)) {
            std::cout << "Savestate file does not exist: " << candidate.string() << "\n";
            continue;
        }
        *savestate_path = candidate;
        return PromptResult::Done;
    }
}

bool ParseFakeAttackCount(const std::string& value, std::uint32_t* count_out) {
    const auto trimmed = TrimPromptValue(value);
    if (trimmed.empty()) return false;
    std::uint32_t count = 0;
    const char* first = trimmed.data();
    const char* last = trimmed.data() + trimmed.size();
    const auto result = std::from_chars(first, last, count);
    if (result.ec != std::errc{} || result.ptr != last) return false;
    if (count > 255) return false;
    if (count_out) *count_out = count;
    return true;
}

PromptResult PromptBattleMacroFakeAttackCount(
    std::uint32_t default_count,
    std::uint32_t* count_out,
    std::string* error_out) {
    if (!IsInteractiveStdin()) {
        if (error_out) *error_out = "battle_macro_probe requires explicit battle plan arguments when stdin is not interactive";
        return PromptResult::Cancel;
    }

    for (;;) {
        std::string line;
        if (!ReadPromptLine("Fake attacks [0..255, Enter current, b back, q cancel]\n  current: "
            + std::to_string(default_count) + "\n> ", &line, error_out)) {
            return PromptResult::Cancel;
        }
        line = TrimPromptValue(std::move(line));
        if (IsCancelInput(line)) {
            if (error_out) *error_out = "battle macro probe cancelled";
            return PromptResult::Cancel;
        }
        if (IsBackInput(line)) {
            return PromptResult::Back;
        }
        if (line.empty()) {
            *count_out = default_count;
            return PromptResult::Done;
        }
        std::uint32_t parsed = 0;
        if (ParseFakeAttackCount(line, &parsed)) {
            *count_out = parsed;
            return PromptResult::Done;
        }
        std::cout << "Enter a fake attack count from 0 through 255.\n";
    }
}

struct BattleMacroInteractiveDraft {
    std::filesystem::path savestate_path;
    std::vector<phase::battle::macroprobe::MacroCommand> commands;
    std::uint32_t fake_attack_count{0};
};

PromptResult PromptBattleMacroInteractiveDraft(
    const std::filesystem::path& default_savestate_path,
    std::uint32_t default_fake_attack_count,
    BattleMacroInteractiveDraft* draft,
    std::string* error_out) {
    enum class Step {
        Savestate,
        FakeAttacks,
        Plan,
        Done,
    };

    Step step = Step::Savestate;
    std::filesystem::path savestate_path = default_savestate_path;
    std::vector<phase::battle::macroprobe::MacroCommand> commands;
    std::uint32_t fake_attack_count = default_fake_attack_count;

    while (step != Step::Done) {
        switch (step) {
        case Step::Savestate: {
            const auto result = PromptBattleMacroSavestatePath(savestate_path, &savestate_path, error_out);
            if (result == PromptResult::Cancel) return PromptResult::Cancel;
            step = Step::FakeAttacks;
            break;
        }
        case Step::FakeAttacks: {
            const auto result = PromptBattleMacroFakeAttackCount(fake_attack_count, &fake_attack_count, error_out);
            if (result == PromptResult::Cancel) return PromptResult::Cancel;
            if (result == PromptResult::Back) {
                step = Step::Savestate;
                break;
            }
            step = Step::Plan;
            break;
        }
        case Step::Plan: {
            const auto result = PromptBattleMacroPlan(&commands, error_out);
            if (result == PromptResult::Cancel) return PromptResult::Cancel;
            if (result == PromptResult::Back) {
                step = Step::FakeAttacks;
                break;
            }
            step = Step::Done;
            break;
        }
        case Step::Done:
            break;
        }
    }

    draft->savestate_path = std::move(savestate_path);
    draft->commands = std::move(commands);
    draft->fake_attack_count = fake_attack_count;
    return PromptResult::Done;
}

bool ResolveBattleMacroPlan(
    const CliOptions& options,
    std::vector<phase::battle::macroprobe::MacroCommand>* commands,
    std::string* error_out) {
    if (options.battle_macro_plan_spec.has_value()) {
        return phase::battle::macroprobe::ParseCommandPlanSpec(*options.battle_macro_plan_spec, commands, error_out);
    }

    if (options.battle_macro_args_supplied) {
        phase::battle::macroprobe::MacroMode mode = phase::battle::macroprobe::MacroMode::Attack;
        if (!phase::battle::macroprobe::TryParseMacroMode(options.battle_macro_mode, &mode)) {
            if (error_out) *error_out = "unknown --battle-macro: " + options.battle_macro_mode;
            return false;
        }
        const std::uint32_t target_slot = static_cast<std::uint32_t>(options.battle_macro_target_slot.value_or(4));
        commands->assign(1, phase::battle::macroprobe::MacroCommand{.mode = mode, .target_slot = target_slot});
        return true;
    }

    const auto result = PromptBattleMacroPlan(commands, error_out);
    return result == PromptResult::Done;
}

bool WriteFakeAttackSweepJson(
    const std::filesystem::path& path,
    const std::vector<FakeAttackSweepSummary>& summaries,
    const FakeAttackSweepSummary* repeat_recommendation,
    std::string* error_out) {
    std::error_code ec;
    if (!path.parent_path().empty()) {
        std::filesystem::create_directories(path.parent_path(), ec);
        if (ec) {
            if (error_out) *error_out = "failed creating sweep report directory: " + ec.message();
            return false;
        }
    }

    std::ofstream out(path, std::ios::out | std::ios::trunc);
    if (!out) {
        if (error_out) *error_out = "failed opening sweep report: " + path.string();
        return false;
    }

    const auto write_recommendation = [&](const char* name, const FakeAttackSweepSummary* summary) {
        out << "  \"" << name << "\": ";
        if (summary == nullptr) {
            out << "null";
            return;
        }
        out << "{\"candidate\":\"" << JsonEscape(summary->candidate.id()) << "\""
            << ",\"gate\":\"" << SweepGateModeName(summary->candidate.pattern.memory_gate_mode) << "\""
            << ",\"target_neutral\":" << summary->candidate.pattern.target_neutral_before_b_frames
            << ",\"input_neutral\":" << summary->candidate.pattern.input_neutral_after_b_frames
            << ",\"frame_cost\":" << summary->candidate.frame_cost()
            << ",\"max_polls\":" << summary->max_polls
            << ",\"avg_polls\":" << std::fixed << std::setprecision(3) << summary->avg_polls()
            << "}";
    };

    out << "{\n";
    out << "  \"fixed_last_pattern\": {\"gate\":\"target\",\"target_neutral\":0,\"input_neutral\":0},\n";
    write_recommendation("repeat_recommendation", repeat_recommendation);
    out << ",\n";
    out << "  \"candidates\": [\n";
    for (std::size_t i = 0; i < summaries.size(); ++i) {
        const auto& summary = summaries[i];
        out << "    {"
            << "\"candidate\":\"" << JsonEscape(summary.candidate.id()) << "\""
            << ",\"gate\":\"" << SweepGateModeName(summary.candidate.pattern.memory_gate_mode) << "\""
            << ",\"target_neutral\":" << summary.candidate.pattern.target_neutral_before_b_frames
            << ",\"input_neutral\":" << summary.candidate.pattern.input_neutral_after_b_frames
            << ",\"frame_cost\":" << summary.candidate.frame_cost()
            << ",\"trials\":" << summary.trials
            << ",\"reliable\":" << (summary.reliable() ? "true" : "false")
            << ",\"failures\":" << summary.failures
            << ",\"first_successes\":" << summary.first_successes
            << ",\"repeat1_successes\":" << summary.repeat1_successes
            << ",\"repeat2_successes\":" << summary.repeat2_successes
            << ",\"repeat_reliable\":" << (summary.repeat_reliable() ? "true" : "false")
            << ",\"max_polls\":" << summary.max_polls
            << ",\"avg_polls\":" << std::fixed << std::setprecision(3) << summary.avg_polls()
            << "}";
        if (i + 1 < summaries.size()) {
            out << ",";
        }
        out << "\n";
    }
    out << "  ]\n";
    out << "}\n";
    return true;
}

bool RunBattleFakeAttackSweep(
    const CliOptions& options,
    const std::filesystem::path& worker_exe,
    savor::ProcessWorker& worker,
    TSQueue<savor::PRResult>& results,
    TSQueue<savor::PRProgress>& progress,
    DurableLogFile& durable_log,
    std::uint64_t* next_job_id,
    std::string* error_out) {
    constexpr std::uint32_t kTransitionNeutralFrames = 3;
    constexpr std::uint32_t kFakeAttacksPerTrial = 3;
    const phase::battle::macroprobe::FakeAttackPattern kFixedLastFakeAttackPattern{
        .memory_gate_mode = phase::battle::macroprobe::FakeAttackMemoryGateMode::TargetSide,
        .target_neutral_before_b_frames = 0,
        .input_neutral_after_b_frames = 0,
    };

    std::vector<phase::battle::macroprobe::MacroCommand> commands;
    if (options.battle_macro_plan_spec.has_value()) {
        if (!phase::battle::macroprobe::ParseCommandPlanSpec(*options.battle_macro_plan_spec, &commands, error_out)) {
            return false;
        }
    } else {
        commands = {
            phase::battle::macroprobe::MacroCommand{
                .mode = phase::battle::macroprobe::MacroMode::Block,
                .target_slot = 4,
            },
            phase::battle::macroprobe::MacroCommand{
                .mode = phase::battle::macroprobe::MacroMode::Focus,
                .target_slot = 4,
            },
        };
    }

    const auto candidates = BuildFakeAttackSweepCandidates(
        static_cast<std::uint32_t>(options.battle_fake_sweep_min_target_neutral),
        static_cast<std::uint32_t>(options.battle_fake_sweep_max_target_neutral),
        static_cast<std::uint32_t>(options.battle_fake_sweep_min_input_neutral),
        static_cast<std::uint32_t>(options.battle_fake_sweep_max_input_neutral));
    std::vector<FakeAttackSweepSummary> summaries;
    summaries.reserve(candidates.size());

    const auto report_path = options.battle_fake_sweep_output.value_or(
        durable_log.path().parent_path() / "battle-fake-sweep-summary.json");
    durable_log.AppendLine("[battle-fake-sweep-start] candidates=" + std::to_string(candidates.size())
        + " trials=" + std::to_string(options.battle_fake_sweep_trials)
        + " target_neutral_range=" + std::to_string(options.battle_fake_sweep_min_target_neutral)
        + ".." + std::to_string(options.battle_fake_sweep_max_target_neutral)
        + " input_neutral_range=" + std::to_string(options.battle_fake_sweep_min_input_neutral)
        + ".." + std::to_string(options.battle_fake_sweep_max_input_neutral)
        + " fake_attacks_per_trial=" + std::to_string(kFakeAttacksPerTrial)
        + " plan=" + phase::battle::macroprobe::FormatCommandPlanSpec(commands)
        + " report=\"" + EscapeLogValue(report_path.string()) + "\""
        + " worker=\"" + EscapeLogValue(worker_exe.string()) + "\"");

    const auto drain_progress = [&]() {
        savor::PRProgress progress_item{};
        while (progress.try_pop(progress_item)) {
        }
    };

    for (const auto& candidate : candidates) {
        FakeAttackSweepSummary summary{};
        summary.candidate = candidate;
        summary.trials = static_cast<std::uint32_t>(options.battle_fake_sweep_trials);
        phase::battle::macroprobe::FailureCode build_failure = phase::battle::macroprobe::FailureCode::Ok;
        const auto steps = phase::battle::macroprobe::BuildMacroProbePlanSteps(
            commands,
            kTransitionNeutralFrames,
            kFakeAttacksPerTrial,
            candidate.pattern,
            candidate.pattern,
            kFixedLastFakeAttackPattern,
            nullptr,
            &build_failure);
        if (steps.empty() || build_failure != phase::battle::macroprobe::FailureCode::Ok) {
            summary.failures = summary.trials;
            summaries.push_back(summary);
            continue;
        }

        for (int trial = 0; trial < options.battle_fake_sweep_trials; ++trial) {
            savor::PSInit init{};
            init.savestate_path = options.savestate_file.string();
            init.derived_buffer_type = savor::DBuf::DK_None;
            if (!worker.ctl_set_program(savor::PK_None, savor::PK_BattleMacroProbe, init)) {
                if (error_out) *error_out = "battle fake sweep worker failed SET_PROGRAM";
                return false;
            }
            if (!worker.ctl_activate_main()) {
                if (error_out) *error_out = "battle fake sweep worker failed ACTIVATE_MAIN";
                return false;
            }

            std::vector<std::uint8_t> payload;
            phase::battle::macroprobe::encode_payload(
                phase::battle::macroprobe::EncodeSpec{
                    .commands = commands,
                    .transition_neutral_frames = kTransitionNeutralFrames,
                    .fake_attack_count = kFakeAttacksPerTrial,
                    .fake_attack_pattern = candidate.pattern,
                    .use_mixed_fake_attack_patterns = true,
                    .first_fake_attack_pattern = candidate.pattern,
                    .repeat_fake_attack_pattern = candidate.pattern,
                    .use_final_fake_attack_pattern = true,
                    .final_fake_attack_pattern = kFixedLastFakeAttackPattern,
                },
                payload);
            savor::PSJob job{};
            job.payload = std::move(payload);
            if (!worker.try_acquire_slot()) {
                if (error_out) *error_out = "battle fake sweep worker slot was unexpectedly busy";
                return false;
            }
            const std::uint64_t job_id = (*next_job_id)++;
            if (!worker.send_job(job_id, job_id, job)) {
                if (error_out) *error_out = "battle fake sweep worker failed sending job";
                return false;
            }

            savor::PRResult result{};
            std::string wait_error;
            const bool have_result = WaitForWorkerResult(
                worker,
                results,
                std::chrono::milliseconds(options.poll_ms),
                drain_progress,
                &result,
                &wait_error);
            drain_progress();
            if (!have_result) {
                if (error_out != nullptr) {
                    *error_out =
                        "battle fake sweep worker result failed: "
                        + wait_error;
                }
                return false;
            }

            FakeAttackSweepTrial trial_result{};
            std::uint32_t macro_result = 1;
            result.ps.ctx.get(savor::context::key::battle::MACRO_RESULT, macro_result);
            result.ps.ctx.get(savor::context::key::battle::MACRO_FAILURE_CODE, trial_result.failure_code);
            result.ps.ctx.get(savor::context::key::battle::MACRO_MEMORY_FIRST_BASELINE, trial_result.first_before);
            result.ps.ctx.get(savor::context::key::battle::MACRO_MEMORY_FIRST_LATEST, trial_result.first_after);
            result.ps.ctx.get(savor::context::key::battle::MACRO_MEMORY_FIRST_POLL_COUNT, trial_result.first_polls);
            result.ps.ctx.get(savor::context::key::battle::MACRO_MEMORY_REPEAT_BASELINE, trial_result.repeat1_before);
            result.ps.ctx.get(savor::context::key::battle::MACRO_MEMORY_REPEAT_LATEST, trial_result.repeat1_after);
            result.ps.ctx.get(savor::context::key::battle::MACRO_MEMORY_REPEAT_POLL_COUNT, trial_result.repeat1_polls);
            result.ps.ctx.get(savor::context::key::battle::MACRO_MEMORY_REPEAT2_BASELINE, trial_result.repeat2_before);
            result.ps.ctx.get(savor::context::key::battle::MACRO_MEMORY_REPEAT2_LATEST, trial_result.repeat2_after);
            result.ps.ctx.get(savor::context::key::battle::MACRO_MEMORY_REPEAT2_POLL_COUNT, trial_result.repeat2_polls);
            std::uint32_t first_changed = 0;
            std::uint32_t repeat1_changed = 0;
            std::uint32_t repeat2_changed = 0;
            result.ps.ctx.get(savor::context::key::battle::MACRO_MEMORY_FIRST_CHANGED, first_changed);
            result.ps.ctx.get(savor::context::key::battle::MACRO_MEMORY_REPEAT_CHANGED, repeat1_changed);
            result.ps.ctx.get(savor::context::key::battle::MACRO_MEMORY_REPEAT2_CHANGED, repeat2_changed);
            trial_result.worker_ok = result.ps.ok;
            trial_result.macro_ok = result.ps.ok && macro_result == 0;
            trial_result.first_ok = first_changed != 0 && trial_result.first_before != trial_result.first_after;
            trial_result.repeat1_ok = repeat1_changed != 0 && trial_result.repeat1_before != trial_result.repeat1_after;
            trial_result.repeat2_ok = repeat2_changed != 0 && trial_result.repeat2_before != trial_result.repeat2_after;

            if (!trial_result.worker_ok
                || !trial_result.macro_ok
                || !trial_result.first_ok
                || !trial_result.repeat1_ok
                || !trial_result.repeat2_ok) {
                ++summary.failures;
            }
            if (trial_result.first_ok) ++summary.first_successes;
            if (trial_result.repeat1_ok) ++summary.repeat1_successes;
            if (trial_result.repeat2_ok) ++summary.repeat2_successes;
            const std::uint32_t trial_polls = trial_result.first_polls + trial_result.repeat1_polls + trial_result.repeat2_polls;
            summary.total_polls += trial_polls;
            summary.max_polls = std::max(
                summary.max_polls,
                std::max(trial_result.first_polls, std::max(trial_result.repeat1_polls, trial_result.repeat2_polls)));

            std::ostringstream trial_line;
            trial_line << "[battle-fake-sweep-trial]"
                << " candidate=" << candidate.id()
                << " trial=" << (trial + 1)
                << " gate=" << SweepGateModeName(candidate.pattern.memory_gate_mode)
                << " target_neutral=" << candidate.pattern.target_neutral_before_b_frames
                << " input_neutral=" << candidate.pattern.input_neutral_after_b_frames
                << " first_ok=" << (trial_result.first_ok ? 1 : 0)
                << " repeat1_ok=" << (trial_result.repeat1_ok ? 1 : 0)
                << " repeat2_ok=" << (trial_result.repeat2_ok ? 1 : 0)
                << " first_before=" << FormatHexPc(trial_result.first_before)
                << " first_after=" << FormatHexPc(trial_result.first_after)
                << " repeat1_before=" << FormatHexPc(trial_result.repeat1_before)
                << " repeat1_after=" << FormatHexPc(trial_result.repeat1_after)
                << " repeat2_before=" << FormatHexPc(trial_result.repeat2_before)
                << " repeat2_after=" << FormatHexPc(trial_result.repeat2_after)
                << " polls=" << trial_polls
                << " failure=" << trial_result.failure_code;
            durable_log.AppendLine(trial_line.str());
        }

        std::ostringstream summary_line;
        summary_line << "[battle-fake-sweep-summary]"
            << " candidate=" << candidate.id()
            << " reliable=" << (summary.reliable() ? 1 : 0)
            << " failures=" << summary.failures
            << " first_successes=" << summary.first_successes << "/" << summary.trials
            << " repeat1_successes=" << summary.repeat1_successes << "/" << summary.trials
            << " repeat2_successes=" << summary.repeat2_successes << "/" << summary.trials
            << " max_polls=" << summary.max_polls
            << " avg_polls=" << std::fixed << std::setprecision(3) << summary.avg_polls()
            << " frame_cost=" << candidate.frame_cost();
        durable_log.AppendLine(summary_line.str());
        std::cout << summary_line.str() << '\n';
        summaries.push_back(summary);
    }

    const FakeAttackSweepSummary* repeat_recommendation = nullptr;
    for (const auto& summary : summaries) {
        if (IsSweepRecommendationEligible(summary)
            && summary.repeat_reliable()
            && (repeat_recommendation == nullptr || IsBetterSweepRecommendation(summary, *repeat_recommendation))) {
            repeat_recommendation = &summary;
        }
    }

    std::ostringstream recommendation_line;
    recommendation_line << "[battle-fake-sweep-recommendation]"
        << " fixed_last=target-tn0-in0";
    if (repeat_recommendation != nullptr) {
        recommendation_line << " repeat=" << repeat_recommendation->candidate.id();
    } else {
        recommendation_line << " repeat=none";
    }
    recommendation_line << " report=\"" << EscapeLogValue(report_path.string()) << "\"";
    durable_log.AppendLine(recommendation_line.str());
    std::cout << recommendation_line.str() << '\n';

    return WriteFakeAttackSweepJson(
        report_path,
        summaries,
        repeat_recommendation,
        error_out);
}

bool IsBattleMacroPromptCancel(const std::string& error) {
    return error == "battle macro probe cancelled";
}

} // namespace

bool RunBattleMacroProbeScenario(
    const CliOptions& options,
    const char* argv0,
    savor::db::core::DBService*,
    std::string* error_out) {
    const auto source_worker_exe = ResolveWorkerExePath(argv0);
    if (!std::filesystem::exists(source_worker_exe)) {
        if (error_out) {
            *error_out =
                "SavorWorker.exe was not found next to SavorE2E: "
                + source_worker_exe.string();
        }
        return false;
    }

    const auto preflight = savor::RunWorkerCapabilityPreflight(
        savor::WorkerCapabilityPreflightRequest{
            .worker_exe_path = source_worker_exe.string(),
            .timeout_ms = kWorkerHostOperationTimeoutMs,
            .required_capabilities = savor::runtime::CapabilityMask(
                savor::runtime::WorkerCapability::WorksetDispatch),
            .require_complete_exact_catalog = true,
        });
    if (!preflight) {
        if (error_out) {
            *error_out = preflight.message;
        }
        return false;
    }

    std::filesystem::path worker_exe;
    std::string runtime_error;
    if (!savor::runner::parallel::savordb::DBWorkflowWorkerCoordinator::
            MaterializeWorkerRuntimeForTest(
                0,
                savor::runner::parallel::savordb::DBWorkflowWorkerCoordinatorConfig{
                    .worker_exe_path = source_worker_exe.string(),
                    .iso_path = options.iso_path.string(),
                    .dolphin_base_dir = options.dolphin_base_dir.string(),
                },
                &worker_exe,
                &runtime_error)) {
        if (error_out) {
            *error_out =
                "failed preparing direct battle macro worker runtime: "
                + runtime_error;
        }
        return false;
    }

    constexpr std::uint32_t kTransitionNeutralFrames = 3;
    auto scenario_options = options;
    if (scenario_options.scenarios.size() > 1
        && !scenario_options.battle_macro_args_supplied
        && !scenario_options.battle_macro_plan_spec.has_value()) {
        // Multi-scenario matrices must be unattended. A direct, single-scenario
        // macro probe keeps its interactive command-development loop.
        scenario_options.battle_macro_args_supplied = true;
    }
    const bool interactive_loop =
        !scenario_options.battle_macro_args_supplied
        && !scenario_options.battle_macro_plan_spec.has_value();

    DurableLogFile durable_log;
    if (!durable_log.Open(options, options.scenario, error_out)) {
        return false;
    }
    std::cout << "[durable-log] path=" << durable_log.path().string() << '\n';

    const auto workspace_root = ResolveWorkspaceRoot(options);
    const auto worker_root = ResolveWorkerRoot(options) / "battle-macro-probe";
    const auto user_dir = worker_root / "worker-0";
    const auto screenshots = options.visual_screenshot_dir.value_or(workspace_root / "visual-screenshots");
    std::error_code ec;
    std::filesystem::create_directories(user_dir, ec);
    if (ec) {
        if (error_out) *error_out = "failed creating worker directory: " + user_dir.string() + " error=" + ec.message();
        return false;
    }
    std::filesystem::create_directories(screenshots, ec);

    TSQueue<savor::PRResult> results;
    TSQueue<savor::PRProgress> progress;
    savor::ProcessWorker worker;
    savor::ProcStartParams params{};
    params.worker_id = 0;
    params.exe_path = worker_exe.string();
    params.iso_path = options.iso_path.string();
    params.dolphin_base_dir = options.dolphin_base_dir.string();
    params.user_dir = user_dir.string();
    params.vm_control = true;
    params.visual = options.visual_worker || options.battle_macro_debug;
    params.visual_debug = options.battle_macro_debug;
    params.runtime_artifact_root = screenshots.string();

    if (!worker.start(params, &results)) {
        if (error_out) *error_out = "failed starting SavorWorker for battle macro probe";
        return false;
    }
    worker.set_progress_queue(&progress);

    const auto stop_worker = [&]() {
        worker.stop();
    };

    if (!worker.wait_ready(kWorkerHostOperationTimeoutMs)) {
        stop_worker();
        if (error_out) *error_out = "battle macro worker did not become ready";
        return false;
    }

    std::uint64_t next_job_id = 1;
    std::uint64_t successful_runs = 0;
    std::filesystem::path current_savestate = options.savestate_file;
    std::uint32_t current_fake_attack_count = static_cast<std::uint32_t>(options.battle_macro_fake_attacks.value_or(0));
    if (options.battle_fake_attack_sweep) {
        const bool ok = RunBattleFakeAttackSweep(
            options,
            worker_exe,
            worker,
            results,
            progress,
            durable_log,
            &next_job_id,
            error_out);
        stop_worker();
        return ok;
    }
    for (;;) {
        std::filesystem::path run_savestate;
        std::vector<phase::battle::macroprobe::MacroCommand> commands;
        std::uint32_t fake_attack_count = current_fake_attack_count;
        if (interactive_loop) {
            BattleMacroInteractiveDraft draft{};
            std::string prompt_error;
            const auto prompt_result = PromptBattleMacroInteractiveDraft(
                current_savestate,
                current_fake_attack_count,
                &draft,
                &prompt_error);
            if (prompt_result != PromptResult::Done) {
                stop_worker();
                if (successful_runs > 0 && IsBattleMacroPromptCancel(prompt_error)) {
                    durable_log.AppendLine("[battle-macro-probe-loop-end] reason=cancel successful_runs="
                        + std::to_string(successful_runs));
                    return true;
                }
                if (error_out) *error_out = prompt_error;
                return false;
            }
            run_savestate = std::move(draft.savestate_path);
            commands = std::move(draft.commands);
            fake_attack_count = draft.fake_attack_count;
        } else {
            run_savestate = current_savestate;
            std::string prompt_error;
            if (!ResolveBattleMacroPlan(scenario_options, &commands, &prompt_error)) {
                stop_worker();
                if (error_out) *error_out = prompt_error;
                return false;
            }
            fake_attack_count = current_fake_attack_count;
        }
        current_savestate = run_savestate;
        current_fake_attack_count = fake_attack_count;

        phase::battle::macroprobe::FailureCode build_failure = phase::battle::macroprobe::FailureCode::Ok;
        const auto steps = phase::battle::macroprobe::BuildMacroProbePlanSteps(
            commands,
            kTransitionNeutralFrames,
            fake_attack_count,
            nullptr,
            &build_failure);
        if (steps.empty() || build_failure != phase::battle::macroprobe::FailureCode::Ok) {
            stop_worker();
            if (error_out) {
                *error_out = "invalid battle macro configuration: "
                    + std::string(phase::battle::macroprobe::FailureCodeName(build_failure));
            }
            return false;
        }

        const std::string plan_spec = phase::battle::macroprobe::FormatCommandPlanSpec(commands);
        durable_log.AppendLine("[battle-macro-probe-start] job=" + std::to_string(next_job_id)
            + " plan=" + plan_spec
            + " fake_attacks=" + std::to_string(fake_attack_count)
            + " savestate=\"" + EscapeLogValue(run_savestate.string()) + "\""
            + " commands=" + std::to_string(commands.size())
            + " transition_neutral_frames=" + std::to_string(kTransitionNeutralFrames)
            + " steps=" + std::to_string(steps.size())
            + " visual=" + (params.visual ? "true" : "false")
            + " visual_debug=" + (params.visual_debug ? "true" : "false"));

        savor::PSInit init{};
        init.savestate_path = run_savestate.string();
        init.derived_buffer_type = savor::DBuf::DK_None;
        if (!worker.ctl_set_program(savor::PK_None, savor::PK_BattleMacroProbe, init)) {
            stop_worker();
            if (error_out) *error_out = "battle macro worker failed SET_PROGRAM";
            return false;
        }
        if (!worker.ctl_activate_main()) {
            stop_worker();
            if (error_out) *error_out = "battle macro worker failed ACTIVATE_MAIN";
            return false;
        }

        std::vector<std::uint8_t> payload;
        phase::battle::macroprobe::encode_payload(
            phase::battle::macroprobe::EncodeSpec{
                .commands = commands,
                .transition_neutral_frames = kTransitionNeutralFrames,
                .fake_attack_count = fake_attack_count,
            },
            payload);
        savor::PSJob job{};
        job.payload = std::move(payload);
        if (!worker.try_acquire_slot()) {
            stop_worker();
            if (error_out) *error_out = "battle macro worker slot was unexpectedly busy";
            return false;
        }
        if (!worker.send_job(next_job_id, next_job_id, job)) {
            stop_worker();
            if (error_out) *error_out = "battle macro worker failed sending job";
            return false;
        }

        const auto drain_progress = [&]() {
            savor::PRProgress progress_item{};
            while (progress.try_pop(progress_item)) {
                const auto progress_line = MacroProgressLine(progress_item);
                durable_log.AppendLine(progress_line);
                std::cout << progress_line << '\n';
            }
        };

        savor::PRResult result{};
        std::string wait_error;
        const bool have_result = WaitForWorkerResult(
            worker,
            results,
            std::chrono::milliseconds(options.poll_ms),
            drain_progress,
            &result,
            &wait_error);
        drain_progress();

        if (!have_result) {
            stop_worker();
            if (error_out) {
                *error_out =
                    "battle macro probe worker result failed: "
                    + wait_error;
            }
            durable_log.AppendLine(
                "[battle-macro-probe-result] job="
                + std::to_string(next_job_id)
                + " worker_unavailable=true diagnostic=\""
                + EscapeLogValue(wait_error) + "\"");
            return false;
        }

        const auto result_line = MacroResultLine(result);
        durable_log.AppendLine(result_line);
        std::cout << result_line << '\n';
        if (params.visual) {
            const auto screenshot = screenshots
                / ("worker-0-job-" + std::to_string(result.job_id) + "-epoch-" + std::to_string(result.epoch) + ".png");
            durable_log.AppendLine("[battle-macro-probe-screenshot] path=\"" + screenshot.string() + "\"");
            std::cout << "[battle-macro-probe-screenshot] path=" << screenshot.string() << '\n';
        }

        std::uint32_t macro_result = 1;
        result.ps.ctx.get(savor::context::key::battle::MACRO_RESULT, macro_result);
        if (!result.ps.ok || macro_result != 0) {
            stop_worker();
            if (error_out) *error_out = "battle macro probe failed; see durable log: " + durable_log.path().string();
            return false;
        }

        ++successful_runs;
        ++next_job_id;
        if (!interactive_loop) {
            break;
        }
        std::cout << "[PASS] battle_macro_probe run " << successful_runs
                  << " - enter another plan or q to finish.\n";
    }

    stop_worker();
    return true;
}

} // namespace savor::e2e

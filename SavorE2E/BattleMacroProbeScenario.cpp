#include "BattleMacroProbeScenario.h"

#include <chrono>
#include <cctype>
#include <cstdio>
#include <filesystem>
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
#include "Phases/Programs/BattleMacroProbe/BattleMacroProbePayload.h"
#include "Runner/IPC/Wire.h"
#include "Runner/Parallel/PRTypes.h"
#include "Worker/ProcessWorker.h"
#include "Worker/TSQueue.h"

namespace savor::e2e {
namespace {

using Clock = std::chrono::steady_clock;

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
    result.ps.ctx.get(savor::context::key::core::DW_RUN_OUTCOME_CODE, dw_err);
    result.ps.ctx.get(savor::context::key::battle::MACRO_RESULT, macro_result);
    result.ps.ctx.get(savor::context::key::battle::MACRO_FAILURE_CODE, macro_failure);
    result.ps.ctx.get(savor::context::key::battle::MACRO_STEP_COUNT, step_count);
    result.ps.ctx.get(savor::context::key::battle::MACRO_LAST_STEP_INDEX, last_step);
    result.ps.ctx.get(savor::context::key::battle::MACRO_LAST_EXPECTED_BP, expected_bp);
    result.ps.ctx.get(savor::context::key::battle::MACRO_LAST_HIT_BP, hit_bp);
    result.ps.ctx.get(savor::context::key::battle::MACRO_LAST_HIT_PC, hit_pc);

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
         << " hit_pc=" << FormatHexPc(hit_pc);
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

bool PromptCommand(int character_index, phase::battle::macroprobe::MacroCommand* command, std::string* error_out) {
    for (;;) {
        std::string line;
        if (!ReadPromptLine("Character " + std::to_string(character_index) + " command [block/focus/attack, q cancel]: ", &line, error_out)) {
            return false;
        }
        if (line == "q" || line == "Q" || line == "cancel") {
            if (error_out) *error_out = "battle macro probe cancelled";
            return false;
        }
        std::vector<phase::battle::macroprobe::MacroCommand> parsed_command;
        if (!phase::battle::macroprobe::ParseCommandPlanSpec(line, &parsed_command, nullptr) || parsed_command.size() != 1) {
            std::cout << "Enter block, focus, attack, or q.\n";
            continue;
        }
        *command = parsed_command.front();
        if (command->mode != phase::battle::macroprobe::MacroMode::Attack) {
            return true;
        }

        for (;;) {
            std::string target_line;
            if (!ReadPromptLine("Attack target slot [4..11, q cancel]: ", &target_line, error_out)) {
                return false;
            }
            if (target_line == "q" || target_line == "Q" || target_line == "cancel") {
                if (error_out) *error_out = "battle macro probe cancelled";
                return false;
            }
            std::vector<phase::battle::macroprobe::MacroCommand> parsed;
            std::string parse_error;
            if (phase::battle::macroprobe::ParseCommandPlanSpec("attack:" + target_line, &parsed, &parse_error)) {
                *command = parsed.front();
                return true;
            }
            std::cout << "Enter a target slot from 4 through 11.\n";
        }
    }
}

bool PromptBattleMacroPlan(std::vector<phase::battle::macroprobe::MacroCommand>* commands, std::string* error_out) {
    if (!IsInteractiveStdin()) {
        if (error_out) *error_out = "battle_macro_probe requires --battle-plan or --battle-macro when stdin is not interactive";
        return false;
    }

    std::cout << "Battle Macro Probe Plan\n";
    std::vector<phase::battle::macroprobe::MacroCommand> plan;
    plan.resize(2);
    if (!PromptCommand(1, &plan[0], error_out)) return false;
    if (!PromptCommand(2, &plan[1], error_out)) return false;

    const std::string spec = phase::battle::macroprobe::FormatCommandPlanSpec(plan);
    std::cout << "Plan: " << spec << "\n";
    for (;;) {
        std::string confirm;
        if (!ReadPromptLine("Press Enter to dispatch, or type q to cancel: ", &confirm, error_out)) {
            return false;
        }
        if (confirm.empty()) {
            break;
        }
        if (confirm == "q" || confirm == "Q" || confirm == "cancel") {
            if (error_out) *error_out = "battle macro probe cancelled";
            return false;
        }
        std::cout << "Press Enter to dispatch, or type q to cancel.\n";
    }

    *commands = std::move(plan);
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

bool PromptBattleMacroSavestatePath(
    const std::filesystem::path& default_path,
    std::filesystem::path* savestate_path,
    std::string* error_out) {
    if (!IsInteractiveStdin()) {
        if (error_out) *error_out = "battle_macro_probe requires --savestate-file when stdin is not interactive";
        return false;
    }

    for (;;) {
        std::string line;
        if (!ReadPromptLine("Savestate path [Enter current, q cancel]\n  current: "
            + default_path.string() + "\n> ", &line, error_out)) {
            return false;
        }
        line = TrimPromptValue(std::move(line));
        if (line == "q" || line == "Q" || line == "cancel") {
            if (error_out) *error_out = "battle macro probe cancelled";
            return false;
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
        return true;
    }
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

    return PromptBattleMacroPlan(commands, error_out);
}

bool ResolveBattleMacroSavestatePath(
    bool interactive_loop,
    const std::filesystem::path& default_path,
    std::filesystem::path* savestate_path,
    std::string* error_out) {
    if (!interactive_loop) {
        *savestate_path = default_path;
        return true;
    }
    return PromptBattleMacroSavestatePath(default_path, savestate_path, error_out);
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
    const auto worker_exe = ResolveWorkerExePath(argv0);
    if (!std::filesystem::exists(worker_exe)) {
        if (error_out) *error_out = "SavorWorker.exe was not found next to SavorE2E: " + worker_exe.string();
        return false;
    }

    constexpr std::uint32_t kTransitionNeutralFrames = 3;
    const bool interactive_loop = !options.battle_macro_args_supplied && !options.battle_macro_plan_spec.has_value();

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
    params.visual_screenshot_dir = screenshots.string();

    if (!worker.start(params, &results)) {
        if (error_out) *error_out = "failed starting SavorWorker for battle macro probe";
        return false;
    }
    worker.set_progress_queue(&progress);

    const auto stop_worker = [&]() {
        worker.stop();
    };

    if (!worker.wait_ready(static_cast<std::uint32_t>(options.timeout_ms))) {
        stop_worker();
        if (error_out) *error_out = "battle macro worker did not become ready";
        return false;
    }

    std::uint64_t next_job_id = 1;
    std::uint64_t successful_runs = 0;
    std::filesystem::path current_savestate = options.savestate_file;
    for (;;) {
        std::filesystem::path run_savestate;
        std::string savestate_error;
        if (!ResolveBattleMacroSavestatePath(interactive_loop, current_savestate, &run_savestate, &savestate_error)) {
            stop_worker();
            if (interactive_loop && successful_runs > 0 && IsBattleMacroPromptCancel(savestate_error)) {
                durable_log.AppendLine("[battle-macro-probe-loop-end] reason=cancel successful_runs="
                    + std::to_string(successful_runs));
                return true;
            }
            if (error_out) *error_out = savestate_error;
            return false;
        }
        current_savestate = run_savestate;

        std::vector<phase::battle::macroprobe::MacroCommand> commands;
        std::string prompt_error;
        if (!ResolveBattleMacroPlan(options, &commands, &prompt_error)) {
            stop_worker();
            if (interactive_loop && successful_runs > 0 && IsBattleMacroPromptCancel(prompt_error)) {
                durable_log.AppendLine("[battle-macro-probe-loop-end] reason=cancel successful_runs="
                    + std::to_string(successful_runs));
                return true;
            }
            if (error_out) *error_out = prompt_error;
            return false;
        }

        phase::battle::macroprobe::FailureCode build_failure = phase::battle::macroprobe::FailureCode::Ok;
        const auto steps = phase::battle::macroprobe::BuildMacroPlanSteps(commands, kTransitionNeutralFrames, &build_failure);
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
            + " savestate=\"" + EscapeLogValue(run_savestate.string()) + "\""
            + " commands=" + std::to_string(commands.size())
            + " transition_neutral_frames=" + std::to_string(kTransitionNeutralFrames)
            + " steps=" + std::to_string(steps.size())
            + " visual=" + (params.visual ? "true" : "false")
            + " visual_debug=" + (params.visual_debug ? "true" : "false"));

        savor::PSInit init{};
        init.savestate_path = run_savestate.string();
        init.default_timeout_ms = static_cast<std::uint32_t>(options.timeout_ms);
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
                .step_timeout_ms = static_cast<std::uint32_t>(options.timeout_ms),
                .vi_stall_ms = 5000,
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

        const auto result_timeout_ms = options.timeout_ms * static_cast<std::int64_t>(steps.size() + 3);
        const auto deadline = Clock::now() + std::chrono::milliseconds(result_timeout_ms);
        savor::PRResult result{};
        bool have_result = false;
        while (Clock::now() < deadline) {
            drain_progress();
            if (results.try_pop(result)) {
                have_result = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(options.poll_ms));
        }
        drain_progress();

        if (!have_result) {
            stop_worker();
            if (error_out) *error_out = "battle macro probe timed out waiting for worker result";
            durable_log.AppendLine("[battle-macro-probe-result] job=" + std::to_string(next_job_id) + " timed_out=true");
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

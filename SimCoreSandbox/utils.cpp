#include <filesystem>
#include <string>
#include <iostream>
#include "Utils/EnsureSys.h"
#include "Utils/Log.h"
#include "Runner/Parallel/ParallelPhaseScriptRunner.h"
#include "SandboxAppState.h"


namespace fs = std::filesystem;

bool ensure_sys_from_base_or_warn(const std::string& base)
{
    // Copies Sys/User beside the exe as your current flow expects. :contentReference[oaicite:5]{index=5}
    if (!simcore::EnsureSysBesideExe(base)) {
        SCLOGE("EnsureSysBesideExe failed. Expected Sys under sandbox / worker exe directory.");
        return false;
    }
    return true;
}

simcore::BootPlan make_boot_plan(const AppState& g)
{
    simcore::BootPlan boot{};
    boot.boot.user_dir = (g.exe_dir / ".workers").string(); // per-worker subdirs are derived later
    boot.boot.dolphin_qt_base = g.dolphin_base_dir;
    boot.boot.force_resync_from_base = true;
    boot.boot.save_config_on_success = true;
    boot.iso_path = g.iso_path;
    return boot;
}

std::string trim(std::string s) {
    auto ws = [](unsigned char c) { return std::isspace(c); };
    while (!s.empty() && ws(s.front())) s.erase(s.begin());
    while (!s.empty() && ws(s.back()))  s.pop_back();
    return s;
}

std::string strip_quotes(std::string s) {
    if (s.size() >= 2) {
        if ((s.front() == '"' && s.back() == '"') ||
            (s.front() == '\'' && s.back() == '\''))
            return s.substr(1, s.size() - 2);
    }
    return s;
}

fs::path prompt_path(const char* prompt, bool require_exists, bool allow_empty, std::string def) {
    for (;;) {
        std::cout << prompt;
        std::string line;
        if (!std::getline(std::cin, line)) return {};
        line = trim(strip_quotes(line));
        if (line.empty()) {
            if (allow_empty) return { def };
            continue;
        }
        fs::path p = fs::path(line).lexically_normal();
        if (require_exists && !fs::exists(p)) {
            std::cout << "Not found, try again.\n";
            continue;
        }
        return p;
    }
}

bool prompt_bool(const std::string& message, bool default_value = false)
{
    while (true)
    {
        std::cout << message
            << " [Y/n]: "
            << std::flush;

        std::string input;
        std::getline(std::cin, input);

        // Trim whitespace
        input.erase(remove_if(input.begin(), input.end(), ::isspace), input.end());

        if (input.empty())
        {
            return default_value; // Enter pressed -> default
        }

        // Normalize case
        std::transform(input.begin(), input.end(), input.begin(), ::tolower);

        if (input == "y" || input == "yes")
            return true;
        if (input == "n" || input == "no")
            return false;

        std::cout << "Please enter Y or N.\n";
    }
}
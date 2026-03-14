// SimCoreSandbox.cpp : REPL menu for SimCoreSandbox
#define NOMINMAX
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <format>
#include <vector>
#include <unordered_map>
#include <chrono>
#include <thread>
#include <filesystem>
#include <algorithm>
#include <fstream>
#include <iostream>
#include <limits>

#include "SandboxConfig.h"
#include "MenuConfig.h"
#include "SeedProbe.h"
#include "TASMoviePlayer.h"
#include "TASMovieDBEnqueue.h"
#include "DB/DBCore/DbService.h"
#include "utils.h"
#include "MenuBattleExplorer.h"

using namespace simcore;

// ------------------------- helpers ---------------------------
static void init_logging(AppState& g )
{
    char exePath[MAX_PATH]{};
    GetModuleFileNameA(NULL, exePath, MAX_PATH);
    g.exe_dir = fs::path(exePath).parent_path();
    logger::Logger::get().set_levels(logger::Level::Info, logger::Level::Debug);
    logger::Logger::get().open_file((g.exe_dir / "sandbox.log").string().c_str(), false);
    SCLOGI("[sandbox] Starting...");
}


// ------------------------- REPL menu -----------------------------------------
static void menu_loop(AppState& g)
{
    for (;;) {
        std::cout << "\n=== SOASim Sandbox ===\n";
        std::cout << "1) Configure paths (ISO, Dolphin portable base, default savestate)\n";
        std::cout << "2) Run RNGSeedDeltaMap probe\n";
        std::cout << "3) TAS Movie -> BP -> Savestate (scaffold)\n";
        std::cout << "4) Battle Context\n";
        std::cout << "5) Battle Runner\n";
        std::cout << "6) TAS Movie (DB enqueue)\n";
        std::cout << "q) Quit\n";
        std::cout << "> ";

        std::string choice;
        if (!std::getline(std::cin, choice)) break;
        if (choice == "q" || choice == "Q") break;
        if (choice == "1") menu_configure_paths(g);
        else if (choice == "2") sandbox::run_rng_seed_probe_menu(g);
        else if (choice == "3") sandbox::menu_tas_movie(g);
        else if (choice == "4") sandbox::get_battle_context(g);
        else if (choice == "5") sandbox::run_battle_explorer_menu(g);
        else if (choice == "6") sandbox::menu_tas_movie_db_enqueue(g);
        else {
            std::cout << "Unknown option.\n";
        }
    }
}

int main(int, char**)
{
    AppState g{};
    char exePath[MAX_PATH]{};
    GetModuleFileNameA(NULL, exePath, MAX_PATH);
    g.exe_dir = std::filesystem::path(exePath).parent_path();

    const auto ini_path = g.exe_dir / "sandbox.ini";
    if (load_appstate_ini(g, ini_path)) {
        std::cout << "[cfg] loaded " << ini_path.string() << "\n";
    }

    init_logging(g);
    simcore::db::DBService::instance().start();
    menu_loop(g);
    save_appstate_ini(g, ini_path);
    return 0;
}

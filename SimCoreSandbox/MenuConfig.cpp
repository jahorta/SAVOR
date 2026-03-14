// MenuConfig.cpp
#include "MenuConfig.h"
#include <algorithm>
#include <iostream>
#include "utils.h"
#include "SandboxConfig.h"

void menu_configure_paths(AppState& g) {
    for (;;) {
        std::cout << "\n--- Configure Paths ---\n"
            << "ISO: " << (g.iso_path.empty() ? "<unset>" : g.iso_path) << "\n"
            << "Dolphin base: " << (g.dolphin_base_dir.empty() ? "<unset>" : g.dolphin_base_dir) << "\n"
            << "Default savestate: " << (g.default_savestate.empty() ? "<unset>" : g.default_savestate) << "\n"
            << "Workers: " << g.workers << "\n"
            << "1) Set ISO path\n2) Set Dolphin base\n3) Set default savestate\n4) Set worker count\ns) Save & back\n> ";

        std::string c; if (!std::getline(std::cin, c)) return;
        if (c == "1") g.iso_path = prompt_path("ISO path: ", true, true, g.iso_path).string();
        else if (c == "2") g.dolphin_base_dir = prompt_path("Dolphin (portable) base dir: ", true, true, g.dolphin_base_dir).string();
        else if (c == "3") g.default_savestate = prompt_path("Default savestate (blank=clear): ", true, true, g.default_savestate).string();
        else if (c == "4") { std::cout << "Workers (1..128): "; std::string s; std::getline(std::cin, s); if (!s.empty()) g.workers = std::clamp<size_t>(std::stoul(s), 1u, 128u); }
        else if (c == "s" || c == "S") { save_appstate_ini(g, g.exe_dir / "sandbox.ini"); return; }
    }
}

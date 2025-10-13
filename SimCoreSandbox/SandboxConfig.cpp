#include "SandboxConfig.h"
#include <fstream>
#include <sstream>
#include <cctype>

static std::string trim(std::string s) {
    auto ws = [](unsigned char c) { return std::isspace(c) != 0; };
    while (!s.empty() && ws(s.front())) s.erase(s.begin());
    while (!s.empty() && ws(s.back()))  s.pop_back();
    return s;
}
static std::string strip_quotes(std::string s) {
    if (s.size() >= 2) {
        if ((s.front() == '"' && s.back() == '"') || (s.front() == '\'' && s.back() == '\'')) {
            return s.substr(1, s.size() - 2);
        }
    }
    return s;
}
static void put_kv(std::ofstream& out, const char* k, const std::string& v) {
    const bool needs_quotes = v.find_first_of(" \t#;=") != std::string::npos;
    if (needs_quotes) out << k << "=\"" << v << "\"\n";
    else out << k << "=" << v << "\n";
}

bool load_appstate_ini(AppState& s, std::filesystem::path ini_path)
{
    if (ini_path.empty()) return false;
    
    std::ifstream in(ini_path, std::ios::binary);
    if (!in) return false;

    std::string line;
    while (std::getline(in, line)) {
        auto t = trim(line);
        if (t.empty() || t[0] == '#' || t[0] == ';' || t[0] == '[') continue;
        auto eq = t.find('=');
        if (eq == std::string::npos) continue;
        auto key = trim(t.substr(0, eq));
        auto val = strip_quotes(trim(t.substr(eq + 1)));

        if (key == "iso_path")            s.iso_path = val;
        else if (key == "dolphin_base_dir")    s.dolphin_base_dir = val;
        else if (key == "default_savestate") s.default_savestate = val;
        else if (key == "workers") {
            try { s.workers = std::clamp<size_t>(std::stoul(val), 1u, 128u); }
            catch (...) {}
        }
    }
    return true;
}

bool save_appstate_ini(const AppState& s, std::filesystem::path ini_path)
{
    if (ini_path.empty()) return false;

    std::filesystem::create_directories(ini_path.parent_path());
    std::ofstream out(ini_path, std::ios::binary | std::ios::trunc);
    if (!out) return false;

    out << "# SOASim Sandbox config\n";
    out << "[paths]\n";
    put_kv(out, "iso_path", s.iso_path);
    put_kv(out, "dolphin_base_dir", s.dolphin_base_dir);
    put_kv(out, "default_savestate", s.default_savestate);
    out << "\n[run]\n";
    out << "workers=" << s.workers << "\n";
    return true;
}

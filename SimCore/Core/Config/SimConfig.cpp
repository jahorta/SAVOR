#include "SimConfig.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

#include "../../Utils/SafeEnv.h"

#if defined(_WIN32)
#include <windows.h>
#endif

namespace simcore {
    namespace {

        static inline std::string ToUtf8(const std::filesystem::path& p) {
#if defined(_WIN32)
            const std::u8string u8 = p.u8string();
            return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
#else
            return p.string();
#endif
        }

        static inline std::filesystem::path MakeAbsoluteRelativeToFile(
            const std::filesystem::path& file, const std::filesystem::path& maybe_rel) {
            if (maybe_rel.is_absolute()) return maybe_rel;
            return std::filesystem::weakly_canonical(file.parent_path() / maybe_rel);
        }

    } // namespace

    namespace SimConfigIO {

        void ApplyDefaultDbPaths(SimConfig& cfg) {
            const auto db_root = cfg.user_dir / "DB";
            cfg.execution_db_path = db_root / "Execution.sqlite";
            cfg.state_db_path = db_root / "State.sqlite";
            cfg.analysis_db_path = db_root / "Analysis.sqlite";
            cfg.authoring_db_path = db_root / "Authoring.sqlite";
            cfg.ui_read_db_path = db_root / "UiRead.sqlite";
            cfg.archive_db_path = db_root / "ArchiveIndex.sqlite";
            cfg.object_store_root = cfg.user_dir / "ObjectStore";
            cfg.archive_store_root = cfg.user_dir / "ArchiveStore";
        }

        std::string TrimAndUnquote(std::string s) {
            auto is_space = [](unsigned char c) { return std::isspace(c) != 0; };
            // trim left
            s.erase(s.begin(), std::find_if(s.begin(), s.end(), [&](unsigned char c) { return !is_space(c); }));
            // trim right
            s.erase(std::find_if(s.rbegin(), s.rend(), [&](unsigned char c) { return !is_space(c); }).base(), s.end());
            if (!s.empty() && (s.front() == '"' || s.front() == '\'')) {
                char q = s.front();
                if (s.size() >= 2 && s.back() == q) {
                    s = s.substr(1, s.size() - 2);
                }
            }
            return s;
        }

        std::filesystem::path DefaultConfigPath() {
#if defined(_WIN32)
            // Prefer LOCALAPPDATA, then APPDATA, else fall back to CWD
            std::filesystem::path base;

            if (auto lad = env::getenv_safe("LOCALAPPDATA"); lad && !lad->empty()) {
                base = std::filesystem::path(*lad);
            }
            else if (auto ad = env::getenv_safe("APPDATA"); ad && !ad->empty()) {
                base = std::filesystem::path(*ad);
            }
            else {
                base = std::filesystem::current_path();
            }

            return base / "SOASim" / "simulator.ini";
#else
            // ~/.config/SOASim/simulator.ini
            const char* home = std::getenv("HOME");
            std::filesystem::path base = home && *home ? std::filesystem::path(home)
                : std::filesystem::current_path();
            return base / ".config" / "SOASim" / "simulator.ini";
#endif
        }

        std::optional<SimConfig> Load(const std::filesystem::path& path, std::string* error_out) {
            std::ifstream ifs(path, std::ios::binary);
            if (!ifs) { if (error_out) *error_out = "Could not open config: " + ToUtf8(path); return std::nullopt; }

            std::string line;
            std::string section;
            SimConfig cfg;

            while (std::getline(ifs, line)) {
                // Strip CR if present
                if (!line.empty() && line.back() == '\r') line.pop_back();

                // Skip comments/blank
                std::string trimmed = TrimAndUnquote(line);
                if (trimmed.empty()) continue;
                if (trimmed[0] == '#' || trimmed[0] == ';') continue;

                // Section?
                if (trimmed.front() == '[' && trimmed.back() == ']') {
                    section = trimmed.substr(1, trimmed.size() - 2);
                    continue;
                }

                // Key/Value
                auto pos = trimmed.find('=');
                if (pos == std::string::npos) continue;
                std::string key = trimmed.substr(0, pos);
                std::string val = TrimAndUnquote(trimmed.substr(pos + 1));

                // lower-case key
                std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return (char)std::tolower(c); });

                if (section == "Paths" || section == "paths" || section.empty()) {
                    if (key == "user_dir") {
                        cfg.user_dir = MakeAbsoluteRelativeToFile(path, std::filesystem::path(val));
                    }
                    else if (key == "dolphin_base_dir") {
                        cfg.dolphin_base_dir = MakeAbsoluteRelativeToFile(path, std::filesystem::path(val));
                    }
                    else if (key == "execution_db_path") {
                        cfg.execution_db_path = MakeAbsoluteRelativeToFile(path, std::filesystem::path(val));
                    }
                    else if (key == "state_db_path") {
                        cfg.state_db_path = MakeAbsoluteRelativeToFile(path, std::filesystem::path(val));
                    }
                    else if (key == "analysis_db_path" ||
                        key == "analysis_spine_db_path" ||
                        key == "analysis_seed_probe_db_path" ||
                        key == "analysis_battle_db_path") {
                        cfg.analysis_db_path = MakeAbsoluteRelativeToFile(path, std::filesystem::path(val));
                    }
                    else if (key == "authoring_db_path") {
                        cfg.authoring_db_path = MakeAbsoluteRelativeToFile(path, std::filesystem::path(val));
                    }
                    else if (key == "ui_read_db_path") {
                        cfg.ui_read_db_path = MakeAbsoluteRelativeToFile(path, std::filesystem::path(val));
                    }
                    else if (key == "archive_db_path") {
                        cfg.archive_db_path = MakeAbsoluteRelativeToFile(path, std::filesystem::path(val));
                    }
                    else if (key == "object_store_root") {
                        cfg.object_store_root = MakeAbsoluteRelativeToFile(path, std::filesystem::path(val));
                    }
                    else if (key == "archive_store_root") {
                        cfg.archive_store_root = MakeAbsoluteRelativeToFile(path, std::filesystem::path(val));
                    }
                }
            }

            if (cfg.user_dir.empty() || cfg.dolphin_base_dir.empty()) {
                if (error_out) *error_out = "Config missing required keys (user_dir, dolphin_base_dir).";
                return std::nullopt;
            }
            const SimConfig defaults = [&cfg]() {
                SimConfig out = cfg;
                ApplyDefaultDbPaths(out);
                return out;
            }();
            if (cfg.execution_db_path.empty()) cfg.execution_db_path = defaults.execution_db_path;
            if (cfg.state_db_path.empty()) cfg.state_db_path = defaults.state_db_path;
            if (cfg.analysis_db_path.empty()) cfg.analysis_db_path = defaults.analysis_db_path;
            if (cfg.authoring_db_path.empty()) cfg.authoring_db_path = defaults.authoring_db_path;
            if (cfg.ui_read_db_path.empty()) cfg.ui_read_db_path = defaults.ui_read_db_path;
            if (cfg.archive_db_path.empty()) cfg.archive_db_path = defaults.archive_db_path;
            if (cfg.object_store_root.empty()) cfg.object_store_root = defaults.object_store_root;
            if (cfg.archive_store_root.empty()) cfg.archive_store_root = defaults.archive_store_root;
            return cfg;
        }

        bool Save(const SimConfig& cfg, const std::filesystem::path& path, std::string* error_out) {
            try {
                SimConfig cfg_to_save = cfg;
                if (cfg_to_save.execution_db_path.empty() ||
                    cfg_to_save.state_db_path.empty() ||
                    cfg_to_save.analysis_db_path.empty() ||
                    cfg_to_save.authoring_db_path.empty() ||
                    cfg_to_save.ui_read_db_path.empty() ||
                    cfg_to_save.archive_db_path.empty() ||
                    cfg_to_save.object_store_root.empty() ||
                    cfg_to_save.archive_store_root.empty()) {
                    ApplyDefaultDbPaths(cfg_to_save);
                }

                std::filesystem::create_directories(path.parent_path());
                std::ostringstream os;
                os << "# SOASim simulator configuration\n"
                    "# Stores paths for your isolated User folder and a DolphinQt *portable* base.\n"
                    "\n[Paths]\n";
                os << "user_dir=" << ToUtf8(std::filesystem::weakly_canonical(cfg_to_save.user_dir)) << "\n";
                os << "dolphin_base_dir=" << ToUtf8(std::filesystem::weakly_canonical(cfg_to_save.dolphin_base_dir)) << "\n";
                os << "execution_db_path=" << ToUtf8(std::filesystem::weakly_canonical(cfg_to_save.execution_db_path)) << "\n";
                os << "state_db_path=" << ToUtf8(std::filesystem::weakly_canonical(cfg_to_save.state_db_path)) << "\n";
                os << "analysis_db_path=" << ToUtf8(std::filesystem::weakly_canonical(cfg_to_save.analysis_db_path)) << "\n";
                os << "authoring_db_path=" << ToUtf8(std::filesystem::weakly_canonical(cfg_to_save.authoring_db_path)) << "\n";
                os << "ui_read_db_path=" << ToUtf8(std::filesystem::weakly_canonical(cfg_to_save.ui_read_db_path)) << "\n";
                os << "archive_db_path=" << ToUtf8(std::filesystem::weakly_canonical(cfg_to_save.archive_db_path)) << "\n";
                os << "object_store_root=" << ToUtf8(std::filesystem::weakly_canonical(cfg_to_save.object_store_root)) << "\n";
                os << "archive_store_root=" << ToUtf8(std::filesystem::weakly_canonical(cfg_to_save.archive_store_root)) << "\n";

                std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
                if (!ofs) { if (error_out) *error_out = "Could not open for write: " + ToUtf8(path); return false; }
                ofs << os.str();
                return true;
            }
            catch (const std::exception& e) {
                if (error_out) *error_out = e.what();
                return false;
            }
        }

    } // namespace SimConfigIO
} // namespace simcore

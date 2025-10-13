#pragma once
#include <filesystem>
#include <optional>
#include <string>

namespace simcore {

	struct SimConfig {
		std::filesystem::path user_dir;     // our isolated User/ folder (per-run or persistent)
		std::filesystem::path dolphin_base_dir;  // required: DolphinQt portable base (must contain portable.txt)
	};

	// Read/write an INI-style config with two keys under [Paths]:
	//   user_dir = C:\...\SOASim\User
	//   dolphin_base_dir = C:\...\DolphinQt
	namespace SimConfigIO {

		// Suggested default location (Windows):
		//   %LOCALAPPDATA%\SOASim\simulator.ini
		// If LOCALAPPDATA is not set, falls back to the current working directory.
		std::filesystem::path DefaultConfigPath();

		// Load config from `path`. Returns std::nullopt on parse/IO error and
		// sets error_out (optional). Relative paths are resolved relative to the file.
		std::optional<SimConfig> Load(const std::filesystem::path& path, std::string* error_out = nullptr);

		// Save config to `path`, creating parent dirs as needed. Returns false on error.
		bool Save(const SimConfig& cfg, const std::filesystem::path& path, std::string* error_out = nullptr);

		// Utility: trim spaces and surrounding quotes from a string (exposed for tests)
		std::string TrimAndUnquote(std::string s);

	} // namespace SimConfigIO

} // namespace simcore

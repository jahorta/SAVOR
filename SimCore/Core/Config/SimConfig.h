#pragma once
#include <filesystem>
#include <optional>
#include <string>

namespace simcore {

	struct SimConfig {
		std::filesystem::path user_dir;     // our isolated User/ folder (per-run or persistent)
		std::filesystem::path dolphin_base_dir;  // required: DolphinQt portable base (must contain portable.txt)
		std::filesystem::path execution_db_path;
		std::filesystem::path state_db_path;
		std::filesystem::path analysis_db_path;
		std::filesystem::path authoring_db_path;
		std::filesystem::path ui_read_db_path;
		std::filesystem::path archive_db_path;
		std::filesystem::path object_store_root;
		std::filesystem::path archive_store_root;
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

		// Generate default stage-1 DB paths rooted beneath cfg.user_dir.
		void ApplyDefaultDbPaths(SimConfig& cfg);

	} // namespace SimConfigIO

} // namespace simcore

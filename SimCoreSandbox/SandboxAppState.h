#pragma once
#include <string>
#include <filesystem>

struct AppState {
	std::filesystem::path exe_dir;
	std::string iso_path;
	std::string dolphin_base_dir;      // Dolphin portable base (has Sys/User)
	std::string default_savestate;
	size_t workers{ 10 };
};

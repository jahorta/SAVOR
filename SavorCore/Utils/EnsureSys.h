#pragma once
#include <string>

namespace savor {
	// Returns true if <exe_dir>\Sys exists (and contains dsp_coef.bin), else copies from dolphin_base_dir\Sys.
	bool EnsureSysBesideExe(const std::string& dolphin_base_dir);
}

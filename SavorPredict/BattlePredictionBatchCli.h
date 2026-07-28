#pragma once

#include "BattlePredictionBatchRun.h"

#include <filesystem>
#include <iosfwd>
#include <string>
#include <vector>

namespace savor::predict {

struct BattlePredictionBatchCliOptions {
    BattlePredictionBatchRunOptions batch;
    std::filesystem::path disc_dump_root;
    std::filesystem::path spice_file_parsing_exe;
    BattlePredictorResourceBundlePtr resource_inputs;
};

struct BattlePredictionBatchCliParseResult {
    BattlePredictionBatchCliOptions options;
    bool help_requested = false;
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
};

BattlePredictionBatchCliParseResult parse_battle_prediction_batch_tokens(
    const std::vector<std::string>& args,
    const std::filesystem::path& executable_path);

int run_battle_prediction_batch_cli(
    const BattlePredictionBatchCliOptions& options,
    std::ostream& out,
    std::ostream& err);

} // namespace savor::predict

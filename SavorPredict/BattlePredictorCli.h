#pragma once

#include "BattlePredictor.h"

#include <filesystem>
#include <iosfwd>
#include <optional>
#include <string>
#include <vector>

namespace savor::predict {

struct BattlePredictorCliOptions {
    std::filesystem::path db_root = "D:/SavorPredictDB";
    std::filesystem::path context_file;
    std::optional<long long> turn_job_id;
    std::optional<long long> exec_job_id;
    std::optional<std::uint32_t> start_seed;
    std::filesystem::path start_seed_list;
    std::optional<int> fake_attacks;
    std::string turn_plan_hex;
    std::string profile_name = "first-battle";
    bool json = false;
    bool allow_seed_candidate_fallback = false;
};

struct BattlePredictorCliParseResult {
    BattlePredictorCliOptions options;
    bool help_requested = false;
    std::vector<std::string> errors;
};

BattlePredictorCliParseResult parse_predict_battle_tokens(const std::vector<std::string>& args);
std::vector<std::string> validate_predict_battle_options(const BattlePredictorCliOptions& options);
int run_predict_battle(const BattlePredictorCliOptions& options, std::ostream& out, std::ostream& err);

} // namespace savor::predict

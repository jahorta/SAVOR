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
    std::optional<std::string> scenario_name;
    std::optional<BattleSourceSelection> source_selection;
    std::optional<BattleEncounterIdentity> expected_encounter;
    std::optional<int> fake_attacks;
    std::filesystem::path action_view_std_json_dir;
    std::filesystem::path std_disc_dump_root;
    std::filesystem::path spice_file_parsing_exe;
    std::string turn_plan_hex;
    std::string profile_name = std::string(kFirstBattleSoldiersProfileName);
    bool json = false;
    bool allow_seed_candidate_fallback = false;
    bool allow_profile_overrides = false;
    bool emit_causal_diagnostics = false;
    bool profile_explicit = false;
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

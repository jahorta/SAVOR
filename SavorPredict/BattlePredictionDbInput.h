#pragma once

#include "BattlePredictor.h"

#include <Analysis/IAnalysisDb.h>

#include <cstdint>
#include <filesystem>
#include <iosfwd>
#include <optional>
#include <string>
#include <vector>

namespace savor::predict {

enum class BattlePredictionSeedSource {
    Unknown,
    Override,
    SeedProbeUniqueSeed,
    SeedCandidate,
    SeedCandidateFallback,
};

enum class BattlePredictionFakeAttackSource {
    Unknown,
    Override,
    TurnJob,
};

enum class BattlePredictionContextSource {
    Unknown,
    WaveContextProbe,
    LatestWaveContextProbe,
};

struct BattlePredictionJobSelector {
    std::optional<long long> turn_job_id;
    std::optional<long long> exec_job_id;
};

struct BattlePredictionDbInputOptions {
    std::filesystem::path db_root = "D:/SavorPredictDB";
    BattlePredictionJobSelector selector;
    std::string profile_name = "first-battle";
    std::optional<std::uint32_t> start_seed_override;
    std::optional<int> fake_attacks_override;
    std::optional<int> enemy_event_id;
    bool allow_seed_candidate_fallback = false;
};

struct BattlePredictionDbInputMetadata {
    std::filesystem::path source_db_root;
    std::optional<long long> requested_turn_job_id;
    std::optional<long long> requested_exec_job_id;
    long long turn_job_id = 0;
    std::optional<long long> exec_job_id;
    long long wave_id = 0;
    long long battle_set_id = 0;
    int turn_index = 0;
    std::optional<long long> seed_candidate_id;
    std::uint32_t starting_rng_seed = 0;
    BattlePredictionSeedSource seed_source = BattlePredictionSeedSource::Unknown;
    int fake_attacks = 0;
    BattlePredictionFakeAttackSource fake_attack_source = BattlePredictionFakeAttackSource::Unknown;
    std::optional<int> enemy_event_id;
    std::optional<long long> context_probe_id;
    BattlePredictionContextSource context_source = BattlePredictionContextSource::Unknown;
    std::optional<int> context_version;
    std::string context_status;
    std::optional<std::string> resolved_turn_variant_key;
    std::vector<std::string> warnings;
};

struct BattlePredictionDbInput {
    BattlePredictionInput input;
    BattlePredictionDbInputMetadata metadata;
};

const char* battle_prediction_seed_source_name(BattlePredictionSeedSource source);
const char* battle_prediction_fake_attack_source_name(BattlePredictionFakeAttackSource source);
const char* battle_prediction_context_source_name(BattlePredictionContextSource source);

std::optional<BattlePredictionDbInput> build_battle_prediction_input_from_analysis_db(
    const savor::db::IAnalysisDb& analysis_db,
    const BattlePredictionDbInputOptions& options,
    std::ostream& err);

std::optional<BattlePredictionDbInput> build_battle_prediction_input_from_db_root(
    const BattlePredictionDbInputOptions& options,
    std::ostream& err);

void write_battle_prediction_db_metadata_text(
    const BattlePredictionDbInputMetadata& metadata,
    std::ostream& out);

void write_battle_prediction_run_json(
    const BattlePredictionDbInputMetadata& metadata,
    const BattlePredictionResult& result,
    std::ostream& out);

} // namespace savor::predict

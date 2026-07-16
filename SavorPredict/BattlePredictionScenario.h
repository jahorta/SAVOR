#pragma once

#include "BattlePredictor.h"
#include <string>
#include <string_view>

namespace savor::predict {

struct BattlePredictionScenario {
    std::string name;
    std::string profile_name;
    BattleSourceSelection source_selection;
};

BattlePredictionScenario first_battle_soldiers_prediction_scenario();
std::optional<BattlePredictionScenario> battle_prediction_scenario_by_name(
    std::string_view name);

} // namespace savor::predict

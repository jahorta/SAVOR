#include "BattlePredictionScenario.h"

#include "BattlePredictionProfileNames.h"

namespace savor::predict {
BattlePredictionScenario first_battle_soldiers_prediction_scenario() {
    return BattlePredictionScenario{
        .name = std::string(kFirstBattleSoldiersProfileName),
        .profile_name = std::string(kFirstBattleSoldiersProfileName),
        .source_manifest_key = "first-battle-soldiers-us-final",
        .movement_backend = BattlePredictionMovementBackend::FrameStateMachine,
    };
}

std::optional<BattlePredictionScenario> battle_prediction_scenario_by_name(
    std::string_view name) {
    if (is_first_battle_soldiers_profile_name(name)) {
        return first_battle_soldiers_prediction_scenario();
    }
    return std::nullopt;
}

} // namespace savor::predict

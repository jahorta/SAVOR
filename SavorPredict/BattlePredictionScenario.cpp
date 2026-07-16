#include "BattlePredictionScenario.h"

#include "BattlePredictionProfileNames.h"

namespace savor::predict {
BattlePredictionScenario first_battle_soldiers_prediction_scenario() {
    return BattlePredictionScenario{
        .name = std::string(kFirstBattleSoldiersProfileName),
        .profile_name = std::string(kFirstBattleSoldiersProfileName),
        .source_selection = BattleSourceSelection{
            .producer_kind = BattleSourceProducerKind::ScriptedBattleRequest,
            .scripted_request = ScriptedBattleRequestIdentity{
                .script_identity = "me201a.sct",
                .section_identity = "loop",
                .instruction_payload_offset = 396,
            },
        },
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

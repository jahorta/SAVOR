#pragma once
#include "ActionTypes.h"
#include "PlanWriter.h"
#include "../../../Core/Input/InputPlan.h"
#include "../../Memory/Soa/Battle/BattleContext.h"

namespace soa::battle::actions {

    struct ActionLibrary {
        static bool generateTurnPlan(const soa::battle::ctx::BattleContext& bc,
            const BattleTurnExecutionSpec& plan,
            savor::ControllerInputSequence& out,
            MaterializeErr& err);
    };

    bool MaterializeBattleTurnInputs(
        const soa::battle::ctx::BattleContext& bc,
        const BattleTurnExecutionSpec& plan,
        savor::ControllerInputSequence& out,
        MaterializeErr& err);

} // namespace soa::battle::actions

#include "ActionLibrary.h"

namespace soa::battle::actions {

    bool ActionLibrary::generateTurnPlan(const soa::battle::ctx::BattleContext& bc,
        const BattleTurnExecutionSpec& plan,
        savor::ControllerInputSequence& out,
        MaterializeErr& err)
    {
        return MaterializeBattleTurnInputs(bc, plan, out, err);
    }

    bool MaterializeBattleTurnInputs(
        const soa::battle::ctx::BattleContext& bc,
        const BattleTurnExecutionSpec& plan,
        savor::ControllerInputSequence& out,
        MaterializeErr& err)
    {
        BattleInputMaterializer materializer(bc);
        return materializer.buildTurn(plan, out, err);
    }

} // namespace soa::battle::actions

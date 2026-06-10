#include "ActionLibrary.h"

namespace soa::battle::actions {

    bool ActionLibrary::generateTurnPlan(const soa::battle::ctx::BattleContext& bc,
        const TurnPlan& plan,
        savor::InputPlan& out,
        MaterializeErr& err)
    {
        PlanWriter w(bc);
        return w.buildTurn(plan, out, err);
    }

} // namespace soa::battle::actions

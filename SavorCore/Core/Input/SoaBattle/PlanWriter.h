#pragma once
#include <cstdint>
#include <vector>
#include "../InputPlan.h"
#include "../../Memory/Soa/Battle/BattleContext.h"
#include "ActionTypes.h"

namespace soa::battle::actions {

    enum class MaterializeErr : uint32_t {
        OK = 0,
        NoValidTarget = 1,
        NotEnoughResource = 2,
        InvalidNavigation = 3,
        OutOfTurns = 4,
        BadBlob = 5,
        InvalidTurnIdxZero = 6
    };

    inline std::string get_materialize_err_string(MaterializeErr e) {
        switch (e) {
        case MaterializeErr::OK: return "No Error";
        case MaterializeErr::NoValidTarget: return "No Valid Target";
        case MaterializeErr::NotEnoughResource: return "Not Enough Resources";
        case MaterializeErr::InvalidNavigation: return "Invalid Menu Navigation";
        case MaterializeErr::OutOfTurns: return "Out of Turns";
        case MaterializeErr::BadBlob: return "Bad BattlePath Blob";
        case MaterializeErr::InvalidTurnIdxZero: return "Invalid Turn Number (0)";
        }
        return "No Error";
    }

    class PlanWriter {
    public:
        PlanWriter(const soa::battle::ctx::BattleContext& bc);

        bool buildTurn(const TurnPlan& plan, savor::InputPlan& out, MaterializeErr& err);

    private:
        soa::battle::ctx::BattleContext bc_;
        uint8_t cmd_index_ = 3; // Attack
        uint8_t actor_slot_ = 0;

        void tapA(savor::InputPlan& p);
        void tapB(savor::InputPlan& p);
        void tapUp(savor::InputPlan& p);
        void tapDown(savor::InputPlan& p);
        void neutral(savor::InputPlan& p, uint32_t n);

        bool if_stop_rotate();
        bool stop_rotate(savor::InputPlan& p);
        bool stop_zoom(savor::InputPlan& p);

        void navMainTo(savor::InputPlan& p, uint8_t dst); // main menu, no wrap, +neutral(2) after each move
        bool attack(savor::InputPlan& p, const ActionParameters& ap, MaterializeErr& err);
        bool defend(savor::InputPlan& p, MaterializeErr& err);
        bool focus(savor::InputPlan& p, MaterializeErr& err);
        bool fake_attack(savor::InputPlan& p, const TurnPlan& ap);

        // submenu targeting helpers (wrap allowed)
        int currentTargetIndex() const; // TODO-WRAP: derive or assume 0
        int firstAliveEnemyIndex() const; // uses bc_; assumes top->bottom
        int resolveRequestedTargetIndex(uint32_t mask) const; // single-target for now

        void navTargetTo(savor::InputPlan& p, int cur, int dst); // wrap-aware

        // TODO-RES: resource checks (Focus/SP gating)
    };

} // namespace soa::battle::actions

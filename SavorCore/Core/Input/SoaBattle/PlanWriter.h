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

    class BattleInputMaterializer {
    public:
        BattleInputMaterializer(const soa::battle::ctx::BattleContext& bc);

        bool buildTurn(const BattleTurnExecutionSpec& plan, savor::ControllerInputSequence& out, MaterializeErr& err);

    private:
        soa::battle::ctx::BattleContext bc_;
        uint8_t cmd_index_ = 3; // Attack
        uint8_t actor_slot_ = 0;

        void tapA(savor::ControllerInputSequence& p);
        void tapB(savor::ControllerInputSequence& p);
        void tapUp(savor::ControllerInputSequence& p);
        void tapDown(savor::ControllerInputSequence& p);
        void neutral(savor::ControllerInputSequence& p, uint32_t n);

        bool if_stop_rotate();
        bool stop_rotate(savor::ControllerInputSequence& p);
        bool stop_zoom(savor::ControllerInputSequence& p);

        void navMainTo(savor::ControllerInputSequence& p, uint8_t dst); // main menu, no wrap, +neutral(2) after each move
        bool attack(savor::ControllerInputSequence& p, const ActionParameters& ap, MaterializeErr& err);
        bool defend(savor::ControllerInputSequence& p, MaterializeErr& err);
        bool focus(savor::ControllerInputSequence& p, MaterializeErr& err);
        bool fake_attack(savor::ControllerInputSequence& p, const BattleTurnExecutionSpec& ap);

        // submenu targeting helpers (wrap allowed)
        int currentTargetIndex() const; // TODO-WRAP: derive or assume 0
        int firstAliveEnemyIndex() const; // uses bc_; assumes top->bottom
        int resolveRequestedTargetIndex(uint32_t mask) const; // single-target for now

        void navTargetTo(savor::ControllerInputSequence& p, int cur, int dst); // wrap-aware

        // TODO-RES: resource checks (Focus/SP gating)
    };

    using PlanWriter = BattleInputMaterializer;

} // namespace soa::battle::actions

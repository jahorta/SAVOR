#pragma once
#include <cstdint>

namespace savor::battle {
    enum class Outcome : uint16_t {
        Victory =                   0x0000u,
        Defeat =                    0x0001u,
        PredFailure =               0x0002u,
        PlanMaterializeFailure =    0x0003u,
        TurnsExhausted =            0x0004u,
        DWRunErr =                  0x0005u,
        ReachedNextTurn =           0x0006u,
        HitTurnLimit =              0x0007u,
        Unknown =                   0xFFFFu,
    };

    inline std::string get_outcome_string(Outcome o) {
        switch (o) {
        case Outcome::Victory: return "Victory";
        case Outcome::Defeat: return "Defeat";
        case Outcome::PredFailure: return "Predicate Failure";
        case Outcome::PlanMaterializeFailure: return "Input Plan Materialize failure";
        case Outcome::TurnsExhausted: return "Turns Exhausted";
        case Outcome::DWRunErr: return "DW Run Error";
        case Outcome::ReachedNextTurn: return "Reached Next Turn";
        case Outcome::HitTurnLimit: return "Hit Turn Limit";
        }
        return "Unknown Outcome";
    }
} // namespace savor::battle

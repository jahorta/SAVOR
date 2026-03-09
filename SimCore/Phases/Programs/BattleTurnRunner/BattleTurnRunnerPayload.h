#pragma once
#include <vector>
#include <cstdint>

#include "../../../Runner/Script/PhaseScriptVM.h"
#include "../../../Runner/Breakpoints/Predicate.h"
#include "../../../Core/Input/SoaBattle/ActionTypes.h"

namespace phase::battle::turnrunner {

    static constexpr int PayloadVersion = 1;

    struct EncodeSpec {
        uint32_t run_ms{ 0 };
        uint32_t vi_stall_ms{ 0 };
        uint32_t current_turn{ 1 };
        bool has_initial_input{ false };
        GCInputFrame initial{};

        soa::battle::actions::TurnPlan turn_plan{};
        std::vector<simcore::pred::Spec> predicates;

        // bookkeeping metadata; enforced by coordinator (not decoder)
        uint32_t fake_attack_budget_max{ 0 };
        uint32_t fake_attacks_used_before_turn{ 0 };
    };

    bool encode_payload(const EncodeSpec& spec, std::vector<uint8_t>& out);
    bool decode_payload(const std::vector<uint8_t>& in, simcore::PSContext& out_ctx);

} // namespace phase::battle::turnrunner

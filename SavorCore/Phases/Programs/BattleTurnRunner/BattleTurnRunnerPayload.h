#pragma once
#include <vector>
#include <cstdint>
#include <string>
#include <optional>

#include "../../../Runner/Script/PhaseScriptVM.h"
#include "../../../Runner/Breakpoints/Predicate.h"
#include "../../../Core/Input/SoaBattle/ActionTypes.h"
#include "../../../Core/Input/InputPlan.h"

namespace phase::battle::turnrunner {

    static constexpr int PayloadVersion = 5;

    struct EncodeSpec {
        uint32_t run_ms{ 0 };
        uint32_t vi_stall_ms{ 0 };
        uint32_t current_turn{ 1 };
        uint32_t max_turn{ 1 };
        bool has_initial_input{ false };
        savor::GCInputFrame initial{};

        soa::battle::actions::TurnPlan turn_plan{};
        std::vector<savor::pred::Spec> predicates;

        // bookkeeping metadata; enforced by coordinator (not decoder)
        uint32_t fake_attack_budget_max{ 0 };
        uint32_t fake_attacks_used_before_turn{ 0 };
        std::string output_savestate_path{};
        std::string capture_profile_path{};
        std::string capture_output_path{};
        std::optional<uint32_t> override_start_rng_seed{};
    };

    bool encode_payload(const EncodeSpec& spec, std::vector<uint8_t>& out);
    bool decode_payload(const std::vector<uint8_t>& in, savor::PSContext& out_ctx);

} // namespace phase::battle::turnrunner

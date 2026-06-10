#pragma once
#include <vector>
#include <cstdint>
#include <string>
#include "../../../Runner/Script/PhaseScriptVM.h"
#include "../../../Runner/Breakpoints/Predicate.h"

using namespace savor;

namespace phase::battle::runner {

    static constexpr int PayloadVersion = 2;

    // encode side (parent)
    struct EncodeSpec {
        uint32_t run_ms{ 0 };
        uint32_t vi_stall_ms{ 0 };
        GCInputFrame initial{};
        soa::battle::actions::BattlePath path;
        std::vector<savor::pred::Spec> predicates;
    };

    // ProgramRegistry.decode -> fill ctx
    bool decode_payload(const std::vector<uint8_t>& in, PSContext& out_ctx);

    // parent helper
    bool encode_payload(const EncodeSpec& spec, std::vector<uint8_t>& out);

} // namespace savor::battle

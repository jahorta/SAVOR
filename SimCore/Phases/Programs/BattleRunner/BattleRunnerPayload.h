#pragma once
#include <vector>
#include <cstdint>
#include <string>
#include "../../../Runner/Script/PhaseScriptVM.h"
#include "../../../Runner/Breakpoints/Predicate.h"

using namespace simcore;

namespace phase::battle::runner {

    static constexpr int PayloadVersion = 1;

    // encode side (parent)
    struct EncodeSpec {
        uint32_t run_ms{ 0 };
        uint32_t vi_stall_ms{ 0 };
        GCInputFrame initial{};
        soa::battle::actions::BattlePath path;
        std::vector<simcore::pred::Spec> predicates;
    };

    // ProgramRegistry.decode -> fill ctx
    bool decode_payload(const std::vector<uint8_t>& in, PSContext& out_ctx);

    // parent helper
    bool encode_payload(const EncodeSpec& spec, std::vector<uint8_t>& out);

} // namespace simcore::battle

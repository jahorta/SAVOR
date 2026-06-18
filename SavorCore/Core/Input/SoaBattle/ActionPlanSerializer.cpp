#include "ActionPlanSerializer.h"

namespace soa::battle::actions {

    void encode_battle_plan_to_buffer(const actions::BattlePath& path, std::vector<std::uint8_t>& out) {
        encode_battle_execution_script_to_buffer(path, out);
    }

    bool decode_battle_plan_from_buffer(std::span<const std::uint8_t> buf, actions::BattlePath& out) {
        return decode_battle_execution_script_from_buffer(buf, out);
    }

} // namespace savor::programs::battle

#pragma once

#include "InteractionComposition.h"
#include "Phases/Programs/BattleCompletion/BattleCompletionContracts.h"

#include <array>
#include <span>
#include <string>

namespace savor::runtime::program::composition {

struct BattleResultsHandlerInputV1
{
    battlecompletion::BattleCompletionManifestV1 completion;
    battlecompletion::BattleStopProvenanceV1 postseed_entry;
};

struct BattleResultsHandlerReceiptV1
{
    battlecompletion::BattleResultsPresentationV1 expected;
    battlecompletion::BattleResultsPresentationV1 observed;
    std::array<std::uint32_t, 10> observed_segment_counts{};
    battlecompletion::BattleStopProvenanceV1 terminal;
    std::uint32_t entry_rng = 0;
    std::uint32_t exit_rng = 0;
    bool lifecycle_complete = false;
    bool completion_flag_set = false;
    bool result_pointer_cleared = false;
    bool field_mode_restored = false;
    bool rng_unchanged = false;
};

[[nodiscard]] bool DecodeBattleResultsHandlerReceiptV1(
    std::span<const std::uint8_t> bytes,
    BattleResultsHandlerReceiptV1& output,
    std::string* diagnostic = nullptr);

[[nodiscard]] InteractionDefinition BattleResultsInteractionV1();

// Adds a reusable function with parameters (BattleResultsHandlerInputV1,
// A frame) and a typed Battle Results receipt. The input is represented by the
// named HandlerInput IR record. The function correlates its bound postseed
// provenance with the canonical paused-PC receipt before doing any handler
// work, and never creates a Full Phase entrypoint.
[[nodiscard]] CompositionResult LowerBattleResultsHandlerV1(
    ProgramModule& module);

} // namespace savor::runtime::program::composition

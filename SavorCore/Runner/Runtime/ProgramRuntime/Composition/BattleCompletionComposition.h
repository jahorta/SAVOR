#pragma once

#include "InteractionComposition.h"

namespace savor::runtime::program::composition {

[[nodiscard]] InteractionDefinition BattleCompletionInteractionV1();
[[nodiscard]] CompositionResult LowerBattleCompletionInteractionV1(
    ProgramModule& module);

// The sequence is the reusable completion boundary shared by
// battle.completion and battle.record.  Its local function accepts:
//
//   victory pc, victory VI, victory epoch, neutral input,
//   battle-set id, wave id, turn-job id, execution-job id
//
// and returns a typed record containing the completion manifest, field
// transition context, and the movie input count at the accepted preseed.
[[nodiscard]] SchemaIdentity BattleCompletionSequenceReceiptSchemaIdentity();
[[nodiscard]] CompositionResult LowerBattleCompletionSequenceV1(
    ProgramModule& module);

} // namespace savor::runtime::program::composition

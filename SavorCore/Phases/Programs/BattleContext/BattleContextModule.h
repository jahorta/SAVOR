#pragma once

#include "Core/Memory/Soa/Battle/BattleContext.h"
#include "Runner/Runtime/FullPhase/FullPhaseProgram.h"

#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace savor::runtime::battlecontext {

inline constexpr std::int32_t ProgramVersion = 1;
inline constexpr std::string_view Entrypoint = "capture";
inline constexpr std::string_view ModuleCanonicalId =
    "soa.battle.context";
inline constexpr std::string_view FullPhaseCanonicalId =
    "savor.full_phase.battle_context";
inline constexpr std::string_view BaselineLineage =
    "soa.battle.context/prebattle-entry/v1";
inline constexpr std::uint32_t BeforeRandSeedSetPc = 0x80101e48u;
inline constexpr std::uint32_t TurnInputsPc = 0x80071740u;

struct BattleContextCaptureResultV1
{
    soa::battle::ctx::BattleContext context;
    std::uint32_t entry_pc = 0;
    std::uint64_t entry_vi_count = 0;
    WorksetEpoch entry_epoch;
    std::uint32_t capture_pc = 0;
    std::uint64_t capture_vi_count = 0;
    WorksetEpoch capture_epoch;
};

[[nodiscard]] program::SchemaIdentity
BattleContextCaptureResultSchemaIdentityV1();

[[nodiscard]] std::vector<std::uint8_t>
EncodeBattleContextExecutionInputV1();
[[nodiscard]] bool DecodeBattleContextExecutionInputV1(
    std::span<const std::uint8_t> input,
    std::string* diagnostic = nullptr);

class IBattleContextFullPhaseDefinitionV1
    : public fullphase::IFullPhaseProgramDefinition
{
public:
    [[nodiscard]] virtual bool DecodeProgramResult(
        std::span<const program::Byte> encoded_result,
        BattleContextCaptureResultV1& result,
        std::string* diagnostic = nullptr) const = 0;
};

[[nodiscard]] std::shared_ptr<
    const IBattleContextFullPhaseDefinitionV1>
BattleContextFullPhaseDefinitionV1();

} // namespace savor::runtime::battlecontext

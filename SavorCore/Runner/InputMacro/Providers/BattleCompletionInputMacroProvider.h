#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <string>

#include "../IInputMacroPlanDriver.h"
#include "../../../Phases/Programs/BattleCompletion/BattleCompletionManifest.h"
#include "../../../Phases/Programs/BattleEndResults/BattleEndResultsReport.h"

namespace savor::inputmacro {

class BattleCompletionInputMacroProvider final : public IInputMacroPlanDriver {
public:
    struct GuestAddresses {
        static constexpr std::uint32_t PcDataBase = 0x8030B7F4u;
        static constexpr std::uint32_t PcDataStride = 0x5Cu;
        static constexpr std::uint32_t BattleInputState = 0x80347338u;
        static constexpr std::uint32_t RewardPhase = 0x8034737Cu;
        static constexpr std::uint32_t RewardItemsBase = 0x80308304u;
        static constexpr std::uint32_t RewardItemStride = 4u;
        static constexpr std::uint32_t NormalExperienceReward = 0x803082F8u;
        static constexpr std::uint32_t MagicExperienceReward = 0x803082FCu;
        static constexpr std::uint32_t GoldReward = 0x80308300u;
    };

    struct DiagnosticsSnapshot {
        phase::battle::endresults::Outcome outcome{
            phase::battle::endresults::Outcome::Failed};
        phase::battle::endresults::FailureCode failure{
            phase::battle::endresults::FailureCode::None};
        bool completed{false};
        BPKey last_expected_key{0};
        BPKey last_hit_key{0};
        std::uint32_t last_hit_pc{0};
        std::uint32_t invariant_flags{0};
        std::uint32_t expected_level_panels{0};
        std::uint32_t expected_stat_waves{0};
        std::uint32_t expected_magic_rank_events{0};
        std::uint32_t expected_learned_waves{0};
        std::uint32_t expected_item_popups{0};
        std::uint64_t start_vi{0};
        std::uint64_t end_vi{0};
    };

    BattleCompletionInputMacroProvider();
    ~BattleCompletionInputMacroProvider() override;
    BattleCompletionInputMacroProvider(const BattleCompletionInputMacroProvider&) = delete;
    BattleCompletionInputMacroProvider& operator=(const BattleCompletionInputMacroProvider&) = delete;

    static std::span<const BPKey> required_breakpoint_keys() noexcept;
    std::span<const BPKey> declared_breakpoint_keys() const override;
    InputMacroDriverDecision Start(IInputMacroDriverHost& host) override;
    InputMacroDriverDecision Advance(
        IInputMacroDriverHost& host,
        const InputMacroStepResult& completed_segment_result) override;
    void Cancel() noexcept override;

    phase::battle::endresults::FailureCode failure() const noexcept;
    const std::string& diagnostic() const noexcept;
    const phase::battle::completion::Manifest& manifest() const noexcept;
    const std::string& manifest_blob() const noexcept;
    bool completed() const noexcept;
    DiagnosticsSnapshot diagnostics() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace savor::inputmacro

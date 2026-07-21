#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <string>

#include "../IInputMacroPlanDriver.h"
#include "../../../Phases/Programs/BattleEndResults/BattleEndResultsReport.h"

namespace savor::inputmacro {

class BattleEndResultsInputMacroProvider final : public IInputMacroPlanDriver {
public:
    struct Request {
        phase::battle::endresults::AccelerationPolicy acceleration_policy{
            phase::battle::endresults::AccelerationPolicy::FullAdaptive};
        // Compatibility class only: false preserves the original monolithic
        // victory-to-results behavior used by focused legacy tests. New VM
        // programs set this and supply a BCMB manifest.
        bool start_at_results_screen{false};
        std::string completion_manifest_blob;
    };

    struct GuestAddresses {
        static constexpr std::uint32_t PcDataBase = 0x8030B7F4u;
        static constexpr std::uint32_t PcDataStride = 0x5Cu;
        static constexpr std::uint32_t Controller0Pointer = 0x80311A60u;
        static constexpr std::uint32_t ControllerInfo0 = 0x8030A6D4u;
        static constexpr std::uint32_t BattleInputState = 0x80347338u;
        static constexpr std::uint32_t RewardPhase = 0x8034737Cu;
        static constexpr std::uint32_t ResultObjectPointer = 0x80346DCCu;
        static constexpr std::uint32_t ResultLifecycleState = 0x80346DD0u;
        static constexpr std::uint32_t ResultDone = 0x80346DD4u;
        static constexpr std::uint32_t GameMode = 0x803475CCu;
        static constexpr std::uint32_t FieldControllerState = 0x80311AECu;
        static constexpr std::uint32_t RngSeed = 0x803469A8u;
    };

    struct DiagnosticsSnapshot {
        phase::battle::endresults::AccelerationPolicy policy{
            phase::battle::endresults::AccelerationPolicy::FullAdaptive};
        phase::battle::endresults::Outcome outcome{
            phase::battle::endresults::Outcome::Failed};
        phase::battle::endresults::FailureCode failure{
            phase::battle::endresults::FailureCode::None};
        bool completed{false};
        std::uint32_t action_count{0};
        std::uint32_t input_request_count{0};
        std::uint32_t input_observed_count{0};
        std::uint32_t release_request_count{0};
        std::uint32_t release_observed_count{0};
        BPKey last_expected_key{0};
        BPKey last_hit_key{0};
        std::uint32_t last_hit_pc{0};
        std::uint32_t last_state{0};
        std::uint32_t last_substate{0};
        std::uint64_t last_token{0};
        std::uint32_t expected_stat_waves{0};
        std::uint32_t observed_stat_waves{0};
        std::uint32_t expected_learned_waves{0};
        std::uint32_t observed_learned_waves{0};
        bool expected_item_popup{false};
        bool observed_item_popup{false};
        std::uint32_t mismatch_flags{0};
        std::uint32_t invariant_flags{0};
        std::uint32_t entry_rng_seed{0};
        std::uint32_t final_rng_seed{0};
    };

    explicit BattleEndResultsInputMacroProvider(Request request = {});
    ~BattleEndResultsInputMacroProvider() override;

    BattleEndResultsInputMacroProvider(const BattleEndResultsInputMacroProvider&) = delete;
    BattleEndResultsInputMacroProvider& operator=(const BattleEndResultsInputMacroProvider&) = delete;

    static std::span<const BPKey> required_breakpoint_keys() noexcept;

    std::span<const BPKey> declared_breakpoint_keys() const override;
    InputMacroDriverDecision Start(IInputMacroDriverHost& host) override;
    InputMacroDriverDecision Advance(
        IInputMacroDriverHost& host,
        const InputMacroStepResult& completed_segment_result) override;
    void Cancel() noexcept override;

    phase::battle::endresults::FailureCode failure() const noexcept;
    const std::string& diagnostic() const noexcept;
    const phase::battle::endresults::Report& report() const noexcept;
    const std::string& report_blob() const noexcept;
    bool completed() const noexcept;
    DiagnosticsSnapshot diagnostics() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace savor::inputmacro

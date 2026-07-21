#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <string>

#include "../IInputMacroPlanDriver.h"
#include "BattleEndResultsInputMacroProvider.h"

namespace savor::inputmacro {

class BattleResultsScreenInputMacroProvider final : public IInputMacroPlanDriver {
public:
    struct Request {
        phase::battle::endresults::AccelerationPolicy acceleration_policy{
            phase::battle::endresults::AccelerationPolicy::FullAdaptive};
        std::string completion_manifest_blob;
    };

    using GuestAddresses = BattleEndResultsInputMacroProvider::GuestAddresses;
    using DiagnosticsSnapshot = BattleEndResultsInputMacroProvider::DiagnosticsSnapshot;

    explicit BattleResultsScreenInputMacroProvider(Request request);
    ~BattleResultsScreenInputMacroProvider() override;
    BattleResultsScreenInputMacroProvider(const BattleResultsScreenInputMacroProvider&) = delete;
    BattleResultsScreenInputMacroProvider& operator=(const BattleResultsScreenInputMacroProvider&) = delete;

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
    BattleEndResultsInputMacroProvider implementation_;
};

} // namespace savor::inputmacro

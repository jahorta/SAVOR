#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace savor::runtime::program::composition {

inline constexpr std::string_view kBattleResultsAdvanceHandlerKey =
    "soa.battle.results.advance";

inline constexpr std::uint32_t kBattleResultsDescriptorReadyPc = 0x800e35f0u;
inline constexpr std::uint32_t kBattleResultsConfirmReadyPc = 0x800e6128u;
inline constexpr std::uint32_t kBattleResultsConfirmAcceptedPc = 0x800e6138u;
inline constexpr std::uint32_t kBattleResultsGuestPadReadReturnedPc = 0x801d6e7cu;
inline constexpr std::uint32_t kBattleResultsLifecycleExitPc = 0x800e64a0u;
inline constexpr std::uint32_t kBattleResultsCleanupCompletePc = 0x800e3694u;

inline constexpr std::uint32_t kBattleResultsRngAddress = 0x803469a8u;
inline constexpr std::uint32_t kBattleResultsResultPointerAddress = 0x80346dccu;
inline constexpr std::uint32_t kBattleResultsCompletionFlagAddress = 0x80346dd4u;
inline constexpr std::uint32_t kBattleResultsGameModeAddress = 0x803475ccu;

enum class BattleResultsHandlerAction : std::uint8_t
{
    ContinueNeutral,
    PressA,
    ReleaseA,
    Complete,
};

struct BattleResultsStopProvenanceV1
{
    std::uint32_t pc = 0;
    std::uint64_t vi_count = 0;
    std::uint64_t workset_epoch = 0;

    auto operator<=>(const BattleResultsStopProvenanceV1&) const = default;
};

struct BattleResultsHandlerStepV1
{
    BattleResultsHandlerAction action =
        BattleResultsHandlerAction::ContinueNeutral;
    std::array<std::uint32_t, 2> wake_pcs{};
    std::size_t wake_pc_count = 0;
    std::uint64_t sequence = 0;
    std::string_view diagnostic_selector;

    [[nodiscard]] std::span<const std::uint32_t> WakePcs() const noexcept
    {
        return {wake_pcs.data(), wake_pc_count};
    }
};

struct BattleResultsHandlerReceiptV1
{
    BattleResultsStopProvenanceV1 entry;
    BattleResultsStopProvenanceV1 terminal;
    std::uint32_t entry_rng = 0;
    std::uint32_t exit_rng = 0;
};

class BattleResultsHandlerV1 final
{
public:
    [[nodiscard]] bool Begin(
        const BattleResultsStopProvenanceV1& entry,
        std::uint32_t entry_rng,
        std::string* diagnostic = nullptr);
    [[nodiscard]] BattleResultsHandlerStepV1 NextStep() const;
    [[nodiscard]] bool Observe(
        const BattleResultsStopProvenanceV1& stop,
        std::string* diagnostic = nullptr);
    [[nodiscard]] bool Finalize(
        std::uint32_t exit_rng,
        std::uint32_t completion_flag,
        std::uint32_t result_pointer,
        std::uint32_t game_mode,
        BattleResultsHandlerReceiptV1& receipt,
        std::string* diagnostic = nullptr) const;

    [[nodiscard]] bool active() const noexcept;
    [[nodiscard]] bool complete() const noexcept;

private:
    enum class State : std::uint8_t
    {
        Inactive,
        AwaitingReady,
        AwaitingAccepted,
        AwaitingNeutralObserved,
        AwaitingLifecycleExit,
        AwaitingCleanup,
        Complete,
        Failed,
    };

    [[nodiscard]] bool Fail(std::string message, std::string* diagnostic);

    State state_ = State::Inactive;
    BattleResultsStopProvenanceV1 entry_;
    BattleResultsStopProvenanceV1 terminal_;
    std::uint32_t entry_rng_ = 0;
    std::uint64_t transition_sequence_ = 0;
};

} // namespace savor::runtime::program::composition

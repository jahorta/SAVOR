#pragma once

#include "BattleCoreDerivedState.h"
#include "DerivedStateRegistry.h"
#include "../Services/Memory/IHitTimeGuestMemoryBackendPort.h"
#include "../Services/Memory/GuestMemory.h"
#include "../StopPoints/StopPointRouter.h"

#include <memory>
#include <optional>
#include <string>
#include <thread>

namespace savor::runtime::derived {

enum class DerivedStateErrorCode : std::uint8_t
{
    None,
    InvalidArgument,
    InvalidState,
    StaleEpoch,
    MissingSnapshot,
    GuestReadFailure,
    MalformedEvidence,
    RoutingFailure,
};

struct DerivedStateReceipt
{
    bool ok = false;
    DerivedStateErrorCode code = DerivedStateErrorCode::None;
    std::string message;

    [[nodiscard]] static DerivedStateReceipt Success();
    [[nodiscard]] static DerivedStateReceipt Failure(
        DerivedStateErrorCode code,
        std::string message);
};

template <typename Snapshot>
struct DerivedStateQueryResult
{
    DerivedStateReceipt receipt;
    std::optional<Snapshot> snapshot;
};

class DerivedStateService final
    : public IStopPointConsumer,
      public IStopPointCpuObserver
{
public:
    DerivedStateService(
        GuestMemory& memory,
        IHitTimeGuestMemoryBackendPort& hit_time_memory,
        const DerivedStateRegistry& registry =
            ProductionDerivedStateRegistry());
    ~DerivedStateService() override;

    DerivedStateService(const DerivedStateService&) = delete;
    DerivedStateService& operator=(const DerivedStateService&) = delete;

    [[nodiscard]] DerivedStateReceipt BindRouter(
        StopPointRouter& router,
        WorksetEpoch epoch);

    [[nodiscard]] DerivedStateReceipt ActivateItem(
        const WorksetDerivedStateBindingV1& binding,
        WorksetEpoch epoch,
        WorkerWorksetItemId item_id);
    [[nodiscard]] DerivedStateReceipt CloseItem() noexcept;

    [[nodiscard]] bool active() const noexcept { return active_; }
    [[nodiscard]] WorksetEpoch workset_epoch() const noexcept { return epoch_; }
    [[nodiscard]] WorkerWorksetItemId item_id() const noexcept { return item_id_; }

    [[nodiscard]] DerivedStateQueryResult<BattleTurnEntrySnapshotV1>
    QueryBattleTurnEntry(const DerivedStateQueryV1& query) const;
    [[nodiscard]] DerivedStateQueryResult<BattleTurnOrderSnapshotV1>
    QueryBattleTurnOrder(const DerivedStateQueryV1& query) const;
    [[nodiscard]] DerivedStateQueryResult<BattleRewardsSnapshotV1>
    QueryBattleRewards(const DerivedStateQueryV1& query) const;

    void OnStopPoint(const StopDelivery& delivery) override;
    [[nodiscard]] StopCpuObservationResult ObserveRoutedHit(
        std::uint32_t descriptor_id,
        const RoutedStopEvent& event) noexcept override;

private:
    struct CpuState;

    [[nodiscard]] bool OnOwnerThread() const noexcept;
    [[nodiscard]] DerivedStateReceipt ValidateQuery(
        const DerivedStateQueryV1& query,
        const DerivedStateSnapshotProvenanceV1* provenance) const;
    [[nodiscard]] DerivedStateSnapshotProvenanceV1 MakeProvenance(
        const DerivedStateRefreshGroupDescriptor& group,
        std::uint64_t generation,
        const RoutedStopEvent& event) const;
    void RefreshFromRetainedCurrentPoint(const RoutedStopEvent& event);
    void CommitNativeEvidence(const RoutedStopEvent& event);

    GuestMemory& memory_;
    IHitTimeGuestMemoryBackendPort& hit_time_memory_;
    StopPointRouter* router_ = nullptr;
    const DerivedStateRegistry& registry_;
    std::unique_ptr<CpuState> cpu_;
    std::thread::id owner_thread_;
    bool active_ = false;
    bool battle_core_active_ = false;
    bool accepting_current_point_ = false;
    WorksetEpoch epoch_;
    WorkerWorksetItemId item_id_;
    DerivedStateBlockIdentityV1 battle_core_identity_;
    StopSubscriptionGroupHandle initialization_subscriptions_;
    StopSubscriptionGroupHandle native_subscriptions_;
    std::uint64_t turn_entry_generation_ = 0;
    std::uint64_t turn_order_generation_ = 0;
    std::uint64_t rewards_generation_ = 0;
    std::optional<BattleTurnEntrySnapshotV1> turn_entry_;
    std::optional<BattleTurnOrderSnapshotV1> turn_order_;
    std::optional<BattleRewardsSnapshotV1> rewards_;
};

} // namespace savor::runtime::derived

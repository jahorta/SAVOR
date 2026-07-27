#pragma once

#include "../../RuntimeTypes.h"
#include "../../StopPoints/StopPointRouter.h"
#include "ICaptureBackendPort.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

namespace savor::runtime {

struct CaptureAttachmentIdTag;
using CaptureAttachmentId = StrongId<CaptureAttachmentIdTag>;

enum class CaptureServiceErrorCode : std::uint16_t
{
    None,
    InvalidArgument,
    WrongThread,
    NotBound,
    AlreadyBound,
    AlreadyAttached,
    NotAttached,
    StaleAttachment,
    StaleEpoch,
    ProfileParseFailed,
    BackendUnavailable,
    AdapterStartFailed,
    RouterRegistrationFailed,
    RouterReconcileFailed,
    StateReplacementActive,
    StateReplacementNotPrepared,
    StateReplacementFailed,
    FinalizationFailed,
    RuntimeStopping,
};

struct CaptureServiceError
{
    CaptureServiceErrorCode code = CaptureServiceErrorCode::None;
    std::string message;

    [[nodiscard]] explicit operator bool() const noexcept
    {
        return code != CaptureServiceErrorCode::None;
    }
};

enum class CaptureAttachmentStatus : std::uint8_t
{
    Rejected,
    Attached,
    Detached,
    Finalized,
};

struct CaptureAttachmentRequest
{
    std::string profile_json;
    savor::probe::SessionOptions options;
    StateEpoch expected_epoch;
};

struct CaptureServiceReceipt
{
    bool ok = false;
    CaptureAttachmentStatus status =
        CaptureAttachmentStatus::Rejected;
    CaptureAttachmentId attachment;
    StateEpoch epoch;
    StopDispatchGeneration dispatch_generation;
    PhysicalPlanGeneration physical_generation;
    bool capture_complete = true;
    bool artifacts_finalized = false;
    bool requires_session_taint = false;
    std::uint64_t capture_drop_count = 0;
    std::uint64_t progress_drop_count = 0;
    std::string incomplete_reason;
    CaptureServiceError error;
};

struct CaptureServiceSnapshot
{
    bool bound = false;
    bool attached = false;
    bool state_replacement_prepared = false;
    bool stopping = false;
    CaptureAttachmentId attachment;
    StateEpoch epoch;
    bool router_group_active = false;
    std::uint64_t reconcile_count = 0;
};

class CaptureService final
    : public IStopPointCpuObserver,
      public IStopPointConsumer
{
public:
    CaptureService(
        ICaptureBackendPort& backend,
        ProbeRouterAdapterConfig adapter_config);
    ~CaptureService() override;

    CaptureService(const CaptureService&) = delete;
    CaptureService& operator=(const CaptureService&) = delete;

    [[nodiscard]] CaptureServiceReceipt BindRouter(
        StopPointRouter& router,
        StateEpoch epoch);
    [[nodiscard]] CaptureServiceReceipt Attach(
        CaptureAttachmentRequest request);
    [[nodiscard]] CaptureServiceReceipt Detach(
        CaptureAttachmentId attachment);
    [[nodiscard]] CaptureServiceReceipt ReconcileBeforeResume();
    [[nodiscard]] CaptureServiceReceipt SetProfileGroupEnabled(
        CaptureAttachmentId attachment,
        std::string_view group,
        bool enabled);
    [[nodiscard]] CaptureServiceReceipt ReplaceProfile(
        CaptureAttachmentId attachment,
        std::string profile_json);

    [[nodiscard]] CaptureServiceReceipt PrepareStateReplacement(
        StateEpoch expected_epoch);
    [[nodiscard]] CaptureServiceReceipt CommitStateReplacement(
        StateEpoch new_epoch);
    [[nodiscard]] CaptureServiceReceipt RollbackStateReplacement(
        StateEpoch restored_epoch);
    [[nodiscard]] CaptureServiceReceipt Shutdown() noexcept;

    [[nodiscard]] CaptureServiceSnapshot snapshot() const noexcept;

    [[nodiscard]] StopCpuObservationResult ObserveRoutedHit(
        std::uint32_t descriptor_id,
        const RoutedStopEvent& event) noexcept override;
    void OnStopPoint(const StopDelivery& delivery) override;

private:
    [[nodiscard]] CaptureServiceError CheckActor() const;
    [[nodiscard]] CaptureServiceReceipt Failure(
        CaptureServiceErrorCode code,
        std::string message,
        bool taint = false) const;
    [[nodiscard]] CaptureServiceReceipt Success(
        CaptureAttachmentStatus status) const;
    [[nodiscard]] CaptureServiceReceipt ApplyDefinition(
        StopSubscriptionGroupDefinition definition);
    [[nodiscard]] CaptureServiceReceipt RebuildAndApplyDefinition();
    [[nodiscard]] CaptureServiceReceipt FinalizeAndReset(
        CaptureAttachmentStatus status,
        bool continue_after_release_failure) noexcept;
    void RouteDeliveriesThroughStableEndpoint(
        StopSubscriptionGroupDefinition& definition) noexcept;

    ICaptureBackendPort& backend_;
    ProbeRouterAdapterConfig adapter_config_;
    StopPointRouter* router_ = nullptr;
    std::unique_ptr<ICaptureProfileAdapter> adapter_;
    std::atomic<ICaptureProfileAdapter*> cpu_adapter_{nullptr};
    StopSubscriptionGroupHandle group_;
    std::optional<StopSubscriptionGroupDefinition>
        pending_definition_;
    std::thread::id owner_thread_;
    CaptureAttachmentId attachment_;
    StateEpoch epoch_;
    std::uint64_t next_attachment_ = 1;
    std::uint64_t reconcile_count_ = 0;
    bool state_replacement_prepared_ = false;
    bool stopping_ = false;
    bool tainted_ = false;
    std::optional<CaptureServiceReceipt> shutdown_receipt_;
};

} // namespace savor::runtime

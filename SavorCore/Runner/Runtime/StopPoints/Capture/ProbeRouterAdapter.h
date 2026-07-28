#pragma once

#include "../StopPointRouter.h"

#include "../../../../../SavorProbe/ProbeRuntime.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace Core {
class System;
}

namespace savor::runtime {

struct ProbeRouterAdapterConfig
{
    StopSourceIdentity source;
    StopSubscriptionGroupId group_id;
    StopSubscriptionId first_subscription_id;
    std::uint32_t cpu_observer_descriptor_id = 0;
    StopEpochPolicy epoch_policy = StopEpochPolicy::RebindAfterRestore;
    std::int32_t priority = -100;
};

struct ProbeRouterGroupBuildResult
{
    bool ok = false;
    StopSubscriptionGroupDefinition definition;
    std::string error;
};

enum class ProbeRouterReconcileAction : std::uint8_t
{
    ReplaceGroup,
    ReleaseGroup,
};

struct ProbeRouterReconcileRequest
{
    std::uint64_t request_sequence = 0;
    ProbeRouterReconcileAction action =
        ProbeRouterReconcileAction::ReplaceGroup;
    RoutedStopIdentity cause;
    StopSubscriptionGroupDefinition replacement;
};

// Passive compatibility bridge for savor.capture.profile/1. It owns one
// instantiable ProbeRuntime, translates its current logical requirements into
// one source-scoped router group, and reports group replacement work to the
// session actor. It never registers a Wake/Guard/Intercept subscription and
// never mutates physical stop points itself.
class ProbeRouterAdapter final
    : public IStopPointConsumer,
      public IStopPointCpuObserver
{
public:
    ProbeRouterAdapter(
        Core::System& system,
        ProbeRouterAdapterConfig config,
        std::unique_ptr<savor::probe::ProbeRuntime> runtime =
            std::make_unique<savor::probe::ProbeRuntime>());
    ~ProbeRouterAdapter() override;

    ProbeRouterAdapter(const ProbeRouterAdapter&) = delete;
    ProbeRouterAdapter& operator=(const ProbeRouterAdapter&) = delete;

    [[nodiscard]] bool Start(
        savor::probe::Profile profile,
        savor::probe::SessionOptions options,
        std::string* error_out = nullptr);
    void Stop();

    [[nodiscard]] bool active() const noexcept;
    [[nodiscard]] savor::probe::ProbeRuntime& probe_runtime() noexcept;
    [[nodiscard]] const savor::probe::ProbeRuntime&
    probe_runtime() const noexcept;

    [[nodiscard]] ProbeRouterGroupBuildResult
    BuildCurrentGroupDefinition();
    [[nodiscard]] std::optional<ProbeRouterReconcileRequest>
    TakeReconcileRequest();
    [[nodiscard]] const std::string& last_error() const noexcept
    {
        return last_error_;
    }

    [[nodiscard]] bool SetProfileGroupEnabled(
        std::string_view group,
        bool enabled);
    [[nodiscard]] bool ReplaceProfile(
        savor::probe::Profile profile,
        std::string profile_json,
        std::string* error_out = nullptr);
    [[nodiscard]] bool EmitMarker(
        std::string_view id,
        std::uint64_t value = 0);

    [[nodiscard]] StopCpuObservationResult ObserveRoutedHit(
        std::uint32_t descriptor_id,
        const RoutedStopEvent& event) noexcept override;
    void OnStopPoint(const StopDelivery& delivery) override;

private:
    enum class CpuObserverFailure : std::uint32_t
    {
        None,
        UnknownDescriptor,
        UnsupportedEvidence,
        ProcessingException,
    };

    void QueueReconcile(const RoutedStopIdentity& cause);

    Core::System& system_;
    ProbeRouterAdapterConfig config_;
    std::unique_ptr<savor::probe::ProbeRuntime> runtime_;
    std::optional<ProbeRouterReconcileRequest> pending_reconcile_;
    std::uint64_t next_reconcile_sequence_ = 1;
    std::atomic<CpuObserverFailure> cpu_observer_failure_{
        CpuObserverFailure::None};
    std::string last_error_;
};

} // namespace savor::runtime

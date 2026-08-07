#pragma once

#include "../../StopPoints/Capture/ProbeRouterAdapter.h"

#include <memory>
#include <string>

namespace Core {
class System;
}

namespace savor::runtime {

struct CaptureAdapterFinalization
{
    bool ok = false;
    bool capture_complete = true;
    std::uint64_t capture_drop_count = 0;
    std::uint64_t progress_drop_count = 0;
    std::string incomplete_reason;
    std::string message;
};

// Actor-facing seam around the existing passive capture-profile processor.
// Implementations may inspect routed evidence and write capture artifacts, but
// they never own physical stop points or guest execution.
class ICaptureProfileAdapter
{
public:
    virtual ~ICaptureProfileAdapter() = default;

    ICaptureProfileAdapter(const ICaptureProfileAdapter&) = delete;
    ICaptureProfileAdapter& operator=(const ICaptureProfileAdapter&) = delete;

    [[nodiscard]] virtual bool Start(
        savor::probe::Profile profile,
        savor::probe::SessionOptions options,
        std::string* error_out) = 0;
    [[nodiscard]] virtual bool active() const noexcept = 0;
    [[nodiscard]] virtual ProbeRouterGroupBuildResult
    BuildCurrentGroupDefinition() = 0;
    [[nodiscard]] virtual std::optional<ProbeRouterReconcileRequest>
    TakeReconcileRequest() = 0;
    [[nodiscard]] virtual bool SetProfileGroupEnabled(
        std::string_view group,
        bool enabled) = 0;
    [[nodiscard]] virtual bool ReplaceProfile(
        savor::probe::Profile profile,
        std::string profile_json,
        std::string* error_out) = 0;
    [[nodiscard]] virtual bool EmitMarker(
        std::string_view id,
        std::uint64_t value) = 0;
    [[nodiscard]] virtual StopCpuObservationResult ObserveRoutedHit(
        std::uint32_t descriptor_id,
        const RoutedStopEvent& event) noexcept = 0;
    virtual void OnStopPoint(const StopDelivery& delivery) = 0;
    [[nodiscard]] virtual CaptureAdapterFinalization Finalize() noexcept = 0;

protected:
    ICaptureProfileAdapter() = default;
};

// Private backend factory. The concrete Dolphin adapter uses this seam to
// provide the Core::System dependency required by ProbeRouterAdapter without
// exposing that dependency to CaptureService.
class ICaptureBackendPort
{
public:
    virtual ~ICaptureBackendPort() = default;

    ICaptureBackendPort(const ICaptureBackendPort&) = delete;
    ICaptureBackendPort& operator=(const ICaptureBackendPort&) = delete;

    [[nodiscard]] virtual std::unique_ptr<ICaptureProfileAdapter>
    CreateCaptureProfileAdapter(
        const ProbeRouterAdapterConfig& config,
        std::string* error_out) = 0;

protected:
    ICaptureBackendPort() = default;
};

// Production wrapper preserving savor.capture.profile/1 behavior behind the
// new service boundary.
class ProbeCaptureProfileAdapter final : public ICaptureProfileAdapter
{
public:
    ProbeCaptureProfileAdapter(
        Core::System& system,
        ProbeRouterAdapterConfig config);
    ~ProbeCaptureProfileAdapter() override;

    [[nodiscard]] bool Start(
        savor::probe::Profile profile,
        savor::probe::SessionOptions options,
        std::string* error_out) override;
    [[nodiscard]] bool active() const noexcept override;
    [[nodiscard]] ProbeRouterGroupBuildResult
    BuildCurrentGroupDefinition() override;
    [[nodiscard]] std::optional<ProbeRouterReconcileRequest>
    TakeReconcileRequest() override;
    [[nodiscard]] bool SetProfileGroupEnabled(
        std::string_view group,
        bool enabled) override;
    [[nodiscard]] bool ReplaceProfile(
        savor::probe::Profile profile,
        std::string profile_json,
        std::string* error_out) override;
    [[nodiscard]] bool EmitMarker(
        std::string_view id,
        std::uint64_t value) override;
    [[nodiscard]] StopCpuObservationResult ObserveRoutedHit(
        std::uint32_t descriptor_id,
        const RoutedStopEvent& event) noexcept override;
    void OnStopPoint(const StopDelivery& delivery) override;
    [[nodiscard]] CaptureAdapterFinalization Finalize() noexcept override;

private:
    ProbeRouterAdapter adapter_;
};

} // namespace savor::runtime

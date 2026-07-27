#include "ICaptureBackendPort.h"

#include <exception>
#include <utility>

namespace savor::runtime {

ProbeCaptureProfileAdapter::ProbeCaptureProfileAdapter(
    Core::System& system,
    ProbeRouterAdapterConfig config)
    : adapter_(system, std::move(config))
{
}

ProbeCaptureProfileAdapter::~ProbeCaptureProfileAdapter()
{
    (void)Finalize();
}

bool ProbeCaptureProfileAdapter::Start(
    savor::probe::Profile profile,
    savor::probe::SessionOptions options,
    std::string* error_out)
{
    return adapter_.Start(
        std::move(profile),
        std::move(options),
        error_out);
}

bool ProbeCaptureProfileAdapter::active() const noexcept
{
    return adapter_.active();
}

ProbeRouterGroupBuildResult
ProbeCaptureProfileAdapter::BuildCurrentGroupDefinition()
{
    return adapter_.BuildCurrentGroupDefinition();
}

std::optional<ProbeRouterReconcileRequest>
ProbeCaptureProfileAdapter::TakeReconcileRequest()
{
    return adapter_.TakeReconcileRequest();
}

bool ProbeCaptureProfileAdapter::SetProfileGroupEnabled(
    std::string_view group,
    bool enabled)
{
    return adapter_.SetProfileGroupEnabled(group, enabled);
}

bool ProbeCaptureProfileAdapter::ReplaceProfile(
    savor::probe::Profile profile,
    std::string profile_json,
    std::string* error_out)
{
    return adapter_.ReplaceProfile(
        std::move(profile),
        std::move(profile_json),
        error_out);
}

bool ProbeCaptureProfileAdapter::PrepareForStateReplacement(
    std::string* error_out)
{
    return adapter_.probe_runtime().prepare_for_core_shutdown(error_out);
}

bool ProbeCaptureProfileAdapter::ResumeAfterStateReplacement(
    StateEpoch epoch,
    std::string* error_out)
{
    adapter_.probe_runtime().set_guest_state_epoch(epoch.value());
    return adapter_.probe_runtime().resume_after_core_boot(error_out);
}

StopCpuObservationResult ProbeCaptureProfileAdapter::ObserveRoutedHit(
    std::uint32_t descriptor_id,
    const RoutedStopEvent& event) noexcept
{
    return adapter_.ObserveRoutedHit(descriptor_id, event);
}

void ProbeCaptureProfileAdapter::OnStopPoint(
    const StopDelivery& delivery)
{
    adapter_.OnStopPoint(delivery);
}

CaptureAdapterFinalization
ProbeCaptureProfileAdapter::Finalize() noexcept
{
    CaptureAdapterFinalization result;
    try
    {
        if (!adapter_.active())
        {
            result.ok = true;
            return result;
        }

        auto& runtime = adapter_.probe_runtime();
        result.capture_complete = runtime.capture_complete();
        result.capture_drop_count = runtime.capture_drop_count();
        result.progress_drop_count = runtime.progress_drop_count();
        result.incomplete_reason = runtime.capture_incomplete_reason();
        adapter_.Stop();
        result.ok = true;
        if (!result.capture_complete)
            result.message = result.incomplete_reason;
    }
    catch (const std::exception& ex)
    {
        result.message =
            std::string("Capture profile finalization failed: ") +
            ex.what();
    }
    catch (...)
    {
        result.message =
            "Capture profile finalization failed with an unknown error";
    }
    return result;
}

} // namespace savor::runtime

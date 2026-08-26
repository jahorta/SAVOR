#include "ProbeRouterAdapter.h"

#include "Core/System.h"

#include <algorithm>
#include <limits>
#include <ranges>
#include <utility>
#include <variant>
#include <vector>

namespace savor::runtime {
namespace {

static_assert(
    savor::probe::kMaxProbeRoutedHitSamples >=
    kMaxRoutedHitSamples);

struct CanonicalPcSite
{
    std::uint32_t pc = 0;
    bool progress_only = true;
    std::vector<std::uint32_t> sample_descriptor_ids;
};

struct CanonicalMemorySite
{
    std::uint32_t start = 0;
    std::uint32_t end = 0;
    bool read = false;
    bool write = false;
    bool progress_only = true;
};

[[nodiscard]] bool IsProgressOnly(
    const savor::probe::ProfileStopRequirement& requirement)
{
    if (requirement.kind ==
        savor::probe::ProfileStopRequirementKind::ActivationPc)
    {
        return false;
    }
    return savor::probe::has_subscription(
               requirement.subscriptions,
               savor::probe::Subscription::Progress) &&
        !savor::probe::has_subscription(
            requirement.subscriptions,
            savor::probe::Subscription::Capture) &&
        !savor::probe::has_subscription(
            requirement.subscriptions,
            savor::probe::Subscription::Control);
}

[[nodiscard]] bool IsCurrent(
    const savor::probe::ProfileStopRequirement& requirement)
{
    if (requirement.kind ==
        savor::probe::ProfileStopRequirementKind::ActivationPc)
    {
        return requirement.group_enabled;
    }
    if (!requirement.active ||
        requirement.exhausted ||
        !requirement.group_enabled)
    {
        return false;
    }
    return requirement.kind !=
            savor::probe::ProfileStopRequirementKind::Memory ||
        requirement.address_resolved;
}

void AddMemoryAccess(
    CanonicalMemorySite& site,
    savor::probe::MemoryAccess access)
{
    site.read = site.read ||
        access == savor::probe::MemoryAccess::Read ||
        access == savor::probe::MemoryAccess::Access;
    site.write = site.write ||
        access == savor::probe::MemoryAccess::Write ||
        access == savor::probe::MemoryAccess::Access;
}

[[nodiscard]] StopMemoryAccess ToStopMemoryAccess(
    const CanonicalMemorySite& site)
{
    if (site.read && site.write)
        return StopMemoryAccess::Access;
    return site.read
        ? StopMemoryAccess::Read
        : StopMemoryAccess::Write;
}

[[nodiscard]] bool ValidConfig(
    const ProbeRouterAdapterConfig& config,
    std::string& error)
{
    if (!config.source.id)
        error = "capture router source ID must be nonzero";
    else if (config.source.stable_name.empty())
        error = "capture router source stable name is required";
    else if (!config.group_id)
        error = "capture router group ID must be nonzero";
    else if (!config.first_subscription_id)
        error = "capture router first subscription ID must be nonzero";
    else if (config.cpu_observer_descriptor_id == 0)
        error = "capture router CPU observer descriptor ID must be nonzero";
    else
        return true;
    return false;
}

} // namespace

ProbeRouterAdapter::ProbeRouterAdapter(
    Core::System& system,
    ProbeRouterAdapterConfig config,
    std::unique_ptr<savor::probe::ProbeRuntime> runtime)
    : system_(system),
      config_(std::move(config)),
      runtime_(std::move(runtime))
{
    if (!runtime_)
        runtime_ = std::make_unique<savor::probe::ProbeRuntime>();
}

ProbeRouterAdapter::~ProbeRouterAdapter()
{
    Stop();
}

bool ProbeRouterAdapter::Start(
    savor::probe::Profile profile,
    savor::probe::SessionOptions options,
    std::string* error_out)
{
    Stop();
    last_error_.clear();
    if (!ValidConfig(config_, last_error_))
    {
        if (error_out)
            *error_out = last_error_;
        return false;
    }
    if (!runtime_->start(
            system_,
            std::move(profile),
            std::move(options),
            &last_error_))
    {
        if (error_out)
            *error_out = last_error_;
        return false;
    }

    ProbeRouterGroupBuildResult initial = BuildCurrentGroupDefinition();
    if (!initial.ok)
    {
        runtime_->stop();
        last_error_ = std::move(initial.error);
        if (error_out)
            *error_out = last_error_;
        return false;
    }
    return true;
}

void ProbeRouterAdapter::Stop()
{
    pending_reconcile_.reset();
    cpu_observer_failure_.store(
        CpuObserverFailure::None,
        std::memory_order_release);
    last_error_.clear();
    if (runtime_)
        runtime_->stop();
}

bool ProbeRouterAdapter::active() const noexcept
{
    return runtime_ && runtime_->active();
}

savor::probe::ProbeRuntime&
ProbeRouterAdapter::probe_runtime() noexcept
{
    return *runtime_;
}

const savor::probe::ProbeRuntime&
ProbeRouterAdapter::probe_runtime() const noexcept
{
    return *runtime_;
}

ProbeRouterGroupBuildResult
ProbeRouterAdapter::BuildCurrentGroupDefinition()
{
    ProbeRouterGroupBuildResult result;
    if (!ValidConfig(config_, result.error))
        return result;
    if (!runtime_ || !runtime_->active())
    {
        result.error = "capture probe runtime is not active";
        return result;
    }

    std::vector<CanonicalPcSite> pcs;
    std::vector<CanonicalMemorySite> memory;
    for (const auto& requirement :
        runtime_->profile_stop_requirements())
    {
        if (!IsCurrent(requirement))
            continue;

        const bool progress_only = IsProgressOnly(requirement);
        if (requirement.kind ==
                savor::probe::ProfileStopRequirementKind::Pc ||
            requirement.kind ==
                savor::probe::ProfileStopRequirementKind::ActivationPc)
        {
            if (requirement.address == 0)
            {
                result.error =
                    "capture profile requested a zero PC stop";
                return result;
            }
            pcs.push_back({requirement.address, progress_only,
                requirement.routed_sample_descriptor_ids});
            continue;
        }

        if (requirement.address == 0 || requirement.size == 0)
        {
            result.error =
                "capture profile requested an invalid memory stop";
            return result;
        }
        const std::uint64_t wide_end =
            static_cast<std::uint64_t>(requirement.address) +
            requirement.size - 1;
        if (wide_end > std::numeric_limits<std::uint32_t>::max())
        {
            result.error =
                "capture profile memory stop overflows the guest address space";
            return result;
        }
        CanonicalMemorySite site{
            requirement.address,
            static_cast<std::uint32_t>(wide_end),
            false,
            false,
            progress_only,
        };
        AddMemoryAccess(site, requirement.memory_access);
        memory.push_back(site);
    }

    std::ranges::sort(pcs, {}, &CanonicalPcSite::pc);
    std::vector<CanonicalPcSite> unique_pcs;
    unique_pcs.reserve(pcs.size());
    for (const auto& site : pcs)
    {
        if (!unique_pcs.empty() &&
            unique_pcs.back().pc == site.pc)
        {
            unique_pcs.back().progress_only =
                unique_pcs.back().progress_only &&
                site.progress_only;
            for (const auto descriptor_id : site.sample_descriptor_ids) {
                if (std::ranges::find(unique_pcs.back().sample_descriptor_ids,
                        descriptor_id) == unique_pcs.back().sample_descriptor_ids.end())
                    unique_pcs.back().sample_descriptor_ids.push_back(descriptor_id);
            }
        }
        else
        {
            unique_pcs.push_back(site);
        }
    }

    std::ranges::sort(memory, [](const auto& lhs, const auto& rhs) {
        if (lhs.start != rhs.start)
            return lhs.start < rhs.start;
        return lhs.end < rhs.end;
    });
    std::vector<CanonicalMemorySite> merged_memory;
    merged_memory.reserve(memory.size());
    for (const auto& site : memory)
    {
        if (merged_memory.empty() ||
            site.start > merged_memory.back().end)
        {
            merged_memory.push_back(site);
            continue;
        }
        auto& existing = merged_memory.back();
        existing.end = std::max(existing.end, site.end);
        existing.read = existing.read || site.read;
        existing.write = existing.write || site.write;
        existing.progress_only =
            existing.progress_only && site.progress_only;
    }

    const std::size_t subscription_count =
        unique_pcs.size() + merged_memory.size();
    if (subscription_count > kMaxLogicalStopSubscriptions)
    {
        result.error =
            "capture profile exceeds the router logical subscription capacity";
        return result;
    }
    const std::uint64_t first_id =
        config_.first_subscription_id.value();
    if (subscription_count != 0 &&
        first_id >
            std::numeric_limits<std::uint64_t>::max() -
                (subscription_count - 1))
    {
        result.error =
            "capture router subscription ID range overflows";
        return result;
    }

    result.definition.id = config_.group_id;
    result.definition.source = config_.source;
    result.definition.subscriptions.reserve(subscription_count);
    std::uint64_t next_id = first_id;
    const auto add_subscription = [&](
                                      StopPointSpec point,
                                      bool progress_only,
                                      std::vector<std::uint32_t> sample_descriptor_ids = {}) {
        result.definition.subscriptions.push_back(
            StopSubscriptionDefinition{
                .id = StopSubscriptionId(next_id++),
                .point = std::move(point),
                .route = PassiveStopObservation{
                    .cpu_observer_descriptor_id =
                        config_.cpu_observer_descriptor_id,
                    .lossless = !progress_only,
                },
                .lifetime = StopSubscriptionLifetime::Scoped,
                .priority = config_.priority,
                .sample_descriptor_ids = std::move(sample_descriptor_ids),
                .consumer = this,
            });
    };
    for (const auto& site : unique_pcs)
    {
        add_subscription(
            PcStopPointSpec{site.pc},
            site.progress_only,
            site.sample_descriptor_ids);
    }
    for (const auto& site : merged_memory)
    {
        add_subscription(
            MemoryStopPointSpec{
                site.start,
                site.end - site.start + 1,
                ToStopMemoryAccess(site),
            },
            site.progress_only);
    }

    result.ok = true;
    return result;
}

std::optional<ProbeRouterReconcileRequest>
ProbeRouterAdapter::TakeReconcileRequest()
{
    return std::exchange(pending_reconcile_, std::nullopt);
}

bool ProbeRouterAdapter::EmitMarker(
    std::string_view id,
    std::uint64_t value)
{
    if (!runtime_->active() || id.empty())
        return false;
    return runtime_->emit_marker(id, value);
}

StopCpuObservationResult ProbeRouterAdapter::ObserveRoutedHit(
    std::uint32_t descriptor_id,
    const RoutedStopEvent& event) noexcept
{
    if (!active())
        return StopCpuObservationResult::Ignored;
    if (descriptor_id != config_.cpu_observer_descriptor_id)
    {
        cpu_observer_failure_.store(
            CpuObserverFailure::UnknownDescriptor,
            std::memory_order_release);
        return StopCpuObservationResult::Failed;
    }

    savor::probe::ProbeRoutedHitContext context;
    context.routed_sequence = event.identity.sequence.value();
    context.sample_snapshot_id =
        event.identity.sample_snapshot.value();
    context.guest_workset_epoch =
        event.identity.workset_epoch.value();
    context.active_foreground_wake =
        event.active_foreground_wait;
    context.sample_count = static_cast<std::uint8_t>(
        std::min<std::size_t>(
            event.sample_count,
            context.samples.size()));
    for (std::size_t i = 0; i < context.sample_count; ++i)
    {
        context.samples[i] = {
            event.samples[i].descriptor_id,
            event.samples[i].value,
            event.samples[i].available,
        };
    }

    try
    {
        if (std::holds_alternative<PcStopPointSpec>(
                event.evidence.point))
        {
            runtime_->process_routed_pc(
                system_.GetPowerPC(),
                event.evidence.hit_pc,
                context);
        }
        else if (const auto* memory =
                     std::get_if<MemoryStopPointSpec>(
                         &event.evidence.point))
        {
            runtime_->process_routed_memory(
                system_,
                event.evidence.hit_pc,
                memory->address,
                memory->size,
                event.evidence.value,
                memory->access != StopMemoryAccess::Read,
                event.evidence.post_write,
                context);
        }
        else
        {
            cpu_observer_failure_.store(
                CpuObserverFailure::UnsupportedEvidence,
                std::memory_order_release);
            return StopCpuObservationResult::Failed;
        }
    }
    catch (...)
    {
        cpu_observer_failure_.store(
            CpuObserverFailure::ProcessingException,
            std::memory_order_release);
        return StopCpuObservationResult::Failed;
    }
    return runtime_->requires_physical_reconcile_before_resume()
        ? StopCpuObservationResult::ObservedRequiresReconcile
        : StopCpuObservationResult::Observed;
}

void ProbeRouterAdapter::OnStopPoint(
    const StopDelivery& delivery)
{
    if (!active())
        return;
    if (delivery.source_id != config_.source.id ||
        delivery.group_id != config_.group_id)
    {
        last_error_ =
            "capture router delivery belongs to a different source group";
        return;
    }
    switch (cpu_observer_failure_.exchange(
        CpuObserverFailure::None,
        std::memory_order_acq_rel))
    {
    case CpuObserverFailure::None:
        break;
    case CpuObserverFailure::UnknownDescriptor:
        last_error_ =
            "capture CPU observer received an unknown descriptor";
        break;
    case CpuObserverFailure::UnsupportedEvidence:
        last_error_ =
            "capture CPU observer received unsupported routed evidence";
        break;
    case CpuObserverFailure::ProcessingException:
        last_error_ =
            "capture CPU observer failed during profile processing";
        break;
    }

    if (runtime_->consume_physical_reconcile_request())
        QueueReconcile(delivery.event.identity);
}

void ProbeRouterAdapter::QueueReconcile(
    const RoutedStopIdentity& cause)
{
    ProbeRouterGroupBuildResult built =
        BuildCurrentGroupDefinition();
    if (!built.ok)
    {
        last_error_ = std::move(built.error);
        pending_reconcile_.reset();
        return;
    }

    ProbeRouterReconcileRequest request;
    request.request_sequence = next_reconcile_sequence_++;
    request.action = built.definition.subscriptions.empty()
        ? ProbeRouterReconcileAction::ReleaseGroup
        : ProbeRouterReconcileAction::ReplaceGroup;
    request.cause = cause;
    request.replacement = std::move(built.definition);
    pending_reconcile_ = std::move(request);
}

} // namespace savor::runtime

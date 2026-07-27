#include "CaptureService.h"

#include <limits>
#include <utility>

namespace savor::runtime {
namespace {

[[nodiscard]] bool PhysicalIntegrityUnknown(
    const StopPointError& error) noexcept
{
    return error.code == StopPointErrorCode::PhysicalIntegrityUnknown;
}

} // namespace

CaptureService::CaptureService(
    ICaptureBackendPort& backend,
    ProbeRouterAdapterConfig adapter_config)
    : backend_(backend),
      adapter_config_(std::move(adapter_config)),
      owner_thread_(std::this_thread::get_id())
{
}

CaptureService::~CaptureService()
{
    (void)Shutdown();
}

CaptureServiceError CaptureService::CheckActor() const
{
    if (owner_thread_ != std::this_thread::get_id())
    {
        return {
            CaptureServiceErrorCode::WrongThread,
            "CaptureService operation used the wrong actor thread",
        };
    }
    if (stopping_)
    {
        return {
            CaptureServiceErrorCode::RuntimeStopping,
            "CaptureService is stopping",
        };
    }
    if (tainted_)
    {
        return {
            CaptureServiceErrorCode::FinalizationFailed,
            "CaptureService cleanup integrity is unproven",
        };
    }
    return {};
}

CaptureServiceReceipt CaptureService::Failure(
    CaptureServiceErrorCode code,
    std::string message,
    bool taint) const
{
    CaptureServiceReceipt result;
    result.attachment = attachment_;
    result.epoch = epoch_;
    result.requires_session_taint = taint;
    result.error = {code, std::move(message)};
    if (router_)
    {
        result.dispatch_generation = router_->dispatch_generation();
        result.physical_generation = router_->physical_generation();
    }
    return result;
}

CaptureServiceReceipt CaptureService::Success(
    CaptureAttachmentStatus status) const
{
    CaptureServiceReceipt result;
    result.ok = true;
    result.status = status;
    result.attachment = attachment_;
    result.epoch = epoch_;
    if (router_)
    {
        result.dispatch_generation =
            router_->dispatch_generation();
        result.physical_generation =
            router_->physical_generation();
    }
    return result;
}

CaptureServiceReceipt CaptureService::BindRouter(
    StopPointRouter& router,
    StateEpoch epoch)
{
    if (CaptureServiceError error = CheckActor())
        return Failure(error.code, std::move(error.message));
    if (router_)
    {
        return Failure(
            CaptureServiceErrorCode::AlreadyBound,
            "CaptureService is already bound to a stop-point router");
    }
    if (!epoch || router.state_epoch() != epoch)
    {
        return Failure(
            CaptureServiceErrorCode::StaleEpoch,
            "CaptureService requires the router's current nonzero StateEpoch");
    }
    router_ = &router;
    epoch_ = epoch;
    return Success(CaptureAttachmentStatus::Detached);
}

CaptureServiceReceipt CaptureService::Attach(
    CaptureAttachmentRequest request)
{
    if (CaptureServiceError error = CheckActor())
        return Failure(error.code, std::move(error.message));
    if (!router_)
    {
        return Failure(
            CaptureServiceErrorCode::NotBound,
            "CaptureService must be bound before attaching a profile");
    }
    if (adapter_)
    {
        return Failure(
            CaptureServiceErrorCode::AlreadyAttached,
            "Only one capture profile may be attached to a session");
    }
    if (state_replacement_prepared_)
    {
        return Failure(
            CaptureServiceErrorCode::StateReplacementActive,
            "A capture profile cannot attach during state replacement");
    }
    if (!request.expected_epoch ||
        request.expected_epoch != epoch_ ||
        router_->state_epoch() != epoch_)
    {
        return Failure(
            CaptureServiceErrorCode::StaleEpoch,
            "Capture profile attachment expected a different StateEpoch");
    }
    if (request.profile_json.empty())
    {
        return Failure(
            CaptureServiceErrorCode::InvalidArgument,
            "Capture profile JSON is required");
    }

    savor::probe::ProfileParseResult parsed =
        savor::probe::parse_profile_json(request.profile_json);
    if (!parsed.profile)
    {
        return Failure(
            CaptureServiceErrorCode::ProfileParseFailed,
            savor::probe::format_profile_errors(parsed));
    }
    request.options.metadata.profile_json = request.profile_json;

    std::string error;
    std::unique_ptr<ICaptureProfileAdapter> candidate =
        backend_.CreateCaptureProfileAdapter(adapter_config_, &error);
    if (!candidate)
    {
        return Failure(
            CaptureServiceErrorCode::BackendUnavailable,
            error.empty()
                ? "Capture backend did not create a profile adapter"
                : std::move(error));
    }
    if (!candidate->Start(
            std::move(*parsed.profile),
            std::move(request.options),
            &error))
    {
        const CaptureAdapterFinalization finalized =
            candidate->Finalize();
        const bool taint = !finalized.ok;
        if (taint)
            tainted_ = true;
        if (!finalized.ok && !finalized.message.empty())
        {
            if (!error.empty())
                error += "; ";
            error += finalized.message;
        }
        return Failure(
            CaptureServiceErrorCode::AdapterStartFailed,
            error.empty()
                ? "Capture profile adapter failed to start"
                : std::move(error),
            taint);
    }

    ProbeRouterGroupBuildResult built =
        candidate->BuildCurrentGroupDefinition();
    if (!built.ok)
    {
        const CaptureAdapterFinalization finalized =
            candidate->Finalize();
        const bool taint = !finalized.ok;
        if (taint)
            tainted_ = true;
        if (!finalized.ok && !finalized.message.empty())
        {
            if (!built.error.empty())
                built.error += "; ";
            built.error += finalized.message;
        }
        return Failure(
            CaptureServiceErrorCode::RouterRegistrationFailed,
            built.error.empty()
                ? "Capture profile could not build its router group"
                : std::move(built.error),
            taint);
    }

    adapter_ = std::move(candidate);
    cpu_adapter_.store(adapter_.get(), std::memory_order_release);
    RouteDeliveriesThroughStableEndpoint(built.definition);
    if (!built.definition.subscriptions.empty())
    {
        StopGroupRegistrationResult registration =
            router_->RegisterGroup(std::move(built.definition));
        if (!registration.receipt.ok)
        {
            cpu_adapter_.store(nullptr, std::memory_order_release);
            const bool taint =
                PhysicalIntegrityUnknown(registration.receipt.error);
            const std::string message =
                registration.receipt.error.message;
            const CaptureAdapterFinalization finalized =
                adapter_->Finalize();
            adapter_.reset();
            const bool cleanup_taint = !finalized.ok;
            if (cleanup_taint)
                tainted_ = true;
            std::string combined = message.empty()
                ? "Capture router group registration failed"
                : message;
            if (!finalized.ok && !finalized.message.empty())
            {
                combined += "; ";
                combined += finalized.message;
            }
            return Failure(
                CaptureServiceErrorCode::RouterRegistrationFailed,
                std::move(combined),
                taint || cleanup_taint);
        }
        group_ = std::move(registration.handle);
    }

    if (next_attachment_ == 0 ||
        next_attachment_ == std::numeric_limits<std::uint64_t>::max())
    {
        CaptureServiceReceipt finalized =
            FinalizeAndReset(
                CaptureAttachmentStatus::Detached,
                true);
        return Failure(
            CaptureServiceErrorCode::InvalidArgument,
            "Capture attachment identity exhausted",
            finalized.requires_session_taint);
    }
    attachment_ = CaptureAttachmentId(next_attachment_++);
    return Success(CaptureAttachmentStatus::Attached);
}

CaptureServiceReceipt CaptureService::Detach(
    CaptureAttachmentId attachment)
{
    if (CaptureServiceError error = CheckActor())
        return Failure(error.code, std::move(error.message));
    if (!adapter_)
    {
        return Failure(
            CaptureServiceErrorCode::NotAttached,
            "No capture profile is attached");
    }
    if (!attachment || attachment != attachment_)
    {
        return Failure(
            CaptureServiceErrorCode::StaleAttachment,
            "Capture detach named a different attachment");
    }
    if (state_replacement_prepared_)
    {
        return Failure(
            CaptureServiceErrorCode::StateReplacementActive,
            "Capture detach is unavailable during state replacement");
    }
    return FinalizeAndReset(
        CaptureAttachmentStatus::Detached,
        false);
}

CaptureServiceReceipt CaptureService::ApplyDefinition(
    StopSubscriptionGroupDefinition definition)
{
    RouteDeliveriesThroughStableEndpoint(definition);
    try
    {
        pending_definition_ = definition;
    }
    catch (...)
    {
        return Failure(
            CaptureServiceErrorCode::RouterReconcileFailed,
            "Capture reconciliation could not retain its retry definition");
    }
    if (definition.subscriptions.empty())
    {
        if (group_.active())
        {
            StopReleaseReceipt released = group_.Release();
            if (!released.ok)
            {
                return Failure(
                    CaptureServiceErrorCode::RouterReconcileFailed,
                    released.error.message,
                    PhysicalIntegrityUnknown(released.error));
            }
        }
        pending_definition_.reset();
        ++reconcile_count_;
        return Success(CaptureAttachmentStatus::Attached);
    }

    if (group_.active())
    {
        StopGroupReceipt replaced =
            group_.Replace(std::move(definition));
        if (!replaced.ok)
        {
            return Failure(
                CaptureServiceErrorCode::RouterReconcileFailed,
                replaced.error.message,
                PhysicalIntegrityUnknown(replaced.error));
        }
    }
    else
    {
        StopGroupRegistrationResult registered =
            router_->RegisterGroup(std::move(definition));
        if (!registered.receipt.ok)
        {
            return Failure(
                CaptureServiceErrorCode::RouterReconcileFailed,
                registered.receipt.error.message,
                PhysicalIntegrityUnknown(registered.receipt.error));
        }
        group_ = std::move(registered.handle);
    }
    pending_definition_.reset();
    ++reconcile_count_;
    return Success(CaptureAttachmentStatus::Attached);
}

CaptureServiceReceipt CaptureService::RebuildAndApplyDefinition()
{
    ProbeRouterGroupBuildResult built =
        adapter_->BuildCurrentGroupDefinition();
    if (!built.ok)
    {
        return Failure(
            CaptureServiceErrorCode::RouterReconcileFailed,
            built.error.empty()
                ? "Capture profile failed to rebuild its router group"
                : std::move(built.error));
    }
    return ApplyDefinition(std::move(built.definition));
}

CaptureServiceReceipt CaptureService::ReconcileBeforeResume()
{
    if (CaptureServiceError error = CheckActor())
        return Failure(error.code, std::move(error.message));
    if (!adapter_)
    {
        return Failure(
            CaptureServiceErrorCode::NotAttached,
            "No capture profile is attached");
    }
    if (state_replacement_prepared_)
    {
        return Failure(
            CaptureServiceErrorCode::StateReplacementActive,
            "Capture reconciliation is unavailable during state replacement");
    }
    if (pending_definition_)
        return ApplyDefinition(*pending_definition_);

    std::optional<ProbeRouterReconcileRequest> request =
        adapter_->TakeReconcileRequest();
    if (!request)
        return Success(CaptureAttachmentStatus::Attached);
    return ApplyDefinition(std::move(request->replacement));
}

CaptureServiceReceipt CaptureService::SetProfileGroupEnabled(
    CaptureAttachmentId attachment,
    std::string_view group,
    bool enabled)
{
    if (CaptureServiceError error = CheckActor())
        return Failure(error.code, std::move(error.message));
    if (!adapter_)
    {
        return Failure(
            CaptureServiceErrorCode::NotAttached,
            "No capture profile is attached");
    }
    if (attachment != attachment_)
    {
        return Failure(
            CaptureServiceErrorCode::StaleAttachment,
            "Capture group update named a different attachment");
    }
    if (state_replacement_prepared_)
    {
        return Failure(
            CaptureServiceErrorCode::StateReplacementActive,
            "Capture group update is unavailable during state replacement");
    }
    if (!adapter_->SetProfileGroupEnabled(group, enabled))
    {
        return Failure(
            CaptureServiceErrorCode::InvalidArgument,
            "Capture profile group was not found");
    }
    return ReconcileBeforeResume();
}

CaptureServiceReceipt CaptureService::ReplaceProfile(
    CaptureAttachmentId attachment,
    std::string profile_json)
{
    if (CaptureServiceError error = CheckActor())
        return Failure(error.code, std::move(error.message));
    if (!adapter_)
    {
        return Failure(
            CaptureServiceErrorCode::NotAttached,
            "No capture profile is attached");
    }
    if (attachment != attachment_)
    {
        return Failure(
            CaptureServiceErrorCode::StaleAttachment,
            "Capture profile replacement named a different attachment");
    }
    if (state_replacement_prepared_)
    {
        return Failure(
            CaptureServiceErrorCode::StateReplacementActive,
            "Capture profile replacement is unavailable during state replacement");
    }

    savor::probe::ProfileParseResult parsed =
        savor::probe::parse_profile_json(profile_json);
    if (!parsed.profile)
    {
        return Failure(
            CaptureServiceErrorCode::ProfileParseFailed,
            savor::probe::format_profile_errors(parsed));
    }
    std::string error;
    if (!adapter_->ReplaceProfile(
            std::move(*parsed.profile),
            std::move(profile_json),
            &error))
    {
        return Failure(
            CaptureServiceErrorCode::AdapterStartFailed,
            error.empty()
                ? "Capture profile replacement failed"
                : std::move(error));
    }
    return ReconcileBeforeResume();
}

CaptureServiceReceipt CaptureService::PrepareStateReplacement(
    StateEpoch expected_epoch)
{
    if (CaptureServiceError error = CheckActor())
        return Failure(error.code, std::move(error.message));
    if (state_replacement_prepared_)
    {
        return Failure(
            CaptureServiceErrorCode::StateReplacementActive,
            "Capture state replacement is already prepared");
    }
    if (!expected_epoch || expected_epoch != epoch_)
    {
        return Failure(
            CaptureServiceErrorCode::StaleEpoch,
            "Capture state replacement expected a different StateEpoch");
    }
    if (adapter_)
    {
        std::string error;
        if (!adapter_->PrepareForStateReplacement(&error))
        {
            return Failure(
                CaptureServiceErrorCode::StateReplacementFailed,
                error.empty()
                    ? "Capture profile failed to prepare for state replacement"
                    : std::move(error));
        }
    }
    state_replacement_prepared_ = true;
    return Success(
        adapter_
            ? CaptureAttachmentStatus::Attached
            : CaptureAttachmentStatus::Detached);
}

CaptureServiceReceipt CaptureService::CommitStateReplacement(
    StateEpoch new_epoch)
{
    if (CaptureServiceError error = CheckActor())
        return Failure(error.code, std::move(error.message));
    if (!state_replacement_prepared_)
    {
        return Failure(
            CaptureServiceErrorCode::StateReplacementNotPrepared,
            "Capture state replacement was not prepared");
    }
    if (!new_epoch || new_epoch.value() <= epoch_.value() ||
        !router_ || router_->state_epoch() != new_epoch)
    {
        return Failure(
            CaptureServiceErrorCode::StaleEpoch,
            "Capture state replacement requires the router's advanced StateEpoch");
    }

    epoch_ = new_epoch;
    state_replacement_prepared_ = false;
    if (!adapter_)
        return Success(CaptureAttachmentStatus::Detached);

    std::string error;
    if (!adapter_->ResumeAfterStateReplacement(new_epoch, &error))
    {
        return Failure(
            CaptureServiceErrorCode::StateReplacementFailed,
            error.empty()
                ? "Capture profile failed to resume after state replacement"
                : std::move(error),
            true);
    }
    CaptureServiceReceipt reconciled =
        RebuildAndApplyDefinition();
    if (!reconciled.ok)
        reconciled.requires_session_taint = true;
    return reconciled;
}

CaptureServiceReceipt CaptureService::RollbackStateReplacement(
    StateEpoch restored_epoch)
{
    if (CaptureServiceError error = CheckActor())
        return Failure(error.code, std::move(error.message));
    if (!state_replacement_prepared_)
    {
        return Failure(
            CaptureServiceErrorCode::StateReplacementNotPrepared,
            "Capture state replacement was not prepared");
    }
    if (!restored_epoch || restored_epoch != epoch_ ||
        !router_ || router_->state_epoch() != restored_epoch)
    {
        return Failure(
            CaptureServiceErrorCode::StaleEpoch,
            "Capture rollback requires the preserved router StateEpoch");
    }

    state_replacement_prepared_ = false;
    if (!adapter_)
        return Success(CaptureAttachmentStatus::Detached);

    std::string error;
    if (!adapter_->ResumeAfterStateReplacement(restored_epoch, &error))
    {
        return Failure(
            CaptureServiceErrorCode::StateReplacementFailed,
            error.empty()
                ? "Capture profile failed to resume after state rollback"
                : std::move(error),
            true);
    }
    CaptureServiceReceipt reconciled =
        RebuildAndApplyDefinition();
    if (!reconciled.ok)
        reconciled.requires_session_taint = true;
    return reconciled;
}

CaptureServiceReceipt CaptureService::FinalizeAndReset(
    CaptureAttachmentStatus status,
    bool continue_after_release_failure) noexcept
{
    CaptureServiceReceipt result;
    result.status = status;
    result.attachment = attachment_;
    result.epoch = epoch_;

    bool release_failed = false;
    bool release_taint = false;
    std::string release_error;
    if (group_.active())
    {
        StopReleaseReceipt released = group_.Release();
        if (!released.ok)
        {
            release_failed = true;
            release_taint =
                PhysicalIntegrityUnknown(released.error);
            release_error = std::move(released.error.message);
            if (!continue_after_release_failure)
            {
                result.requires_session_taint = release_taint;
                result.error = {
                    CaptureServiceErrorCode::RouterReconcileFailed,
                    release_error.empty()
                        ? "Capture router group release failed"
                        : std::move(release_error),
                };
                return result;
            }
        }
    }

    cpu_adapter_.store(nullptr, std::memory_order_release);
    CaptureAdapterFinalization finalized;
    if (adapter_)
        finalized = adapter_->Finalize();
    else
        finalized.ok = true;
    adapter_.reset();
    pending_definition_.reset();
    state_replacement_prepared_ = false;

    result.capture_complete = finalized.capture_complete;
    result.artifacts_finalized = finalized.ok;
    result.capture_drop_count = finalized.capture_drop_count;
    result.progress_drop_count = finalized.progress_drop_count;
    result.incomplete_reason =
        std::move(finalized.incomplete_reason);
    result.requires_session_taint =
        tainted_ ||
        release_taint ||
        (release_failed && continue_after_release_failure) ||
        !finalized.ok;
    if (router_)
    {
        result.dispatch_generation = router_->dispatch_generation();
        result.physical_generation = router_->physical_generation();
    }

    if (release_failed)
    {
        result.error = {
            CaptureServiceErrorCode::RouterReconcileFailed,
            release_error.empty()
                ? "Capture router group release failed"
                : std::move(release_error),
        };
    }
    else if (!finalized.ok)
    {
        result.error = {
            CaptureServiceErrorCode::FinalizationFailed,
            finalized.message.empty()
                ? "Capture artifact finalization failed"
                : std::move(finalized.message),
        };
    }
    else if (tainted_)
    {
        result.error = {
            CaptureServiceErrorCode::FinalizationFailed,
            "A prior capture cleanup failure left session integrity unproven",
        };
    }
    else
    {
        result.ok = true;
    }
    attachment_ = {};
    if (result.requires_session_taint)
        tainted_ = true;
    return result;
}

CaptureServiceReceipt CaptureService::Shutdown() noexcept
{
    if (owner_thread_ != std::this_thread::get_id())
    {
        return Failure(
            CaptureServiceErrorCode::WrongThread,
            "CaptureService shutdown used the wrong actor thread",
            true);
    }
    if (shutdown_receipt_.has_value())
        return *shutdown_receipt_;
    stopping_ = true;
    CaptureServiceReceipt result =
        FinalizeAndReset(
            CaptureAttachmentStatus::Finalized,
            true);
    if (!result.error)
        result.ok = true;
    shutdown_receipt_ = result;
    return *shutdown_receipt_;
}

CaptureServiceSnapshot CaptureService::snapshot() const noexcept
{
    return {
        .bound = router_ != nullptr,
        .attached = adapter_ != nullptr,
        .state_replacement_prepared =
            state_replacement_prepared_,
        .stopping = stopping_,
        .attachment = attachment_,
        .epoch = epoch_,
        .router_group_active = group_.active(),
        .reconcile_count = reconcile_count_,
    };
}

StopCpuObservationResult CaptureService::ObserveRoutedHit(
    std::uint32_t descriptor_id,
    const RoutedStopEvent& event) noexcept
{
    ICaptureProfileAdapter* adapter =
        cpu_adapter_.load(std::memory_order_acquire);
    if (!adapter)
        return StopCpuObservationResult::Ignored;
    return adapter->ObserveRoutedHit(descriptor_id, event);
}

void CaptureService::OnStopPoint(
    const StopDelivery& delivery)
{
    if (adapter_)
        adapter_->OnStopPoint(delivery);
}

void CaptureService::RouteDeliveriesThroughStableEndpoint(
    StopSubscriptionGroupDefinition& definition) noexcept
{
    for (StopSubscriptionDefinition& subscription :
        definition.subscriptions)
    {
        subscription.consumer = this;
    }
}

} // namespace savor::runtime

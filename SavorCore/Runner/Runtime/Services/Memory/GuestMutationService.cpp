#include "GuestMutationService.h"

#include <algorithm>
#include <exception>
#include <limits>
#include <utility>

namespace savor::runtime {

GuestMutationService::GuestMutationService(
    GuestMemory& memory,
    IGuestMemoryBackendPort& backend)
    : memory_(memory),
      backend_(backend),
      owner_thread_(std::this_thread::get_id())
{
}

void GuestMutationService::InitializeWorksetEpoch(WorksetEpoch epoch) noexcept
{
    if (!OnOwnerThread() || stopped_)
        return;
    epoch_ = epoch;
    memory_.InitializeWorksetEpoch(epoch);
}

GuestMutationReceipt GuestMutationService::Apply(
    const GuestMutationRequest& request)
{
    if (!OnOwnerThread())
        return Failure(request, "GuestMutationService operation used the wrong actor thread");
    if (stopped_)
        return Failure(request, "GuestMutationService is stopped");
    const std::uint64_t width_mask = WidthMask(request.width);
    if (!request.owner || !request.scope || !request.epoch ||
        request.epoch != epoch_)
    {
        return Failure(request, "mutation owner, scope, and current epoch are required");
    }
    if (!backend_.IsPaused())
        return Failure(request, "guest mutation requires a paused core");
    const std::size_t size = static_cast<std::size_t>(request.width);
    if (size != 1 && size != 2 && size != 4)
        return Failure(request, "only u8, u16, and u32 mutations are supported");
    if (request.kind == GuestMutationKind::ExecutablePatch)
    {
        if (request.address % 4 != 0 || size != 4)
            return Failure(request, "executable patches require an aligned u32 target");
        if ((request.mask & width_mask) != width_mask)
            return Failure(request, "executable patches require an exact precondition");
        if (request.lifetime != GuestMutationLifetime::RestoreOnScopeExit)
            return Failure(request, "executable patches are always reversible");
    }

    for (const auto& [_, active] : mutations_)
    {
        if (!active.restoration_required ||
            !Overlaps(active, request.address, size))
        {
            continue;
        }
        const bool nested =
            request.parent &&
            *request.parent == active.receipt.mutation &&
            request.owner == active.receipt.owner;
        if (!nested)
            return Failure(request, "mutation overlaps an unrelated active mutation");
    }

    GuestReadReceipt before =
        memory_.ReadScalar(request.address, request.width, request.epoch);
    if (!before.ok)
        return Failure(request, std::move(before.message));
    const std::uint64_t mask = request.mask & width_mask;
    if ((before.value & mask) != (request.expected & mask))
        return Failure(request, "mutation precondition did not match");
    const std::uint64_t replacement =
        ((before.value & ~mask) | (request.replacement & mask)) & width_mask;

    BackendResult write = WriteValue(request.address, request.width, replacement);
    if (!write.ok)
        return Failure(request, std::move(write.message), write.integrity == BackendIntegrity::Unknown);

    const GuestMutationId id(next_mutation_++);
    GuestMutationReceipt receipt{
        true,
        GuestMutationStatus::Active,
        id,
        request.owner,
        request.scope,
        request.epoch,
        request.address,
        request.width,
        before.value,
        replacement,
        mask,
        false,
        false,
        {}};
    auto [stored, inserted] = mutations_.emplace(
        id.value(),
        MutationState{
            receipt,
            request.kind,
            request.lifetime,
            request.parent,
            next_acquisition_sequence_++,
            true});
    if (!inserted)
    {
        return Failure(
            request,
            "mutation identity collision after guest write",
            true);
    }

    const auto compensate =
        [&](std::string message) -> GuestMutationReceipt {
            MutationState& state = stored->second;
            state.receipt.ok = false;
            state.receipt.status = GuestMutationStatus::Failed;
            state.receipt.taint_required = true;
            state.receipt.message = std::move(message);
            GuestMutationReceipt restored =
                Restore(id, request.epoch);
            if (restored.ok)
            {
                restored.ok = false;
                restored.taint_required = false;
                restored.message =
                    "mutation failed after write and was restored";
            }
            return restored;
        };

    if (request.kind == GuestMutationKind::ExecutablePatch)
    {
        BackendResult invalidate =
            backend_.InvalidateExecutableRange(request.address, size);
        if (!invalidate.ok)
        {
            return compensate(
                invalidate.message.empty()
                    ? "executable patch invalidation failed after write"
                    : std::move(invalidate.message));
        }
        stored->second.receipt.cache_invalidated = true;
    }

    GuestReadReceipt after =
        memory_.ReadScalar(request.address, request.width, request.epoch);
    if (!after.ok || after.value != replacement)
    {
        return compensate(
            after.message.empty()
                ? "mutation readback did not match"
                : std::move(after.message));
    }

    stored->second.receipt.ok = true;
    stored->second.receipt.status = GuestMutationStatus::Active;
    stored->second.receipt.taint_required = false;
    stored->second.receipt.message.clear();
    return stored->second.receipt;
}

GuestMutationReceipt GuestMutationService::Restore(
    GuestMutationId mutation,
    WorksetEpoch epoch)
{
    if (!OnOwnerThread())
    {
        GuestMutationReceipt result;
        result.mutation = mutation;
        result.epoch = epoch;
        result.message =
            "GuestMutationService operation used the wrong actor thread";
        return result;
    }
    if (stopped_)
    {
        GuestMutationReceipt result =
            ServiceFailure("GuestMutationService is stopped");
        result.mutation = mutation;
        result.epoch = epoch;
        return result;
    }
    const auto found = mutations_.find(mutation.value());
    if (found == mutations_.end())
    {
        return {
            true,
            GuestMutationStatus::Restored,
            mutation,
            {},
            {},
            epoch,
            0,
            GuestScalarWidth::U8,
            0,
            0,
            0,
            false,
            false,
            {}};
    }
    MutationState& state = found->second;
    if (!state.restoration_required)
        return state.receipt;
    if (state.receipt.epoch != epoch || epoch_ != epoch)
    {
        state.receipt.ok = false;
        state.receipt.message = "mutation belongs to a stale epoch";
        return state.receipt;
    }
    if (HasActiveChild(mutation))
    {
        state.receipt.ok = false;
        state.receipt.message = "nested mutations must restore in reverse order";
        return state.receipt;
    }

    GuestReadReceipt current =
        memory_.ReadScalar(state.receipt.address, state.receipt.width, epoch);
    if (!current.ok)
    {
        state.receipt.ok = false;
        state.receipt.status = GuestMutationStatus::Failed;
        state.receipt.taint_required = true;
        state.receipt.message = std::move(current.message);
        return state.receipt;
    }

    const bool replacement_present =
        (current.value & state.receipt.mask) ==
        (state.receipt.replacement & state.receipt.mask);
    const bool original_present =
        current.value == state.receipt.original;
    if (!replacement_present && !original_present)
    {
        state.receipt.ok = false;
        state.receipt.status = GuestMutationStatus::Failed;
        state.receipt.taint_required = true;
        state.receipt.message =
            "mutation restoration precondition did not match";
        return state.receipt;
    }
    if (!original_present)
    {
        BackendResult write = WriteValue(
            state.receipt.address,
            state.receipt.width,
            state.receipt.original);
        if (!write.ok)
        {
            state.receipt.ok = false;
            state.receipt.status = GuestMutationStatus::Failed;
            state.receipt.taint_required = true;
            state.receipt.message = std::move(write.message);
            return state.receipt;
        }
    }
    if (state.kind == GuestMutationKind::ExecutablePatch)
    {
        BackendResult invalidate = backend_.InvalidateExecutableRange(
            state.receipt.address,
            static_cast<std::size_t>(state.receipt.width));
        if (!invalidate.ok)
        {
            state.receipt.ok = false;
            state.receipt.status = GuestMutationStatus::Failed;
            state.receipt.taint_required = true;
            state.receipt.message = std::move(invalidate.message);
            return state.receipt;
        }
        state.receipt.cache_invalidated = true;
    }
    GuestReadReceipt after =
        memory_.ReadScalar(state.receipt.address, state.receipt.width, epoch);
    if (!after.ok || after.value != state.receipt.original)
    {
        state.receipt.ok = false;
        state.receipt.status = GuestMutationStatus::Failed;
        state.receipt.taint_required = true;
        state.receipt.message = after.message.empty()
            ? "mutation restoration readback did not match"
            : std::move(after.message);
        return state.receipt;
    }
    state.receipt.ok = true;
    state.receipt.status = GuestMutationStatus::Restored;
    state.receipt.taint_required = false;
    state.receipt.message.clear();
    state.restoration_required = false;
    return state.receipt;
}

GuestMutationReceipt GuestMutationService::Commit(
    GuestMutationId mutation,
    WorksetEpoch epoch)
{
    if (!OnOwnerThread())
    {
        GuestMutationReceipt result;
        result.mutation = mutation;
        result.epoch = epoch;
        result.message =
            "GuestMutationService operation used the wrong actor thread";
        return result;
    }
    if (stopped_)
    {
        GuestMutationReceipt result =
            ServiceFailure("GuestMutationService is stopped");
        result.mutation = mutation;
        result.epoch = epoch;
        return result;
    }
    const auto found = mutations_.find(mutation.value());
    if (found == mutations_.end())
        return {false, GuestMutationStatus::Rejected, mutation, {}, {}, epoch, 0};
    MutationState& state = found->second;
    if (state.receipt.epoch != epoch || epoch != epoch_)
    {
        state.receipt.ok = false;
        state.receipt.message = "mutation belongs to a stale epoch";
        return state.receipt;
    }
    if (state.kind == GuestMutationKind::ExecutablePatch)
    {
        state.receipt.ok = false;
        state.receipt.message = "executable patches cannot be committed";
        return state.receipt;
    }
    if (HasActiveChild(mutation))
    {
        state.receipt.ok = false;
        state.receipt.message = "nested mutations must close in reverse order";
        return state.receipt;
    }
    state.receipt.ok = true;
    state.receipt.status = GuestMutationStatus::Committed;
    state.receipt.message.clear();
    state.restoration_required = false;
    return state.receipt;
}


std::vector<GuestMutationReceipt> GuestMutationService::RestoreScope(
    MutationScopeId scope,
    WorksetEpoch epoch)
{
    if (!OnOwnerThread())
    {
        GuestMutationReceipt failure;
        failure.scope = scope;
        failure.epoch = epoch;
        failure.message =
            "GuestMutationService operation used the wrong actor thread";
        return {std::move(failure)};
    }
    if (stopped_)
    {
        GuestMutationReceipt failure =
            ServiceFailure("GuestMutationService is stopped");
        failure.scope = scope;
        failure.epoch = epoch;
        return {std::move(failure)};
    }
    std::vector<std::pair<std::size_t, GuestMutationId>> ordered;
    for (const auto& [_, state] : mutations_)
    {
        if (state.receipt.scope == scope &&
            state.restoration_required)
        {
            ordered.emplace_back(
                state.acquisition_sequence,
                state.receipt.mutation);
        }
    }
    std::ranges::sort(
        ordered,
        [](const auto& lhs, const auto& rhs) {
            return lhs.first > rhs.first;
        });
    std::vector<GuestMutationReceipt> receipts;
    receipts.reserve(ordered.size());
    for (const auto& [_, id] : ordered)
        receipts.push_back(Restore(id, epoch));
    return receipts;
}

GuestMutationCleanupReceipt GuestMutationService::RestoreAll()
{
    if (!OnOwnerThread())
    {
        return {
            false,
            true,
            false,
            {},
            "GuestMutationService cleanup used the wrong actor thread"};
    }
    if (stopped_)
    {
        if (shutdown_result_.has_value())
        {
            GuestMutationCleanupReceipt result = *shutdown_result_;
            result.already_shutdown = true;
            return result;
        }
        return {
            false,
            true,
            false,
            {},
            "GuestMutationService is stopped without a cleanup receipt"};
    }

    GuestMutationCleanupReceipt result;
    std::vector<std::pair<std::size_t, GuestMutationId>> ordered;
    try
    {
        ordered.reserve(mutations_.size());
        for (const auto& [_, state] : mutations_)
        {
            if (state.restoration_required)
            {
                ordered.emplace_back(
                    state.acquisition_sequence,
                    state.receipt.mutation);
            }
        }
        std::ranges::sort(
            ordered,
            [](const auto& lhs, const auto& rhs) {
                return lhs.first > rhs.first;
            });
        result.restorations.reserve(ordered.size());
    }
    catch (const std::exception& ex)
    {
        result.taint_required = true;
        result.message =
            std::string("Guest mutation cleanup preparation failed: ") +
            ex.what();
        return result;
    }
    catch (...)
    {
        result.taint_required = true;
        result.message =
            "Guest mutation cleanup preparation failed";
        return result;
    }

    for (const auto& [_, mutation] : ordered)
    {
        GuestMutationReceipt receipt;
        if (const auto state = mutations_.find(mutation.value());
            state != mutations_.end())
        {
            receipt = state->second.receipt;
        }
        try
        {
            receipt = Restore(mutation, epoch_);
        }
        catch (const std::exception& ex)
        {
            receipt.ok = false;
            receipt.status = GuestMutationStatus::Failed;
            receipt.taint_required = true;
            receipt.message =
                std::string("Guest mutation restoration threw: ") +
                ex.what();
        }
        catch (...)
        {
            receipt.ok = false;
            receipt.status = GuestMutationStatus::Failed;
            receipt.taint_required = true;
            receipt.message = "Guest mutation restoration threw";
        }
        if (!receipt.ok)
        {
            receipt.taint_required = true;
            const auto state = mutations_.find(mutation.value());
            if (state != mutations_.end() &&
                state->second.restoration_required)
            {
                state->second.receipt.ok = false;
                state->second.receipt.status =
                    GuestMutationStatus::Failed;
                state->second.receipt.taint_required = true;
                state->second.receipt.message = receipt.message;
            }
            result.taint_required = true;
            if (result.message.empty())
            {
                result.message = receipt.message.empty()
                    ? "Guest mutation restoration failed"
                    : receipt.message;
            }
        }
        result.restorations.push_back(std::move(receipt));
    }
    result.ok = !result.taint_required;
    return result;
}

GuestMutationCleanupReceipt GuestMutationService::Shutdown() noexcept
{
    if (!OnOwnerThread())
    {
        return {
            false,
            true,
            false,
            {},
            "GuestMutationService shutdown used the wrong actor thread"};
    }
    if (shutdown_result_.has_value())
    {
        GuestMutationCleanupReceipt result = *shutdown_result_;
        result.already_shutdown = true;
        return result;
    }

    GuestMutationCleanupReceipt result;
    try
    {
        result = RestoreAll();
    }
    catch (const std::exception& ex)
    {
        result.taint_required = true;
        result.message =
            std::string("GuestMutationService shutdown failed: ") +
            ex.what();
    }
    catch (...)
    {
        result.taint_required = true;
        result.message = "GuestMutationService shutdown failed";
    }
    result.ok = result.ok && !result.taint_required;
    stopped_ = true;
    shutdown_result_ = result;
    return result;
}

GuestMutationReceipt GuestMutationService::Failure(
    const GuestMutationRequest& request,
    std::string message,
    bool taint) const
{
    return {
        false,
        GuestMutationStatus::Rejected,
        {},
        request.owner,
        request.scope,
        request.epoch,
        request.address,
        request.width,
        0,
        request.replacement,
        request.mask,
        false,
        taint,
        std::move(message)};
}

GuestMutationReceipt GuestMutationService::ServiceFailure(
    std::string message,
    bool taint) const
{
    GuestMutationReceipt result;
    result.status = GuestMutationStatus::Rejected;
    result.epoch = epoch_;
    result.taint_required = taint;
    result.message = std::move(message);
    return result;
}

bool GuestMutationService::OnOwnerThread() const noexcept
{
    return owner_thread_ == std::this_thread::get_id();
}

bool GuestMutationService::Overlaps(
    const MutationState& state,
    std::uint32_t address,
    std::size_t size) const noexcept
{
    const std::uint64_t left_begin = state.receipt.address;
    const std::uint64_t left_end =
        left_begin + static_cast<std::size_t>(state.receipt.width);
    const std::uint64_t right_begin = address;
    const std::uint64_t right_end = right_begin + size;
    return left_begin < right_end && right_begin < left_end;
}

bool GuestMutationService::HasActiveChild(
    GuestMutationId mutation) const noexcept
{
    return std::ranges::any_of(mutations_, [&](const auto& item) {
        return item.second.restoration_required &&
            item.second.parent == mutation;
    });
}

BackendResult GuestMutationService::WriteValue(
    std::uint32_t address,
    GuestScalarWidth width,
    std::uint64_t value)
{
    const std::size_t size = static_cast<std::size_t>(width);
    std::vector<std::uint8_t> bytes(size);
    for (std::size_t index = 0; index < size; ++index)
    {
        const std::size_t shift = (size - index - 1) * 8;
        bytes[index] = static_cast<std::uint8_t>((value >> shift) & 0xff);
    }
    return backend_.Write(address, bytes);
}

std::uint64_t GuestMutationService::WidthMask(
    GuestScalarWidth width) noexcept
{
    switch (width)
    {
    case GuestScalarWidth::U8:
        return 0xffu;
    case GuestScalarWidth::U16:
        return 0xffffu;
    case GuestScalarWidth::U32:
        return 0xffffffffu;
    case GuestScalarWidth::U64:
        return std::numeric_limits<std::uint64_t>::max();
    }
    return 0;
}

} // namespace savor::runtime

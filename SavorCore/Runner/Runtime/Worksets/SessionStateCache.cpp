#include "SessionStateCache.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace savor::runtime {

SessionStateCache::Lease::Lease(
    SessionStateCache* cache,
    StateCacheLeaseId lease_id,
    std::string cache_hash,
    StateCacheKey key,
    StateHandleReceipt receipt)
    : cache_(cache),
      lease_id_(lease_id),
      cache_hash_(std::move(cache_hash)),
      key_(std::move(key)),
      handle_(receipt.handle),
      receipt_(std::move(receipt))
{
}

SessionStateCache::Lease::~Lease()
{
    Reset();
}

SessionStateCache::Lease::Lease(Lease&& other) noexcept
    : cache_(std::exchange(other.cache_, nullptr)),
      lease_id_(std::exchange(other.lease_id_, {})),
      cache_hash_(std::move(other.cache_hash_)),
      key_(std::move(other.key_)),
      handle_(std::exchange(other.handle_, {})),
      receipt_(std::move(other.receipt_))
{
}

SessionStateCache::Lease& SessionStateCache::Lease::operator=(
    Lease&& other) noexcept
{
    if (this == &other)
        return *this;
    Reset();
    cache_ = std::exchange(other.cache_, nullptr);
    lease_id_ = std::exchange(other.lease_id_, {});
    cache_hash_ = std::move(other.cache_hash_);
    key_ = std::move(other.key_);
    handle_ = std::exchange(other.handle_, {});
    receipt_ = std::move(other.receipt_);
    return *this;
}

void SessionStateCache::Lease::Reset() noexcept
{
    if (cache_ && lease_id_)
        cache_->ReleaseLease(lease_id_, cache_hash_);
    cache_ = nullptr;
    lease_id_ = {};
    cache_hash_.clear();
    handle_ = {};
}

SessionStateCache::SessionStateCache(
    StateService& states,
    std::size_t maximum_entries,
    std::size_t maximum_bytes)
    : states_(states),
      maximum_entries_(maximum_entries),
      maximum_bytes_(maximum_bytes)
{
}

SessionStateCache::~SessionStateCache()
{
    (void)InvalidateAll();
}

std::optional<SessionStateCache::Lease> SessionStateCache::Acquire(
    const StateCacheKey& key)
{
    const std::string cache_hash = ComputeStateCacheKeyHash(key);
    auto found = entries_.find(cache_hash);
    if (found == entries_.end() || found->second.key != key ||
        next_lease_id_ == 0)
    {
        return std::nullopt;
    }
    Entry& entry = found->second;
    ++entry.lease_count;
    entry.last_use = lru_clock_++;
    const StateCacheLeaseId lease_id(next_lease_id_++);
    return Lease(
        this,
        lease_id,
        cache_hash,
        entry.key,
        entry.receipt);
}

std::optional<SessionStateCache::Lease>
SessionStateCache::CaptureAndAcquire(
    const StateCacheKey& key,
    const StateHandleCaptureRequest& request,
    StateServiceResult* error_out)
{
    if (auto existing = Acquire(key))
        return existing;

    StateHandleReceipt captured = states_.CaptureMemoryHandle(request);
    if (!captured.result.ok)
    {
        if (error_out)
            *error_out = captured.result;
        return std::nullopt;
    }
    return InsertAndAcquire(key, std::move(captured), error_out);
}

std::optional<SessionStateCache::Lease>
SessionStateCache::InsertAndAcquire(
    const StateCacheKey& key,
    StateHandleReceipt captured,
    StateServiceResult* error_out)
{
    if (!captured.result.ok || !captured.handle)
    {
        if (error_out)
        {
            *error_out = captured.result.ok
                ? StateServiceResult::Failure(
                      StateServiceErrorCode::InvalidArgument,
                      "State-cache insertion requires a captured handle")
                : captured.result;
        }
        return std::nullopt;
    }

    if (auto existing = Acquire(key))
    {
        const StateServiceResult released =
            states_.ReleaseMemoryHandle(captured.handle);
        if (!released.ok)
        {
            existing->Reset();
            if (error_out)
                *error_out = released;
            return std::nullopt;
        }
        return existing;
    }
    if (!EnsureCapacity(captured.size_bytes, error_out))
    {
        const StateServiceResult released =
            states_.ReleaseMemoryHandle(captured.handle);
        if (!released.ok && error_out)
            *error_out = released;
        return std::nullopt;
    }

    const std::string cache_hash = ComputeStateCacheKeyHash(key);
    Entry entry;
    entry.key = key;
    entry.receipt = captured;
    entry.lease_count = 0;
    entry.last_use = lru_clock_++;
    bytes_in_use_ += captured.size_bytes;
    entries_.emplace(cache_hash, std::move(entry));
    return Acquire(key);
}

StateOperationReceipt SessionStateCache::Restore(const Lease& lease)
{
    if (!lease || lease.cache_ != this)
    {
        StateOperationReceipt receipt;
        receipt.operation = StateReplacementKind::RestoreMemoryHandle;
        receipt.result = StateServiceResult::Failure(
            StateServiceErrorCode::InvalidArgument,
            "State-cache restore requires an active lease");
        return receipt;
    }
    return states_.RestoreMemoryHandle(lease.handle_);
}

StateServiceResult SessionStateCache::Remove(const StateCacheKey& key)
{
    const std::string cache_hash = ComputeStateCacheKeyHash(key);
    const auto found = entries_.find(cache_hash);
    if (found == entries_.end() || found->second.key != key)
    {
        return StateServiceResult::Failure(
            StateServiceErrorCode::NotFound,
            "State-cache entry was not found");
    }
    if (found->second.lease_count != 0)
    {
        return StateServiceResult::Failure(
            StateServiceErrorCode::InvalidState,
            "State-cache entry is still leased");
    }
    const StateServiceResult released =
        states_.ReleaseMemoryHandle(found->second.receipt.handle);
    if (!released.ok)
        return released;
    bytes_in_use_ -= std::min(
        bytes_in_use_,
        found->second.receipt.size_bytes);
    entries_.erase(found);
    return StateServiceResult::Success();
}

StateServiceResult SessionStateCache::InvalidateAll() noexcept
{
    for (const auto& [_, entry] : entries_)
    {
        if (entry.lease_count != 0)
        {
            return StateServiceResult::Failure(
                StateServiceErrorCode::InvalidState,
                "State cache cannot invalidate leased entries");
        }
    }

    StateServiceResult result = StateServiceResult::Success();
    for (auto current = entries_.begin(); current != entries_.end();)
    {
        StateServiceResult released =
            states_.ReleaseMemoryHandle(
                current->second.receipt.handle);
        if (!released.ok && result.ok)
        {
            result = std::move(released);
            ++current;
            continue;
        }
        if (!released.ok)
        {
            ++current;
            continue;
        }
        bytes_in_use_ -= std::min(
            bytes_in_use_,
            current->second.receipt.size_bytes);
        current = entries_.erase(current);
    }
    return result;
}

SessionStateCacheSnapshot SessionStateCache::snapshot() const noexcept
{
    SessionStateCacheSnapshot result;
    result.entry_count = entries_.size();
    result.bytes_in_use = bytes_in_use_;
    for (const auto& [_, entry] : entries_)
    {
        if (entry.lease_count != 0)
            ++result.leased_entries;
    }
    return result;
}

void SessionStateCache::ReleaseLease(
    StateCacheLeaseId lease_id,
    const std::string& cache_hash) noexcept
{
    if (!lease_id)
        return;
    auto found = entries_.find(cache_hash);
    if (found == entries_.end() || found->second.lease_count == 0)
        return;
    --found->second.lease_count;
    found->second.last_use = lru_clock_++;
}

bool SessionStateCache::EnsureCapacity(
    std::size_t new_entry_bytes,
    StateServiceResult* error_out)
{
    if (maximum_entries_ == 0 || new_entry_bytes > maximum_bytes_)
    {
        if (error_out)
        {
            *error_out = StateServiceResult::Failure(
                StateServiceErrorCode::CapacityExceeded,
                "State-cache entry exceeds configured limits");
        }
        return false;
    }
    while (entries_.size() >= maximum_entries_ ||
           new_entry_bytes >
               maximum_bytes_ -
                   std::min(bytes_in_use_, maximum_bytes_))
    {
        StateServiceResult eviction_error =
            StateServiceResult::Success();
        if (!EvictOne(&eviction_error))
        {
            if (error_out)
            {
                *error_out = eviction_error.ok
                    ? StateServiceResult::Failure(
                          StateServiceErrorCode::CapacityExceeded,
                          "State-cache capacity is retained by active leases")
                    : std::move(eviction_error);
            }
            return false;
        }
    }
    return true;
}

bool SessionStateCache::EvictOne(StateServiceResult* error_out)
{
    auto victim = entries_.end();
    for (auto it = entries_.begin(); it != entries_.end(); ++it)
    {
        if (it->second.lease_count != 0)
            continue;
        if (victim == entries_.end() ||
            it->second.last_use < victim->second.last_use)
        {
            victim = it;
        }
    }
    if (victim == entries_.end())
        return false;

    const StateServiceResult released =
        states_.ReleaseMemoryHandle(victim->second.receipt.handle);
    if (!released.ok)
    {
        if (error_out)
            *error_out = released;
        return false;
    }
    bytes_in_use_ -= std::min(
        bytes_in_use_,
        victim->second.receipt.size_bytes);
    entries_.erase(victim);
    return true;
}

} // namespace savor::runtime

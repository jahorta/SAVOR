#pragma once

#include "WorksetTypes.h"

#include "../Services/State/StateService.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>

namespace savor::runtime {

struct SessionStateCacheSnapshot
{
    std::size_t entry_count = 0;
    std::size_t bytes_in_use = 0;
    std::size_t leased_entries = 0;
};

class SessionStateCache final
{
public:
    class Lease final
    {
    public:
        Lease() = default;
        ~Lease();

        Lease(const Lease&) = delete;
        Lease& operator=(const Lease&) = delete;

        Lease(Lease&& other) noexcept;
        Lease& operator=(Lease&& other) noexcept;

        [[nodiscard]] explicit operator bool() const noexcept
        {
            return cache_ != nullptr && lease_id_;
        }
        [[nodiscard]] StateCacheLeaseId id() const noexcept
        {
            return lease_id_;
        }
        [[nodiscard]] const StateCacheKey& key() const noexcept
        {
            return key_;
        }
        [[nodiscard]] StateHandleId handle() const noexcept
        {
            return handle_;
        }
        [[nodiscard]] const StateHandleReceipt& receipt() const noexcept
        {
            return receipt_;
        }

        void Reset() noexcept;

    private:
        friend class SessionStateCache;
        Lease(
            SessionStateCache* cache,
            StateCacheLeaseId lease_id,
            std::string cache_hash,
            StateCacheKey key,
            StateHandleReceipt receipt);

        SessionStateCache* cache_ = nullptr;
        StateCacheLeaseId lease_id_;
        std::string cache_hash_;
        StateCacheKey key_;
        StateHandleId handle_;
        StateHandleReceipt receipt_;
    };

    SessionStateCache(
        StateService& states,
        std::size_t maximum_entries,
        std::size_t maximum_bytes);
    ~SessionStateCache();

    SessionStateCache(const SessionStateCache&) = delete;
    SessionStateCache& operator=(const SessionStateCache&) = delete;

    [[nodiscard]] std::optional<Lease> Acquire(
        const StateCacheKey& key);
    [[nodiscard]] std::optional<Lease> CaptureAndAcquire(
        const StateCacheKey& key,
        const StateHandleCaptureRequest& request,
        StateServiceResult* error_out = nullptr);
    [[nodiscard]] std::optional<Lease> InsertAndAcquire(
        const StateCacheKey& key,
        StateHandleReceipt captured,
        StateServiceResult* error_out = nullptr);
    [[nodiscard]] StateOperationReceipt Restore(const Lease& lease);
    [[nodiscard]] StateServiceResult Remove(const StateCacheKey& key);

    [[nodiscard]] StateServiceResult InvalidateAll() noexcept;
    [[nodiscard]] SessionStateCacheSnapshot snapshot() const noexcept;

private:
    struct Entry
    {
        StateCacheKey key;
        StateHandleReceipt receipt;
        std::size_t lease_count = 0;
        std::uint64_t last_use = 0;
    };

    void ReleaseLease(
        StateCacheLeaseId lease_id,
        const std::string& cache_hash) noexcept;
    [[nodiscard]] bool EnsureCapacity(
        std::size_t new_entry_bytes,
        StateServiceResult* error_out);
    [[nodiscard]] bool EvictOne(StateServiceResult* error_out);

    StateService& states_;
    std::size_t maximum_entries_ = 0;
    std::size_t maximum_bytes_ = 0;
    std::size_t bytes_in_use_ = 0;
    std::uint64_t next_lease_id_ = 1;
    std::uint64_t lru_clock_ = 1;
    std::unordered_map<std::string, Entry> entries_;
};

} // namespace savor::runtime

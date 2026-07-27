#pragma once

#include "GuestMemory.h"

#include <cstdint>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace savor::runtime {

struct GuestMutationIdTag;
struct MutationOwnerIdTag;
struct MutationScopeIdTag;

using GuestMutationId = StrongId<GuestMutationIdTag>;
using MutationOwnerId = StrongId<MutationOwnerIdTag>;
using MutationScopeId = StrongId<MutationScopeIdTag>;

enum class GuestMutationKind : std::uint8_t
{
    Data,
    ExecutablePatch,
};

enum class GuestMutationLifetime : std::uint8_t
{
    RestoreOnScopeExit,
    CommitToCurrentState,
};

enum class GuestMutationStatus : std::uint8_t
{
    Rejected,
    Active,
    Restored,
    Committed,
    SupersededByStateReplacement,
    Failed,
};

struct GuestMutationRequest
{
    MutationOwnerId owner;
    MutationScopeId scope;
    std::optional<GuestMutationId> parent;
    StateEpoch epoch;
    std::uint32_t address = 0;
    GuestScalarWidth width = GuestScalarWidth::U32;
    std::uint64_t expected = 0;
    std::uint64_t replacement = 0;
    std::uint64_t mask = ~std::uint64_t{0};
    GuestMutationKind kind = GuestMutationKind::Data;
    GuestMutationLifetime lifetime =
        GuestMutationLifetime::RestoreOnScopeExit;
};

struct GuestMutationReceipt
{
    bool ok = false;
    GuestMutationStatus status = GuestMutationStatus::Rejected;
    GuestMutationId mutation;
    MutationOwnerId owner;
    MutationScopeId scope;
    StateEpoch epoch;
    std::uint32_t address = 0;
    GuestScalarWidth width = GuestScalarWidth::U32;
    std::uint64_t original = 0;
    std::uint64_t replacement = 0;
    std::uint64_t mask = 0;
    bool cache_invalidated = false;
    bool taint_required = false;
    std::string message;
};

struct GuestMutationCleanupReceipt
{
    bool ok = false;
    bool taint_required = false;
    bool already_shutdown = false;
    std::vector<GuestMutationReceipt> restorations;
    std::string message;
};

class GuestMutationService final
{
public:
    GuestMutationService(
        GuestMemory& memory,
        IGuestMemoryBackendPort& backend);

    void CommitStateEpoch(StateEpoch epoch) noexcept;
    [[nodiscard]] GuestMutationReceipt Apply(
        const GuestMutationRequest& request);
    [[nodiscard]] GuestMutationReceipt Restore(
        GuestMutationId mutation,
        StateEpoch epoch);
    [[nodiscard]] GuestMutationReceipt Commit(
        GuestMutationId mutation,
        StateEpoch epoch);
    [[nodiscard]] std::vector<GuestMutationReceipt>
    SupersedeForStateReplacement(StateEpoch old_epoch);
    [[nodiscard]] std::vector<GuestMutationReceipt> RestoreScope(
        MutationScopeId scope,
        StateEpoch epoch);
    [[nodiscard]] GuestMutationCleanupReceipt RestoreAll();
    [[nodiscard]] GuestMutationCleanupReceipt Shutdown() noexcept;

private:
    struct MutationState
    {
        GuestMutationReceipt receipt;
        GuestMutationKind kind = GuestMutationKind::Data;
        GuestMutationLifetime lifetime =
            GuestMutationLifetime::RestoreOnScopeExit;
        std::optional<GuestMutationId> parent;
        std::size_t acquisition_sequence = 0;
        bool restoration_required = false;
    };

    [[nodiscard]] GuestMutationReceipt Failure(
        const GuestMutationRequest& request,
        std::string message,
        bool taint = false) const;
    [[nodiscard]] bool Overlaps(
        const MutationState& state,
        std::uint32_t address,
        std::size_t size) const noexcept;
    [[nodiscard]] bool HasActiveChild(GuestMutationId mutation) const noexcept;
    [[nodiscard]] BackendResult WriteValue(
        std::uint32_t address,
        GuestScalarWidth width,
        std::uint64_t value);
    [[nodiscard]] GuestMutationReceipt ServiceFailure(
        std::string message,
        bool taint = false) const;
    [[nodiscard]] bool OnOwnerThread() const noexcept;
    [[nodiscard]] static std::uint64_t WidthMask(
        GuestScalarWidth width) noexcept;

    GuestMemory& memory_;
    IGuestMemoryBackendPort& backend_;
    std::thread::id owner_thread_;
    StateEpoch epoch_;
    std::uint64_t next_mutation_ = 1;
    std::size_t next_acquisition_sequence_ = 1;
    std::unordered_map<std::uint64_t, MutationState> mutations_;
    bool stopped_ = false;
    std::optional<GuestMutationCleanupReceipt> shutdown_result_;
};

} // namespace savor::runtime

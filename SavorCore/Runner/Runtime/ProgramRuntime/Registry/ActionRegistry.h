#pragma once

#include "RegistryResult.h"
#include "TypeSchemaRegistry.h"
#include "Runner/Runtime/ProgramRuntime/Model/ProgramTypes.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace savor::runtime::program {

enum class SessionServiceCapability : std::uint32_t
{
    None = 0,
    Savestate = 1u << 0,
    Execution = 1u << 1,
    StopPoints = 1u << 2,
    Input = 1u << 3,
    Movie = 1u << 4,
    GuestMemory = 1u << 5,
    GuestMutation = 1u << 6,
    Capture = 1u << 7,
    Telemetry = 1u << 9,
    Artifact = 1u << 10,
    DerivedState = 1u << 11,
};

using SessionServiceCapabilityMask = std::uint32_t;

enum class ActionEffect : std::uint32_t
{
    None = 0,
    ReadGuest = 1u << 0,
    MutateGuest = 1u << 1,
    AdvanceEmulation = 1u << 2,
    PublishInput = 1u << 3,
    MoviePlayback = 1u << 5,
    MovieRecording = 1u << 6,
    Movie = (1u << 5) | (1u << 6),
    Capture = 1u << 7,
    ArtifactIo = 1u << 8,
    Telemetry = 1u << 9,
    ReadDerivedState = 1u << 10,
    ReadMovieState = 1u << 11,
};

using ActionEffectMask = std::uint32_t;

enum class ActionEpochPolicy : std::uint8_t
{
    EpochAgnostic,
    RequiresCurrentEpoch,
};

enum class ActionReplayClass : std::uint8_t
{
    Deterministic,
    RecordedEvidence,
    ExternalCommit,
};

enum class ActionCancellationMode : std::uint8_t
{
    BeforeMutationOnly,
    Cooperative,
    CleanupRequired,
};

enum class ActionTimingClass : std::uint8_t
{
    // Guest-dependent work has no elapsed deadline. It ends through semantic
    // completion, cancellation, movie policy, epoch invalidation, or health
    // failure.
    CancellationDriven,
    // Host-only work such as state/file I/O and cleanup retains a finite
    // infrastructure timeout.
    BoundedHostOperation,
};

enum class ActionResourceBehavior : std::uint8_t
{
    None,
    Scoped,
    Promotable,
};

enum class ActionCleanupGuarantee : std::uint8_t
{
    None,
    Automatic,
    VerifiedCompensation,
};

enum class ActionIdempotency : std::uint8_t
{
    NotRetryable,
    NaturallyIdempotent,
    ReceiptProven,
};

struct ActionDescriptor
{
    ExactDependencyIdentity identity;
    CapabilityPackIdentity providing_pack;
    TypeRef input_type;
    TypeRef output_type;
    std::optional<TypeRef> domain_observation_type;
    std::optional<TypeRef> receipt_type;
    std::optional<TypeRef> diagnostic_type;
    std::string required_derived_state_block_id;
    SessionServiceCapabilityMask required_services = 0;
    ActionEffectMask effects = 0;
    ActionEpochPolicy epoch_policy = ActionEpochPolicy::RequiresCurrentEpoch;
    ActionReplayClass replay_class = ActionReplayClass::Deterministic;
    ActionCancellationMode cancellation =
        ActionCancellationMode::Cooperative;
    ActionTimingClass timing = ActionTimingClass::CancellationDriven;
    std::uint64_t maximum_non_cancellable_milliseconds = 0;
    std::uint64_t default_host_timeout_milliseconds = 0;
    ActionResourceBehavior resource_behavior =
        ActionResourceBehavior::None;
    ActionCleanupGuarantee cleanup = ActionCleanupGuarantee::None;
    bool taints_on_unproven_cleanup = false;
    ActionIdempotency idempotency = ActionIdempotency::NotRetryable;
    std::vector<std::string> diagnostic_categories;

    auto operator<=>(const ActionDescriptor&) const = default;
};

struct ReducerDescriptor
{
    ExactDependencyIdentity identity;
    CapabilityPackIdentity providing_pack;
    std::vector<TypeRef> input_types;
    TypeRef output_type;
    std::vector<ExactDependencyIdentity> permitted_actions;
    std::vector<ExactDependencyIdentity> permitted_subprograms;
    std::uint64_t maximum_steps = 0;
    std::uint64_t maximum_value_bytes = 0;

    auto operator<=>(const ReducerDescriptor&) const = default;
};

class ActionRegistry final
{
public:
    explicit ActionRegistry(
        const TypeSchemaRegistry* schemas = nullptr) noexcept
        : schemas_(schemas)
    {
    }

    [[nodiscard]] RegistryResult RegisterAction(
        ActionDescriptor descriptor);
    [[nodiscard]] RegistryResult RegisterReducer(
        ReducerDescriptor descriptor);
    [[nodiscard]] RegistryResult RegisterCatalog(
        std::vector<ActionDescriptor> actions,
        std::vector<ReducerDescriptor> reducers);

    [[nodiscard]] const ActionDescriptor* ResolveAction(
        const ExactDependencyIdentity& identity) const noexcept;
    [[nodiscard]] const ReducerDescriptor* ResolveReducer(
        const ExactDependencyIdentity& identity) const noexcept;

    [[nodiscard]] std::size_t action_count() const noexcept
    {
        return actions_.size();
    }
    [[nodiscard]] std::size_t reducer_count() const noexcept
    {
        return reducers_.size();
    }

private:
    using Key = std::pair<std::string, std::uint32_t>;

    [[nodiscard]] RegistryResult Validate(
        const ActionDescriptor& descriptor) const;
    [[nodiscard]] RegistryResult Validate(
        const ReducerDescriptor& descriptor) const;
    [[nodiscard]] bool IsKnown(const TypeRef& type) const noexcept;

    const TypeSchemaRegistry* schemas_ = nullptr;
    std::map<Key, ActionDescriptor> actions_;
    std::map<Key, ReducerDescriptor> reducers_;
};

// Computes the canonical, provider-hash-independent identity of the complete
// descriptor contract. The provider's canonical id and version participate,
// but its manifest hash is deliberately excluded to avoid a circular
// action-pack identity dependency.
[[nodiscard]] ContentHash256 ComputeActionDescriptorContractHash(
    const ActionDescriptor& descriptor);
[[nodiscard]] ContentHash256 ComputeReducerDescriptorContractHash(
    const ReducerDescriptor& descriptor);

[[nodiscard]] constexpr SessionServiceCapabilityMask ServiceMask(
    SessionServiceCapability capability) noexcept
{
    return static_cast<SessionServiceCapabilityMask>(capability);
}

[[nodiscard]] constexpr ActionEffectMask EffectMask(
    ActionEffect effect) noexcept
{
    return static_cast<ActionEffectMask>(effect);
}

} // namespace savor::runtime::program

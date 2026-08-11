#include <gtest/gtest.h>

#include "Runner/Runtime/Services/Input/InputArbiter.h"
#include "Runner/Runtime/Services/Memory/GuestMutationService.h"
#include "Runner/Runtime/Services/Screenshot/ScreenshotService.h"
#include "Runner/Runtime/Services/Telemetry/TelemetryBus.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;
using namespace savor;
using namespace savor::runtime;

constexpr WorksetEpoch kEpoch{11};

class FakeInputBackend final : public IInputBackendPort
{
public:
    bool IsAvailable(std::uint8_t port) const noexcept override
    {
        ++availability_queries;
        return port == 0;
    }

    BackendInputPublication Publish(
        std::uint8_t port,
        const GCInputFrame& frame) override
    {
        ++publish_calls;
        if (port != 0)
            return {BackendResult::Failure(BackendErrorCode::Unavailable, "port"), 0};
        current_frame = frame;
        current_publication_epoch = ++next_publication_epoch;
        callback_count = 0;
        return {BackendResult::Success(), current_publication_epoch};
    }

    BackendInputPoll QueryPoll(std::uint8_t port) const override
    {
        ++poll_queries;
        if (port != 0)
            return {BackendResult::Failure(BackendErrorCode::Unavailable, "port")};
        return {
            BackendResult::Success(),
            current_publication_epoch,
            callback_count,
            current_frame};
    }

    void Poll(std::uint32_t count = 1)
    {
        callback_count += count;
    }

    std::uint64_t next_publication_epoch = 0;
    std::uint64_t current_publication_epoch = 0;
    std::uint32_t callback_count = 0;
    GCInputFrame current_frame{};
    mutable std::size_t availability_queries = 0;
    std::size_t publish_calls = 0;
    mutable std::size_t poll_queries = 0;
};

class FakeGuestMemoryBackend final : public IGuestMemoryBackendPort
{
public:
    bool IsPaused() const noexcept override
    {
        ++is_paused_calls;
        return paused;
    }

    GuestBytesResult Read(
        std::uint32_t address,
        std::size_t size) const override
    {
        ++read_calls;
        std::vector<std::uint8_t> bytes;
        bytes.reserve(size);
        for (std::size_t index = 0; index < size; ++index)
        {
            const auto found = memory.find(
                address + static_cast<std::uint32_t>(index));
            if (found == memory.end())
            {
                return {
                    BackendResult::Failure(
                        BackendErrorCode::OperationFailed,
                        "unmapped"),
                    {}};
            }
            bytes.push_back(found->second);
        }
        if (corrupt_read_call.has_value() &&
            *corrupt_read_call == read_calls &&
            !bytes.empty())
        {
            bytes.back() ^= 1;
        }
        return {BackendResult::Success(), std::move(bytes)};
    }

    BackendResult Write(
        std::uint32_t address,
        const std::vector<std::uint8_t>& bytes) override
    {
        ++write_calls;
        if (!paused)
            return BackendResult::Failure(BackendErrorCode::InvalidState, "running");
        if (fail_write_address.has_value() &&
            *fail_write_address == address)
        {
            return BackendResult::Failure(
                BackendErrorCode::OperationFailed,
                "injected write failure");
        }
        for (std::size_t index = 0; index < bytes.size(); ++index)
            memory[address + static_cast<std::uint32_t>(index)] = bytes[index];
        return BackendResult::Success();
    }

    BackendResult InvalidateExecutableRange(
        std::uint32_t address,
        std::size_t size) override
    {
        ++invalidate_calls;
        invalidations.emplace_back(address, size);
        return invalidate_result;
    }

    void PutU32(std::uint32_t address, std::uint32_t value)
    {
        memory[address] = static_cast<std::uint8_t>(value >> 24);
        memory[address + 1] = static_cast<std::uint8_t>(value >> 16);
        memory[address + 2] = static_cast<std::uint8_t>(value >> 8);
        memory[address + 3] = static_cast<std::uint8_t>(value);
    }

    bool paused = true;
    mutable std::size_t is_paused_calls = 0;
    mutable std::size_t read_calls = 0;
    std::size_t write_calls = 0;
    std::size_t invalidate_calls = 0;
    std::optional<std::uint32_t> fail_write_address;
    std::optional<std::size_t> corrupt_read_call;
    std::map<std::uint32_t, std::uint8_t> memory;
    std::vector<std::pair<std::uint32_t, std::size_t>> invalidations;
    BackendResult invalidate_result = BackendResult::Success();
};

class FakeScreenshotBackend final : public IScreenshotBackendPort
{
public:
    BackendResult Capture(
        const std::filesystem::path& path,
        std::chrono::milliseconds timeout) override
    {
        paths.push_back(path);
        timeouts.push_back(timeout);
        return result;
    }

    BackendResult result = BackendResult::Success();
    std::vector<std::filesystem::path> paths;
    std::vector<std::chrono::milliseconds> timeouts;
};

[[nodiscard]] InputExecutionBindingEvidence Evidence(
    const InputExecutionBindingReceipt& binding)
{
    return {
        binding.lease,
        binding.binding,
        binding.publication,
        binding.epoch,
        binding.state_generation,
        binding.frame};
}

[[nodiscard]] InputExecutionRelationshipReceipt Relate(
    InputArbiter& arbiter,
    const InputExecutionBindingReceipt& binding)
{
    return arbiter.CreateExecutionRelationship(Evidence(binding));
}

TEST(InputArbiter, AcquisitionAndStableNeutralDoNotPublish)
{
    FakeInputBackend backend;
    InputArbiter arbiter(backend);
    ASSERT_TRUE(arbiter.InitializeWorksetEpoch(kEpoch).ok);

    const InputLeaseReceipt lease =
        arbiter.Acquire({.owner = InputOwnerId(1)}, kEpoch);
    ASSERT_TRUE(lease.ok);
    EXPECT_EQ(backend.publish_calls, 0u);
    EXPECT_EQ(arbiter.snapshot().active_state, InputState::Neutral);

    const InputExecutionBindingReceipt neutral =
        arbiter.ApplyState(lease.lease, {}, kEpoch);
    ASSERT_TRUE(neutral.ok) << neutral.message;
    EXPECT_EQ(neutral.kind, InputBindingKind::StableNeutral);
    EXPECT_FALSE(neutral.requires_observation);
    EXPECT_FALSE(neutral.publication);
    EXPECT_EQ(backend.publish_calls, 0u);

    const InputExecutionRelationshipReceipt relationship =
        Relate(arbiter, neutral);
    ASSERT_TRUE(relationship.ok) << relationship.message;
    EXPECT_TRUE(arbiter.Validate(relationship.relationship, kEpoch).ok);
    EXPECT_TRUE(arbiter.Complete(relationship.relationship, kEpoch).ok);
    EXPECT_EQ(backend.poll_queries, 0u);

    const InputLeaseCloseReceipt closed =
        arbiter.CloseLease(lease.lease, kEpoch);
    ASSERT_TRUE(closed.ok) << closed.message;
    EXPECT_FALSE(closed.neutral_publication);
    EXPECT_EQ(backend.publish_calls, 0u);
}

TEST(InputArbiter, HeldAndNeutralTransitionsRequireExactGuestObservation)
{
    FakeInputBackend backend;
    InputArbiter arbiter(backend);
    ASSERT_TRUE(arbiter.InitializeWorksetEpoch(kEpoch).ok);
    const InputLeaseReceipt lease =
        arbiter.Acquire({.owner = InputOwnerId(1)}, kEpoch);
    ASSERT_TRUE(lease.ok);

    GCInputFrame pressed;
    pressed.A();
    const InputExecutionBindingReceipt held =
        arbiter.ApplyState(lease.lease, pressed, kEpoch);
    ASSERT_TRUE(held.ok) << held.message;
    EXPECT_EQ(held.kind, InputBindingKind::Held);
    EXPECT_TRUE(held.requires_observation);
    EXPECT_EQ(backend.publish_calls, 1u);

    InputExecutionRelationshipReceipt held_relationship =
        Relate(arbiter, held);
    ASSERT_TRUE(held_relationship.ok);
    EXPECT_FALSE(
        arbiter.Complete(held_relationship.relationship, kEpoch).ok);
    backend.Poll();
    held_relationship = Relate(arbiter, held);
    ASSERT_TRUE(held_relationship.ok);
    EXPECT_TRUE(arbiter.Complete(held_relationship.relationship, kEpoch).ok);

    const InputExecutionBindingReceipt released =
        arbiter.ApplyState(lease.lease, {}, kEpoch);
    ASSERT_TRUE(released.ok) << released.message;
    EXPECT_EQ(released.kind, InputBindingKind::NeutralTransition);
    EXPECT_EQ(backend.publish_calls, 2u);
    EXPECT_EQ(arbiter.snapshot().active_state,
              InputState::NeutralTransitionPending);
    EXPECT_FALSE(arbiter.ApplyState(lease.lease, pressed, kEpoch).ok);

    InputExecutionRelationshipReceipt release_relationship =
        Relate(arbiter, released);
    ASSERT_TRUE(release_relationship.ok);
    EXPECT_FALSE(
        arbiter.Complete(release_relationship.relationship, kEpoch).ok);
    backend.Poll();
    release_relationship = Relate(arbiter, released);
    ASSERT_TRUE(release_relationship.ok);
    EXPECT_TRUE(
        arbiter.Complete(release_relationship.relationship, kEpoch).ok);
    EXPECT_EQ(arbiter.snapshot().active_state, InputState::Neutral);

    const std::size_t publications = backend.publish_calls;
    EXPECT_TRUE(arbiter.CloseLease(lease.lease, kEpoch).ok);
    EXPECT_EQ(backend.publish_calls, publications);
}

TEST(InputArbiter, BindingsRejectStaleSupersededWrongLeaseAndWrongEpochEvidence)
{
    FakeInputBackend backend;
    InputArbiter arbiter(backend);
    ASSERT_TRUE(arbiter.InitializeWorksetEpoch(kEpoch).ok);
    const InputLeaseReceipt lease =
        arbiter.Acquire({.owner = InputOwnerId(1)}, kEpoch);
    ASSERT_TRUE(lease.ok);

    GCInputFrame pressed;
    pressed.A();
    const InputExecutionBindingReceipt first =
        arbiter.ApplyState(lease.lease, pressed, kEpoch);
    ASSERT_TRUE(first.ok);
    EXPECT_TRUE(arbiter.ValidateBinding(Evidence(first)).ok);

    InputExecutionBindingEvidence wrong_lease = Evidence(first);
    wrong_lease.lease = InputLeaseId(first.lease.value() + 1);
    EXPECT_FALSE(arbiter.ValidateBinding(wrong_lease).ok);
    InputExecutionBindingEvidence wrong_epoch = Evidence(first);
    wrong_epoch.epoch = WorksetEpoch(kEpoch.value() + 1);
    EXPECT_FALSE(arbiter.ValidateBinding(wrong_epoch).ok);
    InputExecutionBindingEvidence wrong_frame = Evidence(first);
    wrong_frame.frame = {};
    EXPECT_FALSE(arbiter.ValidateBinding(wrong_frame).ok);
    InputExecutionBindingEvidence wrong_publication = Evidence(first);
    wrong_publication.publication =
        InputPublicationToken(first.publication.value() + 1);
    EXPECT_FALSE(arbiter.ValidateBinding(wrong_publication).ok);

    GCInputFrame replacement;
    replacement.B();
    const InputExecutionBindingReceipt second =
        arbiter.ApplyState(lease.lease, replacement, kEpoch);
    ASSERT_TRUE(second.ok);
    EXPECT_FALSE(arbiter.ValidateBinding(Evidence(first)).ok);
    EXPECT_TRUE(arbiter.ValidateBinding(Evidence(second)).ok);
}

TEST(InputArbiter, OneShotDeliveryProducesOneReceiptAndRestoresNeutral)
{
    {
        FakeInputBackend backend;
        InputArbiter arbiter(backend);
        ASSERT_TRUE(arbiter.InitializeWorksetEpoch(kEpoch).ok);
        const InputLeaseReceipt lease =
            arbiter.Acquire({.owner = InputOwnerId(1)}, kEpoch);
        ASSERT_TRUE(lease.ok);

        const InputExecutionBindingReceipt delivery =
            arbiter.BeginDelivery(lease.lease, {}, kEpoch);
        ASSERT_TRUE(delivery.ok) << delivery.message;
        EXPECT_EQ(backend.publish_calls, 1u);
        InputExecutionRelationshipReceipt relationship =
            Relate(arbiter, delivery);
        ASSERT_TRUE(relationship.ok);
        backend.Poll();
        ASSERT_TRUE(arbiter.Complete(relationship.relationship, kEpoch).ok);
        const InputDeliveryReceipt receipt = arbiter.CompleteDelivery(
            lease.lease, delivery.binding, kEpoch);
        ASSERT_TRUE(receipt.ok) << receipt.message;
        EXPECT_EQ(receipt.binding, delivery.binding);
        EXPECT_EQ(receipt.frame, GCInputFrame{});
        EXPECT_TRUE(receipt.poll);
        EXPECT_EQ(backend.publish_calls, 1u);
        EXPECT_EQ(arbiter.snapshot().active_state, InputState::Neutral);
    }

    {
        FakeInputBackend backend;
        InputArbiter arbiter(backend);
        ASSERT_TRUE(arbiter.InitializeWorksetEpoch(kEpoch).ok);
        const InputLeaseReceipt lease =
            arbiter.Acquire({.owner = InputOwnerId(2)}, kEpoch);
        ASSERT_TRUE(lease.ok);
        GCInputFrame pressed;
        pressed.A();
        const InputExecutionBindingReceipt delivery =
            arbiter.BeginDelivery(lease.lease, pressed, kEpoch);
        ASSERT_TRUE(delivery.ok);
        InputExecutionRelationshipReceipt relationship =
            Relate(arbiter, delivery);
        ASSERT_TRUE(relationship.ok);
        backend.Poll();
        ASSERT_TRUE(arbiter.Complete(relationship.relationship, kEpoch).ok);
        const InputDeliveryReceipt receipt = arbiter.CompleteDelivery(
            lease.lease, delivery.binding, kEpoch);
        ASSERT_TRUE(receipt.ok) << receipt.message;
        EXPECT_EQ(receipt.frame, pressed);
        EXPECT_EQ(backend.publish_calls, 2u);
        EXPECT_EQ(backend.current_frame, GCInputFrame{});
        EXPECT_EQ(arbiter.snapshot().active_state, InputState::Neutral);
        const std::size_t publications = backend.publish_calls;
        EXPECT_TRUE(arbiter.CloseLease(lease.lease, kEpoch).ok);
        EXPECT_EQ(backend.publish_calls, publications);
    }
}

TEST(InputArbiter, DeliveryCannotCompleteBeforeItsExecutionBindingIsObserved)
{
    FakeInputBackend backend;
    InputArbiter arbiter(backend);
    ASSERT_TRUE(arbiter.InitializeWorksetEpoch(kEpoch).ok);
    const InputLeaseReceipt lease =
        arbiter.Acquire({.owner = InputOwnerId(1)}, kEpoch);
    ASSERT_TRUE(lease.ok);
    const InputExecutionBindingReceipt delivery =
        arbiter.BeginDelivery(lease.lease, {}, kEpoch);
    ASSERT_TRUE(delivery.ok);

    EXPECT_FALSE(arbiter.CompleteDelivery(
        lease.lease, delivery.binding, kEpoch).ok);
    InputExecutionRelationshipReceipt relationship =
        Relate(arbiter, delivery);
    ASSERT_TRUE(relationship.ok);
    EXPECT_FALSE(arbiter.Complete(relationship.relationship, kEpoch).ok);
    EXPECT_FALSE(arbiter.CompleteDelivery(
        lease.lease, delivery.binding, kEpoch).ok);
}

TEST(InputArbiter, BorrowingUsesInternalStateWithoutNeutralWitnesses)
{
    FakeInputBackend backend;
    InputArbiter arbiter(backend);
    ASSERT_TRUE(arbiter.InitializeWorksetEpoch(kEpoch).ok);
    const InputLeaseReceipt parent = arbiter.Acquire(
        {.owner = InputOwnerId(1),
         .priority = 10,
         .interruption_borrowable = true,
         .borrow_policy = InputBorrowPolicy::RequireStableNeutral},
        kEpoch);
    ASSERT_TRUE(parent.ok);

    const InputLeaseReceipt child = arbiter.Borrow(
        parent.lease,
        {.owner = InputOwnerId(2), .priority = 20},
        kEpoch);
    ASSERT_TRUE(child.ok) << child.message;
    EXPECT_EQ(arbiter.snapshot().suspended_count, 1u);
    const InputLeaseCloseReceipt closed =
        arbiter.CloseLease(child.lease, kEpoch);
    ASSERT_TRUE(closed.ok) << closed.message;
    ASSERT_TRUE(closed.resumed_lease.has_value());
    EXPECT_EQ(*closed.resumed_lease, parent.lease);
    EXPECT_EQ(backend.publish_calls, 0u);

    GCInputFrame pressed;
    pressed.A();
    ASSERT_TRUE(arbiter.ApplyState(parent.lease, pressed, kEpoch).ok);
    EXPECT_FALSE(arbiter.Borrow(
        parent.lease,
        {.owner = InputOwnerId(3), .priority = 30},
        kEpoch).ok);
}

TEST(InputArbiter, RejectsEveryActorOwnedApiOffThreadWithoutMutation)
{
    FakeInputBackend backend;
    InputArbiter arbiter(backend);
    ASSERT_TRUE(arbiter.InitializeWorksetEpoch(kEpoch).ok);
    const InputLeaseReceipt lease =
        arbiter.Acquire({.owner = InputOwnerId(1)}, kEpoch);
    ASSERT_TRUE(lease.ok);
    const InputExecutionBindingReceipt binding =
        arbiter.ApplyState(lease.lease, {}, kEpoch);
    ASSERT_TRUE(binding.ok);
    const InputExecutionRelationshipReceipt relationship =
        Relate(arbiter, binding);
    ASSERT_TRUE(relationship.ok);

    const InputArbiterSnapshot before = arbiter.snapshot();
    const std::size_t availability_before = backend.availability_queries;
    const std::size_t publishes_before = backend.publish_calls;
    const std::size_t polls_before = backend.poll_queries;

    InputLeaseReceipt acquired;
    InputLeaseReceipt borrowed;
    InputExecutionBindingReceipt applied;
    InputExecutionBindingReceipt delivery;
    InputDeliveryReceipt completed_delivery;
    InputArbiterOperationReceipt validated_binding;
    InputExecutionRelationshipReceipt created_relationship;
    InputArbiterOperationReceipt removed_relationship;
    InputLeaseCloseReceipt closed;
    InputArbiterOperationReceipt committed_epoch;
    InputExecutionRelationshipOperationReceipt validated;
    InputExecutionRelationshipOperationReceipt completed;
    InputExecutionRelationshipOperationReceipt cancelled;
    InputArbiterShutdownReceipt shutdown;

    std::thread off_actor([&] {
        acquired = arbiter.Acquire(
            {.owner = InputOwnerId(2), .priority = 20}, kEpoch);
        borrowed = arbiter.Borrow(
            lease.lease,
            {.owner = InputOwnerId(3), .priority = 30},
            kEpoch);
        applied = arbiter.ApplyState(lease.lease, {}, kEpoch);
        delivery = arbiter.BeginDelivery(lease.lease, {}, kEpoch);
        completed_delivery = arbiter.CompleteDelivery(
            lease.lease, binding.binding, kEpoch);
        validated_binding = arbiter.ValidateBinding(Evidence(binding));
        created_relationship =
            arbiter.CreateExecutionRelationship(Evidence(binding));
        removed_relationship = arbiter.RemoveExecutionRelationship(
            relationship.relationship);
        closed = arbiter.CloseLease(lease.lease, kEpoch);
        committed_epoch = arbiter.InitializeWorksetEpoch(WorksetEpoch(12));
        validated = arbiter.Validate(relationship.relationship, kEpoch);
        completed = arbiter.Complete(relationship.relationship, kEpoch);
        cancelled = arbiter.Cancel(relationship.relationship, kEpoch);
        shutdown = arbiter.Shutdown();
    });
    off_actor.join();

    EXPECT_EQ(acquired.error, InputArbiterErrorCode::WrongThread);
    EXPECT_EQ(borrowed.error, InputArbiterErrorCode::WrongThread);
    EXPECT_EQ(applied.error, InputArbiterErrorCode::WrongThread);
    EXPECT_EQ(delivery.error, InputArbiterErrorCode::WrongThread);
    EXPECT_EQ(completed_delivery.error, InputArbiterErrorCode::WrongThread);
    EXPECT_EQ(validated_binding.error, InputArbiterErrorCode::WrongThread);
    EXPECT_EQ(created_relationship.error, InputArbiterErrorCode::WrongThread);
    EXPECT_EQ(removed_relationship.error, InputArbiterErrorCode::WrongThread);
    EXPECT_EQ(closed.error, InputArbiterErrorCode::WrongThread);
    EXPECT_EQ(committed_epoch.error, InputArbiterErrorCode::WrongThread);
    EXPECT_FALSE(validated.ok);
    EXPECT_FALSE(completed.ok);
    EXPECT_FALSE(cancelled.ok);
    EXPECT_EQ(shutdown.error, InputArbiterErrorCode::WrongThread);

    const InputArbiterSnapshot after = arbiter.snapshot();
    EXPECT_EQ(after.epoch, before.epoch);
    EXPECT_EQ(after.active_lease, before.active_lease);
    EXPECT_EQ(after.suspended_count, before.suspended_count);
    EXPECT_EQ(after.lease_count, before.lease_count);
    EXPECT_EQ(after.binding_count, before.binding_count);
    EXPECT_EQ(after.relationship_count, before.relationship_count);
    EXPECT_FALSE(after.stopped);
    EXPECT_EQ(backend.availability_queries, availability_before);
    EXPECT_EQ(backend.publish_calls, publishes_before);
    EXPECT_EQ(backend.poll_queries, polls_before);
}

TEST(InputArbiter, ShutdownRestoresNonNeutralOnceAndIsIdempotent)
{
    FakeInputBackend backend;
    InputArbiter arbiter(backend);
    ASSERT_TRUE(arbiter.InitializeWorksetEpoch(kEpoch).ok);
    const InputLeaseReceipt lease =
        arbiter.Acquire({.owner = InputOwnerId(1)}, kEpoch);
    ASSERT_TRUE(lease.ok);
    GCInputFrame pressed;
    pressed.A();
    ASSERT_TRUE(arbiter.ApplyState(lease.lease, pressed, kEpoch).ok);
    const std::size_t before = backend.publish_calls;

    const InputArbiterShutdownReceipt first = arbiter.Shutdown();
    ASSERT_TRUE(first.ok) << first.message;
    EXPECT_TRUE(first.cleanup_complete);
    EXPECT_FALSE(first.taint_required);
    EXPECT_TRUE(first.neutral_publication);
    EXPECT_EQ(backend.publish_calls, before + 1);
    EXPECT_EQ(backend.current_frame, GCInputFrame{});
    EXPECT_TRUE(arbiter.snapshot().stopped);
    EXPECT_EQ(arbiter.snapshot().lease_count, 0u);

    const InputArbiterShutdownReceipt second = arbiter.Shutdown();
    EXPECT_EQ(second.ok, first.ok);
    EXPECT_EQ(second.neutral_publication, first.neutral_publication);
    EXPECT_EQ(backend.publish_calls, before + 1);
}

TEST(InputArbiter, ShutdownOfNeutralLeaseIsHostOnly)
{
    FakeInputBackend backend;
    InputArbiter arbiter(backend);
    ASSERT_TRUE(arbiter.InitializeWorksetEpoch(kEpoch).ok);
    ASSERT_TRUE(arbiter.Acquire(
        {.owner = InputOwnerId(1)}, kEpoch).ok);
    const InputArbiterShutdownReceipt receipt = arbiter.Shutdown();
    ASSERT_TRUE(receipt.ok) << receipt.message;
    EXPECT_TRUE(receipt.cleanup_complete);
    EXPECT_FALSE(receipt.neutral_publication);
    EXPECT_EQ(backend.publish_calls, 0u);
    EXPECT_EQ(backend.poll_queries, 0u);
}

TEST(GuestMutationService, AppliesNestedCheckedWritesAndRestoresInReverse)
{
    FakeGuestMemoryBackend backend;
    backend.PutU32(0x80001000, 0x11223344);
    GuestMemory memory(backend);
    GuestMutationService mutations(memory, backend);
    mutations.InitializeWorksetEpoch(kEpoch);

    GuestMutationReceipt outer = mutations.Apply(
        {
            .owner = MutationOwnerId(1),
            .scope = MutationScopeId(1),
            .epoch = kEpoch,
            .address = 0x80001000,
            .expected = 0x11223344,
            .replacement = 0x55667788});
    ASSERT_TRUE(outer.ok);
    GuestMutationReceipt inner = mutations.Apply(
        {
            .owner = MutationOwnerId(1),
            .scope = MutationScopeId(2),
            .parent = outer.mutation,
            .epoch = kEpoch,
            .address = 0x80001000,
            .expected = 0x55667788,
            .replacement = 0x99aabbcc});
    ASSERT_TRUE(inner.ok);
    EXPECT_FALSE(mutations.Restore(outer.mutation, kEpoch).ok);
    EXPECT_TRUE(mutations.Restore(inner.mutation, kEpoch).ok);
    EXPECT_TRUE(mutations.Restore(outer.mutation, kEpoch).ok);
    EXPECT_EQ(
        memory.ReadScalar(
            0x80001000,
            GuestScalarWidth::U32,
            kEpoch).value,
        0x11223344u);
}

TEST(GuestMutationService, ExecutablePatchInvalidatesAndCannotCommit)
{
    FakeGuestMemoryBackend backend;
    backend.PutU32(0x801dc288, 0x9421fff0);
    GuestMemory memory(backend);
    GuestMutationService mutations(memory, backend);
    mutations.InitializeWorksetEpoch(kEpoch);

    GuestMutationReceipt patch = mutations.Apply(
        {
            .owner = MutationOwnerId(1),
            .scope = MutationScopeId(1),
            .epoch = kEpoch,
            .address = 0x801dc288,
            .expected = 0x9421fff0,
            .replacement = 0x60000000,
            .mask = 0xffffffff,
            .kind = GuestMutationKind::ExecutablePatch});
    ASSERT_TRUE(patch.ok);
    EXPECT_EQ(backend.invalidations.size(), 1u);
    EXPECT_FALSE(mutations.Commit(patch.mutation, kEpoch).ok);
    EXPECT_TRUE(mutations.Restore(patch.mutation, kEpoch).ok);
    EXPECT_EQ(backend.invalidations.size(), 2u);
}

TEST(
    GuestMutationService,
    PostWriteInvalidationFailureRetainsCleanupEvidenceForShutdownRetry)
{
    constexpr std::uint32_t kAddress = 0x801dc288;
    constexpr std::uint32_t kOriginal = 0x9421fff0;
    FakeGuestMemoryBackend backend;
    backend.PutU32(kAddress, kOriginal);
    backend.invalidate_result = BackendResult::Failure(
        BackendErrorCode::OperationFailed,
        "injected invalidation failure");
    GuestMemory memory(backend);
    GuestMutationService mutations(memory, backend);
    mutations.InitializeWorksetEpoch(kEpoch);

    const GuestMutationReceipt failed = mutations.Apply(
        {
            .owner = MutationOwnerId(1),
            .scope = MutationScopeId(1),
            .epoch = kEpoch,
            .address = kAddress,
            .expected = kOriginal,
            .replacement = 0x60000000,
            .mask = 0xffffffff,
            .kind = GuestMutationKind::ExecutablePatch,
        });

    EXPECT_FALSE(failed.ok);
    EXPECT_TRUE(failed.taint_required);
    EXPECT_EQ(
        memory.ReadScalar(kAddress, GuestScalarWidth::U32, kEpoch).value,
        kOriginal);

    backend.invalidate_result = BackendResult::Success();
    const GuestMutationCleanupReceipt cleanup = mutations.Shutdown();
    ASSERT_TRUE(cleanup.ok) << cleanup.message;
    EXPECT_FALSE(cleanup.taint_required);
    ASSERT_EQ(cleanup.restorations.size(), 1u);
    EXPECT_TRUE(cleanup.restorations.front().ok);
}

TEST(
    GuestMutationService,
    ReplacementReadbackMismatchIsCompensatedBeforeReturning)
{
    constexpr std::uint32_t kAddress = 0x80001000;
    constexpr std::uint32_t kOriginal = 0x11223344;
    FakeGuestMemoryBackend backend;
    backend.PutU32(kAddress, kOriginal);
    backend.corrupt_read_call = 2;
    GuestMemory memory(backend);
    GuestMutationService mutations(memory, backend);
    mutations.InitializeWorksetEpoch(kEpoch);

    const GuestMutationReceipt failed = mutations.Apply(
        {
            .owner = MutationOwnerId(1),
            .scope = MutationScopeId(1),
            .epoch = kEpoch,
            .address = kAddress,
            .expected = kOriginal,
            .replacement = 0x55667788,
        });

    EXPECT_FALSE(failed.ok);
    EXPECT_FALSE(failed.taint_required);
    EXPECT_EQ(failed.status, GuestMutationStatus::Restored);
    EXPECT_EQ(
        memory.ReadScalar(kAddress, GuestScalarWidth::U32, kEpoch).value,
        kOriginal);
    EXPECT_TRUE(mutations.Shutdown().ok);
}

TEST(
    GuestMutationService,
    RestorationReadbackFailureRemainsPendingUntilShutdownCanProveCleanup)
{
    constexpr std::uint32_t kAddress = 0x801dc288;
    constexpr std::uint32_t kOriginal = 0x9421fff0;
    FakeGuestMemoryBackend backend;
    backend.PutU32(kAddress, kOriginal);
    GuestMemory memory(backend);
    GuestMutationService mutations(memory, backend);
    mutations.InitializeWorksetEpoch(kEpoch);
    const GuestMutationReceipt patch = mutations.Apply(
        {
            .owner = MutationOwnerId(1),
            .scope = MutationScopeId(1),
            .epoch = kEpoch,
            .address = kAddress,
            .expected = kOriginal,
            .replacement = 0x60000000,
            .mask = 0xffffffff,
            .kind = GuestMutationKind::ExecutablePatch,
        });
    ASSERT_TRUE(patch.ok);
    backend.corrupt_read_call = backend.read_calls + 2;

    const GuestMutationReceipt failed =
        mutations.Restore(patch.mutation, kEpoch);
    EXPECT_FALSE(failed.ok);
    EXPECT_TRUE(failed.taint_required);
    EXPECT_EQ(
        memory.ReadScalar(kAddress, GuestScalarWidth::U32, kEpoch).value,
        kOriginal);

    backend.corrupt_read_call.reset();
    const GuestMutationCleanupReceipt cleanup = mutations.Shutdown();
    ASSERT_TRUE(cleanup.ok) << cleanup.message;
    ASSERT_EQ(cleanup.restorations.size(), 1u);
    EXPECT_TRUE(cleanup.restorations.front().ok);
}

TEST(
    GuestMutationService,
    ShutdownRestoresAllActiveMutationsInReverseOrderAndIsIdempotent)
{
    FakeGuestMemoryBackend backend;
    backend.PutU32(0x80001000, 0x11223344);
    backend.PutU32(0x801dc288, 0x9421fff0);
    GuestMemory memory(backend);
    GuestMutationService mutations(memory, backend);
    mutations.InitializeWorksetEpoch(kEpoch);

    const GuestMutationReceipt outer = mutations.Apply(
        {
            .owner = MutationOwnerId(1),
            .scope = MutationScopeId(1),
            .epoch = kEpoch,
            .address = 0x80001000,
            .expected = 0x11223344,
            .replacement = 0x55667788});
    ASSERT_TRUE(outer.ok);
    const GuestMutationReceipt inner = mutations.Apply(
        {
            .owner = MutationOwnerId(1),
            .scope = MutationScopeId(2),
            .parent = outer.mutation,
            .epoch = kEpoch,
            .address = 0x80001000,
            .expected = 0x55667788,
            .replacement = 0x99aabbcc});
    ASSERT_TRUE(inner.ok);
    const GuestMutationReceipt patch = mutations.Apply(
        {
            .owner = MutationOwnerId(1),
            .scope = MutationScopeId(3),
            .epoch = kEpoch,
            .address = 0x801dc288,
            .expected = 0x9421fff0,
            .replacement = 0x60000000,
            .mask = 0xffffffff,
            .kind = GuestMutationKind::ExecutablePatch});
    ASSERT_TRUE(patch.ok);

    const GuestMutationCleanupReceipt first = mutations.Shutdown();
    ASSERT_TRUE(first.ok) << first.message;
    ASSERT_EQ(first.restorations.size(), 3u);
    EXPECT_EQ(first.restorations[0].mutation, patch.mutation);
    EXPECT_EQ(first.restorations[1].mutation, inner.mutation);
    EXPECT_EQ(first.restorations[2].mutation, outer.mutation);
    EXPECT_EQ(backend.invalidations.size(), 2u);
    EXPECT_EQ(
        memory.ReadScalar(
            0x80001000,
            GuestScalarWidth::U32,
            kEpoch).value,
        0x11223344u);
    EXPECT_EQ(
        memory.ReadScalar(
            0x801dc288,
            GuestScalarWidth::U32,
            kEpoch).value,
        0x9421fff0u);

    const std::size_t writes_after_first = backend.write_calls;
    const std::size_t invalidations_after_first =
        backend.invalidate_calls;
    const GuestMutationCleanupReceipt second = mutations.Shutdown();
    EXPECT_TRUE(second.ok);
    EXPECT_TRUE(second.already_shutdown);
    EXPECT_EQ(backend.write_calls, writes_after_first);
    EXPECT_EQ(
        backend.invalidate_calls,
        invalidations_after_first);
}

TEST(
    GuestMutationService,
    ShutdownContinuesAfterRestorationFailureAndRequiresTaint)
{
    constexpr std::uint32_t kFirst = 0x80001000;
    constexpr std::uint32_t kSecond = 0x80002000;
    FakeGuestMemoryBackend backend;
    backend.PutU32(kFirst, 0x11111111);
    backend.PutU32(kSecond, 0x22222222);
    GuestMemory memory(backend);
    GuestMutationService mutations(memory, backend);
    mutations.InitializeWorksetEpoch(kEpoch);
    ASSERT_TRUE(mutations.Apply(
        {
            .owner = MutationOwnerId(1),
            .scope = MutationScopeId(1),
            .epoch = kEpoch,
            .address = kFirst,
            .expected = 0x11111111,
            .replacement = 0xaaaaaaaa}).ok);
    ASSERT_TRUE(mutations.Apply(
        {
            .owner = MutationOwnerId(1),
            .scope = MutationScopeId(1),
            .epoch = kEpoch,
            .address = kSecond,
            .expected = 0x22222222,
            .replacement = 0xbbbbbbbb}).ok);
    backend.fail_write_address = kSecond;

    const GuestMutationCleanupReceipt cleanup = mutations.Shutdown();

    EXPECT_FALSE(cleanup.ok);
    EXPECT_TRUE(cleanup.taint_required);
    ASSERT_EQ(cleanup.restorations.size(), 2u);
    EXPECT_FALSE(cleanup.restorations[0].ok);
    EXPECT_TRUE(cleanup.restorations[0].taint_required);
    EXPECT_TRUE(cleanup.restorations[1].ok);
    EXPECT_EQ(
        memory.ReadScalar(
            kFirst,
            GuestScalarWidth::U32,
            kEpoch).value,
        0x11111111u);
    EXPECT_EQ(
        memory.ReadScalar(
            kSecond,
            GuestScalarWidth::U32,
            kEpoch).value,
        0xbbbbbbbbu);
}

TEST(
    SessionServicesOwnership,
    GuestMemoryRejectsOffActorWithoutBackendCalls)
{
    FakeGuestMemoryBackend memory_backend;
    memory_backend.PutU32(0x80001000, 0x11223344);
    GuestMemory memory(memory_backend);
    memory.InitializeWorksetEpoch(kEpoch);

    GuestReadReceipt read;
    GuestBytesResult bytes;
    std::thread other([&] {
        memory.InitializeWorksetEpoch(WorksetEpoch(12));
        read = memory.ReadScalar(
            0x80001000,
            GuestScalarWidth::U32,
            kEpoch);
        bytes = memory.ReadBytes(0x80001000, 4, kEpoch);
    });
    other.join();

    EXPECT_FALSE(read.ok);
    EXPECT_FALSE(bytes.result.ok);
    EXPECT_EQ(memory_backend.is_paused_calls, 0u);
    EXPECT_EQ(memory_backend.read_calls, 0u);
}

TEST(
    SessionServicesOwnership,
    GuestMutationServiceRejectsOffActorWithoutBackendCalls)
{
    FakeGuestMemoryBackend memory_backend;
    memory_backend.PutU32(0x80001000, 0x11223344);
    GuestMemory memory(memory_backend);
    GuestMutationService mutations(memory, memory_backend);
    memory.InitializeWorksetEpoch(kEpoch);
    mutations.InitializeWorksetEpoch(kEpoch);

    GuestMutationReceipt applied;
    GuestMutationReceipt restored;
    GuestMutationReceipt committed;
    std::vector<GuestMutationReceipt> scope_restored;
    GuestMutationCleanupReceipt restored_all;
    GuestMutationCleanupReceipt shutdown;
    std::thread other([&] {
        mutations.InitializeWorksetEpoch(WorksetEpoch(12));
        applied = mutations.Apply(
            {
                .owner = MutationOwnerId(1),
                .scope = MutationScopeId(1),
                .epoch = kEpoch,
                .address = 0x80001000,
                .expected = 0x11223344,
                .replacement = 0x55667788});
        restored = mutations.Restore(GuestMutationId(1), kEpoch);
        committed = mutations.Commit(GuestMutationId(1), kEpoch);
        scope_restored =
            mutations.RestoreScope(MutationScopeId(1), kEpoch);
        restored_all = mutations.RestoreAll();
        shutdown = mutations.Shutdown();
    });
    other.join();

    EXPECT_FALSE(applied.ok);
    EXPECT_FALSE(restored.ok);
    EXPECT_FALSE(committed.ok);
    ASSERT_EQ(scope_restored.size(), 1u);
    EXPECT_FALSE(scope_restored.front().ok);
    EXPECT_FALSE(restored_all.ok);
    EXPECT_TRUE(restored_all.taint_required);
    EXPECT_FALSE(shutdown.ok);
    EXPECT_TRUE(shutdown.taint_required);
    EXPECT_EQ(memory_backend.is_paused_calls, 0u);
    EXPECT_EQ(memory_backend.read_calls, 0u);
    EXPECT_EQ(memory_backend.write_calls, 0u);
    EXPECT_EQ(memory_backend.invalidate_calls, 0u);
}

TEST(
    SessionServicesOwnership,
    ScreenshotServiceRejectsOffActorWithoutBackendCalls)
{
    FakeScreenshotBackend screenshot_backend;
    ScreenshotService screenshots(screenshot_backend);
    screenshots.InitializeWorksetEpoch(kEpoch);

    ScreenshotReceipt captured;
    ScreenshotReceipt cancelled;
    std::optional<ScreenshotReceipt> last;
    std::thread other([&] {
        screenshots.InitializeWorksetEpoch(WorksetEpoch(12));
        captured = screenshots.Capture("wrong-thread.png", 1s, kEpoch);
        cancelled =
            screenshots.Cancel(ScreenshotRequestId(1), kEpoch);
        last = screenshots.last_receipt();
    });
    other.join();

    EXPECT_FALSE(captured.ok);
    EXPECT_EQ(captured.status, ScreenshotStatus::Rejected);
    EXPECT_FALSE(cancelled.ok);
    EXPECT_FALSE(last.has_value());
    EXPECT_TRUE(screenshot_backend.paths.empty());
}

TEST(TelemetryBus, CoalescesLossyEventsAndFailsRequiredOverflow)
{
    TelemetryBus bus(1);
    EXPECT_TRUE(bus.Enqueue(
        {.source = "capture", .kind = "progress", .payload = "one"}).ok);
    TelemetryEnqueueReceipt coalesced = bus.Enqueue(
        {.source = "capture", .kind = "progress", .payload = "two"});
    EXPECT_TRUE(coalesced.coalesced);
    TelemetryEnqueueReceipt overflow = bus.Enqueue(
        {
            .source = "state",
            .kind = "restored",
            .payload = "required",
            .loss_policy = TelemetryLossPolicy::Required});
    EXPECT_TRUE(overflow.authoritative_overflow);
    auto events = bus.Drain();
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events.front().payload, "two");
}

TEST(TelemetryBus, CoalescingPreservesSequenceOrderAtCapacityTwo)
{
    TelemetryBus bus(2);
    const TelemetryEnqueueReceipt first = bus.Enqueue(
        {.source = "capture", .kind = "progress", .payload = "one"});
    const TelemetryEnqueueReceipt second = bus.Enqueue(
        {.source = "execution", .kind = "state", .payload = "middle"});
    const TelemetryEnqueueReceipt replacement = bus.Enqueue(
        {.source = "capture", .kind = "progress", .payload = "two"});

    ASSERT_TRUE(first.ok);
    ASSERT_TRUE(second.ok);
    ASSERT_TRUE(replacement.ok);
    ASSERT_TRUE(replacement.coalesced);
    ASSERT_GT(replacement.sequence.value(), second.sequence.value());

    const auto events = bus.Drain();
    ASSERT_EQ(events.size(), 2u);
    EXPECT_EQ(events[0].payload, "middle");
    EXPECT_EQ(events[0].sequence, second.sequence);
    EXPECT_EQ(events[1].payload, "two");
    EXPECT_EQ(events[1].sequence, replacement.sequence);
}

TEST(ScreenshotService, CorrelatesOneRequestAndPreservesBackendFailure)
{
    FakeScreenshotBackend backend;
    ScreenshotService service(backend);
    service.InitializeWorksetEpoch(kEpoch);
    const std::filesystem::path path = "shot.png";
    ScreenshotReceipt success = service.Capture(path, 2s, kEpoch);
    ASSERT_TRUE(success.ok);
    EXPECT_EQ(success.status, ScreenshotStatus::Complete);
    ASSERT_EQ(backend.paths.size(), 1u);
    EXPECT_EQ(backend.paths.front(), path);

    backend.result = BackendResult::Failure(
        BackendErrorCode::Timeout,
        "timeout",
        BackendIntegrity::Preserved);
    ScreenshotReceipt failed = service.Capture(path, 1s, kEpoch);
    EXPECT_FALSE(failed.ok);
    EXPECT_EQ(failed.status, ScreenshotStatus::Failed);
    EXPECT_EQ(failed.message, "timeout");
}

} // namespace

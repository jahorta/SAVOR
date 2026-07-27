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

constexpr StateEpoch kEpoch{11};

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
        current_sequence = ++next_sequence;
        callback_count = 0;
        return {BackendResult::Success(), current_sequence};
    }

    BackendInputPoll QueryPoll(std::uint8_t port) const override
    {
        ++poll_queries;
        if (port != 0)
            return {BackendResult::Failure(BackendErrorCode::Unavailable, "port")};
        return {
            BackendResult::Success(),
            current_sequence,
            callback_count,
            current_frame};
    }

    void Poll(std::uint32_t count = 1)
    {
        callback_count += count;
    }

    std::uint64_t next_sequence = 0;
    std::uint64_t current_sequence = 0;
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

TEST(InputArbiter, PublishesFreshTokensAndRequiresObservedNeutralRelease)
{
    FakeInputBackend backend;
    InputArbiter arbiter(backend);
    ASSERT_TRUE(arbiter.CommitStateEpoch(kEpoch).ok);

    InputLeaseReceipt lease = arbiter.Acquire(
        {
            .owner = InputOwnerId(1),
            .priority = 10,
            .suspendable = true,
            .interruption_borrowable = true,
            .require_neutral_acknowledgement = true},
        kEpoch);
    ASSERT_TRUE(lease.ok);

    GCInputFrame frame;
    frame.A();
    InputPublicationReceipt published =
        arbiter.Publish(lease.lease, frame, kEpoch);
    ASSERT_TRUE(published.ok);
    EXPECT_FALSE(arbiter.Observe(
        lease.lease,
        published.publication,
        kEpoch).acknowledged);
    backend.Poll();
    EXPECT_TRUE(arbiter.Observe(
        lease.lease,
        published.publication,
        kEpoch).acknowledged);

    InputReleaseReceipt release =
        arbiter.BeginRelease(lease.lease, kEpoch);
    ASSERT_EQ(
        release.status,
        InputLeaseStatus::AwaitingNeutralAcknowledgement);
    EXPECT_NE(release.neutral_publication, published.publication);
    InputReleaseReceipt pending = arbiter.CompleteRelease(
        lease.lease,
        release.neutral_publication,
        kEpoch);
    EXPECT_TRUE(pending.ok);
    EXPECT_EQ(
        pending.status,
        InputLeaseStatus::AwaitingNeutralAcknowledgement);
    backend.Poll();
    InputReleaseReceipt complete = arbiter.CompleteRelease(
        lease.lease,
        release.neutral_publication,
        kEpoch);
    EXPECT_TRUE(complete.ok);
    EXPECT_EQ(complete.status, InputLeaseStatus::Released);
}

TEST(InputArbiter, EnforcesBorrowPolicyAndInvalidatesOnEpochReplacement)
{
    FakeInputBackend backend;
    InputArbiter arbiter(backend);
    ASSERT_TRUE(arbiter.CommitStateEpoch(kEpoch).ok);
    InputLeaseReceipt parent = arbiter.Acquire(
        {
            .owner = InputOwnerId(1),
            .priority = 10,
            .suspendable = true,
            .interruption_borrowable = true,
            .borrow_policy = InputBorrowPolicy::RequireNeutralWitness},
        kEpoch);
    ASSERT_TRUE(parent.ok);

    InputLeaseRequest borrower{
        .owner = InputOwnerId(2),
        .priority = 20};
    EXPECT_EQ(
        arbiter.Borrow(
            parent.lease,
            borrower,
            kEpoch,
            std::nullopt).status,
        InputLeaseStatus::AwaitingNeutralAcknowledgement);
    InputPublicationReceipt neutral =
        arbiter.Publish(parent.lease, {}, kEpoch);
    ASSERT_TRUE(neutral.ok);
    EXPECT_FALSE(
        arbiter.ProveNeutralWitness(
            parent.lease,
            neutral.publication,
            kEpoch).ok);
    backend.Poll();
    InputNeutralWitnessReceipt witness =
        arbiter.ProveNeutralWitness(
            parent.lease,
            neutral.publication,
            kEpoch);
    ASSERT_TRUE(witness.ok) << witness.message;
    InputLeaseReceipt borrowed =
        arbiter.Borrow(
            parent.lease,
            borrower,
            kEpoch,
            witness.witness);
    ASSERT_TRUE(borrowed.ok);
    const InputReleaseReceipt releasing_child =
        arbiter.BeginRelease(borrowed.lease, kEpoch);
    ASSERT_EQ(
        releasing_child.status,
        InputLeaseStatus::AwaitingNeutralAcknowledgement);
    backend.Poll();
    ASSERT_EQ(
        arbiter.CompleteRelease(
            borrowed.lease,
            releasing_child.neutral_publication,
            kEpoch).status,
        InputLeaseStatus::Released);
    EXPECT_FALSE(
        arbiter.Borrow(
            parent.lease,
            borrower,
            kEpoch,
            witness.witness).ok);
    EXPECT_EQ(arbiter.snapshot().suspended_count, 0u);

    ASSERT_TRUE(
        arbiter.InvalidateForStateReplacement(StateEpoch(12)).ok);
    EXPECT_FALSE(arbiter.snapshot().active_lease.has_value());
    EXPECT_EQ(arbiter.snapshot().lease_count, 0u);
    EXPECT_EQ(arbiter.snapshot().publication_count, 0u);
    EXPECT_EQ(arbiter.snapshot().binding_count, 0u);
    EXPECT_FALSE(arbiter.Publish(
        parent.lease,
        {},
        kEpoch).ok);
}

TEST(InputArbiter, NeutralWitnessesRejectFabricationWrongLeaseAndNonNeutralInput)
{
    FakeInputBackend backend;
    InputArbiter arbiter(backend);
    ASSERT_TRUE(arbiter.CommitStateEpoch(kEpoch).ok);
    InputLeaseReceipt parent = arbiter.Acquire(
        {
            .owner = InputOwnerId(1),
            .priority = 10,
            .suspendable = true,
            .interruption_borrowable = true,
            .borrow_policy =
                InputBorrowPolicy::RequireNeutralWitness},
        kEpoch);
    ASSERT_TRUE(parent.ok);

    GCInputFrame pressed;
    pressed.A();
    InputPublicationReceipt non_neutral =
        arbiter.Publish(parent.lease, pressed, kEpoch);
    ASSERT_TRUE(non_neutral.ok);
    backend.Poll();
    EXPECT_FALSE(
        arbiter.ProveNeutralWitness(
            parent.lease,
            non_neutral.publication,
            kEpoch).ok);

    InputPublicationReceipt neutral =
        arbiter.Publish(parent.lease, {}, kEpoch);
    ASSERT_TRUE(neutral.ok);
    backend.Poll();
    InputNeutralWitnessReceipt witness =
        arbiter.ProveNeutralWitness(
            parent.lease,
            neutral.publication,
            kEpoch);
    ASSERT_TRUE(witness.ok);
    const InputLeaseRequest borrower{
        .owner = InputOwnerId(3),
        .priority = 30};
    EXPECT_FALSE(
        arbiter.Borrow(
            parent.lease,
            borrower,
            kEpoch,
            InputNeutralWitnessId(9999)).ok);

    InputLeaseReceipt other_parent = arbiter.Acquire(
        {
            .owner = InputOwnerId(2),
            .priority = 20,
            .suspendable = true,
            .interruption_borrowable = true,
            .borrow_policy =
                InputBorrowPolicy::RequireNeutralWitness},
        kEpoch);
    ASSERT_TRUE(other_parent.ok);
    EXPECT_FALSE(
        arbiter.Borrow(
            other_parent.lease,
            borrower,
            kEpoch,
            witness.witness).ok);
}

TEST(InputArbiter, ImplementsExecutionEngineAdvancePortWithBoundedRetries)
{
    FakeInputBackend backend;
    InputArbiter arbiter(backend);
    ASSERT_TRUE(arbiter.CommitStateEpoch(kEpoch).ok);
    InputLeaseReceipt lease = arbiter.Acquire(
        {.owner = InputOwnerId(3), .priority = 10},
        kEpoch);
    ASSERT_TRUE(lease.ok);

    GCInputFrame frame;
    frame.B();
    InputAdvanceBindingReceipt binding_receipt =
        arbiter.CreateAdvanceBinding(lease.lease, {frame}, kEpoch, 1);
    ASSERT_TRUE(binding_receipt.ok) << binding_receipt.message;
    InputAdvanceBindingId binding = binding_receipt.binding;
    ASSERT_TRUE(binding);
    InputAdvanceReceipt prepared =
        arbiter.PrepareNext(binding, kEpoch, 0);
    ASSERT_TRUE(prepared.ok);
    EXPECT_EQ(
        arbiter.ObserveAcknowledgement(
            binding,
            prepared.publication,
            kEpoch).decision,
        InputAdvanceDecision::Retry);
    backend.Poll();
    EXPECT_EQ(
        arbiter.ObserveAcknowledgement(
            binding,
            prepared.publication,
            kEpoch).decision,
        InputAdvanceDecision::Complete);
}

TEST(InputArbiter, CompletedPublishReleaseCyclesDoNotRetainTombstones)
{
    FakeInputBackend backend;
    InputArbiter arbiter(backend);
    ASSERT_TRUE(arbiter.CommitStateEpoch(kEpoch).ok);

    for (std::uint64_t cycle = 1; cycle <= 512; ++cycle)
    {
        InputLeaseReceipt lease = arbiter.Acquire(
            {
                .owner = InputOwnerId(cycle),
                .priority = 10,
                .require_neutral_acknowledgement = false},
            kEpoch);
        ASSERT_TRUE(lease.ok) << lease.message;

        GCInputFrame frame;
        frame.A();
        for (std::size_t publication = 0; publication < 8; ++publication)
        {
            ASSERT_TRUE(arbiter.Publish(lease.lease, frame, kEpoch).ok);
            const InputArbiterSnapshot live = arbiter.snapshot();
            EXPECT_EQ(live.lease_count, 1u);
            EXPECT_EQ(live.publication_count, 1u);
        }

        InputAdvanceBindingReceipt binding =
            arbiter.CreateAdvanceBinding(lease.lease, {frame}, kEpoch, 1);
        ASSERT_TRUE(binding.ok) << binding.message;
        EXPECT_EQ(arbiter.snapshot().binding_count, 1u);

        InputReleaseReceipt released =
            arbiter.BeginRelease(lease.lease, kEpoch);
        ASSERT_TRUE(released.ok) << released.message;
        EXPECT_EQ(released.status, InputLeaseStatus::Released);
        const InputArbiterSnapshot empty = arbiter.snapshot();
        EXPECT_FALSE(empty.active_lease.has_value());
        EXPECT_EQ(empty.suspended_count, 0u);
        EXPECT_EQ(empty.lease_count, 0u);
        EXPECT_EQ(empty.publication_count, 0u);
        EXPECT_EQ(empty.binding_count, 0u);
    }
}

TEST(InputArbiter, EpochReplacementClearsAllSupersededState)
{
    FakeInputBackend backend;
    InputArbiter arbiter(backend);
    ASSERT_TRUE(arbiter.CommitStateEpoch(kEpoch).ok);
    const GCInputFrame neutral{};

    InputLeaseReceipt parent = arbiter.Acquire(
        {
            .owner = InputOwnerId(1),
            .priority = 10,
            .suspendable = true},
        kEpoch);
    ASSERT_TRUE(parent.ok);
    ASSERT_TRUE(arbiter.Publish(parent.lease, {}, kEpoch).ok);
    ASSERT_TRUE(
        arbiter.CreateAdvanceBinding(parent.lease, {neutral}, kEpoch).ok);

    InputLeaseReceipt child = arbiter.Acquire(
        {.owner = InputOwnerId(2), .priority = 20},
        kEpoch);
    ASSERT_TRUE(child.ok);
    ASSERT_TRUE(arbiter.Publish(child.lease, {}, kEpoch).ok);
    ASSERT_TRUE(
        arbiter.CreateAdvanceBinding(child.lease, {neutral}, kEpoch).ok);

    const InputArbiterSnapshot before = arbiter.snapshot();
    EXPECT_EQ(before.lease_count, 2u);
    EXPECT_EQ(before.publication_count, 2u);
    EXPECT_EQ(before.binding_count, 2u);
    EXPECT_EQ(before.suspended_count, 1u);
    const std::size_t publishes_before = backend.publish_calls;

    ASSERT_TRUE(
        arbiter.InvalidateForStateReplacement(StateEpoch(12)).ok);
    const InputArbiterSnapshot after = arbiter.snapshot();
    EXPECT_EQ(after.epoch, StateEpoch(12));
    EXPECT_FALSE(after.active_lease.has_value());
    EXPECT_EQ(after.suspended_count, 0u);
    EXPECT_EQ(after.lease_count, 0u);
    EXPECT_EQ(after.publication_count, 0u);
    EXPECT_EQ(after.binding_count, 0u);
    EXPECT_EQ(backend.publish_calls, publishes_before + 1);
    EXPECT_FALSE(arbiter.Publish(child.lease, {}, StateEpoch(12)).ok);
}

TEST(InputArbiter, RejectsEveryActorOwnedApiOffThreadWithoutMutation)
{
    FakeInputBackend backend;
    InputArbiter arbiter(backend);
    ASSERT_TRUE(arbiter.CommitStateEpoch(kEpoch).ok);
    InputLeaseReceipt lease = arbiter.Acquire(
        {
            .owner = InputOwnerId(1),
            .priority = 10,
            .suspendable = true,
            .interruption_borrowable = true,
            .require_neutral_acknowledgement = false},
        kEpoch);
    ASSERT_TRUE(lease.ok);
    InputPublicationReceipt publication =
        arbiter.Publish(lease.lease, {}, kEpoch);
    ASSERT_TRUE(publication.ok);
    const GCInputFrame neutral{};
    InputAdvanceBindingReceipt binding =
        arbiter.CreateAdvanceBinding(lease.lease, {neutral}, kEpoch);
    ASSERT_TRUE(binding.ok);

    const InputArbiterSnapshot before = arbiter.snapshot();
    const std::size_t availability_before = backend.availability_queries;
    const std::size_t publishes_before = backend.publish_calls;
    const std::size_t polls_before = backend.poll_queries;

    InputLeaseReceipt acquired;
    InputLeaseReceipt borrowed;
    InputPublicationReceipt published;
    InputAcknowledgementReceipt observed;
    InputNeutralWitnessReceipt neutral_witness;
    InputReleaseReceipt began_release;
    InputReleaseReceipt completed_release;
    InputAdvanceBindingReceipt created_binding;
    InputArbiterOperationReceipt removed_binding;
    InputArbiterOperationReceipt committed_epoch;
    InputArbiterOperationReceipt invalidated_epoch;
    InputAdvanceReceipt validated;
    InputAdvanceReceipt prepared;
    InputAdvanceReceipt acknowledged;
    InputAdvanceReceipt cancelled;
    InputArbiterShutdownReceipt shutdown;

    std::thread off_actor([&] {
        acquired = arbiter.Acquire(
            {.owner = InputOwnerId(2), .priority = 20},
            kEpoch);
        borrowed = arbiter.Borrow(
            lease.lease,
            {.owner = InputOwnerId(3), .priority = 30},
            kEpoch,
            InputNeutralWitnessId(1));
        published = arbiter.Publish(lease.lease, {}, kEpoch);
        observed = arbiter.Observe(
            lease.lease,
            publication.publication,
            kEpoch);
        neutral_witness = arbiter.ProveNeutralWitness(
            lease.lease,
            publication.publication,
            kEpoch);
        began_release = arbiter.BeginRelease(lease.lease, kEpoch);
        completed_release = arbiter.CompleteRelease(
            lease.lease,
            publication.publication,
            kEpoch);
        created_binding =
            arbiter.CreateAdvanceBinding(
                lease.lease,
                std::vector<GCInputFrame>{neutral},
                kEpoch);
        removed_binding =
            arbiter.RemoveAdvanceBinding(binding.binding);
        committed_epoch = arbiter.CommitStateEpoch(StateEpoch(12));
        invalidated_epoch =
            arbiter.InvalidateForStateReplacement(StateEpoch(12));
        validated = arbiter.Validate(binding.binding, kEpoch);
        prepared = arbiter.PrepareNext(binding.binding, kEpoch, 0);
        acknowledged = arbiter.ObserveAcknowledgement(
            binding.binding,
            publication.publication,
            kEpoch);
        cancelled = arbiter.Cancel(binding.binding, kEpoch);
        shutdown = arbiter.Shutdown();
    });
    off_actor.join();

    EXPECT_EQ(acquired.error, InputArbiterErrorCode::WrongThread);
    EXPECT_EQ(borrowed.error, InputArbiterErrorCode::WrongThread);
    EXPECT_EQ(published.error, InputArbiterErrorCode::WrongThread);
    EXPECT_EQ(observed.error, InputArbiterErrorCode::WrongThread);
    EXPECT_EQ(
        neutral_witness.error,
        InputArbiterErrorCode::WrongThread);
    EXPECT_EQ(began_release.error, InputArbiterErrorCode::WrongThread);
    EXPECT_EQ(completed_release.error, InputArbiterErrorCode::WrongThread);
    EXPECT_EQ(created_binding.error, InputArbiterErrorCode::WrongThread);
    EXPECT_EQ(removed_binding.error, InputArbiterErrorCode::WrongThread);
    EXPECT_EQ(committed_epoch.error, InputArbiterErrorCode::WrongThread);
    EXPECT_EQ(invalidated_epoch.error, InputArbiterErrorCode::WrongThread);
    EXPECT_FALSE(validated.ok);
    EXPECT_FALSE(prepared.ok);
    EXPECT_FALSE(acknowledged.ok);
    EXPECT_FALSE(cancelled.ok);
    EXPECT_EQ(shutdown.error, InputArbiterErrorCode::WrongThread);

    const InputArbiterSnapshot after = arbiter.snapshot();
    EXPECT_EQ(after.epoch, before.epoch);
    EXPECT_EQ(after.active_lease, before.active_lease);
    EXPECT_EQ(after.suspended_count, before.suspended_count);
    EXPECT_EQ(after.lease_count, before.lease_count);
    EXPECT_EQ(after.publication_count, before.publication_count);
    EXPECT_EQ(after.binding_count, before.binding_count);
    EXPECT_FALSE(after.stopped);
    EXPECT_EQ(backend.availability_queries, availability_before);
    EXPECT_EQ(backend.publish_calls, publishes_before);
    EXPECT_EQ(backend.poll_queries, polls_before);
}

TEST(InputArbiter, ShutdownIsIdempotentAndClearsRetainedState)
{
    FakeInputBackend backend;
    InputArbiter arbiter(backend);
    ASSERT_TRUE(arbiter.CommitStateEpoch(kEpoch).ok);
    InputLeaseReceipt lease = arbiter.Acquire(
        {
            .owner = InputOwnerId(1),
            .require_neutral_acknowledgement = false},
        kEpoch);
    ASSERT_TRUE(lease.ok);
    ASSERT_TRUE(arbiter.Publish(lease.lease, {}, kEpoch).ok);
    const GCInputFrame neutral{};
    ASSERT_TRUE(
        arbiter.CreateAdvanceBinding(lease.lease, {neutral}, kEpoch).ok);

    const std::size_t before = backend.publish_calls;
    const InputArbiterShutdownReceipt first = arbiter.Shutdown();
    ASSERT_TRUE(first.ok) << first.message;
    EXPECT_TRUE(first.cleanup_complete);
    EXPECT_FALSE(first.taint_required);
    EXPECT_TRUE(first.neutral_publication);
    EXPECT_EQ(backend.publish_calls, before + 1);
    const InputArbiterSnapshot stopped = arbiter.snapshot();
    EXPECT_TRUE(stopped.stopped);
    EXPECT_EQ(stopped.lease_count, 0u);
    EXPECT_EQ(stopped.publication_count, 0u);
    EXPECT_EQ(stopped.binding_count, 0u);

    const InputArbiterShutdownReceipt second = arbiter.Shutdown();
    EXPECT_EQ(second.ok, first.ok);
    EXPECT_EQ(second.cleanup_complete, first.cleanup_complete);
    EXPECT_EQ(second.neutral_publication, first.neutral_publication);
    EXPECT_EQ(backend.publish_calls, before + 1);
    EXPECT_EQ(
        arbiter.Acquire({.owner = InputOwnerId(2)}, kEpoch).error,
        InputArbiterErrorCode::Stopped);
}

TEST(InputArbiter, ShutdownReportsWhetherNeutralAcknowledgementWasProven)
{
    {
        FakeInputBackend backend;
        InputArbiter arbiter(backend);
        ASSERT_TRUE(arbiter.CommitStateEpoch(kEpoch).ok);
        InputLeaseReceipt lease = arbiter.Acquire(
            {.owner = InputOwnerId(1)},
            kEpoch);
        ASSERT_TRUE(lease.ok);

        const InputArbiterShutdownReceipt incomplete = arbiter.Shutdown();
        EXPECT_FALSE(incomplete.ok);
        EXPECT_FALSE(incomplete.cleanup_complete);
        EXPECT_TRUE(incomplete.taint_required);
        EXPECT_EQ(arbiter.snapshot().lease_count, 0u);
    }

    {
        FakeInputBackend backend;
        InputArbiter arbiter(backend);
        ASSERT_TRUE(arbiter.CommitStateEpoch(kEpoch).ok);
        InputLeaseReceipt lease = arbiter.Acquire(
            {.owner = InputOwnerId(2)},
            kEpoch);
        ASSERT_TRUE(lease.ok);
        const InputReleaseReceipt pending =
            arbiter.BeginRelease(lease.lease, kEpoch);
        ASSERT_TRUE(pending.ok);
        ASSERT_EQ(
            pending.status,
            InputLeaseStatus::AwaitingNeutralAcknowledgement);
        backend.Poll();
        const std::size_t publishes_before = backend.publish_calls;

        const InputArbiterShutdownReceipt complete = arbiter.Shutdown();
        EXPECT_TRUE(complete.ok) << complete.message;
        EXPECT_TRUE(complete.cleanup_complete);
        EXPECT_FALSE(complete.taint_required);
        EXPECT_EQ(
            complete.neutral_publication,
            pending.neutral_publication);
        EXPECT_EQ(backend.publish_calls, publishes_before);
        EXPECT_EQ(arbiter.snapshot().lease_count, 0u);
    }
}

TEST(GuestMutationService, AppliesNestedCheckedWritesAndRestoresInReverse)
{
    FakeGuestMemoryBackend backend;
    backend.PutU32(0x80001000, 0x11223344);
    GuestMemory memory(backend);
    GuestMutationService mutations(memory, backend);
    mutations.CommitStateEpoch(kEpoch);

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
    mutations.CommitStateEpoch(kEpoch);

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
    mutations.CommitStateEpoch(kEpoch);

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
    mutations.CommitStateEpoch(kEpoch);

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
    mutations.CommitStateEpoch(kEpoch);
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

TEST(GuestMutationService, StateReplacementSupersedesWithoutWritingOldBytes)
{
    FakeGuestMemoryBackend backend;
    backend.PutU32(0x803469a8, 0x12345678);
    GuestMemory memory(backend);
    GuestMutationService mutations(memory, backend);
    mutations.CommitStateEpoch(kEpoch);
    GuestMutationReceipt write = mutations.Apply(
        {
            .owner = MutationOwnerId(1),
            .scope = MutationScopeId(1),
            .epoch = kEpoch,
            .address = 0x803469a8,
            .expected = 0x12345678,
            .replacement = 0x87654321});
    ASSERT_TRUE(write.ok);
    const auto receipts = mutations.SupersedeForStateReplacement(kEpoch);
    ASSERT_EQ(receipts.size(), 1u);
    EXPECT_EQ(
        receipts.front().status,
        GuestMutationStatus::SupersededByStateReplacement);
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
    mutations.CommitStateEpoch(kEpoch);

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
    mutations.CommitStateEpoch(kEpoch);
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
    memory.CommitStateEpoch(kEpoch);

    GuestReadReceipt read;
    GuestBytesResult bytes;
    std::thread other([&] {
        memory.CommitStateEpoch(StateEpoch(12));
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
    memory.CommitStateEpoch(kEpoch);
    mutations.CommitStateEpoch(kEpoch);

    GuestMutationReceipt applied;
    GuestMutationReceipt restored;
    GuestMutationReceipt committed;
    std::vector<GuestMutationReceipt> superseded;
    std::vector<GuestMutationReceipt> scope_restored;
    GuestMutationCleanupReceipt restored_all;
    GuestMutationCleanupReceipt shutdown;
    std::thread other([&] {
        mutations.CommitStateEpoch(StateEpoch(12));
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
        superseded =
            mutations.SupersedeForStateReplacement(kEpoch);
        scope_restored =
            mutations.RestoreScope(MutationScopeId(1), kEpoch);
        restored_all = mutations.RestoreAll();
        shutdown = mutations.Shutdown();
    });
    other.join();

    EXPECT_FALSE(applied.ok);
    EXPECT_FALSE(restored.ok);
    EXPECT_FALSE(committed.ok);
    ASSERT_EQ(superseded.size(), 1u);
    EXPECT_FALSE(superseded.front().ok);
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
    screenshots.CommitStateEpoch(kEpoch);

    ScreenshotReceipt captured;
    ScreenshotReceipt cancelled;
    std::optional<ScreenshotReceipt> last;
    std::thread other([&] {
        screenshots.CommitStateEpoch(StateEpoch(12));
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
    service.CommitStateEpoch(kEpoch);
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

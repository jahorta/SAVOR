#pragma once

#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace savor::runtime {

// Session-owned accounting for synchronous host work that can temporarily
// prevent guest advancement. The CPU hook path uses only a bounded number of
// lock-free atomic operations: entering or leaving an activity never allocates,
// locks, blocks, waits, or throws.
class HostActivityTracker final
{
    struct ActivityToken
    {
        std::uint32_t sequence = 0;
        std::uint64_t started = 0;
    };

public:
    using TickSource = std::uint64_t (*)(void*) noexcept;
    static constexpr std::size_t kCompletedActivityCapacity = 64;

    struct Snapshot
    {
        std::uint64_t generation = 0;
        std::uint32_t in_flight = 0;
    };

    struct CompletedActivity
    {
        std::uint32_t sequence = 0;
        std::chrono::milliseconds elapsed{};
    };

    struct CompletedActivityBatch
    {
        std::array<
            CompletedActivity,
            kCompletedActivityCapacity> activities{};
        std::size_t count = 0;
        std::uint64_t overflow_count = 0;
    };

    class Scope final
    {
    public:
        Scope() = default;
        explicit Scope(HostActivityTracker* tracker) noexcept
            : tracker_(tracker)
        {
            if (tracker_)
                token_ = tracker_->Enter();
        }

        ~Scope()
        {
            Reset();
        }

        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;

        Scope(Scope&& other) noexcept
            : tracker_(other.tracker_),
              token_(other.token_)
        {
            other.tracker_ = nullptr;
            other.token_ = {};
        }

        Scope& operator=(Scope&& other) noexcept
        {
            if (this != &other)
            {
                Reset();
                tracker_ = other.tracker_;
                token_ = other.token_;
                other.tracker_ = nullptr;
                other.token_ = {};
            }
            return *this;
        }

        void Reset() noexcept
        {
            if (!tracker_)
                return;
            tracker_->Leave(token_);
            tracker_ = nullptr;
            token_ = {};
        }

    private:
        HostActivityTracker* tracker_ = nullptr;
        ActivityToken token_;
    };

    explicit HostActivityTracker(
        TickSource ticks = nullptr,
        void* tick_context = nullptr) noexcept
        : ticks_(ticks ? ticks : &DefaultTicks),
          tick_context_(tick_context)
    {
        for (std::atomic<std::uint64_t>& slot : completed_)
            slot.store(0, std::memory_order_relaxed);
    }

    [[nodiscard]] Scope Track() noexcept
    {
        return Scope(this);
    }

    [[nodiscard]] Snapshot snapshot() const noexcept
    {
        const std::uint64_t state =
            state_.load(std::memory_order_acquire);
        return {
            state >> kInFlightBits,
            static_cast<std::uint32_t>(state & kInFlightMask)};
    }

    // Actor-side diagnostic drain. Producers publish each completed activity
    // independently into a bounded lock-free set of slots. The actor sorts the
    // bounded batch by activity sequence so warning order never depends on
    // producer timing or slot selection.
    [[nodiscard]] CompletedActivityBatch
    DrainCompletedActivities() noexcept
    {
        CompletedActivityBatch batch;
        for (std::atomic<std::uint64_t>& slot : completed_)
        {
            const std::uint64_t record =
                slot.exchange(0, std::memory_order_acq_rel);
            if (record == 0)
                continue;
            batch.activities[batch.count++] = {
                static_cast<std::uint32_t>(
                    record & kCompletedSequenceMask),
                std::chrono::milliseconds(
                    static_cast<std::uint32_t>(
                        record >> kCompletedSequenceBits))};
        }
        batch.overflow_count =
            completion_overflow_.exchange(
                0,
                std::memory_order_acq_rel);

        for (std::size_t i = 1; i < batch.count; ++i)
        {
            const CompletedActivity current = batch.activities[i];
            std::size_t position = i;
            while (position != 0 &&
                   batch.activities[position - 1].sequence >
                       current.sequence)
            {
                batch.activities[position] =
                    batch.activities[position - 1];
                --position;
            }
            batch.activities[position] = current;
        }
        return batch;
    }

private:
    static constexpr std::uint64_t kInFlightBits = 16;
    static constexpr std::uint64_t kInFlightMask =
        (std::uint64_t{1} << kInFlightBits) - 1;
    static constexpr std::uint64_t kGenerationIncrement =
        std::uint64_t{1} << kInFlightBits;
    static constexpr std::uint64_t kCompletedSequenceBits = 32;
    static constexpr std::uint64_t kCompletedSequenceMask =
        std::numeric_limits<std::uint32_t>::max();

    [[nodiscard]] static std::uint64_t DefaultTicks(void*) noexcept
    {
        return static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count());
    }

    [[nodiscard]] std::uint64_t NowTicks() const noexcept
    {
        return ticks_(tick_context_);
    }

    [[nodiscard]] static std::uint32_t ClampDuration(
        std::uint64_t ticks) noexcept
    {
        const std::uint64_t maximum =
            std::numeric_limits<std::uint32_t>::max();
        return static_cast<std::uint32_t>(
            ticks > maximum ? maximum : ticks);
    }

    [[nodiscard]] std::uint32_t NextActivitySequence() noexcept
    {
        std::uint32_t sequence =
            next_activity_sequence_.fetch_add(
                1,
                std::memory_order_relaxed) +
            1;
        if (sequence == 0)
        {
            sequence =
                next_activity_sequence_.fetch_add(
                    1,
                    std::memory_order_relaxed) +
                1;
        }
        return sequence;
    }

    [[nodiscard]] ActivityToken Enter() noexcept
    {
        ActivityToken token{
            NextActivitySequence(),
            NowTicks()};
        const std::uint64_t prior = state_.fetch_add(
            kGenerationIncrement + 1,
            std::memory_order_acq_rel);
        assert((prior & kInFlightMask) != kInFlightMask);
        return token;
    }

    void PublishCompletion(
        std::uint32_t sequence,
        std::uint32_t elapsed) noexcept
    {
        const std::uint64_t record =
            (static_cast<std::uint64_t>(elapsed)
             << kCompletedSequenceBits) |
            sequence;
        const std::size_t first =
            sequence % kCompletedActivityCapacity;
        for (std::size_t offset = 0;
             offset < kCompletedActivityCapacity;
             ++offset)
        {
            std::uint64_t empty = 0;
            if (completed_[
                    (first + offset) %
                    kCompletedActivityCapacity]
                    .compare_exchange_strong(
                        empty,
                        record,
                        std::memory_order_release,
                        std::memory_order_relaxed))
            {
                return;
            }
        }
        completion_overflow_.fetch_add(
            1,
            std::memory_order_relaxed);
    }

    void Leave(ActivityToken token) noexcept
    {
        const std::uint64_t ended = NowTicks();
        PublishCompletion(
            token.sequence,
            ClampDuration(
                ended >= token.started
                    ? ended - token.started
                    : 0));
        const std::uint64_t prior = state_.fetch_add(
            kGenerationIncrement - 1,
            std::memory_order_acq_rel);
        assert((prior & kInFlightMask) != 0);
    }

    static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
    static_assert(std::atomic<std::uint32_t>::is_always_lock_free);
    TickSource ticks_;
    void* tick_context_ = nullptr;
    std::atomic<std::uint64_t> state_{kGenerationIncrement};
    std::array<
        std::atomic<std::uint64_t>,
        kCompletedActivityCapacity> completed_{};
    std::atomic<std::uint64_t> completion_overflow_{0};
    std::atomic<std::uint32_t> next_activity_sequence_{0};
};

} // namespace savor::runtime

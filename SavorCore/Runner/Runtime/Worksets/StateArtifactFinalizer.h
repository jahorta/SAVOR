#pragma once

#include "WorksetTypes.h"

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

namespace savor::runtime {

struct StateArtifactFinalizationIdTag;
using StateArtifactFinalizationId =
    StrongId<StateArtifactFinalizationIdTag>;

enum class StateArtifactFinalizerErrorCode : std::uint16_t
{
    None,
    InvalidArgument,
    NotAccepting,
    CapacityExceeded,
    HashMismatch,
    FilesystemFailure,
    IntegrityFailure,
    SequenceExhausted,
};

struct StateArtifactFinalizerResult
{
    bool ok = false;
    StateArtifactFinalizerErrorCode code =
        StateArtifactFinalizerErrorCode::FilesystemFailure;
    std::string message;

    [[nodiscard]] static StateArtifactFinalizerResult Success()
    {
        return {true, StateArtifactFinalizerErrorCode::None, {}};
    }

    [[nodiscard]] static StateArtifactFinalizerResult Failure(
        StateArtifactFinalizerErrorCode code,
        std::string message)
    {
        return {false, code, std::move(message)};
    }
};

using ImmutableArtifactBytes = ImmutableStateBytes;

struct ImmutableArtifactFile
{
    std::filesystem::path final_path;
    ImmutableArtifactBytes bytes;
    // Empty means compute the authoritative digest from the immutable bytes.
    std::string expected_sha256;
};

struct StateArtifactFinalizationRequest
{
    // Preferred pre-terminal identity. It is sufficient to finalize output
    // while ordinary invocation unwind is still establishing its terminal.
    WorkerItemExecutionCorrelation item;
    // Optional later binding retained in the request/completion so the
    // completion ledger can correlate a reservation made before Submit.
    WorkerItemTerminalCorrelation terminal;
    StateArtifactId state_artifact_id;
    std::string logical_artifact_id;
    ImmutableArtifactFile state;
    // Sidecars publish before the state file. Every file is immutable: an
    // existing exact file is reused and a conflicting file is never replaced.
    std::vector<ImmutableArtifactFile> sidecars;
};

struct FinalizedArtifactFile
{
    std::filesystem::path path;
    std::size_t size_bytes = 0;
    std::string sha256;
    bool reused_existing = false;
};

struct StateArtifactFinalizationCompletion
{
    StateArtifactFinalizationId finalization_id;
    WorkerItemExecutionCorrelation item;
    WorkerItemTerminalCorrelation terminal;
    StateArtifactId state_artifact_id;
    std::string logical_artifact_id;
    StateArtifactFinalizerResult result;
    FinalizedArtifactFile state;
    std::vector<FinalizedArtifactFile> sidecars;
};

struct StateArtifactFinalizerSubmission
{
    StateArtifactFinalizerResult result;
    StateArtifactFinalizationId finalization_id;
};

struct StateArtifactFinalizerSnapshot
{
    bool accepting = false;
    bool shutdown = false;
    std::size_t queued = 0;
    std::size_t running = 0;
    std::size_t completed_undrained = 0;
    std::size_t outstanding_jobs = 0;
    std::size_t resident_payload_bytes = 0;
};

// The notifier is deliberately weaker than a completion callback: its only
// permitted role is waking the WorkerRuntime actor. Finalizer threads place the
// complete value in the internal queue before invoking this noexcept seam.
class IStateArtifactFinalizerNotifier
{
public:
    virtual ~IStateArtifactFinalizerNotifier() = default;
    virtual void NotifyStateArtifactFinalizerCompletion() noexcept = 0;
};

// Host-only bounded publication of already-captured immutable bytes. Jobs do
// not contain EmulationSession, Dolphin, StateService, or arbitrary callbacks,
// so finalizer threads cannot mutate guest/session state.
class StateArtifactFinalizer final
{
public:
    explicit StateArtifactFinalizer(
        const WorkerWorksetLimits& limits = {},
        std::shared_ptr<IStateArtifactFinalizerNotifier> notifier = {});
    ~StateArtifactFinalizer();

    StateArtifactFinalizer(const StateArtifactFinalizer&) = delete;
    StateArtifactFinalizer& operator=(const StateArtifactFinalizer&) = delete;

    [[nodiscard]] StateArtifactFinalizerSubmission Submit(
        StateArtifactFinalizationRequest request);

    // May be called by the actor after a notifier wake. Draining frees the
    // corresponding count credit; payload byte credit is freed as soon as a
    // worker finishes because completions retain receipts, not captured bytes.
    [[nodiscard]] std::vector<StateArtifactFinalizationCompletion>
        DrainCompletions();

    // Stops admission, lets every accepted job reach a completion, and joins
    // all finalizer threads. It is idempotent and does not discard completions.
    void Shutdown() noexcept;

    [[nodiscard]] StateArtifactFinalizerSnapshot snapshot() const noexcept;

private:
    struct Job
    {
        StateArtifactFinalizationId id;
        StateArtifactFinalizationRequest request;
        std::size_t resident_bytes = 0;
    };

    void WorkerMain() noexcept;
    [[nodiscard]] StateArtifactFinalizationCompletion Process(
        const Job& job) const noexcept;

    WorkerWorksetLimits limits_;
    std::shared_ptr<IStateArtifactFinalizerNotifier> notifier_;
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::condition_variable stopped_;
    bool accepting_ = true;
    bool shutdown_ = false;
    bool joining_ = false;
    bool joined_ = false;
    std::uint64_t next_finalization_id_ = 1;
    std::size_t running_ = 0;
    std::size_t outstanding_jobs_ = 0;
    std::size_t resident_payload_bytes_ = 0;
    std::deque<Job> jobs_;
    std::deque<StateArtifactFinalizationCompletion> completions_;
    std::unordered_set<std::uint64_t> outstanding_ids_;
    // StateArtifactId is session-owned and unique for each captured output.
    // Multiple independent captures from one invocation/item are allowed.
    std::unordered_set<std::uint64_t> outstanding_artifact_ids_;
    std::vector<std::thread> threads_;
};

} // namespace savor::runtime

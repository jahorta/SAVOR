#pragma once

#include "ProgramBaseline.h"

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace savor::runtime {

struct WorksetStagingIdTag;
using WorksetStagingId = StrongId<WorksetStagingIdTag>;

enum class WorksetStagerErrorCode : std::uint16_t
{
    None,
    InvalidArgument,
    CapacityExceeded,
    NotAccepting,
    ArtifactFailure,
    ComponentFailure,
    SequenceExhausted,
};

struct WorksetStagerResult
{
    bool ok = false;
    WorksetStagerErrorCode code =
        WorksetStagerErrorCode::ArtifactFailure;
    std::string message;

    [[nodiscard]] static WorksetStagerResult Success()
    {
        return {true, WorksetStagerErrorCode::None, {}};
    }

    [[nodiscard]] static WorksetStagerResult Failure(
        WorksetStagerErrorCode code,
        std::string message)
    {
        return {false, code, std::move(message)};
    }
};

struct HostStagedArtifactEvidence
{
    std::size_t state_bytes = 0;
    std::size_t movie_bytes = 0;
    std::string state_sha256;
    std::string movie_sha256;
};

// Immutable host-only package. It has no SessionId/StateEpoch binding and
// carries no resource lease or authority to prepare/advance Dolphin.
struct HostStagedWorksetPackage
{
    WorkerWorksetDefinition definition;
    ProgramBaselineKey baseline_key;
    std::optional<HostStagedArtifactEvidence> artifact;
};

struct WorksetStagingSubmission
{
    WorksetStagerResult result;
    WorksetStagingId staging_id;
};

struct WorksetStagingCompletion
{
    WorksetStagingId staging_id;
    WorkerWorksetId workset_id;
    WorksetStagerResult result;
    std::optional<HostStagedWorksetPackage> package;
};

struct WorksetStagerSnapshot
{
    bool accepting = false;
    bool shutdown = false;
    bool queued = false;
    bool running = false;
    bool completion_undrained = false;
};

// Weaker than a callback: the host thread stores its immutable completion
// first, then may only wake the WorkerRuntime actor through this noexcept seam.
class IWorksetStagerNotifier
{
public:
    virtual ~IWorksetStagerNotifier() = default;
    virtual void NotifyWorksetStagingCompletion() noexcept = 0;
};

// One-job host-only stager used for the single allowed successor package.
// Artifact and component bytes are read/hashed away from the actor. It owns no
// EmulationSession, backend, resource ledger, or program-execution surface.
class WorksetStager final
{
public:
    explicit WorksetStager(
        WorkerWorksetLimits limits = {},
        std::shared_ptr<ProgramBaselineComponentRegistry> components = {},
        std::shared_ptr<IWorksetStagerNotifier> notifier = {});
    ~WorksetStager();

    WorksetStager(const WorksetStager&) = delete;
    WorksetStager& operator=(const WorksetStager&) = delete;

    [[nodiscard]] WorksetStagingSubmission Submit(
        WorkerWorksetDefinition definition);
    [[nodiscard]] std::vector<WorksetStagingCompletion>
        DrainCompletions();
    void Shutdown() noexcept;
    [[nodiscard]] WorksetStagerSnapshot snapshot() const noexcept;

private:
    struct Job
    {
        WorksetStagingId id;
        WorkerWorksetDefinition definition;
    };

    void WorkerMain() noexcept;
    [[nodiscard]] WorksetStagingCompletion Process(Job job) const noexcept;

    WorkerWorksetLimits limits_;
    std::shared_ptr<ProgramBaselineComponentRegistry> components_;
    std::shared_ptr<IWorksetStagerNotifier> notifier_;
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::condition_variable stopped_;
    bool accepting_ = true;
    bool shutdown_ = false;
    bool joining_ = false;
    bool joined_ = false;
    bool running_ = false;
    std::uint64_t next_staging_id_ = 1;
    std::optional<Job> queued_;
    std::deque<WorksetStagingCompletion> completions_;
    std::thread thread_;
};

} // namespace savor::runtime

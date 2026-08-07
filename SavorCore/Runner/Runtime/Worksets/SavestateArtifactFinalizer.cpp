#include "SavestateArtifactFinalizer.h"

#include "Utils/Hash.h"

#include <algorithm>
#include <atomic>
#include <cwctype>
#include <fstream>
#include <limits>
#include <system_error>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#endif

namespace savor::runtime {
namespace {

std::atomic<std::uint64_t> g_staging_nonce{1};

[[nodiscard]] bool CompleteSha256(const std::string& digest) noexcept
{
    if (digest.size() != 64)
        return false;
    return std::all_of(
        digest.begin(),
        digest.end(),
        [](char value) {
            return (value >= '0' && value <= '9') ||
                (value >= 'a' && value <= 'f');
        });
}

[[nodiscard]] bool CompleteExecutionCorrelation(
    const WorkerItemExecutionCorrelation& correlation) noexcept
{
    return static_cast<bool>(correlation);
}

[[nodiscard]] bool HasTerminalBinding(
    const WorkerItemTerminalCorrelation& correlation) noexcept
{
    return correlation.terminal_id || correlation.terminal_order;
}

[[nodiscard]] bool CompleteTerminalBinding(
    const WorkerItemTerminalCorrelation& correlation) noexcept
{
    return CompleteExecutionCorrelation(
               ExecutionCorrelation(correlation)) &&
        correlation.terminal_id && correlation.terminal_order;
}

[[nodiscard]] bool AddSize(
    std::size_t value,
    std::size_t& total) noexcept
{
    if (value > std::numeric_limits<std::size_t>::max() - total)
        return false;
    total += value;
    return true;
}

[[nodiscard]] std::filesystem::path StagingPath(
    const std::filesystem::path& final_path,
    SavestateArtifactFinalizationId id,
    std::size_t ordinal,
    std::uint64_t nonce)
{
    std::filesystem::path staging = final_path;
#ifdef _WIN32
    const std::uint64_t process =
        static_cast<std::uint64_t>(::GetCurrentProcessId());
#else
    const std::uint64_t process = 0;
#endif
    staging +=
        ".savor-finalize-" + std::to_string(process) + "-" +
        std::to_string(id.value()) + "-" + std::to_string(ordinal) +
        "-" + std::to_string(nonce) + ".tmp";
    return staging;
}

[[nodiscard]] std::wstring PathIdentity(
    const std::filesystem::path& path)
{
    std::error_code error;
    std::filesystem::path normalized =
        std::filesystem::absolute(path, error);
    if (error)
    {
        error.clear();
        normalized = path;
    }
    normalized = normalized.lexically_normal();

    if (std::filesystem::exists(normalized, error) && !error)
    {
        const std::filesystem::path canonical =
            std::filesystem::weakly_canonical(normalized, error);
        if (!error)
            normalized = canonical;
    }
    else
    {
        error.clear();
        const std::filesystem::path parent =
            normalized.parent_path();
        if (!parent.empty())
        {
            const std::filesystem::path canonical_parent =
                std::filesystem::weakly_canonical(parent, error);
            if (!error)
                normalized = canonical_parent / normalized.filename();
        }
    }

    std::wstring identity = normalized.lexically_normal().wstring();
#ifdef _WIN32
    std::transform(
        identity.begin(),
        identity.end(),
        identity.begin(),
        [](wchar_t value) {
            return static_cast<wchar_t>(std::towlower(value));
        });
#endif
    return identity;
}

struct FilePublication
{
    SavestateArtifactFinalizerResult result;
    FinalizedArtifactFile receipt;
};

struct StagingWrite
{
    SavestateArtifactFinalizerResult result;
    std::filesystem::path path;
};

[[nodiscard]] StagingWrite WriteDurableStaging(
    const ImmutableArtifactFile& file,
    SavestateArtifactFinalizationId id,
    std::size_t ordinal,
    const std::string& digest)
{
#ifdef _WIN32
    HANDLE handle = INVALID_HANDLE_VALUE;
    std::filesystem::path staging;
    DWORD open_error = ERROR_SUCCESS;
    for (std::uint32_t attempt = 0; attempt < 64; ++attempt)
    {
        const std::uint64_t nonce =
            g_staging_nonce.fetch_add(1, std::memory_order_relaxed);
        staging = StagingPath(file.final_path, id, ordinal, nonce);
        handle = ::CreateFileW(
            staging.c_str(),
            GENERIC_WRITE,
            0,
            nullptr,
            CREATE_NEW,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH,
            nullptr);
        if (handle != INVALID_HANDLE_VALUE)
            break;
        open_error = ::GetLastError();
        if (open_error != ERROR_FILE_EXISTS &&
            open_error != ERROR_ALREADY_EXISTS)
        {
            return {
                SavestateArtifactFinalizerResult::Failure(
                    SavestateArtifactFinalizerErrorCode::FilesystemFailure,
                    "Unable to create immutable artifact staging file: " +
                        std::system_category().message(open_error)),
                {}};
        }
    }
    if (handle == INVALID_HANDLE_VALUE)
    {
        return {
            SavestateArtifactFinalizerResult::Failure(
                SavestateArtifactFinalizerErrorCode::FilesystemFailure,
                "Unable to allocate a unique immutable artifact staging "
                "file"),
            {}};
    }

    bool write_ok = true;
    std::size_t offset = 0;
    while (offset < file.bytes.size())
    {
        const std::size_t remaining = file.bytes.size() - offset;
        const DWORD requested = static_cast<DWORD>(
            std::min<std::size_t>(
                remaining,
                std::numeric_limits<DWORD>::max()));
        DWORD written = 0;
        if (!::WriteFile(
                handle,
                file.bytes.data() + offset,
                requested,
                &written,
                nullptr) ||
            written != requested)
        {
            write_ok = false;
            open_error = ::GetLastError();
            break;
        }
        offset += written;
    }
    if (write_ok && !::FlushFileBuffers(handle))
    {
        write_ok = false;
        open_error = ::GetLastError();
    }
    if (!::CloseHandle(handle) && write_ok)
    {
        write_ok = false;
        open_error = ::GetLastError();
    }
    if (!write_ok)
    {
        ::DeleteFileW(staging.c_str());
        return {
            SavestateArtifactFinalizerResult::Failure(
                SavestateArtifactFinalizerErrorCode::FilesystemFailure,
                "Unable to durably write immutable artifact staging file: " +
                    std::system_category().message(open_error)),
            {}};
    }
#else
    const std::uint64_t nonce =
        g_staging_nonce.fetch_add(1, std::memory_order_relaxed);
    const std::filesystem::path staging =
        StagingPath(file.final_path, id, ordinal, nonce);
    {
        std::ofstream output(
            staging,
            std::ios::binary | std::ios::trunc);
        if (!output)
        {
            return {
                SavestateArtifactFinalizerResult::Failure(
                    SavestateArtifactFinalizerErrorCode::FilesystemFailure,
                    "Unable to open immutable artifact staging file"),
                {}};
        }
        if (file.bytes.size() != 0)
        {
            output.write(
                reinterpret_cast<const char*>(file.bytes.data()),
                static_cast<std::streamsize>(file.bytes.size()));
        }
        output.flush();
        if (!output)
        {
            output.close();
            std::error_code ignored;
            std::filesystem::remove(staging, ignored);
            return {
                SavestateArtifactFinalizerResult::Failure(
                    SavestateArtifactFinalizerErrorCode::FilesystemFailure,
                    "Unable to write immutable artifact staging file"),
                {}};
        }
    }
#endif

    try
    {
        if (hash::sha256_of_file(staging.string()) != digest)
        {
            std::error_code ignored;
            std::filesystem::remove(staging, ignored);
            return {
                SavestateArtifactFinalizerResult::Failure(
                    SavestateArtifactFinalizerErrorCode::IntegrityFailure,
                    "Immutable artifact staging validation failed"),
                {}};
        }
    }
    catch (const std::exception& exception)
    {
        std::error_code ignored;
        std::filesystem::remove(staging, ignored);
        return {
            SavestateArtifactFinalizerResult::Failure(
                SavestateArtifactFinalizerErrorCode::FilesystemFailure,
                exception.what()),
            {}};
    }
    return {SavestateArtifactFinalizerResult::Success(), staging};
}

[[nodiscard]] FilePublication PublishImmutableFile(
    const ImmutableArtifactFile& file,
    SavestateArtifactFinalizationId id,
    std::size_t ordinal)
{
    if (file.final_path.empty() || !file.bytes)
    {
        return {
            SavestateArtifactFinalizerResult::Failure(
                SavestateArtifactFinalizerErrorCode::InvalidArgument,
                "Immutable artifact publication requires a destination and "
                "captured bytes"),
            {}};
    }

    const std::string digest =
        hash::sha256(file.bytes.data(), file.bytes.size());
    if (!CompleteSha256(digest))
    {
        return {
            SavestateArtifactFinalizerResult::Failure(
                SavestateArtifactFinalizerErrorCode::IntegrityFailure,
                "Unable to hash immutable artifact bytes"),
            {}};
    }
    if (!file.expected_sha256.empty() &&
        (!CompleteSha256(file.expected_sha256) ||
         file.expected_sha256 != digest))
    {
        return {
            SavestateArtifactFinalizerResult::Failure(
                SavestateArtifactFinalizerErrorCode::HashMismatch,
                "Immutable artifact bytes do not match their expected SHA-256"),
            {}};
    }

    std::error_code error;
    if (std::filesystem::exists(file.final_path, error))
    {
        if (error)
        {
            return {
                SavestateArtifactFinalizerResult::Failure(
                    SavestateArtifactFinalizerErrorCode::FilesystemFailure,
                    "Unable to inspect immutable artifact destination: " +
                        error.message()),
                {}};
        }
        try
        {
            if (hash::sha256_of_file(file.final_path.string()) != digest)
            {
                return {
                    SavestateArtifactFinalizerResult::Failure(
                        SavestateArtifactFinalizerErrorCode::IntegrityFailure,
                        "Immutable artifact destination already contains "
                        "different bytes"),
                    {}};
            }
        }
        catch (const std::exception& exception)
        {
            return {
                SavestateArtifactFinalizerResult::Failure(
                    SavestateArtifactFinalizerErrorCode::FilesystemFailure,
                    exception.what()),
                {}};
        }
        return {
            SavestateArtifactFinalizerResult::Success(),
            {file.final_path, file.bytes.size(), digest, true}};
    }
    if (error)
    {
        return {
            SavestateArtifactFinalizerResult::Failure(
                SavestateArtifactFinalizerErrorCode::FilesystemFailure,
                "Unable to inspect immutable artifact destination: " +
                    error.message()),
            {}};
    }

    const std::filesystem::path parent = file.final_path.parent_path();
    if (!parent.empty())
    {
        std::filesystem::create_directories(parent, error);
        if (error)
        {
            return {
                SavestateArtifactFinalizerResult::Failure(
                    SavestateArtifactFinalizerErrorCode::FilesystemFailure,
                    "Unable to create immutable artifact directory: " +
                        error.message()),
                {}};
        }
    }

    const StagingWrite staged =
        WriteDurableStaging(file, id, ordinal, digest);
    if (!staged.result.ok)
        return {staged.result, {}};
    const std::filesystem::path& staging = staged.path;

#ifdef _WIN32
    const bool moved = ::MoveFileExW(
        staging.c_str(),
        file.final_path.c_str(),
        MOVEFILE_WRITE_THROUGH) != FALSE;
    const DWORD move_error = moved ? ERROR_SUCCESS : ::GetLastError();
#else
    std::filesystem::rename(staging, file.final_path, error);
    const bool moved = !error;
#endif
    if (!moved)
    {
        // Another finalizer/process may have published the same immutable
        // content after the initial check. Exact reuse is safe.
        std::error_code exists_error;
        if (std::filesystem::exists(file.final_path, exists_error) &&
            !exists_error)
        {
            try
            {
                if (hash::sha256_of_file(file.final_path.string()) ==
                    digest)
                {
                    std::filesystem::remove(staging, exists_error);
                    return {
                        SavestateArtifactFinalizerResult::Success(),
                        {file.final_path,
                         file.bytes.size(),
                         digest,
                         true}};
                }
            }
            catch (...)
            {
            }
        }
        const std::string message =
            "Unable to publish immutable artifact: " +
#ifdef _WIN32
            std::system_category().message(move_error);
#else
            error.message();
#endif
        std::filesystem::remove(staging, exists_error);
        return {
            SavestateArtifactFinalizerResult::Failure(
                SavestateArtifactFinalizerErrorCode::FilesystemFailure,
                message),
            {}};
    }

    return {
        SavestateArtifactFinalizerResult::Success(),
        {file.final_path, file.bytes.size(), digest, false}};
}

} // namespace

SavestateArtifactFinalizer::SavestateArtifactFinalizer(
    const WorkerWorksetLimits& limits,
    std::shared_ptr<ISavestateArtifactFinalizerNotifier> notifier,
    std::function<void()> before_process_for_testing)
    : limits_(limits),
      notifier_(std::move(notifier)),
      before_process_for_testing_(
          std::move(before_process_for_testing))
{
    const std::uint32_t thread_count =
        std::max<std::uint32_t>(1, limits_.finalizer_threads);
    threads_.reserve(thread_count);
    try
    {
        for (std::uint32_t index = 0; index < thread_count; ++index)
            threads_.emplace_back([this] { WorkerMain(); });
    }
    catch (...)
    {
        {
            std::lock_guard lock(mutex_);
            accepting_ = false;
            shutdown_ = true;
            joining_ = true;
        }
        wake_.notify_all();
        for (std::thread& thread : threads_)
        {
            if (thread.joinable())
                thread.join();
        }
        threads_.clear();
        {
            std::lock_guard lock(mutex_);
            joining_ = false;
            joined_ = true;
        }
        throw;
    }
}

SavestateArtifactFinalizer::~SavestateArtifactFinalizer()
{
    Shutdown();
}

SavestateArtifactFinalizerSubmission SavestateArtifactFinalizer::Submit(
    SavestateArtifactFinalizationRequest request)
{
    if (!request.item)
        request.item = ExecutionCorrelation(request.terminal);
    if (!CompleteExecutionCorrelation(request.item) ||
        (HasTerminalBinding(request.terminal) &&
         (!CompleteTerminalBinding(request.terminal) ||
          ExecutionCorrelation(request.terminal) != request.item)) ||
        !request.state_artifact_id ||
        request.logical_artifact_id.empty() ||
        request.state.final_path.empty() || !request.state.bytes)
    {
        return {
            SavestateArtifactFinalizerResult::Failure(
                SavestateArtifactFinalizerErrorCode::InvalidArgument,
                "State-artifact finalization requires exact item "
                "correlation and immutable state bytes"),
            {}};
    }

    std::size_t resident_bytes = request.state.bytes.size();
    std::unordered_set<std::wstring> paths;
    paths.emplace(PathIdentity(request.state.final_path));
    for (const ImmutableArtifactFile& sidecar : request.sidecars)
    {
        if (sidecar.final_path.empty() || !sidecar.bytes ||
            !paths.emplace(PathIdentity(sidecar.final_path)).second ||
            !AddSize(sidecar.bytes.size(), resident_bytes))
        {
            return {
                SavestateArtifactFinalizerResult::Failure(
                    SavestateArtifactFinalizerErrorCode::InvalidArgument,
                    "State-artifact sidecars require unique paths, immutable "
                    "bytes, and a representable aggregate size"),
                {}};
        }
    }

    std::lock_guard lock(mutex_);
    if (!accepting_)
    {
        return {
            SavestateArtifactFinalizerResult::Failure(
                SavestateArtifactFinalizerErrorCode::NotAccepting,
                "State-artifact finalizer is shutting down"),
            {}};
    }
    if (outstanding_artifact_ids_.contains(
            request.state_artifact_id.value()))
    {
        return {
            SavestateArtifactFinalizerResult::Failure(
                SavestateArtifactFinalizerErrorCode::InvalidArgument,
                "The exact captured state artifact already has pending "
                "finalization"),
            {}};
    }
    if (limits_.maximum_pending_finalizers == 0 ||
        outstanding_jobs_ >= limits_.maximum_pending_finalizers ||
        resident_bytes > limits_.maximum_pending_finalizer_bytes ||
        resident_payload_bytes_ >
            limits_.maximum_pending_finalizer_bytes - resident_bytes)
    {
        return {
            SavestateArtifactFinalizerResult::Failure(
                SavestateArtifactFinalizerErrorCode::CapacityExceeded,
                "State-artifact finalizer count or byte capacity is full"),
            {}};
    }
    if (next_finalization_id_ == 0)
    {
        return {
            SavestateArtifactFinalizerResult::Failure(
                SavestateArtifactFinalizerErrorCode::SequenceExhausted,
                "State-artifact finalization identity is exhausted"),
            {}};
    }

    const SavestateArtifactFinalizationId id(next_finalization_id_++);
    jobs_.push_back(Job{id, std::move(request), resident_bytes});
    outstanding_ids_.emplace(id.value());
    outstanding_artifact_ids_.emplace(
        jobs_.back().request.state_artifact_id.value());
    ++outstanding_jobs_;
    resident_payload_bytes_ += resident_bytes;
    wake_.notify_one();
    return {SavestateArtifactFinalizerResult::Success(), id};
}

std::vector<SavestateArtifactFinalizationCompletion>
SavestateArtifactFinalizer::DrainResults()
{
    std::lock_guard lock(mutex_);
    std::vector<SavestateArtifactFinalizationCompletion> drained;
    drained.reserve(completions_.size());
    while (!completions_.empty())
    {
        SavestateArtifactFinalizationCompletion completion =
            std::move(completions_.front());
        completions_.pop_front();
        outstanding_ids_.erase(completion.finalization_id.value());
        outstanding_artifact_ids_.erase(
            completion.state_artifact_id.value());
        if (outstanding_jobs_ != 0)
            --outstanding_jobs_;
        drained.push_back(std::move(completion));
    }
    return drained;
}

void SavestateArtifactFinalizer::Shutdown() noexcept
{
    {
        std::unique_lock lock(mutex_);
        if (joined_)
            return;
        if (joining_)
        {
            stopped_.wait(lock, [&] { return joined_; });
            return;
        }
        accepting_ = false;
        shutdown_ = true;
        joining_ = true;
    }
    wake_.notify_all();
    for (std::thread& thread : threads_)
    {
        if (thread.joinable())
            thread.join();
    }
    threads_.clear();
    {
        std::lock_guard lock(mutex_);
        joining_ = false;
        joined_ = true;
    }
    stopped_.notify_all();
}

SavestateArtifactFinalizerSnapshot
SavestateArtifactFinalizer::snapshot() const noexcept
{
    std::lock_guard lock(mutex_);
    return {
        accepting_,
        shutdown_,
        jobs_.size(),
        running_,
        completions_.size(),
        outstanding_jobs_,
        resident_payload_bytes_};
}

void SavestateArtifactFinalizer::WorkerMain() noexcept
{
    for (;;)
    {
        Job job;
        {
            std::unique_lock lock(mutex_);
            wake_.wait(
                lock,
                [&] { return shutdown_ || !jobs_.empty(); });
            if (jobs_.empty())
            {
                if (shutdown_)
                    return;
                continue;
            }
            job = std::move(jobs_.front());
            jobs_.pop_front();
            ++running_;
        }

        if (before_process_for_testing_)
            before_process_for_testing_();
        SavestateArtifactFinalizationCompletion completion = Process(job);
        job.request.state.bytes = {};
        job.request.sidecars.clear();
        std::shared_ptr<ISavestateArtifactFinalizerNotifier> notifier;
        {
            std::lock_guard lock(mutex_);
            if (running_ != 0)
                --running_;
            if (job.resident_bytes <= resident_payload_bytes_)
                resident_payload_bytes_ -= job.resident_bytes;
            else
                resident_payload_bytes_ = 0;
            completions_.push_back(std::move(completion));
            notifier = notifier_;
        }
        if (notifier)
            notifier->NotifySavestateArtifactFinalizerCompletion();
    }
}

SavestateArtifactFinalizationCompletion SavestateArtifactFinalizer::Process(
    const Job& job) const noexcept
{
    SavestateArtifactFinalizationCompletion completion;
    completion.finalization_id = job.id;
    completion.item = job.request.item;
    completion.terminal = job.request.terminal;
    completion.state_artifact_id =
        job.request.state_artifact_id;
    completion.logical_artifact_id =
        job.request.logical_artifact_id;

    try
    {
        completion.sidecars.reserve(job.request.sidecars.size());
        std::size_t ordinal = 1;
        for (const ImmutableArtifactFile& sidecar :
             job.request.sidecars)
        {
            FilePublication published =
                PublishImmutableFile(sidecar, job.id, ordinal++);
            if (!published.result.ok)
            {
                completion.result = std::move(published.result);
                return completion;
            }
            completion.sidecars.push_back(
                std::move(published.receipt));
        }

        FilePublication state =
            PublishImmutableFile(job.request.state, job.id, 0);
        completion.result = std::move(state.result);
        completion.state = std::move(state.receipt);
        return completion;
    }
    catch (const std::exception& exception)
    {
        completion.result = SavestateArtifactFinalizerResult::Failure(
            SavestateArtifactFinalizerErrorCode::FilesystemFailure,
            exception.what());
        return completion;
    }
    catch (...)
    {
        completion.result = SavestateArtifactFinalizerResult::Failure(
            SavestateArtifactFinalizerErrorCode::FilesystemFailure,
            "Unknown state-artifact finalization failure");
        return completion;
    }
}

} // namespace savor::runtime

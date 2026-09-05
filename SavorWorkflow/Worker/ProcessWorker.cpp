#include "ProcessWorker.h"

#include "Runner/Runtime/Worksets/WorksetWireCodec.h"
#include "Utils/Hash.h"
#include "Utils/Log.h"
#include "Utils/ThreadName.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <limits>
#include <sstream>
#include <utility>

namespace savor {
namespace {

constexpr std::uint32_t kDefaultRequestTimeoutMs = 10000;
constexpr std::uint32_t kExecutionTransportAllowanceMs = 1000;
constexpr std::size_t kMaximumPendingCallbacks = 128;
constexpr std::size_t kMaximumPendingCallbackBytes =
    128ull * 1024ull * 1024ull;

bool WriteAll(
    HANDLE handle,
    const void* data,
    std::size_t size,
    std::uint32_t* error_out)
{
    const auto* next = static_cast<const std::uint8_t*>(data);
    while (size != 0)
    {
        DWORD written = 0;
        const auto chunk = static_cast<DWORD>(std::min<std::size_t>(
            size,
            static_cast<std::size_t>(std::numeric_limits<DWORD>::max())));
        if (!WriteFile(handle, next, chunk, &written, nullptr))
        {
            if (error_out)
                *error_out = GetLastError();
            return false;
        }
        if (written == 0)
        {
            if (error_out)
                *error_out = ERROR_WRITE_FAULT;
            return false;
        }
        next += written;
        size -= written;
    }
    if (error_out)
        *error_out = ERROR_SUCCESS;
    return true;
}

std::string Win32ErrorMessage(const char* operation, DWORD error)
{
    std::ostringstream message;
    message << operation << " failed with Win32 error " << error;
    return message.str();
}

template <typename Payload>
bool EncodeTypedPayload(const Payload& payload, std::vector<std::uint8_t>* output)
{
    output->clear();
    return static_cast<bool>(wrms::EncodePayload(payload, *output));
}

runtime::WorkerState MapWorkerState(
    wrms::WorkerStateCode state) noexcept
{
    using Wire = wrms::WorkerStateCode;
    using Runtime = runtime::WorkerState;
    switch (state)
    {
    case Wire::Starting: return Runtime::Starting;
    case Wire::AwaitingSession: return Runtime::AwaitingSession;
    case Wire::Ready: return Runtime::Ready;
    case Wire::InitializingWorkset: return Runtime::InitializingWorkset;
    case Wire::Running: return Runtime::Running;
    case Wire::Cancelling: return Runtime::Cancelling;
    case Wire::Tainted: return Runtime::Tainted;
    case Wire::Stopping: return Runtime::Stopping;
    case Wire::Stopped: return Runtime::Stopped;
    }
    return Runtime::Tainted;
}

runtime::SessionDisposition MapSessionDisposition(
    wrms::SessionDispositionCode disposition) noexcept
{
    using Wire = wrms::SessionDispositionCode;
    using Runtime = runtime::SessionDisposition;
    switch (disposition)
    {
    case Wire::Closed: return Runtime::Closed;
    case Wire::Clean: return Runtime::Clean;
    case Wire::CleanWithDiagnostics: return Runtime::CleanWithDiagnostics;
    case Wire::Tainted: return Runtime::Tainted;
    }
    return Runtime::Tainted;
}

runtime::WorkerRejectionCode MapRejectionCode(
    wrms::RejectionCode code) noexcept
{
    using Wire = wrms::RejectionCode;
    using Runtime = runtime::WorkerRejectionCode;
    switch (code)
    {
    case Wire::None: return Runtime::None;
    case Wire::Unsupported: return Runtime::Unsupported;
    case Wire::InvalidState: return Runtime::InvalidState;
    case Wire::InvalidArgument: return Runtime::InvalidArgument;
    case Wire::SessionUnavailable: return Runtime::SessionUnavailable;
    case Wire::SessionMismatch: return Runtime::SessionMismatch;
    case Wire::SessionTainted: return Runtime::SessionTainted;
    case Wire::ProgramRuntimeUnavailable:
        return Runtime::ProgramRuntimeUnavailable;
    case Wire::InvocationAlreadyActive:
        return Runtime::InvocationAlreadyActive;
    case Wire::InvocationNotActive: return Runtime::InvocationNotActive;
    case Wire::InvocationMismatch: return Runtime::InvocationMismatch;
    case Wire::DuplicateCancellation: return Runtime::DuplicateCancellation;
    case Wire::WorksetEpochMismatch: return Runtime::WorksetEpochMismatch;
    case Wire::BackendFailure: return Runtime::BackendFailure;
    case Wire::RuntimeStopping: return Runtime::RuntimeStopping;
    case Wire::InternalFailure: return Runtime::InternalFailure;
    case Wire::WorksetAlreadyActive: return Runtime::WorksetAlreadyActive;
    case Wire::WorksetNotFound: return Runtime::WorksetNotFound;
    case Wire::WorksetItemNotFound: return Runtime::WorksetItemNotFound;
    case Wire::ProgramPackageRejected: return Runtime::ProgramPackageRejected;
    case Wire::CapacityExceeded: return Runtime::CapacityExceeded;
    case Wire::TerminalNotFound: return Runtime::TerminalNotFound;
    case Wire::TerminalMismatch: return Runtime::TerminalMismatch;
    case Wire::WorksetItemAlreadyTerminal:
        return Runtime::WorksetItemAlreadyTerminal;
    }
    return Runtime::InternalFailure;
}

runtime::WorkerMode MapWorkerMode(wrms::WorkerModeCode mode) noexcept
{
    switch (mode)
    {
    case wrms::WorkerModeCode::Headless:
        return runtime::WorkerMode::Headless;
    case wrms::WorkerModeCode::Visual:
        return runtime::WorkerMode::Visual;
    case wrms::WorkerModeCode::VisualDebug:
        return runtime::WorkerMode::VisualDebug;
    }
    return runtime::WorkerMode::Headless;
}

wrms::WorkerModeCode MapWorkerMode(runtime::WorkerMode mode) noexcept
{
    switch (mode)
    {
    case runtime::WorkerMode::Headless:
        return wrms::WorkerModeCode::Headless;
    case runtime::WorkerMode::Visual:
        return wrms::WorkerModeCode::Visual;
    case runtime::WorkerMode::VisualDebug:
        return wrms::WorkerModeCode::VisualDebug;
    }
    return wrms::WorkerModeCode::Headless;
}

std::uint32_t RemainingMilliseconds(
    const std::chrono::steady_clock::time_point deadline)
{
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline)
        return 0;
    const auto remaining =
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
    return static_cast<std::uint32_t>(std::min<std::int64_t>(
        std::max<std::int64_t>(remaining, 1),
        std::numeric_limits<std::uint32_t>::max()));
}

} // namespace

ProcessWorker::~ProcessWorker()
{
    stop();
    if (!confirm_process_exit())
    {
        std::lock_guard<std::mutex> lock(stop_mutex_);
        close_process_handles(&last_stop_snapshot_);
    }
    const auto callback_deadline =
        std::chrono::steady_clock::now() +
        kProcessWorkerDefaultCallbackCleanupGrace;
    (void)stop_callback_dispatch(callback_deadline);
    detach_callback_failure_target();
}

bool ProcessWorker::create_child(
    const ProcessLaunchOptions& options,
    std::string* error_out)
{
    SECURITY_ATTRIBUTES inheritable{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};

    HANDLE child_stdout_read = nullptr;
    HANDLE child_stdout_write = nullptr;
    HANDLE child_event_read = nullptr;
    HANDLE child_event_write = nullptr;
    HANDLE child_stdin_read = nullptr;
    HANDLE child_stdin_write = nullptr;
    HANDLE child_stderr = nullptr;

    const auto fail = [&](std::string message) {
        if (child_stdout_read)
            CloseHandle(child_stdout_read);
        if (child_stdout_write)
            CloseHandle(child_stdout_write);
        if (child_event_read)
            CloseHandle(child_event_read);
        if (child_event_write)
            CloseHandle(child_event_write);
        if (child_stdin_read)
            CloseHandle(child_stdin_read);
        if (child_stdin_write)
            CloseHandle(child_stdin_write);
        if (child_stderr && child_stderr != INVALID_HANDLE_VALUE)
            CloseHandle(child_stderr);
        if (error_out)
            *error_out = std::move(message);
        return false;
    };

    if (options.exe_path.empty())
        return fail("worker executable path is empty");

    if (!CreatePipe(
            &child_stdout_read,
            &child_stdout_write,
            &inheritable,
            0))
    {
        return fail(Win32ErrorMessage("CreatePipe(stdout)", GetLastError()));
    }
    if (!SetHandleInformation(child_stdout_read, HANDLE_FLAG_INHERIT, 0))
        return fail(Win32ErrorMessage("SetHandleInformation(stdout)", GetLastError()));

    if (!CreatePipe(
            &child_event_read,
            &child_event_write,
            &inheritable,
            0))
    {
        return fail(Win32ErrorMessage("CreatePipe(events)", GetLastError()));
    }
    if (!SetHandleInformation(child_event_read, HANDLE_FLAG_INHERIT, 0))
        return fail(Win32ErrorMessage("SetHandleInformation(events)", GetLastError()));

    if (!CreatePipe(
            &child_stdin_read,
            &child_stdin_write,
            &inheritable,
            0))
    {
        return fail(Win32ErrorMessage("CreatePipe(stdin)", GetLastError()));
    }
    if (!SetHandleInformation(child_stdin_write, HANDLE_FLAG_INHERIT, 0))
        return fail(Win32ErrorMessage("SetHandleInformation(stdin)", GetLastError()));

    child_stderr = CreateFileA(
        "NUL",
        GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        &inheritable,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (child_stderr == INVALID_HANDLE_VALUE)
    {
        child_stderr = nullptr;
        return fail(Win32ErrorMessage("CreateFile(NUL)", GetLastError()));
    }

    STARTUPINFOA startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = child_stdin_read;
    startup.hStdOutput = child_stdout_write;
    startup.hStdError = child_stderr;

    std::ostringstream command;
    command << '"' << options.exe_path << '"'
            << " --worker"
            << " --id " << options.worker_id
            << " --process-generation " << options.process_generation
            << " --utc-launch-ticks " << options.utc_launch_ticks
            << " --event-handle "
            << static_cast<std::uint64_t>(
                reinterpret_cast<std::uintptr_t>(child_event_write));
    if (!options.log_file_path.empty())
        command << " --log-file \"" << options.log_file_path << '"';
    if (options.breakpoint_diagnostics)
        command << " --breakpoint-diagnostics";

    std::string command_line = command.str();
    command_line.push_back('\0');

    PROCESS_INFORMATION process{};
    const BOOL created = CreateProcessA(
        nullptr,
        command_line.data(),
        nullptr,
        nullptr,
        TRUE,
        CREATE_NO_WINDOW,
        nullptr,
        nullptr,
        &startup,
        &process);

    CloseHandle(child_stdout_write);
    child_stdout_write = nullptr;
    CloseHandle(child_event_write);
    child_event_write = nullptr;
    CloseHandle(child_stdin_read);
    child_stdin_read = nullptr;
    CloseHandle(child_stderr);
    child_stderr = nullptr;

    if (!created)
        return fail(Win32ErrorMessage("CreateProcess", GetLastError()));

    HANDLE job = CreateJobObjectA(nullptr, nullptr);
    if (job)
    {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!SetInformationJobObject(
                job,
                JobObjectExtendedLimitInformation,
                &limits,
                sizeof(limits)) ||
            !AssignProcessToJobObject(job, process.hProcess))
        {
            CloseHandle(job);
            job = nullptr;
        }
    }

    child_stdout_read_ = child_stdout_read;
    child_event_read_ = child_event_read;
    child_stdin_write_ = child_stdin_write;
    process_handle_ = process.hProcess;
    process_thread_handle_ = process.hThread;
    process_id_ = process.dwProcessId;
    job_handle_ = job;
    return true;
}

bool ProcessWorker::launch_and_negotiate(
    const ProcessLaunchOptions& options,
    std::string* error_out)
{
    if (running_.load(std::memory_order_acquire) || process_handle_)
    {
        const std::string error = "worker process is already launched";
        set_last_error(error);
        if (error_out)
            *error_out = error;
        return false;
    }

    stop_started_.store(false, std::memory_order_release);
    stop_repeated_.store(false, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(stop_completion_mutex_);
        stop_deadline_ = {};
        stop_in_progress_snapshot_ = {};
        stop_shutdown_pending_.reset();
        stop_shutdown_write_.reset();
        stop_begin_completed_ = true;
        stop_finishing_ = false;
        stop_completed_ = false;
    }
    accepting_writes_.store(false, std::memory_order_release);
    ready_received_.store(false, std::memory_order_release);
    ready_ok_.store(false, std::memory_order_release);
    ready_error_.store(0, std::memory_order_release);
    busy_.store(false, std::memory_order_release);
    next_request_id_.store(1, std::memory_order_release);
    worker_id_ = options.worker_id;
    protocol_failed_.store(false, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(stop_mutex_);
        last_stop_snapshot_ = {};
    }
    {
        std::lock_guard<std::mutex> lock(writer_mutex_);
        writer_queue_.clear();
        writer_exit_requested_ = false;
        writer_active_ = false;
    }
    writer_cancel_requested_.store(false, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(snapshot_mutex_);
        snapshot_ = {};
        snapshot_.worker_state = runtime::WorkerState::Starting;
        snapshot_.command_response_channel_healthy = true;
        snapshot_.event_channel_healthy = true;
        hello_ = {};
        runtime_contract_.reset();
    }
    start_callback_dispatch();

    std::string create_error;
    if (!create_child(options, &create_error))
    {
        ready_received_.store(true, std::memory_order_release);
        ready_ok_.store(false, std::memory_order_release);
        ready_error_.store(1, std::memory_order_release);
        set_last_error(create_error);
        if (error_out)
            *error_out = create_error;
        return false;
    }

    running_.store(true, std::memory_order_release);
    accepting_writes_.store(true, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(snapshot_mutex_);
        snapshot_.running = true;
    }
    writer_ = std::thread(&ProcessWorker::writer_thread, this);
    reader_ = std::thread(&ProcessWorker::reader_thread, this);
    event_reader_ = std::thread(&ProcessWorker::event_reader_thread, this);

    const auto timeout = std::chrono::milliseconds{
        options.hello_timeout_ms ? options.hello_timeout_ms : kDefaultRequestTimeoutMs};
    bool negotiated = false;
    {
        std::unique_lock<std::mutex> lock(snapshot_mutex_);
        negotiated = hello_cv_.wait_for(lock, timeout, [&]() {
            return ready_received_.load(std::memory_order_acquire) ||
                !running_.load(std::memory_order_acquire);
        });
    }

    if (!negotiated ||
        !ready_ok_.load(std::memory_order_acquire) ||
        !running_.load(std::memory_order_acquire))
    {
        std::string error = last_error();
        if (error.empty())
            error = negotiated
                ? "worker protocol negotiation failed"
                : "timed out waiting for WRMS process hello";
        set_last_error(error);
        if (error_out)
            *error_out = error;
        stop();
        return false;
    }

    if (error_out)
        error_out->clear();
    return true;
}

bool ProcessWorker::open_session(
    const ProcessOpenSessionOptions& options,
    wrms::OpenSessionResultPayload* result_out,
    std::string* error_out,
    std::uint32_t timeout_ms)
{
    wrms::OpenSessionPayload request{
        .runtime_root = options.runtime_root,
        .user_directory = options.user_directory,
        .iso_path = options.iso_path,
        .worker_mode = MapWorkerMode(options.worker_mode),
        .render_window_handle = options.render_widget_handle,
        .runtime_artifact_root = options.runtime_artifact_root,
        .session_filesystem_preparation_id =
            options.session_filesystem_preparation_id,
        .process_generation = options.process_generation,
    };
    std::vector<std::uint8_t> payload;
    if (!EncodeTypedPayload(request, &payload))
    {
        const std::string error = "failed encoding OpenSession payload";
        set_last_error(error);
        if (error_out)
            *error_out = error;
        return false;
    }

    ProcessCommandCompletion completion;
    if (!request_response(
            wrms::MessageKind::OpenSession,
            payload,
            wrms::MessageKind::OpenSessionResult,
            timeout_ms,
            &completion))
    {
        if (error_out)
            *error_out = last_error();
        return false;
    }

    wrms::OpenSessionResultPayload result;
    if (!wrms::DecodePayload(completion.payload, result))
    {
        const std::string error = "invalid OpenSessionResult payload";
        set_last_error(error);
        if (error_out)
            *error_out = error;
        return false;
    }
    if (result_out)
        *result_out = result;
    if (!result.success)
    {
        const std::string error = result.message.empty()
            ? "worker rejected OpenSession: " + result.error_code
            : result.message;
        set_last_error(error);
        if (error_out)
            *error_out = error;
        return false;
    }
    if (result.worker_mode != request.worker_mode)
    {
        const std::string error =
            "worker confirmed a different mode than OpenSession requested";
        set_last_error(error);
        if (error_out)
            *error_out = error;
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(snapshot_mutex_);
        snapshot_.worker_mode = MapWorkerMode(result.worker_mode);
    }

    if (error_out)
        error_out->clear();
    return true;
}

bool ProcessWorker::submit_workset(
    const runtime::WorkerWorksetDefinition& workset,
    wrms::CommandResultPayload* result_out,
    std::uint32_t timeout_ms)
{
    ProcessWorksetSubmitOutcome outcome =
        submit_workset_with_outcome(workset, timeout_ms);
    if (result_out)
        *result_out = outcome.result;
    if (!outcome.accepted() && !outcome.diagnostic.empty())
        set_last_error(outcome.diagnostic);
    return outcome.accepted();
}

ProcessWorksetSubmitOutcome ProcessWorker::submit_workset_with_outcome(
    const runtime::WorkerWorksetDefinition& workset,
    std::uint32_t timeout_ms)
{
    return submit_workset_with_outcome(
        workset,
        runtime::InitialWorksetCancellationSidecarV1{
            .workset_id = workset.workset_id,
        },
        timeout_ms);
}

ProcessWorksetSubmitOutcome ProcessWorker::submit_workset_with_outcome(
    const runtime::WorkerWorksetDefinition& workset,
    const runtime::InitialWorksetCancellationSidecarV1&
        initial_cancellations,
    std::uint32_t timeout_ms)
{
    ProcessWorksetSubmitOutcome outcome;
    outcome.result.command_kind = wrms::MessageKind::SubmitWorkset;
    outcome.result.status = wrms::CommandStatus::Rejected;
    outcome.result.rejection_code = wrms::RejectionCode::InvalidArgument;

    std::vector<std::uint8_t> encoded_workset;
    const auto encoded =
        runtime::EncodeWorkerWorksetV5(workset, encoded_workset);
    if (!encoded)
    {
        outcome.diagnostic = encoded.message.empty()
            ? "failed encoding WorkerWorkset"
            : encoded.message;
        outcome.result.error_code = "LocalWorksetEncodingFailed";
        outcome.result.message = outcome.diagnostic;
        return outcome;
    }
    const auto sidecar_validation =
        runtime::ValidateInitialWorksetCancellationSidecar(
            workset, initial_cancellations);
    if (!sidecar_validation.ok)
    {
        outcome.diagnostic = sidecar_validation.error.message;
        outcome.result.error_code = "LocalCancellationSidecarValidationFailed";
        outcome.result.message = outcome.diagnostic;
        return outcome;
    }
    const auto workset_sha256 = hash::sha256(
        encoded_workset.data(), encoded_workset.size());
    wrms::SubmitWorksetPayload request{
        .encoded_workset = std::move(encoded_workset),
        .workset_sha256 = workset_sha256,
        .cancellation_sidecar_version =
            runtime::kInitialWorksetCancellationSidecarVersionV1,
        .cancellation_sidecar_sha256 =
            runtime::ComputeInitialWorksetCancellationSidecarSha256(
                initial_cancellations)};
    request.initially_cancelled_item_ids.reserve(
        initial_cancellations.item_ids.size());
    for (const auto item_id : initial_cancellations.item_ids)
        request.initially_cancelled_item_ids.push_back(item_id.value());
    std::vector<std::uint8_t> payload;
    if (!EncodeTypedPayload(request, &payload))
    {
        outcome.diagnostic = "failed encoding SubmitWorkset payload";
        outcome.result.error_code = "LocalWorksetEnvelopeEncodingFailed";
        outcome.result.message = outcome.diagnostic;
        return outcome;
    }
    ProcessCommandCompletion completion;
    if (!request_response(
            wrms::MessageKind::SubmitWorkset,
            payload,
            wrms::MessageKind::CommandResult,
            timeout_ms,
            &completion))
    {
        outcome.request_frame_written = completion.request_frame_written;
        outcome.correlated_result_received =
            completion.correlated_response_received;
        outcome.disposition = completion.request_frame_written
            ? ProcessWorksetSubmitDisposition::AmbiguousAfterWrite
            : ProcessWorksetSubmitDisposition::DefiniteRejected;
        outcome.diagnostic = last_error();
        outcome.result.error_code = completion.request_frame_written
            ? "WorksetSubmissionOutcomeAmbiguous"
            : "WorksetSubmissionNotWritten";
        outcome.result.message = outcome.diagnostic;
        return outcome;
    }
    outcome.request_frame_written = completion.request_frame_written;
    outcome.correlated_result_received =
        completion.correlated_response_received;
    wrms::CommandResultPayload result;
    if (!wrms::DecodePayload(completion.payload, result))
    {
        outcome.disposition =
            ProcessWorksetSubmitDisposition::AmbiguousAfterWrite;
        outcome.diagnostic = "invalid SubmitWorkset command result";
        outcome.result.error_code = "MalformedWorksetSubmissionResult";
        outcome.result.message = outcome.diagnostic;
        return outcome;
    }
    outcome.result = std::move(result);
    if (outcome.result.status == wrms::CommandStatus::Succeeded)
    {
        outcome.disposition = ProcessWorksetSubmitDisposition::Accepted;
        return outcome;
    }
    outcome.disposition =
        ProcessWorksetSubmitDisposition::DefiniteRejected;
    outcome.diagnostic = outcome.result.message.empty()
        ? "worker definitively rejected SubmitWorkset"
        : outcome.result.message;
    return outcome;
}

bool ProcessWorker::submit_one_item_workset(
    const runtime::WorkerWorksetDefinition& workset,
    wrms::CommandResultPayload* result_out,
    std::uint32_t timeout_ms)
{
    if (workset.items.size() != 1)
    {
        set_last_error(
            "one-item WorkerWorkset convenience requires exactly one item");
        return false;
    }
    return submit_workset(workset, result_out, timeout_ms);
}

bool ProcessWorker::cancel_workset_item(
    runtime::WorkerWorksetId workset_id,
    runtime::WorkerWorksetItemId item_id,
    std::string reason,
    wrms::CommandResultPayload* result_out,
    std::uint32_t timeout_ms)
{
    auto outcome = cancel_workset_item_with_outcome(
        workset_id, item_id, std::move(reason), timeout_ms);
    if (result_out)
        *result_out = outcome.result;
    if (!outcome.accepted() && !outcome.diagnostic.empty())
        set_last_error(outcome.diagnostic);
    return outcome.accepted();
}

ProcessWorkerCommandOutcome
ProcessWorker::cancel_workset_item_with_outcome(
    runtime::WorkerWorksetId workset_id,
    runtime::WorkerWorksetItemId item_id,
    std::string reason,
    std::uint32_t timeout_ms)
{
    wrms::CancelWorksetItemPayload request{
        .workset_id = workset_id.value(),
        .item_id = item_id.value(),
        .reason = std::move(reason)};
    std::vector<std::uint8_t> payload;
    if (!EncodeTypedPayload(request, &payload))
    {
        return {
            .disposition = ProcessWorkerCommandDisposition::LocalRejected,
            .result = {
                .command_kind = wrms::MessageKind::CancelWorksetItem,
                .status = wrms::CommandStatus::Rejected,
                .rejection_code = wrms::RejectionCode::InvalidArgument,
                .error_code = "LocalCancelWorksetItemEncodingFailed",
                .message = "failed encoding CancelWorksetItem payload",
            },
            .diagnostic = "failed encoding CancelWorksetItem payload",
        };
    }
    return request_command_with_outcome(
        wrms::MessageKind::CancelWorksetItem, payload, timeout_ms);
}

bool ProcessWorker::cancel_workset(
    runtime::WorkerWorksetId workset_id,
    std::string reason,
    wrms::CommandResultPayload* result_out,
    std::uint32_t timeout_ms)
{
    auto outcome = cancel_workset_with_outcome(
        workset_id, std::move(reason), timeout_ms);
    if (result_out)
        *result_out = outcome.result;
    if (!outcome.accepted() && !outcome.diagnostic.empty())
        set_last_error(outcome.diagnostic);
    return outcome.accepted();
}

ProcessWorkerCommandOutcome ProcessWorker::cancel_workset_with_outcome(
    runtime::WorkerWorksetId workset_id,
    std::string reason,
    std::uint32_t timeout_ms)
{
    wrms::CancelWorksetPayload request{
        .workset_id = workset_id.value(),
        .reason = std::move(reason)};
    std::vector<std::uint8_t> payload;
    if (!EncodeTypedPayload(request, &payload))
    {
        return {
            .disposition = ProcessWorkerCommandDisposition::LocalRejected,
            .result = {
                .command_kind = wrms::MessageKind::CancelWorkset,
                .status = wrms::CommandStatus::Rejected,
                .rejection_code = wrms::RejectionCode::InvalidArgument,
                .error_code = "LocalCancelWorksetEncodingFailed",
                .message = "failed encoding CancelWorkset payload",
            },
            .diagnostic = "failed encoding CancelWorkset payload",
        };
    }
    return request_command_with_outcome(
        wrms::MessageKind::CancelWorkset, payload, timeout_ms);
}

bool ProcessWorker::acknowledge_terminal(
    const runtime::WorkerItemTerminalCorrelation& terminal,
    wrms::CommandResultPayload* result_out,
    std::uint32_t timeout_ms)
{
    auto outcome = acknowledge_terminal_with_outcome(terminal, timeout_ms);
    if (result_out)
        *result_out = outcome.result;
    if (!outcome.accepted() && !outcome.diagnostic.empty())
        set_last_error(outcome.diagnostic);
    return outcome.accepted();
}

ProcessWorkerCommandOutcome
ProcessWorker::acknowledge_terminal_with_outcome(
    const runtime::WorkerItemTerminalCorrelation& terminal,
    std::uint32_t timeout_ms)
{
    wrms::AcknowledgeTerminalPayload request{
        .workset_id = terminal.workset_id.value(),
        .item_id = terminal.item_id.value(),
        .item_ordinal = terminal.item_ordinal,
        .invocation_id = terminal.invocation_id.value(),
        .attempt_id = terminal.attempt_id.value(),
        .terminal_id = terminal.terminal_id.value(),
        .terminal_order = terminal.terminal_order.value()};
    std::vector<std::uint8_t> payload;
    if (!EncodeTypedPayload(request, &payload))
    {
        return {
            .disposition = ProcessWorkerCommandDisposition::LocalRejected,
            .result = {
                .command_kind = wrms::MessageKind::AcknowledgeTerminal,
                .status = wrms::CommandStatus::Rejected,
                .rejection_code = wrms::RejectionCode::InvalidArgument,
                .error_code = "LocalAcknowledgeTerminalEncodingFailed",
                .message = "failed encoding AcknowledgeTerminal payload",
            },
            .diagnostic = "failed encoding AcknowledgeTerminal payload",
        };
    }
    return request_command_with_outcome(
        wrms::MessageKind::AcknowledgeTerminal, payload, timeout_ms);
}

ProcessWorkerCommandOutcome ProcessWorker::request_command_with_outcome(
    wrms::MessageKind command_kind,
    std::span<const std::uint8_t> payload,
    std::uint32_t timeout_ms)
{
    ProcessWorkerCommandOutcome outcome;
    outcome.result.command_kind = command_kind;
    outcome.result.status = wrms::CommandStatus::Rejected;
    ProcessCommandCompletion completion;
    if (!request_response(
            command_kind,
            payload,
            wrms::MessageKind::CommandResult,
            timeout_ms,
            &completion))
    {
        outcome.request_frame_written = completion.request_frame_written;
        outcome.correlated_result_received =
            completion.correlated_response_received;
        outcome.disposition = completion.request_frame_written
            ? ProcessWorkerCommandDisposition::AmbiguousAfterWrite
            : ProcessWorkerCommandDisposition::TransportCanceledBeforeWrite;
        outcome.diagnostic = last_error();
        outcome.result.error_code = completion.request_frame_written
            ? "WorkerCommandOutcomeAmbiguousAfterWrite"
            : "WorkerCommandNotWritten";
        outcome.result.message = outcome.diagnostic;
        return outcome;
    }
    outcome.request_frame_written = completion.request_frame_written;
    outcome.correlated_result_received =
        completion.correlated_response_received;
    wrms::CommandResultPayload result;
    if (!wrms::DecodePayload(completion.payload, result))
    {
        outcome.disposition =
            ProcessWorkerCommandDisposition::AmbiguousAfterWrite;
        outcome.diagnostic = "invalid worker command result";
        outcome.result.error_code = "MalformedWorkerCommandResult";
        outcome.result.message = outcome.diagnostic;
        return outcome;
    }
    outcome.result = std::move(result);
    if (outcome.result.status == wrms::CommandStatus::Succeeded)
    {
        outcome.disposition = ProcessWorkerCommandDisposition::Accepted;
        return outcome;
    }
    outcome.disposition = ProcessWorkerCommandDisposition::DefiniteRejected;
    outcome.diagnostic = outcome.result.message.empty()
        ? "worker definitively rejected command"
        : outcome.result.message;
    return outcome;
}

bool ProcessWorker::probe_liveness(
    wrms::CommandResultPayload* result_out,
    std::uint32_t timeout_ms)
{
    ProcessCommandCompletion completion;
    if (!request_response(
            wrms::MessageKind::LivenessProbe,
            {},
            wrms::MessageKind::CommandResult,
            timeout_ms,
            &completion))
    {
        return false;
    }
    wrms::CommandResultPayload result;
    if (!wrms::DecodePayload(completion.payload, result)
        || result.command_kind
            != wrms::MessageKind::LivenessProbe)
    {
        set_last_error("invalid LivenessProbe command result");
        return false;
    }
    if (result_out)
        *result_out = result;
    if (result.status != wrms::CommandStatus::Succeeded)
    {
        set_last_error(
            result.message.empty()
                ? "worker rejected LivenessProbe"
                : result.message);
        return false;
    }
    return true;
}

bool ProcessWorker::cancel_invocation(
    runtime::InvocationId invocation_id,
    std::string reason,
    wrms::CommandResultPayload* result_out,
    std::uint32_t timeout_ms)
{
    wrms::CancelInvocationPayload request{
        .invocation_id = invocation_id.value(),
        .reason = std::move(reason),
    };
    std::vector<std::uint8_t> payload;
    if (!EncodeTypedPayload(request, &payload))
    {
        set_last_error("failed encoding CancelInvocation payload");
        return false;
    }
    ProcessCommandCompletion completion;
    if (!request_response(
            wrms::MessageKind::CancelInvocation,
            payload,
            wrms::MessageKind::CommandResult,
            timeout_ms,
            &completion))
    {
        return false;
    }
    wrms::CommandResultPayload result;
    if (!wrms::DecodePayload(completion.payload, result))
    {
        set_last_error("invalid CancelInvocation command result");
        return false;
    }
    if (result_out)
        *result_out = result;
    return result.status == wrms::CommandStatus::Succeeded;
}

std::uint32_t ProcessWorker::effective_execution_command_timeout(
    std::uint32_t operation_timeout_ms,
    std::uint32_t command_timeout_ms) noexcept
{
    std::uint64_t effective = command_timeout_ms
        ? command_timeout_ms
        : kDefaultRequestTimeoutMs;
    if (operation_timeout_ms != 0)
    {
        const std::uint64_t operation_with_transport =
            static_cast<std::uint64_t>(operation_timeout_ms) +
            kExecutionTransportAllowanceMs;
        effective = std::max(effective, operation_with_transport);
    }
    return static_cast<std::uint32_t>(std::min<std::uint64_t>(
        effective,
        std::numeric_limits<std::uint32_t>::max()));
}

bool ProcessWorker::validate_execution_result(
    wrms::ExecutionControlKind control,
    runtime::SessionId session_id,
    runtime::WorksetEpoch expected_workset_epoch,
    std::uint32_t requested_count,
    const wrms::ExecutionResultPayload& result,
    std::string* error_out)
{
    const auto fail = [&](const char* message) {
        if (error_out)
            *error_out = message;
        return false;
    };

    if (result.control != control ||
        result.session_id != session_id.value())
    {
        return fail("worker returned a mismatched execution-control result");
    }
    if (result.status != wrms::CommandStatus::Succeeded)
        return true;
    if (!result.has_execution_state ||
        result.workset_epoch != expected_workset_epoch.value() ||
        result.operation_id == 0)
    {
        return fail(
            "worker returned an invalid successful execution-control result");
    }

    switch (control)
    {
    case wrms::ExecutionControlKind::Resume:
        if (result.has_terminal_status ||
            result.activity !=
                wrms::ExecutionActivityCode::InteractiveRunning)
        {
            return fail(
                "worker returned an inconsistent successful resume result");
        }
        break;
    case wrms::ExecutionControlKind::Pause:
        if (!result.has_terminal_status ||
            result.terminal_status !=
                wrms::ExecutionTerminalStatusCode::Paused ||
            result.activity != wrms::ExecutionActivityCode::IdlePaused)
        {
            return fail(
                "worker returned an inconsistent successful pause result");
        }
        break;
    case wrms::ExecutionControlKind::StepFrame:
        if (!result.has_terminal_status ||
            result.terminal_status !=
                wrms::ExecutionTerminalStatusCode::StepsCompleted ||
            result.activity != wrms::ExecutionActivityCode::IdlePaused ||
            result.completed_count != requested_count)
        {
            return fail(
                "worker returned an inconsistent successful step result");
        }
        break;
    case wrms::ExecutionControlKind::ReservedGuestInstruction:
        return fail("guest-instruction execution control is reserved");
    }

    if (error_out)
        error_out->clear();
    return true;
}

bool ProcessWorker::classify_shutdown_response(
    std::span<const std::uint8_t> payload,
    bool* graceful_out)
{
    wrms::ShutdownResultPayload result;
    if (!wrms::DecodePayload(payload, result))
    {
        if (graceful_out)
            *graceful_out = false;
        return false;
    }
    if (graceful_out)
    {
        *graceful_out =
            result.status == wrms::ShutdownStatus::Graceful;
    }
    return true;
}

bool ProcessWorker::request_execution_control(
    wrms::ExecutionControlKind control,
    runtime::WorkerWorksetId workset_id,
    runtime::WorkerWorksetItemId item_id,
    std::uint32_t count,
    std::uint32_t operation_timeout_ms,
    wrms::ExecutionResultPayload* result_out,
    std::uint32_t command_timeout_ms)
{
    const ProcessWorkerSnapshot observed = latest_snapshot();
    if (observed.worker_mode != runtime::WorkerMode::VisualDebug)
    {
        set_last_error(
            "execution control is reserved for VisualDebug workers");
        return false;
    }
    if (!observed.session_open ||
        !workset_id || !item_id ||
        !observed.active_workset ||
        !observed.active_workset_item ||
        *observed.active_workset != workset_id ||
        *observed.active_workset_item != item_id)
    {
        set_last_error(
            "execution control requires an exact active workset item");
        return false;
    }

    const bool is_step =
        control == wrms::ExecutionControlKind::StepFrame;
    if ((is_step && count == 0) || (!is_step && count != 0))
    {
        set_last_error(
            "execution control count is valid only for a nonzero step request");
        return false;
    }
    if (control != wrms::ExecutionControlKind::Resume &&
        operation_timeout_ms == 0)
    {
        set_last_error(
            "pause and step execution controls require a bounded timeout");
        return false;
    }

    wrms::ControlExecutionPayload request{
        .control = control,
        .workset_id = workset_id.value(),
        .item_id = item_id.value(),
        .count = count,
        .timeout_ms = operation_timeout_ms,
    };
    std::vector<std::uint8_t> payload;
    if (!EncodeTypedPayload(request, &payload))
    {
        set_last_error("failed encoding ControlExecution payload");
        return false;
    }

    ProcessCommandCompletion completion;
    const std::uint32_t effective_command_timeout =
        effective_execution_command_timeout(
            operation_timeout_ms,
            command_timeout_ms);
    if (!request_response(
            wrms::MessageKind::ControlExecution,
            payload,
            wrms::MessageKind::ExecutionResult,
            effective_command_timeout,
            &completion))
    {
        return false;
    }

    wrms::ExecutionResultPayload result;
    if (!wrms::DecodePayload(completion.payload, result))
    {
        set_last_error("invalid ExecutionResult payload");
        return false;
    }
    std::string validation_error;
    if (!validate_execution_result(
            control,
            observed.session_id,
            observed.workset_epoch,
            count,
            result,
            &validation_error))
    {
        set_last_error(std::move(validation_error));
        return false;
    }
    if (result_out)
        *result_out = result;
    if (result.status != wrms::CommandStatus::Succeeded)
    {
        set_last_error(
            result.message.empty()
                ? "worker rejected execution control"
                : result.message);
        return false;
    }
    return true;
}

bool ProcessWorker::pause_guest_execution(
    runtime::WorkerWorksetId workset_id,
    runtime::WorkerWorksetItemId item_id,
    wrms::ExecutionResultPayload* result_out,
    std::uint32_t operation_timeout_ms,
    std::uint32_t command_timeout_ms)
{
    return request_execution_control(
        wrms::ExecutionControlKind::Pause,
        workset_id,
        item_id,
        0,
        operation_timeout_ms,
        result_out,
        command_timeout_ms);
}

bool ProcessWorker::resume_guest_execution(
    runtime::WorkerWorksetId workset_id,
    runtime::WorkerWorksetItemId item_id,
    wrms::ExecutionResultPayload* result_out,
    std::uint32_t command_timeout_ms)
{
    return request_execution_control(
        wrms::ExecutionControlKind::Resume,
        workset_id,
        item_id,
        0,
        0,
        result_out,
        command_timeout_ms);
}

bool ProcessWorker::step_guest_frames(
    runtime::WorkerWorksetId workset_id,
    runtime::WorkerWorksetItemId item_id,
    std::uint32_t count,
    wrms::ExecutionResultPayload* result_out,
    std::uint32_t operation_timeout_ms,
    std::uint32_t command_timeout_ms)
{
    return request_execution_control(
        wrms::ExecutionControlKind::StepFrame,
        workset_id,
        item_id,
        count,
        operation_timeout_ms,
        result_out,
        command_timeout_ms);
}

ProcessWorkerSnapshot ProcessWorker::latest_snapshot() const
{
    std::lock_guard<std::mutex> lock(snapshot_mutex_);
    return snapshot_;
}

std::optional<runtime::WorkerRuntimeContractV1>
ProcessWorker::runtime_contract() const
{
    std::lock_guard<std::mutex> lock(snapshot_mutex_);
    return runtime_contract_;
}

std::string ProcessWorker::last_error() const
{
    std::lock_guard<std::mutex> lock(snapshot_mutex_);
    return snapshot_.last_error;
}

void ProcessWorker::set_invocation_progress_callback(
    InvocationProgressCallback callback)
{
    std::lock_guard<std::mutex> lock(callback_mutex_);
    invocation_progress_callback_ = std::move(callback);
}

void ProcessWorker::set_host_event_callback(HostEventCallback callback)
{
    std::lock_guard<std::mutex> lock(callback_mutex_);
    host_event_callback_ = std::move(callback);
}

void ProcessWorker::set_execution_state_callback(
    ExecutionStateCallback callback)
{
    std::lock_guard<std::mutex> lock(callback_mutex_);
    execution_state_callback_ = std::move(callback);
}

void ProcessWorker::set_workset_state_callback(
    WorksetStateCallback callback)
{
    std::lock_guard<std::mutex> lock(callback_mutex_);
    workset_state_callback_ = std::move(callback);
}

void ProcessWorker::set_workset_item_started_callback(
    WorksetItemStartedCallback callback)
{
    std::lock_guard<std::mutex> lock(callback_mutex_);
    workset_item_started_callback_ = std::move(callback);
}

void ProcessWorker::set_workset_item_terminal_callback(
    WorksetItemTerminalCallback callback)
{
    std::lock_guard<std::mutex> lock(callback_mutex_);
    workset_item_terminal_callback_ = std::move(callback);
}

void ProcessWorker::set_workset_credits_callback(
    WorksetCreditsCallback callback)
{
    std::lock_guard<std::mutex> lock(callback_mutex_);
    workset_credits_callback_ = std::move(callback);
}

void ProcessWorker::set_workset_summary_callback(
    WorksetSummaryCallback callback)
{
    std::lock_guard<std::mutex> lock(callback_mutex_);
    workset_summary_callback_ = std::move(callback);
}

void ProcessWorker::set_session_event_callback(
    SessionEventCallback callback)
{
    std::lock_guard<std::mutex> lock(callback_mutex_);
    session_event_callback_ = std::move(callback);
}

std::shared_ptr<ProcessWorker::OutboundWrite> ProcessWorker::enqueue_frame(
    wrms::MessageKind kind,
    runtime::WireRequestId request_id,
    std::span<const std::uint8_t> payload,
    bool allow_during_stop)
{
    const auto encoded = wrms::EncodeFrame(kind, request_id.value(), payload);
    if (!encoded)
        return {};

    auto write = std::make_shared<OutboundWrite>();
    write->kind = kind;
    write->bytes = std::move(encoded.bytes);
    {
        std::lock_guard<std::mutex> lock(writer_mutex_);
        if (!child_stdin_write_ ||
            writer_exit_requested_ ||
            (!allow_during_stop &&
             !accepting_writes_.load(std::memory_order_acquire)))
        {
            return {};
        }
        writer_queue_.push_back(write);
        std::lock_guard<std::mutex> snapshot_lock(snapshot_mutex_);
        snapshot_.command_write_queue_depth = writer_queue_.size();
    }
    writer_cv_.notify_one();
    return write;
}

bool ProcessWorker::write_frame(
    wrms::MessageKind kind,
    runtime::WireRequestId request_id,
    std::span<const std::uint8_t> payload,
    std::chrono::steady_clock::time_point deadline,
    bool allow_during_stop,
    bool* timed_out)
{
    if (timed_out)
        *timed_out = false;
    const auto write = enqueue_frame(
        kind,
        request_id,
        payload,
        allow_during_stop);
    if (!write)
        return false;

    std::unique_lock<std::mutex> lock(write->mutex);
    if (deadline == std::chrono::steady_clock::time_point::max())
    {
        write->cv.wait(lock, [&]() { return write->completed; });
    }
    else if (!write->cv.wait_until(
                 lock,
                 deadline,
                 [&]() { return write->completed; }))
    {
        if (timed_out)
            *timed_out = true;
        return false;
    }
    return write->succeeded;
}

void ProcessWorker::writer_thread()
{
    set_this_thread_name_utf8(
        ("WorkerWriterV1-" + std::to_string(worker_id_)).c_str());

    const auto complete = [](
        const std::shared_ptr<OutboundWrite>& write,
        bool succeeded,
        std::uint32_t error) {
        {
            std::lock_guard<std::mutex> lock(write->mutex);
            write->succeeded = succeeded;
            write->error = error;
            write->completed = true;
        }
        write->cv.notify_all();
    };

    for (;;)
    {
        std::shared_ptr<OutboundWrite> write;
        {
            std::unique_lock<std::mutex> lock(writer_mutex_);
            writer_cv_.wait(lock, [&]() {
                return writer_exit_requested_ || !writer_queue_.empty();
            });
            if (writer_queue_.empty())
            {
                writer_active_ = false;
                break;
            }
            write = std::move(writer_queue_.front());
            writer_queue_.pop_front();
            writer_active_ = true;
            std::lock_guard<std::mutex> snapshot_lock(snapshot_mutex_);
            snapshot_.command_write_queue_depth = writer_queue_.size();
        }

        if (test_hooks_)
        {
            if (test_hooks_->observe_writer_frame)
            {
                test_hooks_->observe_writer_frame(
                    write->kind,
                    write->bytes);
            }
            if (test_hooks_->before_writer_write)
                test_hooks_->before_writer_write(write->kind);
        }

        std::uint32_t error = ERROR_OPERATION_ABORTED;
        bool succeeded = false;
        if (!writer_cancel_requested_.load(std::memory_order_acquire))
        {
            succeeded = WriteAll(
                child_stdin_write_,
                write->bytes.data(),
                write->bytes.size(),
                &error);
        }

        if (test_hooks_ && test_hooks_->after_writer_write)
            test_hooks_->after_writer_write(write->kind, succeeded);
        complete(write, succeeded, error);

        std::deque<std::shared_ptr<OutboundWrite>> abandoned;
        {
            std::lock_guard<std::mutex> lock(writer_mutex_);
            writer_active_ = false;
            if (!succeeded)
            {
                accepting_writes_.store(false, std::memory_order_release);
                writer_exit_requested_ = true;
                abandoned.swap(writer_queue_);
            }
        }
        for (const auto& queued : abandoned)
            complete(queued, false, error);
        if (!succeeded)
            break;
    }

    std::deque<std::shared_ptr<OutboundWrite>> abandoned;
    {
        std::lock_guard<std::mutex> lock(writer_mutex_);
        writer_active_ = false;
        abandoned.swap(writer_queue_);
    }
    for (const auto& queued : abandoned)
        complete(queued, false, ERROR_OPERATION_ABORTED);
}

void ProcessWorker::request_writer_stop(
    ProcessWorkerStopSnapshot* snapshot)
{
    bool writer_active = false;
    {
        std::lock_guard<std::mutex> lock(writer_mutex_);
        accepting_writes_.store(false, std::memory_order_release);
        writer_exit_requested_ = true;
        writer_active = writer_active_;
        writer_cancel_requested_.store(true, std::memory_order_release);
    }
    writer_cv_.notify_all();

    if (!writer_.joinable() || !writer_active)
        return;

    snapshot->cancel_writer_attempted = true;
    if (test_hooks_ && test_hooks_->writer_cancel_requested)
        test_hooks_->writer_cancel_requested();
    snapshot->cancel_writer_succeeded =
        CancelSynchronousIo(writer_.native_handle()) != 0;
    if (!snapshot->cancel_writer_succeeded)
        snapshot->cancel_writer_error = GetLastError();
}

void ProcessWorker::join_writer(ProcessWorkerStopSnapshot* snapshot)
{
    if (!writer_.joinable())
        return;
    writer_.join();
    snapshot->writer_joined = true;
}

bool ProcessWorker::request_response(
    wrms::MessageKind request_kind,
    std::span<const std::uint8_t> request_payload,
    wrms::MessageKind expected_response_kind,
    std::uint32_t timeout_ms,
    ProcessCommandCompletion* completion_out,
    bool allow_during_stop)
{
    if (completion_out)
        *completion_out = {};
    const auto deadline = timeout_ms == 0
        ? std::chrono::steady_clock::time_point::max()
        : std::chrono::steady_clock::now() +
            std::chrono::milliseconds{timeout_ms};
    const auto raw_id = next_request_id_.fetch_add(1, std::memory_order_acq_rel);
    if (raw_id == 0)
    {
        set_last_error("WRMS request ID space exhausted");
        return false;
    }
    const runtime::WireRequestId request_id{raw_id};
    if (completion_out)
        completion_out->request_id = request_id;
    auto pending = std::make_shared<PendingResponse>();
    pending->request_kind = request_kind;
    pending->expected_response_kind = expected_response_kind;
    pending->enqueued_at = std::chrono::steady_clock::now();
    pending->deadline = deadline;
    {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        pending_.emplace(raw_id, pending);
        std::lock_guard<std::mutex> snapshot_lock(snapshot_mutex_);
        snapshot_.pending_response_count = pending_.size();
    }

    bool write_timed_out = false;
    if (!write_frame(
            request_kind,
            request_id,
            request_payload,
            deadline,
            allow_during_stop,
            &write_timed_out))
    {
        const auto kind = std::to_string(
            static_cast<std::uint32_t>(request_kind));
        const std::string diagnostic = write_timed_out
            ? "timed out waiting for WRMS request write kind=" + kind
            : "failed writing WRMS request kind=" + kind;
        if (write_timed_out)
        {
            std::lock_guard<std::mutex> lock(pending->mutex);
            pending->state = PendingResponse::State::TimedOut;
            record_transport_diagnostic(
                ProcessWorkerTransportDiagnosticCode::CommandResponseTimeout,
                diagnostic,
                true);
        }
        else
        {
            std::lock_guard<std::mutex> lock(pending_mutex_);
            pending_.erase(raw_id);
            std::lock_guard<std::mutex> snapshot_lock(snapshot_mutex_);
            snapshot_.pending_response_count = pending_.size();
        }
        set_last_error(diagnostic);
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(pending->mutex);
        pending->request_frame_written = true;
        pending->written_at = std::chrono::steady_clock::now();
    }
    if (completion_out)
        completion_out->request_frame_written = true;

    bool completed = false;
    {
        std::unique_lock<std::mutex> lock(pending->mutex);
        if (deadline == std::chrono::steady_clock::time_point::max())
        {
            pending->cv.wait(lock, [&]() {
                return pending->state != PendingResponse::State::Pending;
            });
            completed = true;
        }
        else
        {
            completed = pending->cv.wait_until(
                lock,
                deadline,
                [&]() {
                    return pending->state != PendingResponse::State::Pending;
                });
            if (!completed && pending->state == PendingResponse::State::Pending)
                pending->state = PendingResponse::State::TimedOut;
        }
    }

    if (!completed)
    {
        const auto kind = std::to_string(
            static_cast<std::uint32_t>(request_kind));
        const std::string diagnostic =
            "timed out waiting for WRMS command completion kind=" + kind
            + " request_id=" + std::to_string(raw_id);
        record_transport_diagnostic(
            ProcessWorkerTransportDiagnosticCode::CommandResponseTimeout,
            diagnostic,
            true);
        set_last_error(diagnostic);
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        pending_.erase(raw_id);
        std::lock_guard<std::mutex> snapshot_lock(snapshot_mutex_);
        snapshot_.pending_response_count = pending_.size();
    }
    if (!pending->transport_ok)
    {
        const auto kind = std::to_string(
            static_cast<std::uint32_t>(request_kind));
        set_last_error(
            "worker transport closed before WRMS command completion kind=" + kind);
        return false;
    }
    if (pending->response_kind != expected_response_kind)
    {
        set_last_error("worker returned an unexpected WRMS response kind");
        return false;
    }

    if (completion_out)
    {
        completion_out->correlated_response_received = true;
        completion_out->response_kind = pending->response_kind;
        completion_out->payload = std::move(pending->payload);
    }
    return true;
}

ProcessWorker::PendingCompletionDisposition ProcessWorker::complete_pending(
    std::uint64_t request_id,
    wrms::MessageKind kind,
    std::span<const std::uint8_t> payload)
{
    std::shared_ptr<PendingResponse> pending;
    bool missing = false;
    {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        const auto found = pending_.find(request_id);
        if (found == pending_.end())
            missing = true;
        else
            pending = found->second;
    }
    if (missing)
    {
        fail_protocol(
            "UNSOLICITED_COMMAND_RESPONSE request_id="
                + std::to_string(request_id),
            ProcessWorkerTransportDiagnosticCode::
                UnsolicitedCommandResponse);
        return PendingCompletionDisposition::Invalid;
    }
    if (kind != pending->expected_response_kind)
    {
        fail_protocol(
            "UNSOLICITED_COMMAND_RESPONSE unexpected response kind request_id="
                + std::to_string(request_id),
            ProcessWorkerTransportDiagnosticCode::
                UnsolicitedCommandResponse);
        return PendingCompletionDisposition::Invalid;
    }
    if (kind == wrms::MessageKind::CommandResult)
    {
        wrms::CommandResultPayload result;
        if (!wrms::DecodePayload(payload, result) ||
            result.command_kind != pending->request_kind)
        {
            {
                std::lock_guard<std::mutex> lock(pending->mutex);
                if (!pending->completed)
                {
                    pending->transport_ok = false;
                    pending->completed = true;
                }
            }
            pending->cv.notify_all();
            fail_protocol(
                "WRMS command result does not match its exact request kind",
                ProcessWorkerTransportDiagnosticCode::
                    UnsolicitedCommandResponse);
            return PendingCompletionDisposition::Invalid;
        }
    }
    bool late = false;
    bool duplicate = false;
    {
        std::lock_guard<std::mutex> lock(pending->mutex);
        if (pending->state == PendingResponse::State::Completed ||
            pending->state == PendingResponse::State::CompletedLate ||
            pending->state == PendingResponse::State::AbandonedOnWorkerRetirement)
        {
            duplicate = true;
        }
        else
        {
            late = pending->state == PendingResponse::State::TimedOut;
            pending->response_kind = kind;
            pending->payload.assign(payload.begin(), payload.end());
            pending->transport_ok = true;
            pending->completed = true;
            pending->response_at = std::chrono::steady_clock::now();
            pending->state = late
                ? PendingResponse::State::CompletedLate
                : PendingResponse::State::Completed;
        }
    }
    if (duplicate)
    {
        fail_protocol(
            "UNSOLICITED_COMMAND_RESPONSE duplicate response request_id="
                + std::to_string(request_id),
            ProcessWorkerTransportDiagnosticCode::
                UnsolicitedCommandResponse);
        return PendingCompletionDisposition::Invalid;
    }
    pending->cv.notify_all();
    if (!late)
        return PendingCompletionDisposition::OnTime;

    const auto lateness = pending->deadline ==
        std::chrono::steady_clock::time_point::max()
        ? 0
        : std::max<std::int64_t>(
            0,
            std::chrono::duration_cast<std::chrono::milliseconds>(
                pending->response_at - pending->deadline).count());
    record_transport_diagnostic(
        ProcessWorkerTransportDiagnosticCode::LateCommandResponse,
        "LATE_COMMAND_RESPONSE request_id=" + std::to_string(request_id)
            + " request_kind="
            + std::to_string(static_cast<std::uint32_t>(pending->request_kind))
            + " lateness_ms=" + std::to_string(lateness),
        false);
    {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        pending_.erase(request_id);
        std::lock_guard<std::mutex> snapshot_lock(snapshot_mutex_);
        snapshot_.pending_response_count = pending_.size();
        ++snapshot_.late_command_response_count;
    }
    return PendingCompletionDisposition::Late;
}

bool ProcessWorker::has_exact_pending_request(
    std::uint64_t request_id,
    wrms::MessageKind request_kind) const
{
    if (request_id == 0)
        return false;
    std::lock_guard<std::mutex> lock(pending_mutex_);
    const auto found = pending_.find(request_id);
    if (found == pending_.end() ||
        !found->second ||
        found->second->request_kind != request_kind)
    {
        return false;
    }
    std::lock_guard<std::mutex> response_lock(
        found->second->mutex);
    return found->second->state == PendingResponse::State::Pending ||
        found->second->state == PendingResponse::State::TimedOut;
}

void ProcessWorker::fail_all_pending()
{
    std::vector<std::shared_ptr<PendingResponse>> pending;
    {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        pending.reserve(pending_.size());
        for (const auto& [_, response] : pending_)
            pending.push_back(response);
        pending_.clear();
        std::lock_guard<std::mutex> snapshot_lock(snapshot_mutex_);
        snapshot_.pending_response_count = 0;
    }
    for (const auto& response : pending)
    {
        {
            std::lock_guard<std::mutex> lock(response->mutex);
            if (response->completed)
                continue;
            response->completed = true;
            response->transport_ok = false;
            response->state =
                PendingResponse::State::AbandonedOnWorkerRetirement;
        }
        response->cv.notify_all();
    }
}

bool ProcessWorker::accept_workset_outbound_sequence(
    std::uint64_t sequence)
{
    bool accepted = false;
    {
        std::lock_guard<std::mutex> lock(snapshot_mutex_);
        const std::uint64_t expected =
            snapshot_.last_outbound_sequence + 1;
        accepted = sequence != 0 && sequence == expected;
        if (accepted)
            snapshot_.last_outbound_sequence = sequence;
    }
    if (!accepted)
    {
        fail_protocol(
            "WRMS workset event sequence is zero, duplicated, regressed, or "
            "contains a gap");
    }
    return accepted;
}

void ProcessWorker::record_transport_diagnostic(
    ProcessWorkerTransportDiagnosticCode code,
    std::string diagnostic,
    bool preserve_as_first)
{
    std::lock_guard<std::mutex> lock(snapshot_mutex_);
    snapshot_.last_transport_diagnostic_code = code;
    snapshot_.last_transport_diagnostic = diagnostic;
    if (preserve_as_first &&
        snapshot_.first_transport_diagnostic_code ==
            ProcessWorkerTransportDiagnosticCode::None)
    {
        snapshot_.first_transport_diagnostic_code = code;
        snapshot_.first_transport_diagnostic = std::move(diagnostic);
    }
}

void ProcessWorker::fail_protocol(
    std::string error,
    ProcessWorkerTransportDiagnosticCode code)
{
    record_transport_diagnostic(code, error, true);
    protocol_failed_.store(true, std::memory_order_release);
    accepting_writes_.store(false, std::memory_order_release);
    running_.store(false, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(snapshot_mutex_);
        snapshot_.running = false;
        snapshot_.last_rejection_code =
            runtime::WorkerRejectionCode::InvalidArgument;
        snapshot_.last_error = snapshot_.first_transport_diagnostic.empty()
            ? std::move(error)
            : snapshot_.first_transport_diagnostic;
        if (code == ProcessWorkerTransportDiagnosticCode::
                CommandResponseChannelClosed ||
            code == ProcessWorkerTransportDiagnosticCode::
                UnsolicitedCommandResponse)
        {
            snapshot_.command_response_channel_healthy = false;
        }
        if (code == ProcessWorkerTransportDiagnosticCode::EventChannelClosed ||
            code == ProcessWorkerTransportDiagnosticCode::EventProtocolViolation)
        {
            snapshot_.event_channel_healthy = false;
        }
    }
    ready_received_.store(true, std::memory_order_release);
    ready_ok_.store(false, std::memory_order_release);
    ready_error_.store(1, std::memory_order_release);
    hello_cv_.notify_all();
    fail_all_pending();
}

bool ProcessWorker::enqueue_callback(
    std::function<void()> callback,
    bool authoritative,
    std::size_t resident_bytes)
{
    if (!callback)
        return true;
    const auto state = callback_dispatcher_state_;
    if (!state)
        return false;
    resident_bytes = std::max<std::size_t>(resident_bytes, sizeof(callback));
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (state->stop_requested)
            return false;
        const auto fits = [&]() {
            return state->queue.size() < kMaximumPendingCallbacks &&
                resident_bytes <= kMaximumPendingCallbackBytes &&
                state->queued_bytes <=
                    kMaximumPendingCallbackBytes - resident_bytes;
        };
        if (!fits() && authoritative)
        {
            // Passive observations may be coalesced/dropped under pressure,
            // but an authoritative item lifecycle event is never discarded.
            // Reclaim the oldest passive entries before failing the transport.
            for (auto it = state->queue.begin();
                 it != state->queue.end() && !fits();)
            {
                if (it->authoritative)
                {
                    ++it;
                    continue;
                }
                state->queued_bytes -= it->resident_bytes;
                it = state->queue.erase(it);
            }
        }
        if (!fits())
            return false;
        state->queued_bytes += resident_bytes;
        state->queue.push_back(PendingCallback{
            std::move(callback),
            resident_bytes,
            authoritative});
    }
    state->available.notify_one();
    return true;
}

void ProcessWorker::start_callback_dispatch()
{
    if (callback_dispatcher_.joinable())
        return;
    callback_dispatcher_state_ =
        std::make_shared<CallbackDispatcherState>();
    callback_failure_target_ =
        std::make_shared<CallbackFailureTarget>();
    callback_failure_target_->owner = this;
    callback_dispatcher_ = std::thread(
        &ProcessWorker::callback_thread,
        callback_dispatcher_state_,
        callback_failure_target_,
        worker_id_);
}

void ProcessWorker::callback_thread(
    std::shared_ptr<CallbackDispatcherState> state,
    std::shared_ptr<CallbackFailureTarget> failure_target,
    std::size_t worker_id)
{
    set_this_thread_name_utf8(
        ("WorkerEventsV1-" + std::to_string(worker_id)).c_str());
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->thread_id = std::this_thread::get_id();
    }
    for (;;)
    {
        PendingCallback callback;
        {
            std::unique_lock<std::mutex> lock(state->mutex);
            state->available.wait(lock, [&]() {
                return state->stop_requested ||
                    !state->queue.empty();
            });
            if (state->queue.empty())
            {
                if (state->stop_requested)
                    break;
                continue;
            }
            callback = std::move(state->queue.front());
            state->queue.pop_front();
            state->queued_bytes -= callback.resident_bytes;
        }
        try
        {
            callback.invoke();
        }
        catch (...)
        {
            std::lock_guard<std::mutex> target_lock(
                failure_target->mutex);
            if (failure_target->owner)
            {
                if (callback.authoritative)
                {
                    failure_target->owner->fail_protocol(
                        "authoritative worker event callback threw");
                }
                else
                {
                    failure_target->owner->set_last_error(
                        "passive worker event callback threw");
                }
            }
        }
    }
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->exited = true;
    }
    state->exited_cv.notify_all();
}

bool ProcessWorker::on_callback_dispatcher_thread() const
{
    const auto state = callback_dispatcher_state_;
    if (!state)
        return false;
    std::lock_guard<std::mutex> lock(state->mutex);
    return state->thread_id == std::this_thread::get_id();
}

void ProcessWorker::detach_callback_failure_target() noexcept
{
    const auto target = callback_failure_target_;
    if (!target)
        return;
    std::lock_guard<std::mutex> lock(target->mutex);
    target->owner = nullptr;
}

bool ProcessWorker::stop_callback_dispatch(
    std::chrono::steady_clock::time_point deadline)
{
    const auto state = callback_dispatcher_state_;
    if (!state)
        return true;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->stop_requested = true;
    }
    state->available.notify_all();
    if (!callback_dispatcher_.joinable())
        return true;
    if (on_callback_dispatcher_thread())
        return true;

    bool exited_before_deadline = false;
    {
        std::unique_lock<std::mutex> lock(state->mutex);
        exited_before_deadline = state->exited_cv.wait_until(
            lock,
            deadline,
            [&]() { return state->exited; });
    }
    // Arbitrary user callbacks cannot be cancelled safely because they may
    // legally retain and use ProcessWorker. Preserve memory safety by joining
    // even when callback cleanup exceeds its separately reported grace.
    callback_dispatcher_.join();
    detach_callback_failure_target();
    return exited_before_deadline;
}

void ProcessWorker::reader_thread()
{
    read_channel(
        child_stdout_read_,
        wrms::MessageChannel::Response,
        "WorkerResponseReaderV1-");
}

void ProcessWorker::event_reader_thread()
{
    read_channel(
        child_event_read_,
        wrms::MessageChannel::Event,
        "WorkerEventReaderV1-");
}

void ProcessWorker::read_channel(
    HANDLE input,
    wrms::MessageChannel channel,
    const char* thread_name)
{
    set_this_thread_name_utf8(
        (std::string(thread_name) + std::to_string(worker_id_)).c_str());

    std::vector<std::uint8_t> buffered;
    buffered.reserve(64 * 1024);
    std::array<std::uint8_t, 64 * 1024> chunk{};
    bool clean_eof = false;

    while (running_.load(std::memory_order_acquire))
    {
        DWORD read = 0;
        if (!ReadFile(
                input,
                chunk.data(),
                static_cast<DWORD>(chunk.size()),
                &read,
                nullptr) ||
            read == 0)
        {
            clean_eof = true;
            break;
        }
        buffered.insert(buffered.end(), chunk.begin(), chunk.begin() + read);

        for (;;)
        {
            const auto decoded = wrms::DecodeFrame(buffered, false);
            if (decoded.status == wrms::FrameDecodeStatus::NeedMoreData)
                break;
            if (decoded.status == wrms::FrameDecodeStatus::Error)
            {
                fail_protocol(
                    "worker emitted an invalid WRMS frame (error " +
                    std::to_string(static_cast<unsigned>(decoded.error)) + ")");
                clean_eof = false;
                break;
            }

            if (wrms::DirectionOf(decoded.frame.header.kind) !=
                    wrms::MessageDirection::WorkerToParent ||
                wrms::ChannelOf(decoded.frame.header.kind) != channel)
            {
                fail_protocol(
                    channel == wrms::MessageChannel::Response
                        ? "UNSOLICITED_COMMAND_RESPONSE worker emitted a non-response on the response channel"
                        : "EVENT_PROTOCOL_VIOLATION worker emitted a non-event on the event channel",
                    channel == wrms::MessageChannel::Response
                        ? ProcessWorkerTransportDiagnosticCode::UnsolicitedCommandResponse
                        : ProcessWorkerTransportDiagnosticCode::EventProtocolViolation);
                clean_eof = false;
                break;
            }
            handle_frame(decoded.frame);
            buffered.erase(
                buffered.begin(),
                buffered.begin() + decoded.consumed_size);
            if (protocol_failed_.load(std::memory_order_acquire))
                break;
        }
        if (protocol_failed_.load(std::memory_order_acquire))
            break;
    }

    if (clean_eof && !buffered.empty())
    {
        const auto truncated = wrms::DecodeFrame(buffered, true);
        if (truncated.status == wrms::FrameDecodeStatus::Error)
        {
            fail_protocol(
                channel == wrms::MessageChannel::Response
                    ? "COMMAND_RESPONSE_CHANNEL_CLOSED with a truncated WRMS frame"
                    : "EVENT_PROTOCOL_VIOLATION event channel closed with a truncated WRMS frame",
                channel == wrms::MessageChannel::Response
                    ? ProcessWorkerTransportDiagnosticCode::CommandResponseChannelClosed
                    : ProcessWorkerTransportDiagnosticCode::EventProtocolViolation);
        }
    }

    if (!stop_started_.load(std::memory_order_acquire) &&
        !protocol_failed_.load(std::memory_order_acquire))
    {
        const auto code = channel == wrms::MessageChannel::Response
            ? ProcessWorkerTransportDiagnosticCode::CommandResponseChannelClosed
            : ProcessWorkerTransportDiagnosticCode::EventChannelClosed;
        fail_protocol(
            channel == wrms::MessageChannel::Response
                ? "COMMAND_RESPONSE_CHANNEL_CLOSED"
                : "EVENT_CHANNEL_CLOSED",
            code);
    }

    {
        std::lock_guard<std::mutex> lock(snapshot_mutex_);
        if (channel == wrms::MessageChannel::Response)
            snapshot_.command_response_channel_healthy = false;
        else
            snapshot_.event_channel_healthy = false;
    }
}

void ProcessWorker::handle_frame(const wrms::FrameView& frame)
{
    switch (frame.header.kind)
    {
    case wrms::MessageKind::ProcessHello:
    {
        wrms::ProcessHelloPayload hello;
        if (frame.header.request_id != 0 ||
            !wrms::DecodePayload(frame.payload, hello))
        {
            fail_protocol("invalid WRMS process hello");
            return;
        }
        bool duplicate_hello = false;
        {
            std::lock_guard<std::mutex> lock(snapshot_mutex_);
            duplicate_hello = snapshot_.hello_received;
        }
        if (duplicate_hello ||
            hello.worker_id != static_cast<std::uint64_t>(worker_id_) ||
            hello.process_id != process_id_)
        {
            fail_protocol(
                duplicate_hello
                    ? "worker emitted more than one WRMS process hello"
                    : "WRMS process hello does not identify the launched worker process");
            return;
        }
        runtime::WorkerRuntimeContractV1 contract;
        const auto decoded_contract =
            runtime::DecodeWorkerRuntimeContractV1(
                hello.encoded_runtime_contract,
                contract);
        if (!decoded_contract ||
            contract.wrms_protocol_version != wrms::ProtocolVersion)
        {
            fail_protocol(
                decoded_contract.message.empty()
                    ? "worker runtime contract protocol is incompatible"
                    : decoded_contract.message);
            return;
        }
        {
            std::lock_guard<std::mutex> lock(snapshot_mutex_);
            hello_ = hello;
            runtime_contract_ = std::move(contract);
            snapshot_.hello_received = true;
            snapshot_.runtime_contract_received = true;
            snapshot_.runtime_contract_sha256 =
                runtime_contract_->canonical_sha256;
            snapshot_.worker_state = runtime::WorkerState::AwaitingSession;
            ready_received_.store(true, std::memory_order_release);
            ready_ok_.store(true, std::memory_order_release);
            ready_error_.store(0, std::memory_order_release);
        }
        hello_cv_.notify_all();
        return;
    }
    case wrms::MessageKind::CommandResult:
    {
        wrms::CommandResultPayload result;
        if (frame.header.request_id == 0 ||
            !wrms::DecodePayload(frame.payload, result))
        {
            fail_protocol("invalid WRMS command result");
            return;
        }
        const auto completion = complete_pending(
            frame.header.request_id,
            frame.header.kind,
            frame.payload);
        if (completion != PendingCompletionDisposition::OnTime)
            return;
        if (result.status != wrms::CommandStatus::Succeeded)
        {
            std::lock_guard<std::mutex> lock(snapshot_mutex_);
            snapshot_.last_rejection_code =
                MapRejectionCode(result.rejection_code);
            if (!result.message.empty())
                snapshot_.last_error = result.message;
        }
        return;
    }
    case wrms::MessageKind::OpenSessionResult:
    {
        wrms::OpenSessionResultPayload result;
        if (frame.header.request_id == 0 ||
            !wrms::DecodePayload(frame.payload, result))
        {
            fail_protocol(
                "invalid or unsolicited WRMS OpenSessionResult");
            return;
        }
        const auto completion = complete_pending(
            frame.header.request_id,
            frame.header.kind,
            frame.payload);
        if (completion != PendingCompletionDisposition::OnTime)
            return;
        {
            std::lock_guard<std::mutex> lock(snapshot_mutex_);
            if (result.success)
            {
                snapshot_.session_open = true;
                snapshot_.session_id = runtime::SessionId{result.session_id};
                snapshot_.workset_epoch = runtime::WorksetEpoch{result.workset_epoch};
                snapshot_.worker_mode = MapWorkerMode(result.worker_mode);
                snapshot_.worker_state = MapWorkerState(result.worker_state);
                snapshot_.session_disposition =
                    MapSessionDisposition(result.session_disposition);
                snapshot_.execution_activity =
                    wrms::ExecutionActivityCode::IdlePaused;
                snapshot_.execution_operation_id = 0;
                snapshot_.active_execution_control.reset();
                snapshot_.execution_completed_count = 0;
                snapshot_.execution_program_counter = 0;
            }
            else if (!snapshot_.session_open)
            {
                snapshot_.session_id = runtime::SessionId{result.session_id};
                snapshot_.workset_epoch = runtime::WorksetEpoch{result.workset_epoch};
                snapshot_.worker_mode = MapWorkerMode(result.worker_mode);
                snapshot_.worker_state = MapWorkerState(result.worker_state);
                snapshot_.session_disposition =
                    MapSessionDisposition(result.session_disposition);
            }
            snapshot_.last_rejection_code =
                MapRejectionCode(result.rejection_code);
            if (!result.success)
                snapshot_.last_error = result.message;
        }
        return;
    }
    case wrms::MessageKind::ShutdownResult:
    {
        wrms::ShutdownResultPayload result;
        if (frame.header.request_id == 0 ||
            !wrms::DecodePayload(frame.payload, result))
        {
            fail_protocol(
                "invalid or unsolicited WRMS ShutdownResult");
            return;
        }
        const auto completion = complete_pending(
            frame.header.request_id,
            frame.header.kind,
            frame.payload);
        if (completion != PendingCompletionDisposition::OnTime)
            return;
        {
            std::lock_guard<std::mutex> lock(snapshot_mutex_);
            snapshot_.shutdown_graceful =
                result.status == wrms::ShutdownStatus::Graceful;
            snapshot_.session_open = false;
            snapshot_.execution_activity =
                wrms::ExecutionActivityCode::IdlePaused;
            snapshot_.execution_operation_id = 0;
            snapshot_.active_execution_control.reset();
            snapshot_.execution_completed_count = 0;
            snapshot_.execution_program_counter = 0;
            snapshot_.session_disposition =
                MapSessionDisposition(result.final_disposition);
            snapshot_.worker_state = runtime::WorkerState::Stopped;
            snapshot_.last_rejection_code =
                MapRejectionCode(result.rejection_code);
            if (!result.message.empty())
                snapshot_.last_error = result.message;
        }
        return;
    }
    case wrms::MessageKind::SessionEvent:
    {
        wrms::SessionEventPayload event;
        if (frame.header.request_id != 0 ||
            !wrms::DecodePayload(frame.payload, event))
        {
            fail_protocol("invalid WRMS SessionEvent");
            return;
        }
        {
            std::lock_guard<std::mutex> lock(snapshot_mutex_);
            snapshot_.session_id = runtime::SessionId{event.session_id};
            snapshot_.workset_epoch = runtime::WorksetEpoch{event.workset_epoch};
            snapshot_.worker_mode = MapWorkerMode(event.worker_mode);
            snapshot_.worker_state = MapWorkerState(event.worker_state);
            snapshot_.session_disposition =
                MapSessionDisposition(event.session_disposition);
            snapshot_.session_open =
                event.session_disposition !=
                wrms::SessionDispositionCode::Closed;
            if (!snapshot_.session_open)
            {
                snapshot_.execution_activity =
                    wrms::ExecutionActivityCode::IdlePaused;
                snapshot_.execution_operation_id = 0;
                snapshot_.active_execution_control.reset();
                snapshot_.execution_completed_count = 0;
                snapshot_.execution_program_counter = 0;
            }
            snapshot_.last_rejection_code =
                MapRejectionCode(event.rejection_code);
            if (!event.message.empty())
                snapshot_.last_error = event.message;
        }
        SessionEventCallback callback;
        {
            std::lock_guard<std::mutex> lock(callback_mutex_);
            callback = session_event_callback_;
        }
        if (callback &&
            !enqueue_callback(
                [callback = std::move(callback), event]() {
                    callback(event);
                },
                true,
                sizeof(event) + event.message.size()))
        {
            fail_protocol(
                "authoritative SessionEvent callback queue is full");
        }
        return;
    }
    case wrms::MessageKind::InvocationProgress:
    {
        wrms::InvocationProgressPayload progress;
        if (frame.header.request_id != 0 ||
            !wrms::DecodePayload(frame.payload, progress))
        {
            fail_protocol("invalid WRMS InvocationProgress event");
            return;
        }
        InvocationProgressCallback callback;
        {
            std::lock_guard<std::mutex> lock(callback_mutex_);
            callback = invocation_progress_callback_;
        }
        if (callback)
            enqueue_callback(
                [callback = std::move(callback), progress]() {
                    callback(progress);
                },
                false,
                sizeof(progress)
                    + progress.durable_job_id.size()
                    + progress.library_id.size()
                    + progress.progress_point_id.size()
                    + progress.schema_id.size()
                    + progress.schema_sha256.size()
                    + progress.typed_payload.size()
                    + progress.display_text.size());

        return;
    }
    case wrms::MessageKind::HostEvent:
    {
        wrms::HostEventPayload event;
        if (frame.header.request_id != 0 ||
            !wrms::DecodePayload(frame.payload, event))
        {
            fail_protocol("invalid WRMS HostEvent");
            return;
        }
        HostEventCallback callback;
        {
            std::lock_guard<std::mutex> lock(callback_mutex_);
            callback = host_event_callback_;
        }
        if (callback)
            enqueue_callback(
                [callback = std::move(callback), event]() {
                    callback(event);
                },
                false,
                sizeof(event) + event.name.size() +
                    event.event_data.size());
        return;
    }
    case wrms::MessageKind::RuntimeDiagnostic:
    {
        wrms::RuntimeDiagnosticPayload diagnostic;
        if (frame.header.request_id != 0 ||
            !wrms::DecodePayload(frame.payload, diagnostic))
        {
            fail_protocol("invalid WRMS RuntimeDiagnostic event");
            return;
        }
        std::lock_guard<std::mutex> lock(snapshot_mutex_);
        snapshot_.last_rejection_code =
            MapRejectionCode(diagnostic.rejection_code);
        snapshot_.last_error = diagnostic.message;
        return;
    }
    case wrms::MessageKind::ExecutionResult:
    {
        wrms::ExecutionResultPayload result;
        if (frame.header.request_id == 0 ||
            !wrms::DecodePayload(frame.payload, result))
        {
            fail_protocol(
                "invalid or unsolicited WRMS ExecutionResult");
            return;
        }
        const auto completion = complete_pending(
            frame.header.request_id,
            frame.header.kind,
            frame.payload);
        if (completion != PendingCompletionDisposition::OnTime)
            return;
        {
            std::lock_guard<std::mutex> lock(snapshot_mutex_);
            snapshot_.session_id = runtime::SessionId{result.session_id};
            snapshot_.workset_epoch = runtime::WorksetEpoch{result.workset_epoch};
            snapshot_.execution_activity = result.activity;
            snapshot_.execution_operation_id = result.operation_id;
            snapshot_.execution_completed_count = result.completed_count;
            snapshot_.execution_program_counter = result.program_counter;
            snapshot_.active_execution_control =
                result.activity == wrms::ExecutionActivityCode::IdlePaused ||
                    result.activity == wrms::ExecutionActivityCode::Failed
                ? std::nullopt
                : std::optional{result.control};
            snapshot_.last_rejection_code =
                MapRejectionCode(result.rejection_code);
            if (result.status != wrms::CommandStatus::Succeeded &&
                !result.message.empty())
            {
                snapshot_.last_error = result.message;
            }
        }
        return;
    }
    case wrms::MessageKind::ExecutionState:
    {
        wrms::ExecutionStatePayload state;
        if (frame.header.request_id != 0 ||
            !wrms::DecodePayload(frame.payload, state))
        {
            fail_protocol("invalid WRMS ExecutionState event");
            return;
        }
        {
            std::lock_guard<std::mutex> lock(snapshot_mutex_);
            snapshot_.session_id = runtime::SessionId{state.session_id};
            snapshot_.workset_epoch = runtime::WorksetEpoch{state.workset_epoch};
            snapshot_.execution_activity = state.activity;
            snapshot_.execution_operation_id = state.operation_id;
            snapshot_.active_execution_control = state.has_active_control
                ? std::optional{state.active_control}
                : std::nullopt;
            snapshot_.execution_completed_count = state.completed_count;
            snapshot_.execution_program_counter = state.program_counter;
            snapshot_.last_rejection_code =
                MapRejectionCode(state.rejection_code);
            if (!state.message.empty())
                snapshot_.last_error = state.message;
        }
        ExecutionStateCallback callback;
        {
            std::lock_guard<std::mutex> lock(callback_mutex_);
            callback = execution_state_callback_;
        }
        if (callback)
            enqueue_callback(
                [callback = std::move(callback), state]() {
                    callback(state);
                },
                false,
                sizeof(state) + state.message.size());
        return;
    }
    case wrms::MessageKind::WorksetState:
    {
        wrms::WorksetStatePayload state;
        if (!wrms::DecodePayload(frame.payload, state) ||
            frame.header.request_id != 0)
        {
            fail_protocol("invalid WRMS WorksetState event");
            return;
        }
        if (!accept_workset_outbound_sequence(
                state.outbound_sequence))
            return;
        {
            std::lock_guard<std::mutex> lock(snapshot_mutex_);
            const runtime::WorkerWorksetId id{state.workset_id};
            switch (state.state)
            {
            case wrms::WorksetStateCode::Admitted:
                snapshot_.staged_workset = id;
                break;
            case wrms::WorksetStateCode::Initializing:
            case wrms::WorksetStateCode::Ready:
            case wrms::WorksetStateCode::Running:
            case wrms::WorksetStateCode::ResettingItem:
            case wrms::WorksetStateCode::Draining:
                if (snapshot_.active_workset != id)
                    snapshot_.active_workset_item.reset();
                snapshot_.active_workset = id;
                if (snapshot_.staged_workset == id)
                    snapshot_.staged_workset.reset();
                break;
            case wrms::WorksetStateCode::Completed:
            case wrms::WorksetStateCode::Cancelled:
            case wrms::WorksetStateCode::Failed:
                if (snapshot_.active_workset == id)
                {
                    snapshot_.active_workset.reset();
                    snapshot_.active_workset_item.reset();
                }
                if (snapshot_.staged_workset == id)
                    snapshot_.staged_workset.reset();
                break;
            case wrms::WorksetStateCode::Validating:
                break;
            }
            snapshot_.last_rejection_code =
                MapRejectionCode(state.rejection_code);
            if (!state.message.empty())
                snapshot_.last_error = state.message;
        }
        WorksetStateCallback callback;
        {
            std::lock_guard<std::mutex> lock(callback_mutex_);
            callback = workset_state_callback_;
        }
        if (callback)
        {
            if (!enqueue_callback(
                    [callback = std::move(callback), state]() {
                        callback(state);
                    },
                    true,
                    sizeof(state) + state.message.size()))
            {
                fail_protocol(
                    "authoritative WorksetState callback queue is full");
            }
        }
        return;
    }
    case wrms::MessageKind::WorksetItemStarted:
    {
        wrms::WorksetItemStartedPayload started;
        if (!wrms::DecodePayload(frame.payload, started) ||
            frame.header.request_id != 0)
        {
            fail_protocol("invalid WRMS WorksetItemStarted event");
            return;
        }
        if (!accept_workset_outbound_sequence(
                started.outbound_sequence))
            return;
        {
            std::lock_guard<std::mutex> lock(snapshot_mutex_);
            snapshot_.active_workset =
                runtime::WorkerWorksetId{started.workset_id};
            snapshot_.active_workset_item =
                runtime::WorkerWorksetItemId{started.item_id};
            if (snapshot_.staged_workset ==
                runtime::WorkerWorksetId{started.workset_id})
                snapshot_.staged_workset.reset();
            snapshot_.workset_epoch =
                runtime::WorksetEpoch{started.workset_epoch};
        }
        WorksetItemStartedCallback callback;
        {
            std::lock_guard<std::mutex> lock(callback_mutex_);
            callback = workset_item_started_callback_;
        }
        if (callback)
        {
            if (!enqueue_callback(
                    [callback = std::move(callback), started]() {
                        callback(started);
                    },
                    true,
                    sizeof(started) +
                        started.baseline_sha256.size() +
                        started.baseline_lineage.size()))
            {
                fail_protocol(
                    "authoritative WorksetItemStarted callback queue is full");
            }
        }
        return;
    }
    case wrms::MessageKind::WorksetItemTerminal:
    {
        wrms::WorksetItemTerminalPayload terminal;
        if (!wrms::DecodePayload(frame.payload, terminal) ||
            frame.header.request_id != 0)
        {
            fail_protocol("invalid WRMS WorksetItemTerminal event");
            return;
        }
        if (!accept_workset_outbound_sequence(
                terminal.outbound_sequence))
            return;
        {
            std::lock_guard<std::mutex> lock(snapshot_mutex_);
            snapshot_.workset_epoch =
                runtime::WorksetEpoch{terminal.workset_epoch};
            if (snapshot_.active_workset ==
                    runtime::WorkerWorksetId{terminal.workset_id} &&
                snapshot_.active_workset_item ==
                    runtime::WorkerWorksetItemId{terminal.item_id})
            {
                snapshot_.active_workset_item.reset();
            }
            snapshot_.last_rejection_code =
                MapRejectionCode(terminal.rejection_code);
            if (!terminal.message.empty())
                snapshot_.last_error = terminal.message;
        }
        WorksetItemTerminalCallback callback;
        {
            std::lock_guard<std::mutex> lock(callback_mutex_);
            callback = workset_item_terminal_callback_;
        }
        if (callback)
        {
            const std::size_t resident_bytes =
                sizeof(terminal) + terminal.error_code.size() +
                terminal.message.size() + terminal.result.size();
            if (!enqueue_callback(
                    [callback = std::move(callback), terminal]() {
                        callback(terminal);
                    },
                    true,
                    resident_bytes))
            {
                fail_protocol(
                    "authoritative WorksetItemTerminal callback queue is full");
            }
        }
        return;
    }
    case wrms::MessageKind::WorksetCredits:
    {
        wrms::WorksetCreditsPayload credits;
        if (!wrms::DecodePayload(frame.payload, credits) ||
            frame.header.request_id != 0)
        {
            fail_protocol("invalid WRMS WorksetCredits event");
            return;
        }
        if (!accept_workset_outbound_sequence(
                credits.outbound_sequence))
            return;
        {
            std::lock_guard<std::mutex> lock(snapshot_mutex_);
            snapshot_.available_item_credits =
                credits.available_item_credits;
            snapshot_.active_and_staged_items =
                credits.active_and_staged_items;
            snapshot_.retained_terminals =
                credits.retained_terminals;
        }
        WorksetCreditsCallback callback;
        {
            std::lock_guard<std::mutex> lock(callback_mutex_);
            callback = workset_credits_callback_;
        }
        if (callback)
        {
            if (!enqueue_callback(
                [callback = std::move(callback), credits]() {
                    callback(credits);
                },
                true,
                sizeof(credits)))
            {
                fail_protocol(
                    "authoritative WorksetCredits callback queue is full");
            }
        }
        return;
    }
    case wrms::MessageKind::WorksetSummary:
    {
        wrms::WorksetSummaryPayload summary;
        if (!wrms::DecodePayload(frame.payload, summary) ||
            frame.header.request_id != 0)
        {
            fail_protocol("invalid WRMS WorksetSummary event");
            return;
        }
        if (!accept_workset_outbound_sequence(
                summary.outbound_sequence))
            return;
        bool no_resident_workset = false;
        {
            std::lock_guard<std::mutex> lock(snapshot_mutex_);
            const runtime::WorkerWorksetId id{summary.workset_id};
            if (snapshot_.active_workset == id)
            {
                snapshot_.active_workset.reset();
                snapshot_.active_workset_item.reset();
            }
            if (snapshot_.staged_workset == id)
                snapshot_.staged_workset.reset();
            no_resident_workset =
                !snapshot_.active_workset &&
                !snapshot_.staged_workset;
        }
        WorksetSummaryCallback callback;
        {
            std::lock_guard<std::mutex> lock(callback_mutex_);
            callback = workset_summary_callback_;
        }
        if (callback)
        {
            if (!enqueue_callback(
                    [callback = std::move(callback), summary]() {
                        callback(summary);
                    },
                    true,
                    sizeof(summary)))
            {
                fail_protocol(
                    "authoritative WorksetSummary callback queue is full");
            }
        }
        if (no_resident_workset)
            release_slot();
        return;
    }
    default:
        set_last_error("unexpected WRMS message kind from worker");
        return;
    }
}

void ProcessWorker::set_last_error(std::string error)
{
    std::lock_guard<std::mutex> lock(snapshot_mutex_);
    snapshot_.last_error = std::move(error);
}

bool ProcessWorker::is_ready() const
{
    return running_.load(std::memory_order_acquire) &&
        ready_received_.load(std::memory_order_acquire) &&
        ready_ok_.load(std::memory_order_acquire);
}

bool ProcessWorker::is_failed() const
{
    return ready_received_.load(std::memory_order_acquire) &&
        (!ready_ok_.load(std::memory_order_acquire) ||
         !running_.load(std::memory_order_acquire));
}

bool ProcessWorker::wait_ready(std::uint32_t timeout_ms)
{
    std::unique_lock<std::mutex> lock(snapshot_mutex_);
    hello_cv_.wait_for(
        lock,
        std::chrono::milliseconds{
            timeout_ms ? timeout_ms : kDefaultRequestTimeoutMs},
        [&]() {
            return ready_received_.load(std::memory_order_acquire) ||
                !running_.load(std::memory_order_acquire);
        });
    return is_ready();
}

void ProcessWorker::begin_stop()
{
    bool already_stopping = false;
    {
        std::lock_guard<std::mutex> lock(stop_completion_mutex_);
        already_stopping =
            stop_started_.exchange(true, std::memory_order_acq_rel);
        if (!already_stopping)
        {
            stop_begin_completed_ = false;
            stop_finishing_ = false;
            stop_completed_ = false;
        }
    }
    if (already_stopping)
    {
        stop_repeated_.store(true, std::memory_order_release);
        return;
    }

    ProcessWorkerStopSnapshot snapshot;
    snapshot.was_running = running_.load(std::memory_order_acquire);
    accepting_writes_.store(false, std::memory_order_release);
    if (test_hooks_ && test_hooks_->stop_acceptance_closed)
        test_hooks_->stop_acceptance_closed();

    auto grace = kProcessWorkerDefaultStopGrace;
    if (test_hooks_ && test_hooks_->stop_grace)
        grace = *test_hooks_->stop_grace;
    if (grace < std::chrono::milliseconds::zero())
        grace = std::chrono::milliseconds::zero();
    const auto grace_count = std::min<std::int64_t>(
        grace.count(),
        std::numeric_limits<std::uint32_t>::max());
    snapshot.stop_grace_ms = static_cast<std::uint32_t>(grace_count);
    const auto deadline =
        std::chrono::steady_clock::now() +
        grace;
    std::shared_ptr<PendingResponse> shutdown_pending;
    std::shared_ptr<OutboundWrite> shutdown_write;
    bool shutdown_reported_graceful = false;

    if (snapshot.was_running && child_stdin_write_)
    {
        wrms::ShutdownPayload shutdown{
            .grace_period_ms = snapshot.stop_grace_ms};
        std::vector<std::uint8_t> payload;
        if (EncodeTypedPayload(shutdown, &payload))
        {
            const auto raw_id =
                next_request_id_.fetch_add(1, std::memory_order_acq_rel);
            snapshot.shutdown_request_id = raw_id;
            snapshot.shutdown_frame_attempted = true;
            shutdown_pending = std::make_shared<PendingResponse>();
            shutdown_pending->request_kind =
                wrms::MessageKind::Shutdown;
            shutdown_pending->expected_response_kind =
                wrms::MessageKind::ShutdownResult;
            shutdown_pending->enqueued_at =
                std::chrono::steady_clock::now();
            shutdown_pending->deadline = deadline;
            {
                std::lock_guard<std::mutex> lock(pending_mutex_);
                pending_.emplace(raw_id, shutdown_pending);
                std::lock_guard<std::mutex> snapshot_lock(snapshot_mutex_);
                snapshot_.pending_response_count = pending_.size();
            }
            shutdown_write = enqueue_frame(
                wrms::MessageKind::Shutdown,
                runtime::WireRequestId{raw_id},
                payload,
                true);
            if (!shutdown_write)
            {
                std::lock_guard<std::mutex> lock(pending_mutex_);
                pending_.erase(raw_id);
                shutdown_pending.reset();
            }
        }
    }

    {
        std::lock_guard<std::mutex> lock(stop_completion_mutex_);
        stop_deadline_ = deadline;
        stop_in_progress_snapshot_ = snapshot;
        stop_shutdown_pending_ = std::move(shutdown_pending);
        stop_shutdown_write_ = std::move(shutdown_write);
        stop_begin_completed_ = true;
    }
    stop_completion_cv_.notify_all();
}

void ProcessWorker::finish_stop()
{
    begin_stop();

    std::chrono::steady_clock::time_point deadline;
    ProcessWorkerStopSnapshot snapshot;
    std::shared_ptr<PendingResponse> shutdown_pending;
    std::shared_ptr<OutboundWrite> shutdown_write;
    {
        std::unique_lock<std::mutex> lock(stop_completion_mutex_);
        stop_completion_cv_.wait(
            lock,
            [&]() { return stop_begin_completed_; });
        if (stop_completed_)
        {
            lock.unlock();
            const auto callback_deadline =
                std::chrono::steady_clock::now() +
                kProcessWorkerDefaultCallbackCleanupGrace;
            (void)stop_callback_dispatch(callback_deadline);
            std::lock_guard<std::mutex> snapshot_lock(stop_mutex_);
            last_stop_snapshot_.already_stopping = true;
            last_stop_snapshot_.callback_dispatcher_joined =
                !callback_dispatcher_.joinable();
            return;
        }
        if (stop_finishing_)
        {
            if (on_callback_dispatcher_thread())
            {
                lock.unlock();
                const auto callback_deadline =
                    std::chrono::steady_clock::now();
                (void)stop_callback_dispatch(callback_deadline);
                return;
            }
            stop_completion_cv_.wait(
                lock,
                [&]() { return stop_completed_; });
            lock.unlock();
            const auto callback_deadline =
                std::chrono::steady_clock::now() +
                kProcessWorkerDefaultCallbackCleanupGrace;
            (void)stop_callback_dispatch(callback_deadline);
            std::lock_guard<std::mutex> snapshot_lock(stop_mutex_);
            last_stop_snapshot_.already_stopping = true;
            last_stop_snapshot_.callback_dispatcher_joined =
                !callback_dispatcher_.joinable();
            return;
        }
        stop_finishing_ = true;
        deadline = stop_deadline_;
        snapshot = stop_in_progress_snapshot_;
        shutdown_pending = stop_shutdown_pending_;
        shutdown_write = stop_shutdown_write_;
    }

    bool shutdown_reported_graceful = false;

    if (shutdown_write)
    {
        std::unique_lock<std::mutex> lock(shutdown_write->mutex);
        if (!shutdown_write->cv.wait_until(
                lock,
                deadline,
                [&]() { return shutdown_write->completed; }))
        {
            snapshot.deadline_expired = true;
        }
        snapshot.shutdown_frame_succeeded =
            shutdown_write->completed &&
            shutdown_write->succeeded;
        if (!snapshot.shutdown_frame_succeeded)
            shutdown_pending.reset();
    }

    if (shutdown_pending && !snapshot.deadline_expired)
    {
        std::unique_lock<std::mutex> lock(shutdown_pending->mutex);
        if (!shutdown_pending->cv.wait_until(
                lock,
                deadline,
                [&]() { return shutdown_pending->completed; }))
        {
            snapshot.deadline_expired = true;
        }
        const bool response_arrived =
            shutdown_pending->completed &&
            shutdown_pending->transport_ok &&
            shutdown_pending->response_kind ==
                wrms::MessageKind::ShutdownResult;
        snapshot.shutdown_result_received =
            response_arrived &&
            classify_shutdown_response(
                shutdown_pending->payload,
                &shutdown_reported_graceful);
    }
    if (snapshot.shutdown_request_id != 0)
    {
        std::lock_guard<std::mutex> pending_lock(pending_mutex_);
        pending_.erase(snapshot.shutdown_request_id);
    }

    if (process_handle_)
    {
        const auto remaining = RemainingMilliseconds(deadline);
        if (remaining == 0)
        {
            snapshot.deadline_expired = true;
            snapshot.cooperative_wait_result = WAIT_TIMEOUT;
        }
        else
        {
            snapshot.cooperative_wait_result =
                WaitForSingleObject(process_handle_, remaining);
            if (snapshot.cooperative_wait_result == WAIT_TIMEOUT)
                snapshot.deadline_expired = true;
        }
    }

    request_writer_stop(&snapshot);
    join_writer(&snapshot);

    if (child_stdin_write_)
    {
        if (test_hooks_ && test_hooks_->before_stdin_close)
            test_hooks_->before_stdin_close();
        snapshot.stdin_close_attempted = true;
        if (CloseHandle(child_stdin_write_))
            snapshot.stdin_close_succeeded = true;
        else
            snapshot.stdin_close_error = GetLastError();
        child_stdin_write_ = nullptr;
    }

    bool process_alive = false;
    if (process_handle_)
    {
        std::optional<bool> override;
        if (test_hooks_ && test_hooks_->process_alive_override)
            override = test_hooks_->process_alive_override();
        if (override)
        {
            process_alive = *override;
        }
        else
        {
            DWORD exit_code = 0;
            process_alive =
                GetExitCodeProcess(process_handle_, &exit_code) &&
                exit_code == STILL_ACTIVE;
        }
    }

    if (process_alive)
    {
        snapshot.forced = true;
        snapshot.termination_attempted = true;
        if (job_handle_)
        {
            snapshot.termination_method = "job";
            snapshot.termination_succeeded =
                TerminateJobObject(job_handle_, 1) != 0;
        }
        else
        {
            snapshot.termination_method = "process";
            snapshot.termination_succeeded =
                TerminateProcess(process_handle_, 1) != 0;
        }
        if (!snapshot.termination_succeeded)
            snapshot.termination_error = GetLastError();
    }

    if (child_stdout_read_)
    {
        snapshot.cancel_pipe_attempted = true;
        snapshot.cancel_pipe_succeeded =
            CancelIoEx(child_stdout_read_, nullptr) != 0;
        if (!snapshot.cancel_pipe_succeeded)
            snapshot.cancel_pipe_error = GetLastError();
    }
    if (child_event_read_)
    {
        snapshot.cancel_event_reader_attempted = true;
        snapshot.cancel_event_reader_succeeded =
            CancelIoEx(child_event_read_, nullptr) != 0;
        if (!snapshot.cancel_event_reader_succeeded)
            snapshot.cancel_event_reader_error = GetLastError();
    }
    if (reader_.joinable())
    {
        snapshot.cancel_reader_attempted = true;
        snapshot.cancel_reader_succeeded =
            CancelSynchronousIo(reader_.native_handle()) != 0;
        if (!snapshot.cancel_reader_succeeded)
            snapshot.cancel_reader_error = GetLastError();
        reader_.join();
        snapshot.reader_joined = true;
    }
    if (event_reader_.joinable())
    {
        snapshot.cancel_event_reader_attempted = true;
        snapshot.cancel_event_reader_succeeded =
            CancelSynchronousIo(event_reader_.native_handle()) != 0;
        if (!snapshot.cancel_event_reader_succeeded)
            snapshot.cancel_event_reader_error = GetLastError();
        event_reader_.join();
        snapshot.event_reader_joined = true;
    }
    running_.store(false, std::memory_order_release);
    fail_all_pending();
    auto callback_grace =
        kProcessWorkerDefaultCallbackCleanupGrace;
    if (test_hooks_ && test_hooks_->callback_cleanup_grace)
        callback_grace = *test_hooks_->callback_cleanup_grace;
    if (callback_grace < std::chrono::milliseconds::zero())
        callback_grace = std::chrono::milliseconds::zero();
    const bool callback_within_grace =
        stop_callback_dispatch(
            std::chrono::steady_clock::now() +
            callback_grace);
    snapshot.callback_dispatcher_joined =
        !callback_dispatcher_.joinable();
    snapshot.callback_cleanup_timed_out =
        !callback_within_grace;

    if (process_handle_)
    {
        snapshot.process_wait_result =
            WaitForSingleObject(process_handle_, snapshot.forced ? 2000 : 0);
        if (snapshot.process_wait_result == WAIT_FAILED)
            snapshot.process_wait_error = GetLastError();
        DWORD exit_code = 0;
        if (GetExitCodeProcess(process_handle_, &exit_code))
            snapshot.process_exit_code = exit_code;
    }
    snapshot.graceful =
        !snapshot.deadline_expired &&
        !snapshot.callback_cleanup_timed_out &&
        !snapshot.forced &&
        snapshot.shutdown_result_received &&
        shutdown_reported_graceful &&
        snapshot.process_wait_result == WAIT_OBJECT_0;

    const bool process_exit_confirmed =
        !process_handle_ || snapshot.process_wait_result == WAIT_OBJECT_0;
    if (process_exit_confirmed)
    {
        close_process_handles(&snapshot);
    }
    else if (child_stdout_read_ || child_event_read_)
    {
        if (child_stdout_read_)
        {
            CloseHandle(child_stdout_read_);
            child_stdout_read_ = nullptr;
        }
        if (child_event_read_)
        {
            CloseHandle(child_event_read_);
            child_event_read_ = nullptr;
        }
    }
    {
        std::lock_guard<std::mutex> lock(snapshot_mutex_);
        snapshot_.running = false;
        snapshot_.shutdown_graceful = snapshot.graceful;
        snapshot_.worker_state = runtime::WorkerState::Stopped;
    }
    {
        std::lock_guard<std::mutex> lock(stop_mutex_);
        snapshot.already_stopping =
            stop_repeated_.load(std::memory_order_acquire);
        last_stop_snapshot_ = snapshot;
    }
    {
        std::lock_guard<std::mutex> lock(stop_completion_mutex_);
        stop_shutdown_pending_.reset();
        stop_shutdown_write_.reset();
        stop_finishing_ = false;
        stop_completed_ = true;
    }
    stop_completion_cv_.notify_all();
}

void ProcessWorker::stop()
{
    finish_stop();
}

bool ProcessWorker::confirm_process_exit()
{
    {
        std::lock_guard<std::mutex> completion_lock(stop_completion_mutex_);
        if (stop_started_.load(std::memory_order_acquire) && !stop_completed_)
            return false;
    }

    std::lock_guard<std::mutex> lock(stop_mutex_);
    if (!process_handle_)
        return true;

    const DWORD wait_result = WaitForSingleObject(process_handle_, 0);
    last_stop_snapshot_.process_wait_result = wait_result;
    if (wait_result == WAIT_FAILED)
    {
        last_stop_snapshot_.process_wait_error = GetLastError();
        return false;
    }
    if (wait_result != WAIT_OBJECT_0)
        return false;

    DWORD exit_code = 0;
    if (GetExitCodeProcess(process_handle_, &exit_code))
        last_stop_snapshot_.process_exit_code = exit_code;
    close_process_handles(&last_stop_snapshot_);
    return true;
}

void ProcessWorker::close_process_handles(
    ProcessWorkerStopSnapshot*)
{
    if (child_stdout_read_)
    {
        CloseHandle(child_stdout_read_);
        child_stdout_read_ = nullptr;
    }
    if (child_event_read_)
    {
        CloseHandle(child_event_read_);
        child_event_read_ = nullptr;
    }
    if (process_handle_)
    {
        CloseHandle(process_handle_);
        process_handle_ = nullptr;
    }
    if (process_thread_handle_)
    {
        CloseHandle(process_thread_handle_);
        process_thread_handle_ = nullptr;
    }
    if (job_handle_)
    {
        CloseHandle(job_handle_);
        job_handle_ = nullptr;
    }
    process_id_ = 0;
}

ProcessWorkerStopSnapshot ProcessWorker::last_stop_snapshot() const
{
    std::lock_guard<std::mutex> lock(stop_mutex_);
    return last_stop_snapshot_;
}

} // namespace savor

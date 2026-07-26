#include "ProcessWorker.h"

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
    case Wire::StateEpochMismatch: return Runtime::StateEpochMismatch;
    case Wire::BackendFailure: return Runtime::BackendFailure;
    case Wire::RuntimeStopping: return Runtime::RuntimeStopping;
    case Wire::InternalFailure: return Runtime::InternalFailure;
    }
    return Runtime::InternalFailure;
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
}

bool ProcessWorker::create_child(
    const ProcessLaunchOptions& options,
    std::string* error_out)
{
    SECURITY_ATTRIBUTES inheritable{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};

    HANDLE child_stdout_read = nullptr;
    HANDLE child_stdout_write = nullptr;
    HANDLE child_stdin_read = nullptr;
    HANDLE child_stdin_write = nullptr;
    HANDLE child_stderr = nullptr;

    const auto fail = [&](std::string message) {
        if (child_stdout_read)
            CloseHandle(child_stdout_read);
        if (child_stdout_write)
            CloseHandle(child_stdout_write);
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
            << " --id " << options.worker_id;
    if (!options.log_directory.empty())
        command << " --log-dir \"" << options.log_directory << '"';

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
    accepting_writes_.store(false, std::memory_order_release);
    ready_received_.store(false, std::memory_order_release);
    ready_ok_.store(false, std::memory_order_release);
    ready_error_.store(0, std::memory_order_release);
    busy_.store(false, std::memory_order_release);
    next_request_id_.store(1, std::memory_order_release);
    worker_id_ = options.worker_id;
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
        hello_ = {};
    }

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

    if (!negotiated || !ready_ok_.load(std::memory_order_acquire))
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
        .visual_requested = options.visual,
        .render_window_handle = options.render_widget_handle,
        .screenshot_directory = options.screenshot_directory,
        .screenshot_timeout_ms = options.screenshot_timeout_ms,
        .screenshot_on_terminal = options.screenshot_on_terminal,
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

    if (error_out)
        error_out->clear();
    return true;
}

bool ProcessWorker::prepare_encoded_module(
    const runtime::EncodedModuleEnvelope& module,
    wrms::CommandResultPayload* result_out,
    std::uint32_t timeout_ms)
{
    wrms::PrepareModulePayload request{
        .canonical_id = module.identity.canonical_id,
        .revision = module.identity.revision,
        .canonical_hash = module.identity.canonical_hash,
        .format_version = module.format_version,
        .encoded_module = module.payload,
    };
    std::vector<std::uint8_t> payload;
    if (!EncodeTypedPayload(request, &payload))
    {
        set_last_error("failed encoding PrepareModule payload");
        return false;
    }
    ProcessCommandCompletion completion;
    if (!request_response(
            wrms::MessageKind::PrepareModule,
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
        set_last_error("invalid PrepareModule command result");
        return false;
    }
    if (result_out)
        *result_out = result;
    return result.status == wrms::CommandStatus::Succeeded;
}

bool ProcessWorker::submit_encoded_invocation(
    const runtime::EncodedInvocationEnvelope& invocation,
    wrms::CommandResultPayload* result_out,
    std::uint32_t timeout_ms)
{
    wrms::SubmitInvocationPayload request{
        .invocation_id = invocation.invocation_id.value(),
        .attempt_id = invocation.attempt_id.value(),
        .module_canonical_id = invocation.module.canonical_id,
        .module_revision = invocation.module.revision,
        .module_canonical_hash = invocation.module.canonical_hash,
        .entrypoint = invocation.entrypoint,
        .expected_state_epoch = invocation.expected_state_epoch.value(),
        .encoded_invocation = invocation.input_payload,
    };
    std::vector<std::uint8_t> payload;
    if (!EncodeTypedPayload(request, &payload))
    {
        set_last_error("failed encoding SubmitInvocation payload");
        return false;
    }
    ProcessCommandCompletion completion;
    if (!request_response(
            wrms::MessageKind::SubmitInvocation,
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
        set_last_error("invalid SubmitInvocation command result");
        return false;
    }
    if (result_out)
        *result_out = result;
    return result.status == wrms::CommandStatus::Succeeded;
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

bool ProcessWorker::request_screenshot(
    runtime::SessionId session_id,
    std::string output_path,
    std::uint32_t capture_timeout_ms,
    wrms::ScreenshotResultPayload* result_out,
    std::uint32_t command_timeout_ms)
{
    wrms::CaptureScreenshotPayload request{
        .session_id = session_id.value(),
        .output_path = std::move(output_path),
        .timeout_ms = capture_timeout_ms,
    };
    std::vector<std::uint8_t> payload;
    if (!EncodeTypedPayload(request, &payload))
    {
        set_last_error("failed encoding CaptureScreenshot payload");
        return false;
    }
    ProcessCommandCompletion completion;
    if (!request_response(
            wrms::MessageKind::CaptureScreenshot,
            payload,
            wrms::MessageKind::ScreenshotResult,
            command_timeout_ms,
            &completion))
    {
        return false;
    }
    wrms::ScreenshotResultPayload result;
    if (!wrms::DecodePayload(completion.payload, result))
    {
        set_last_error("invalid ScreenshotResult payload");
        return false;
    }
    if (result_out)
        *result_out = result;
    return result.status == wrms::ScreenshotStatus::Captured;
}

runtime::WorkerCapabilityMask ProcessWorker::process_capabilities() const
{
    std::lock_guard<std::mutex> lock(snapshot_mutex_);
    return snapshot_.process_capabilities;
}

runtime::WorkerCapabilityMask ProcessWorker::session_capabilities() const
{
    std::lock_guard<std::mutex> lock(snapshot_mutex_);
    return snapshot_.session_capabilities;
}

bool ProcessWorker::has_process_capability(
    runtime::WorkerCapability capability) const
{
    return runtime::HasCapability(process_capabilities(), capability);
}

bool ProcessWorker::has_session_capability(
    runtime::WorkerCapability capability) const
{
    return runtime::HasCapability(session_capabilities(), capability);
}

ProcessWorkerSnapshot ProcessWorker::latest_snapshot() const
{
    std::lock_guard<std::mutex> lock(snapshot_mutex_);
    return snapshot_;
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

void ProcessWorker::set_invocation_terminal_callback(
    InvocationTerminalCallback callback)
{
    std::lock_guard<std::mutex> lock(callback_mutex_);
    invocation_terminal_callback_ = std::move(callback);
}

void ProcessWorker::set_host_event_callback(HostEventCallback callback)
{
    std::lock_guard<std::mutex> lock(callback_mutex_);
    host_event_callback_ = std::move(callback);
}

bool ProcessWorker::start(ProcStartParams& params, TSQueue<PRResult>* out_queue)
{
    legacy_result_out_ = out_queue;
    if (params.visual_debug)
    {
        set_last_error(
            "interactive visual debugging is unavailable until Dependency Slice 3");
        ready_received_.store(true, std::memory_order_release);
        ready_ok_.store(false, std::memory_order_release);
        ready_error_.store(1, std::memory_order_release);
        return false;
    }

    std::filesystem::path runtime_root =
        params.dolphin_base_dir.empty()
            ? std::filesystem::path(params.exe_path).parent_path()
            : std::filesystem::path(params.dolphin_base_dir);
    std::filesystem::path log_directory =
        std::filesystem::path(params.user_dir).parent_path();

    std::string error;
    if (!launch_and_negotiate(
            ProcessLaunchOptions{
                .worker_id = params.worker_id,
                .exe_path = params.exe_path,
                .log_directory = log_directory.string(),
            },
            &error))
    {
        return false;
    }

    return open_session(
        ProcessOpenSessionOptions{
            .runtime_root = runtime_root.string(),
            .user_directory = params.user_dir,
            .iso_path = params.iso_path,
            .visual = params.visual,
            .render_widget_handle = params.render_widget_handle,
            .screenshot_directory = params.visual_screenshot_dir,
            .screenshot_timeout_ms = 5000,
            .screenshot_on_terminal = !params.visual_screenshot_dir.empty(),
        },
        nullptr,
        &error,
        30000);
}

bool ProcessWorker::send_job(
    std::uint64_t,
    std::uint64_t,
    const PSJob&)
{
    release_slot();
    return fail_disconnected("send_job");
}

bool ProcessWorker::ctl_set_program(
    std::uint8_t,
    std::uint8_t,
    const PSInit&)
{
    return fail_disconnected("ctl_set_program");
}

bool ProcessWorker::ctl_run_init_once()
{
    return fail_disconnected("ctl_run_init_once");
}

bool ProcessWorker::ctl_activate_main()
{
    return fail_disconnected("ctl_activate_main");
}

bool ProcessWorker::visual_pause_emulation()
{
    set_last_error(
        "interactive pause is unavailable until Dependency Slice 3");
    return false;
}

bool ProcessWorker::visual_resume_emulation()
{
    set_last_error(
        "interactive resume is unavailable until Dependency Slice 3");
    return false;
}

bool ProcessWorker::visual_step_vm()
{
    set_last_error(
        "interactive stepping is unavailable until Dependency Slice 3");
    return false;
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
    if (!write->cv.wait_until(
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
    const auto deadline =
        std::chrono::steady_clock::now() +
        std::chrono::milliseconds{
            timeout_ms ? timeout_ms : kDefaultRequestTimeoutMs};
    const auto raw_id = next_request_id_.fetch_add(1, std::memory_order_acq_rel);
    if (raw_id == 0)
    {
        set_last_error("WRMS request ID space exhausted");
        return false;
    }
    const runtime::WireRequestId request_id{raw_id};
    auto pending = std::make_shared<PendingResponse>();
    {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        pending_.emplace(raw_id, pending);
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
        {
            std::lock_guard<std::mutex> lock(pending_mutex_);
            pending_.erase(raw_id);
        }
        set_last_error(write_timed_out
            ? "timed out waiting for WRMS request write"
            : "failed writing WRMS request");
        return false;
    }

    bool completed = false;
    {
        std::unique_lock<std::mutex> lock(pending->mutex);
        completed = pending->cv.wait_until(
            lock,
            deadline,
            [&]() { return pending->completed; });
    }
    {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        pending_.erase(raw_id);
    }

    if (!completed || !pending->transport_ok)
    {
        set_last_error(completed
            ? "worker transport closed before command completion"
            : "timed out waiting for WRMS command completion");
        return false;
    }
    if (pending->response_kind != expected_response_kind)
    {
        set_last_error("worker returned an unexpected WRMS response kind");
        return false;
    }

    if (completion_out)
    {
        completion_out->request_id = request_id;
        completion_out->response_kind = pending->response_kind;
        completion_out->payload = std::move(pending->payload);
    }
    return true;
}

void ProcessWorker::complete_pending(
    std::uint64_t request_id,
    wrms::MessageKind kind,
    std::span<const std::uint8_t> payload)
{
    std::shared_ptr<PendingResponse> pending;
    {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        const auto found = pending_.find(request_id);
        if (found == pending_.end())
            return;
        pending = found->second;
    }
    {
        std::lock_guard<std::mutex> lock(pending->mutex);
        if (pending->completed)
            return;
        pending->response_kind = kind;
        pending->payload.assign(payload.begin(), payload.end());
        pending->transport_ok = true;
        pending->completed = true;
    }
    pending->cv.notify_all();
}

void ProcessWorker::fail_all_pending()
{
    std::vector<std::shared_ptr<PendingResponse>> pending;
    {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        pending.reserve(pending_.size());
        for (const auto& [_, response] : pending_)
            pending.push_back(response);
    }
    for (const auto& response : pending)
    {
        {
            std::lock_guard<std::mutex> lock(response->mutex);
            if (response->completed)
                continue;
            response->completed = true;
            response->transport_ok = false;
        }
        response->cv.notify_all();
    }
}

void ProcessWorker::reader_thread()
{
    set_this_thread_name_utf8(
        ("WorkerReaderV1-" + std::to_string(worker_id_)).c_str());

    std::vector<std::uint8_t> buffered;
    buffered.reserve(64 * 1024);
    std::array<std::uint8_t, 64 * 1024> chunk{};
    bool clean_eof = false;

    while (running_.load(std::memory_order_acquire))
    {
        DWORD read = 0;
        if (!ReadFile(
                child_stdout_read_,
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
                set_last_error(
                    "worker emitted an invalid WRMS frame (error " +
                    std::to_string(static_cast<unsigned>(decoded.error)) + ")");
                clean_eof = false;
                running_.store(false, std::memory_order_release);
                break;
            }

            if (wrms::DirectionOf(decoded.frame.header.kind) !=
                wrms::MessageDirection::WorkerToParent)
            {
                set_last_error("worker emitted a parent-to-worker WRMS message");
                clean_eof = false;
                running_.store(false, std::memory_order_release);
                break;
            }
            handle_frame(decoded.frame);
            buffered.erase(
                buffered.begin(),
                buffered.begin() + decoded.consumed_size);
        }
    }

    if (clean_eof && !buffered.empty())
    {
        const auto truncated = wrms::DecodeFrame(buffered, true);
        if (truncated.status == wrms::FrameDecodeStatus::Error)
        {
            set_last_error(
                "worker closed stdout with a truncated WRMS frame");
        }
    }

    running_.store(false, std::memory_order_release);
    accepting_writes_.store(false, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(snapshot_mutex_);
        snapshot_.running = false;
    }
    hello_cv_.notify_all();
    fail_all_pending();
    release_slot();
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
            set_last_error("invalid WRMS process hello");
            ready_received_.store(true, std::memory_order_release);
            ready_ok_.store(false, std::memory_order_release);
            ready_error_.store(1, std::memory_order_release);
            hello_cv_.notify_all();
            return;
        }
        {
            std::lock_guard<std::mutex> lock(snapshot_mutex_);
            hello_ = hello;
            snapshot_.hello_received = true;
            snapshot_.process_capabilities = hello.capability_mask;
            snapshot_.worker_state = runtime::WorkerState::AwaitingSession;
        }
        ready_received_.store(true, std::memory_order_release);
        ready_ok_.store(true, std::memory_order_release);
        ready_error_.store(0, std::memory_order_release);
        hello_cv_.notify_all();
        return;
    }
    case wrms::MessageKind::CommandResult:
    {
        wrms::CommandResultPayload result;
        if (wrms::DecodePayload(frame.payload, result) &&
            result.status != wrms::CommandStatus::Succeeded)
        {
            std::lock_guard<std::mutex> lock(snapshot_mutex_);
            snapshot_.last_rejection_code =
                MapRejectionCode(result.rejection_code);
            if (!result.message.empty())
                snapshot_.last_error = result.message;
        }
        complete_pending(
            frame.header.request_id,
            frame.header.kind,
            frame.payload);
        return;
    }
    case wrms::MessageKind::OpenSessionResult:
    {
        wrms::OpenSessionResultPayload result;
        if (wrms::DecodePayload(frame.payload, result))
        {
            std::lock_guard<std::mutex> lock(snapshot_mutex_);
            if (result.success)
            {
                snapshot_.session_open = true;
                snapshot_.session_id = runtime::SessionId{result.session_id};
                snapshot_.state_epoch = runtime::StateEpoch{result.state_epoch};
                snapshot_.session_capabilities = result.capability_mask;
                snapshot_.worker_state = MapWorkerState(result.worker_state);
                snapshot_.session_disposition =
                    MapSessionDisposition(result.session_disposition);
            }
            else if (!snapshot_.session_open)
            {
                snapshot_.session_id = runtime::SessionId{result.session_id};
                snapshot_.state_epoch = runtime::StateEpoch{result.state_epoch};
                snapshot_.session_capabilities = result.capability_mask;
                snapshot_.worker_state = MapWorkerState(result.worker_state);
                snapshot_.session_disposition =
                    MapSessionDisposition(result.session_disposition);
            }
            snapshot_.last_rejection_code =
                MapRejectionCode(result.rejection_code);
            if (!result.success)
                snapshot_.last_error = result.message;
        }
        complete_pending(
            frame.header.request_id,
            frame.header.kind,
            frame.payload);
        return;
    }
    case wrms::MessageKind::ScreenshotResult:
    {
        wrms::ScreenshotResultPayload result;
        if (wrms::DecodePayload(frame.payload, result) &&
            result.status != wrms::ScreenshotStatus::Captured)
        {
            std::lock_guard<std::mutex> lock(snapshot_mutex_);
            snapshot_.last_rejection_code =
                MapRejectionCode(result.rejection_code);
            if (!result.message.empty())
                snapshot_.last_error = result.message;
        }
        complete_pending(
            frame.header.request_id,
            frame.header.kind,
            frame.payload);
        return;
    }
    case wrms::MessageKind::ShutdownResult:
    {
        wrms::ShutdownResultPayload result;
        if (wrms::DecodePayload(frame.payload, result))
        {
            std::lock_guard<std::mutex> lock(snapshot_mutex_);
            snapshot_.shutdown_graceful =
                result.status == wrms::ShutdownStatus::Graceful;
            snapshot_.session_open = false;
            snapshot_.session_disposition =
                MapSessionDisposition(result.final_disposition);
            snapshot_.worker_state = runtime::WorkerState::Stopped;
            snapshot_.last_rejection_code =
                MapRejectionCode(result.rejection_code);
            if (!result.message.empty())
                snapshot_.last_error = result.message;
        }
        complete_pending(
            frame.header.request_id,
            frame.header.kind,
            frame.payload);
        return;
    }
    case wrms::MessageKind::SessionEvent:
    {
        wrms::SessionEventPayload event;
        if (!wrms::DecodePayload(frame.payload, event))
            return;
        std::lock_guard<std::mutex> lock(snapshot_mutex_);
        snapshot_.session_id = runtime::SessionId{event.session_id};
        snapshot_.state_epoch = runtime::StateEpoch{event.state_epoch};
        snapshot_.session_capabilities = event.capability_mask;
        snapshot_.worker_state = MapWorkerState(event.worker_state);
        snapshot_.session_disposition =
            MapSessionDisposition(event.session_disposition);
        snapshot_.session_open =
            event.session_disposition !=
            wrms::SessionDispositionCode::Closed;
        snapshot_.last_rejection_code =
            MapRejectionCode(event.rejection_code);
        if (!event.message.empty())
            snapshot_.last_error = event.message;
        return;
    }
    case wrms::MessageKind::InvocationProgress:
    {
        wrms::InvocationProgressPayload progress;
        if (!wrms::DecodePayload(frame.payload, progress))
            return;
        InvocationProgressCallback callback;
        {
            std::lock_guard<std::mutex> lock(callback_mutex_);
            callback = invocation_progress_callback_;
        }
        if (callback)
            callback(progress);

        PRProgress legacy;
        legacy.worker_id = worker_id_;
        legacy.job_id = progress.invocation_id;
        legacy.text.assign(
            reinterpret_cast<const char*>(progress.progress.data()),
            progress.progress.size());
        legacy.record_progress = true;
        {
            std::lock_guard<std::mutex> lock(progress_mutex_);
            last_progress_ = legacy;
            have_progress_ = true;
        }
        if (progress_out_)
            progress_out_->push(std::move(legacy));
        return;
    }
    case wrms::MessageKind::InvocationTerminal:
    {
        wrms::InvocationTerminalPayload terminal;
        if (!wrms::DecodePayload(frame.payload, terminal))
            return;
        InvocationTerminalCallback callback;
        {
            std::lock_guard<std::mutex> lock(callback_mutex_);
            callback = invocation_terminal_callback_;
        }
        if (callback)
            callback(terminal);
        release_slot();
        return;
    }
    case wrms::MessageKind::HostEvent:
    {
        wrms::HostEventPayload event;
        if (!wrms::DecodePayload(frame.payload, event))
            return;
        HostEventCallback callback;
        {
            std::lock_guard<std::mutex> lock(callback_mutex_);
            callback = host_event_callback_;
        }
        if (callback)
            callback(event);
        return;
    }
    case wrms::MessageKind::RuntimeDiagnostic:
    {
        wrms::RuntimeDiagnosticPayload diagnostic;
        if (!wrms::DecodePayload(frame.payload, diagnostic))
            return;
        std::lock_guard<std::mutex> lock(snapshot_mutex_);
        snapshot_.last_rejection_code =
            MapRejectionCode(diagnostic.rejection_code);
        snapshot_.last_error = diagnostic.message;
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

bool ProcessWorker::fail_disconnected(const char* operation)
{
    std::string message = operation ? operation : "legacy worker operation";
    message += ": ";
    message += kDisconnectedWorkerApiDiagnostic;
    set_last_error(std::move(message));
    return false;
}

bool ProcessWorker::is_ready() const
{
    return ready_received_.load(std::memory_order_acquire) &&
        ready_ok_.load(std::memory_order_acquire);
}

bool ProcessWorker::is_failed() const
{
    return ready_received_.load(std::memory_order_acquire) &&
        !ready_ok_.load(std::memory_order_acquire);
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

bool ProcessWorker::try_get_last_progress(PRProgress& out) const
{
    std::lock_guard<std::mutex> lock(progress_mutex_);
    if (!have_progress_)
        return false;
    out = last_progress_;
    return true;
}

void ProcessWorker::stop()
{
    if (stop_started_.exchange(true, std::memory_order_acq_rel))
    {
        stop_repeated_.store(true, std::memory_order_release);
        std::lock_guard<std::mutex> lock(stop_mutex_);
        last_stop_snapshot_.already_stopping = true;
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
            {
                std::lock_guard<std::mutex> lock(pending_mutex_);
                pending_.emplace(raw_id, shutdown_pending);
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
        snapshot.shutdown_result_received =
            shutdown_pending->completed &&
            shutdown_pending->transport_ok &&
            shutdown_pending->response_kind ==
                wrms::MessageKind::ShutdownResult;
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

    running_.store(false, std::memory_order_release);
    fail_all_pending();

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
        !snapshot.forced &&
        snapshot.shutdown_result_received &&
        snapshot.process_wait_result == WAIT_OBJECT_0;

    close_process_handles(&snapshot);
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
}

void ProcessWorker::close_process_handles(
    ProcessWorkerStopSnapshot*)
{
    if (child_stdout_read_)
    {
        CloseHandle(child_stdout_read_);
        child_stdout_read_ = nullptr;
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

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <future>
#include <iostream>
#include <limits>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <windows.h>

#include "Core/HostStubs.h"
#include "Runner/IPC/WrmsProtocol.h"
#include "Runner/Runtime/WorkerRuntime.h"
#include "Runner/Runtime/Worksets/WorksetWireCodec.h"
#include "Utils/Log.h"
#include "Utils/Hash.h"
#include "Utils/ThreadName.h"

namespace {

using savor::runtime::WorkerCommandResult;
using savor::runtime::WorkerEvent;
using savor::wrms::MessageKind;

enum class WorkerExitCode : int {
    Success = 0,
    InvalidHandles = 100,
    ProtocolFailure = 101,
    PublisherFailure = 102,
};

template <class... Ts>
struct Overloaded : Ts... {
    using Ts::operator()...;
};
template <class... Ts>
Overloaded(Ts...) -> Overloaded<Ts...>;

bool WriteAll(HANDLE handle, const void* data, std::size_t size) {
    const auto* next = static_cast<const std::uint8_t*>(data);
    while (size != 0) {
        DWORD written = 0;
        const auto chunk = static_cast<DWORD>(std::min<std::size_t>(
            size,
            static_cast<std::size_t>(std::numeric_limits<DWORD>::max())));
        if (!WriteFile(handle, next, chunk, &written, nullptr) || written == 0)
            return false;
        next += written;
        size -= written;
    }
    return true;
}

std::uint64_t ParseU64(const char* text) {
    return text
        ? static_cast<std::uint64_t>(std::strtoull(text, nullptr, 10))
        : 0;
}

const char* NextArg(int& index, int argc, char** argv) {
    return index + 1 < argc ? argv[++index] : "";
}

std::filesystem::path ExecutableDirectory() {
    std::array<wchar_t, 32768> buffer{};
    const DWORD length = GetModuleFileNameW(
        nullptr,
        buffer.data(),
        static_cast<DWORD>(buffer.size()));
    if (length == 0 || length == buffer.size())
        return std::filesystem::current_path();
    return std::filesystem::path(
        std::wstring_view(buffer.data(), length)).parent_path();
}

std::string ErrorCodeString(savor::runtime::WorkerRejectionCode code) {
    return std::to_string(static_cast<unsigned>(code));
}

savor::wrms::WorkerStateCode MapWorkerState(
    savor::runtime::WorkerState state) {
    using RuntimeState = savor::runtime::WorkerState;
    using WireState = savor::wrms::WorkerStateCode;
    switch (state) {
    case RuntimeState::Starting:
        return WireState::Starting;
    case RuntimeState::AwaitingSession:
        return WireState::AwaitingSession;
    case RuntimeState::Ready:
        return WireState::Ready;
    case RuntimeState::InitializingWorkset:
        return WireState::InitializingWorkset;
    case RuntimeState::Running:
        return WireState::Running;
    case RuntimeState::Cancelling:
        return WireState::Cancelling;
    case RuntimeState::Tainted:
        return WireState::Tainted;
    case RuntimeState::Stopping:
        return WireState::Stopping;
    case RuntimeState::Stopped:
        return WireState::Stopped;
    }
    return WireState::Tainted;
}

savor::wrms::SessionDispositionCode MapSessionDisposition(
    savor::runtime::SessionDisposition disposition) {
    using RuntimeDisposition = savor::runtime::SessionDisposition;
    using WireDisposition = savor::wrms::SessionDispositionCode;
    switch (disposition) {
    case RuntimeDisposition::Closed:
        return WireDisposition::Closed;
    case RuntimeDisposition::Clean:
        return WireDisposition::Clean;
    case RuntimeDisposition::CleanWithDiagnostics:
        return WireDisposition::CleanWithDiagnostics;
    case RuntimeDisposition::Tainted:
        return WireDisposition::Tainted;
    }
    return WireDisposition::Tainted;
}

savor::wrms::RejectionCode MapRejectionCode(
    savor::runtime::WorkerRejectionCode code) {
    using RuntimeCode = savor::runtime::WorkerRejectionCode;
    using WireCode = savor::wrms::RejectionCode;
    switch (code) {
    case RuntimeCode::None:
        return WireCode::None;
    case RuntimeCode::Unsupported:
        return WireCode::Unsupported;
    case RuntimeCode::InvalidState:
        return WireCode::InvalidState;
    case RuntimeCode::InvalidArgument:
        return WireCode::InvalidArgument;
    case RuntimeCode::SessionUnavailable:
        return WireCode::SessionUnavailable;
    case RuntimeCode::SessionMismatch:
        return WireCode::SessionMismatch;
    case RuntimeCode::SessionTainted:
        return WireCode::SessionTainted;
    case RuntimeCode::ProgramRuntimeUnavailable:
        return WireCode::ProgramRuntimeUnavailable;
    case RuntimeCode::InvocationAlreadyActive:
        return WireCode::InvocationAlreadyActive;
    case RuntimeCode::InvocationNotActive:
        return WireCode::InvocationNotActive;
    case RuntimeCode::InvocationMismatch:
        return WireCode::InvocationMismatch;
    case RuntimeCode::DuplicateCancellation:
        return WireCode::DuplicateCancellation;
    case RuntimeCode::WorksetEpochMismatch:
        return WireCode::WorksetEpochMismatch;
    case RuntimeCode::BackendFailure:
        return WireCode::BackendFailure;
    case RuntimeCode::RuntimeStopping:
        return WireCode::RuntimeStopping;
    case RuntimeCode::InternalFailure:
        return WireCode::InternalFailure;
    case RuntimeCode::WorksetAlreadyActive:
        return WireCode::WorksetAlreadyActive;
    case RuntimeCode::WorksetNotFound:
        return WireCode::WorksetNotFound;
    case RuntimeCode::WorksetItemNotFound:
        return WireCode::WorksetItemNotFound;
    case RuntimeCode::ProgramPackageRejected:
        return WireCode::ProgramPackageRejected;
    case RuntimeCode::CapacityExceeded:
        return WireCode::CapacityExceeded;
    case RuntimeCode::TerminalNotFound:
        return WireCode::TerminalNotFound;
    case RuntimeCode::TerminalMismatch:
        return WireCode::TerminalMismatch;
    case RuntimeCode::WorksetItemAlreadyTerminal:
        return WireCode::WorksetItemAlreadyTerminal;
    }
    return WireCode::InternalFailure;
}

savor::wrms::CommandStatus MapCommandStatus(const WorkerCommandResult& result) {
    if (result.outcome != savor::runtime::WorkerCommandOutcome::Rejected)
        return savor::wrms::CommandStatus::Succeeded;
    if (result.error.code == savor::runtime::WorkerRejectionCode::Unsupported ||
        result.error.code ==
            savor::runtime::WorkerRejectionCode::ProgramRuntimeUnavailable) {
        return savor::wrms::CommandStatus::Unsupported;
    }
    return savor::wrms::CommandStatus::Rejected;
}

MessageKind MapCommandKind(savor::runtime::WorkerCommandKind kind) {
    switch (kind) {
    case savor::runtime::WorkerCommandKind::OpenSession:
        return MessageKind::OpenSession;
    case savor::runtime::WorkerCommandKind::CancelInvocation:
        return MessageKind::CancelInvocation;
    case savor::runtime::WorkerCommandKind::SubmitWorkset:
        return MessageKind::SubmitWorkset;
    case savor::runtime::WorkerCommandKind::CancelWorksetItem:
        return MessageKind::CancelWorksetItem;
    case savor::runtime::WorkerCommandKind::CancelWorkset:
        return MessageKind::CancelWorkset;
    case savor::runtime::WorkerCommandKind::AcknowledgeTerminal:
        return MessageKind::AcknowledgeTerminal;
    case savor::runtime::WorkerCommandKind::CaptureScreenshot:
        return MessageKind::CaptureScreenshot;
    case savor::runtime::WorkerCommandKind::ControlExecution:
        return MessageKind::ControlExecution;
    case savor::runtime::WorkerCommandKind::Shutdown:
        return MessageKind::Shutdown;
    }
    return MessageKind::Shutdown;
}

savor::wrms::ExecutionControlKind MapExecutionControl(
    savor::runtime::WorkerExecutionControlKind control) {
    using Runtime = savor::runtime::WorkerExecutionControlKind;
    using Wire = savor::wrms::ExecutionControlKind;
    switch (control) {
    case Runtime::Pause:
        return Wire::Pause;
    case Runtime::Resume:
        return Wire::Resume;
    case Runtime::StepFrame:
        return Wire::StepFrame;
    }
    return Wire::Pause;
}

savor::wrms::ExecutionActivityCode MapExecutionActivity(
    savor::runtime::ExecutionActivity activity) {
    using Runtime = savor::runtime::ExecutionActivity;
    using Wire = savor::wrms::ExecutionActivityCode;
    switch (activity) {
    case Runtime::IdlePaused:
        return Wire::IdlePaused;
    case Runtime::HandlingInterruption:
        return Wire::HandlingInterruption;
    case Runtime::Failed:
    case Runtime::Closed:
        return Wire::Failed;
    case Runtime::Continuing:
    case Runtime::SteppingFrame:
    case Runtime::Pausing:
    case Runtime::InteractiveRunning:
        return Wire::InteractiveRunning;
    }
    return Wire::Failed;
}

std::optional<savor::wrms::ExecutionControlKind> MapExecutionOperation(
    savor::runtime::ExecutionOperationKind kind) {
    using Runtime = savor::runtime::ExecutionOperationKind;
    using Wire = savor::wrms::ExecutionControlKind;
    switch (kind) {
    case Runtime::SafePause:
        return Wire::Pause;
    case Runtime::InteractiveResume:
        return Wire::Resume;
    case Runtime::StepFrames:
        return Wire::StepFrame;
    case Runtime::ContinueUntil:
        return std::nullopt;
    }
    return std::nullopt;
}

savor::runtime::WorkerRejectionCode MapExecutionError(
    savor::runtime::ExecutionErrorCode code) {
    using Error = savor::runtime::ExecutionErrorCode;
    using Rejection = savor::runtime::WorkerRejectionCode;
    switch (code) {
    case Error::None:
        return Rejection::None;
    case Error::InvalidArgument:
        return Rejection::InvalidArgument;
    case Error::InvalidState:
    case Error::Busy:
    case Error::InterruptionUnavailable:
    case Error::InterruptionPolicyViolation:
    case Error::InterruptionDepthExceeded:
        return Rejection::InvalidState;
    case Error::WorksetEpochMismatch:
        return Rejection::WorksetEpochMismatch;
    case Error::Unsupported:
    case Error::InputUnavailable:
        return Rejection::Unsupported;
    case Error::RuntimeStopping:
        return Rejection::RuntimeStopping;
    case Error::StopPointFailure:
    case Error::BackendFailure:
        return Rejection::BackendFailure;
    case Error::WrongThread:
        return Rejection::InternalFailure;
    }
    return Rejection::InternalFailure;
}

savor::wrms::ExecutionTerminalStatusCode MapExecutionTerminalStatus(
    savor::runtime::ExecutionTerminalStatus status) {
    using Runtime = savor::runtime::ExecutionTerminalStatus;
    using Wire = savor::wrms::ExecutionTerminalStatusCode;
    switch (status) {
    case Runtime::RequestedCompletion:
        return Wire::RequestedCompletion;
    case Runtime::StepsCompleted:
        return Wire::StepsCompleted;
    case Runtime::Paused:
        return Wire::Paused;
    case Runtime::Cancelled:
        return Wire::Cancelled;
    case Runtime::TimedOut:
        return Wire::TimedOut;
    case Runtime::CoreStalled:
        return Wire::CoreStalled;
    case Runtime::MovieEnded:
        return Wire::MovieEnded;
    case Runtime::UnexpectedStop:
        return Wire::UnexpectedStop;
    case Runtime::InterruptionUnavailable:
        return Wire::InterruptionUnavailable;
    case Runtime::InterruptionAborted:
        return Wire::InterruptionAborted;
    case Runtime::InterruptionDepthExceeded:
        return Wire::InterruptionDepthExceeded;
    case Runtime::InterruptionFailed:
        return Wire::InterruptionFailed;
    case Runtime::WorksetEpochMismatch:
        return Wire::WorksetEpochMismatch;
    case Runtime::Unsupported:
        return Wire::Unsupported;
    case Runtime::BackendFailure:
        return Wire::BackendFailure;
    case Runtime::CleanupFailure:
        return Wire::CleanupFailure;
    }
    return Wire::BackendFailure;
}

savor::wrms::InvocationTerminalStatus MapTerminalStatus(
    savor::runtime::InvocationTerminalStatus status) {
    switch (status) {
    case savor::runtime::InvocationTerminalStatus::Completed:
        return savor::wrms::InvocationTerminalStatus::Succeeded;
    case savor::runtime::InvocationTerminalStatus::Cancelled:
        return savor::wrms::InvocationTerminalStatus::Cancelled;
    case savor::runtime::InvocationTerminalStatus::TimedOut:
        return savor::wrms::InvocationTerminalStatus::TimedOut;
    case savor::runtime::InvocationTerminalStatus::CleanupFailure:
        return savor::wrms::InvocationTerminalStatus::CleanupFailure;
    case savor::runtime::InvocationTerminalStatus::InfrastructureFailure:
        return savor::wrms::InvocationTerminalStatus::InfrastructureFailure;
    default:
        return savor::wrms::InvocationTerminalStatus::Failed;
    }
}

savor::wrms::WorkerModeCode MapWorkerMode(
    savor::runtime::WorkerMode mode) {
    switch (mode) {
    case savor::runtime::WorkerMode::Headless:
        return savor::wrms::WorkerModeCode::Headless;
    case savor::runtime::WorkerMode::Visual:
        return savor::wrms::WorkerModeCode::Visual;
    case savor::runtime::WorkerMode::VisualDebug:
        return savor::wrms::WorkerModeCode::VisualDebug;
    }
    return savor::wrms::WorkerModeCode::Headless;
}

savor::runtime::WorkerMode MapWorkerMode(
    savor::wrms::WorkerModeCode mode) {
    switch (mode) {
    case savor::wrms::WorkerModeCode::Headless:
        return savor::runtime::WorkerMode::Headless;
    case savor::wrms::WorkerModeCode::Visual:
        return savor::runtime::WorkerMode::Visual;
    case savor::wrms::WorkerModeCode::VisualDebug:
        return savor::runtime::WorkerMode::VisualDebug;
    }
    return savor::runtime::WorkerMode::Headless;
}

savor::wrms::WorksetStateCode MapWorksetState(
    savor::runtime::WorkerWorksetState state) {
    using Runtime = savor::runtime::WorkerWorksetState;
    using Wire = savor::wrms::WorksetStateCode;
    switch (state) {
    case Runtime::Validating: return Wire::Validating;
    case Runtime::Admitted: return Wire::Admitted;
    case Runtime::Initializing: return Wire::Initializing;
    case Runtime::Ready: return Wire::Ready;
    case Runtime::Running: return Wire::Running;
    case Runtime::ResettingItem: return Wire::ResettingItem;
    case Runtime::Draining: return Wire::Draining;
    case Runtime::Completed: return Wire::Completed;
    case Runtime::Cancelled: return Wire::Cancelled;
    case Runtime::Failed: return Wire::Failed;
    }
    return Wire::Failed;
}

class OutboundPublisher {
public:
    explicit OutboundPublisher(HANDLE output)
        : output_(output) {
        (void)DuplicateHandle(
            GetCurrentProcess(),
            GetCurrentThread(),
            GetCurrentProcess(),
            &reader_thread_,
            0,
            FALSE,
            DUPLICATE_SAME_ACCESS);
        thread_ = std::thread([this]() { Run(); });
    }

    ~OutboundPublisher() {
        StopAndDrain();
        if (reader_thread_) {
            CloseHandle(reader_thread_);
            reader_thread_ = nullptr;
        }
    }

    OutboundPublisher(const OutboundPublisher&) = delete;
    OutboundPublisher& operator=(const OutboundPublisher&) = delete;

    template <typename Payload>
    bool Publish(
        MessageKind kind,
        std::uint64_t request_id,
        const Payload& payload) {
        std::vector<std::uint8_t> encoded_payload;
        if (!savor::wrms::EncodePayload(payload, encoded_payload)) {
            MarkUnhealthy();
            return false;
        }
        return PublishRaw(kind, request_id, std::move(encoded_payload));
    }

    bool PublishRaw(
        MessageKind kind,
        std::uint64_t request_id,
        std::vector<std::uint8_t> payload) {
        if (!savor::wrms::IsKnownMessageKind(kind) ||
            payload.size() > savor::wrms::MaximumPayloadSize) {
            MarkUnhealthy();
            return false;
        }
        bool enqueue_failed = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stopping_)
                return false;
            try {
                const bool authoritative = IsAuthoritative(kind);
                const std::size_t resident_bytes =
                    payload.size() + sizeof(Record);
                const auto fits = [&]() {
                    return queue_.size() < kMaximumQueuedRecords &&
                        resident_bytes <= kMaximumQueuedBytes &&
                        queued_bytes_ <=
                            kMaximumQueuedBytes - resident_bytes;
                };
                if (!fits()) {
                    for (auto it = queue_.begin();
                         it != queue_.end() && !fits();) {
                        if (it->authoritative) {
                            ++it;
                            continue;
                        }
                        queued_bytes_ -= it->resident_bytes;
                        it = queue_.erase(it);
                    }
                }
                if (!fits()) {
                    if (!authoritative)
                        return true;
                    healthy_.store(false, std::memory_order_release);
                    stopping_ = true;
                    enqueue_failed = true;
                } else {
                    queued_bytes_ += resident_bytes;
                    queue_.push_back(Record{
                        .kind = kind,
                        .request_id = request_id,
                        .payload = std::move(payload),
                        .resident_bytes = resident_bytes,
                        .authoritative = authoritative,
                    });
                }
            } catch (...) {
                healthy_.store(false, std::memory_order_release);
                stopping_ = true;
                enqueue_failed = true;
            }
        }
        if (enqueue_failed) {
            CancelReader();
            available_.notify_one();
            return false;
        }
        available_.notify_one();
        return true;
    }

    void StopAndDrain() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
        }
        available_.notify_one();
        if (thread_.joinable())
            thread_.join();
    }

    bool healthy() const noexcept {
        return healthy_.load(std::memory_order_acquire);
    }

    void Fail() noexcept {
        MarkUnhealthy();
    }

private:
    struct Record {
        MessageKind kind{ MessageKind::ProcessHello };
        std::uint64_t request_id{ 0 };
        std::vector<std::uint8_t> payload;
        std::size_t resident_bytes{ 0 };
        bool authoritative{ true };
    };

    static constexpr std::size_t kMaximumQueuedRecords = 128;
    static constexpr std::size_t kMaximumQueuedBytes =
        128ull * 1024ull * 1024ull;

    static bool IsAuthoritative(MessageKind kind) noexcept {
        switch (kind) {
        case MessageKind::InvocationProgress:
        case MessageKind::HostEvent:
        case MessageKind::RuntimeDiagnostic:
        case MessageKind::ExecutionState:
            return false;
        default:
            return true;
        }
    }

    void MarkUnhealthy() noexcept {
        healthy_.store(false, std::memory_order_release);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
        }
        CancelReader();
        available_.notify_one();
    }

    void CancelReader() noexcept {
        if (reader_thread_)
            (void)CancelSynchronousIo(reader_thread_);
    }

    void Run() {
        set_this_thread_name_utf8("WorkerOutboundV1");
        for (;;) {
            Record record;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                available_.wait(lock, [&]() {
                    return stopping_ || !queue_.empty();
                });
                if (queue_.empty()) {
                    if (stopping_)
                        break;
                    continue;
                }
                record = std::move(queue_.front());
                queue_.pop_front();
                queued_bytes_ -= record.resident_bytes;
            }

            const auto frame = savor::wrms::EncodeFrame(
                record.kind,
                record.request_id,
                record.payload);
            if (!frame ||
                !WriteAll(output_, frame.bytes.data(), frame.bytes.size())) {
                healthy_.store(false, std::memory_order_release);
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    stopping_ = true;
                    queue_.clear();
                    queued_bytes_ = 0;
                }
                CancelReader();
                break;
            }
        }
    }

    HANDLE output_{ nullptr };
    HANDLE reader_thread_{ nullptr };
    mutable std::mutex mutex_;
    std::condition_variable available_;
    std::deque<Record> queue_;
    std::size_t queued_bytes_{ 0 };
    bool stopping_{ false };
    std::atomic<bool> healthy_{ true };
    std::thread thread_;
};

class RequestMetadata {
public:
    struct WorksetCorrelation {
        std::uint64_t workset_id = 0;
        std::uint64_t item_id = 0;
        std::uint32_t item_ordinal = 0;
    };

    void RememberScreenshot(
        std::uint64_t request_id,
        std::string output_path) {
        std::lock_guard<std::mutex> lock(mutex_);
        screenshot_paths_[request_id] = std::move(output_path);
    }

    std::string TakeScreenshot(std::uint64_t request_id) {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = screenshot_paths_.find(request_id);
        if (found == screenshot_paths_.end())
            return {};
        std::string value = std::move(found->second);
        screenshot_paths_.erase(found);
        return value;
    }

    void RememberWorksetItem(
        const savor::runtime::WorkerWorksetItemStartedEvent& item) {
        std::lock_guard<std::mutex> lock(mutex_);
        workset_items_[item.invocation_id.value()] = WorksetCorrelation{
            item.workset_id.value(),
            item.item_id.value(),
            item.item_ordinal};
    }

    WorksetCorrelation FindWorksetItem(
        savor::runtime::InvocationId invocation_id) {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = workset_items_.find(invocation_id.value());
        return found == workset_items_.end()
            ? WorksetCorrelation{}
            : found->second;
    }

    void ForgetWorksetItem(savor::runtime::InvocationId invocation_id) {
        std::lock_guard<std::mutex> lock(mutex_);
        workset_items_.erase(invocation_id.value());
    }

private:
    std::mutex mutex_;
    std::unordered_map<std::uint64_t, std::string> screenshot_paths_;
    std::unordered_map<std::uint64_t, WorksetCorrelation> workset_items_;
};

void PublishCommandCompletion(
    OutboundPublisher& publisher,
    RequestMetadata& metadata,
    const WorkerCommandResult& result) {
    const auto& snapshot = result.snapshot;
    const auto& session = snapshot.session;
    const bool succeeded =
        result.outcome != savor::runtime::WorkerCommandOutcome::Rejected;

    switch (result.command_kind) {
    case savor::runtime::WorkerCommandKind::OpenSession: {
        savor::wrms::OpenSessionResultPayload payload{
            .success = succeeded,
            .session_id = session.session_id.value(),
            .workset_epoch = session.workset_epoch.value(),
            .worker_mode = MapWorkerMode(snapshot.mode),
            .worker_state = MapWorkerState(snapshot.state),
            .session_disposition =
                MapSessionDisposition(session.disposition),
            .rejection_code = MapRejectionCode(result.error.code),
            .error_code = ErrorCodeString(result.error.code),
            .message = result.error.message,
        };
        publisher.Publish(
            MessageKind::OpenSessionResult,
            result.request_id.value(),
            payload);
        return;
    }
    case savor::runtime::WorkerCommandKind::CaptureScreenshot: {
        savor::wrms::ScreenshotResultPayload payload{
            .status = succeeded
                ? savor::wrms::ScreenshotStatus::Captured
                : savor::wrms::ScreenshotStatus::Failed,
            .session_id = session.session_id.value(),
            .workset_epoch = session.workset_epoch.value(),
            .output_path =
                metadata.TakeScreenshot(result.request_id.value()),
            .rejection_code = MapRejectionCode(result.error.code),
            .error_code = ErrorCodeString(result.error.code),
            .message = result.error.message,
        };
        publisher.Publish(
            MessageKind::ScreenshotResult,
            result.request_id.value(),
            payload);
        return;
    }
    case savor::runtime::WorkerCommandKind::ControlExecution: {
        const auto& execution = snapshot.execution;
        const auto control = result.execution_control.value_or(
            savor::runtime::WorkerExecutionControlKind::Pause);
        const auto operation_id = result.execution_operation_id
            ? result.execution_operation_id->value()
            : execution && execution->active_operation
                ? execution->active_operation->value()
                : 0;
        publisher.Publish(
            MessageKind::ExecutionResult,
            result.request_id.value(),
            savor::wrms::ExecutionResultPayload{
                .command_sequence = result.command_sequence.value(),
                .control = MapExecutionControl(control),
                .status = MapCommandStatus(result),
                .session_id = session.session_id.value(),
                .workset_epoch = session.workset_epoch.value(),
                .operation_id = operation_id,
                .has_execution_state = execution.has_value(),
                .activity = execution
                    ? MapExecutionActivity(execution->activity)
                    : savor::wrms::ExecutionActivityCode::IdlePaused,
                .has_terminal_status =
                    result.execution_terminal.has_value(),
                .terminal_status = result.execution_terminal
                    ? MapExecutionTerminalStatus(
                          result.execution_terminal->status)
                    : savor::wrms::ExecutionTerminalStatusCode::
                          RequestedCompletion,
                .completed_count = result.execution_terminal
                    ? result.execution_terminal->completed_count
                    : 0,
                .program_counter = result.execution_terminal
                    ? result.execution_terminal->evidence.pc
                    : execution
                        ? execution->evidence.pc
                        : 0,
                .rejection_code = MapRejectionCode(result.error.code),
                .error_code = ErrorCodeString(result.error.code),
                .message = result.error.message,
            });
        return;
    }
    case savor::runtime::WorkerCommandKind::Shutdown: {
        savor::wrms::ShutdownResultPayload payload{
            .status = succeeded
                ? savor::wrms::ShutdownStatus::Graceful
                : savor::wrms::ShutdownStatus::CleanupFailed,
            .final_disposition =
                MapSessionDisposition(session.disposition),
            .rejection_code = MapRejectionCode(result.error.code),
            .error_code = ErrorCodeString(result.error.code),
            .message = result.error.message,
        };
        publisher.Publish(
            MessageKind::ShutdownResult,
            result.request_id.value(),
            payload);
        return;
    }
    default: {
        savor::wrms::CommandResultPayload payload{
            .command_sequence = result.command_sequence.value(),
            .command_kind = MapCommandKind(result.command_kind),
            .status = MapCommandStatus(result),
            .rejection_code = MapRejectionCode(result.error.code),
            .error_code = ErrorCodeString(result.error.code),
            .message = result.error.message,
        };
        if (result.workset_submission.has_value()) {
            const auto& submission = *result.workset_submission;
            savor::wrms::SubmitWorksetResultPayload typed{
                .workset_id = submission.workset_id.value(),
                .sidecar_version = submission.sidecar_version,
                .applied_item_count = submission.applied_item_count,
                .applied_sidecar_sha256 =
                    submission.applied_sidecar_sha256,
                .already_admitted = submission.disposition
                    == savor::runtime::WorksetSubmissionDispositionV1::
                        AlreadyAdmitted,
            };
            if (!savor::wrms::EncodePayload(typed, payload.result)) {
                payload.status = savor::wrms::CommandStatus::Rejected;
                payload.rejection_code =
                    savor::wrms::RejectionCode::InternalFailure;
                payload.error_code = "SubmitWorksetReceiptEncodingFailed";
                payload.message =
                    "failed encoding the accepted workset receipt";
            }
        }
        publisher.Publish(
            MessageKind::CommandResult,
            result.request_id.value(),
            payload);
        return;
    }
    }
}

void PublishWorkerEvent(
    OutboundPublisher& publisher,
    RequestMetadata& metadata,
    const WorkerEvent& event) {
    std::visit(
        Overloaded{
            [&](const savor::runtime::WorkerStateChangedEvent& state) {
                const auto& session = state.current.session;
                publisher.Publish(
                    MessageKind::SessionEvent,
                    0,
                    savor::wrms::SessionEventPayload{
                        .event_type =
                            savor::wrms::SessionEventType::StateChanged,
                        .session_id = session.session_id.value(),
                        .workset_epoch = session.workset_epoch.value(),
                        .worker_mode = MapWorkerMode(state.current.mode),
                        .worker_state = MapWorkerState(state.current.state),
                        .session_disposition =
                            MapSessionDisposition(session.disposition),
                    });
            },
            [&](const savor::runtime::WorkerCommandCompletedEvent& completed) {
                PublishCommandCompletion(
                    publisher,
                    metadata,
                    completed.result);
            },
            [&](const savor::runtime::progress::CanonicalProgressEventV1& progress) {
                const auto correlation =
                    metadata.FindWorksetItem(progress.invocation_id);
                publisher.Publish(
                    MessageKind::InvocationProgress,
                    0,
                    savor::wrms::InvocationProgressPayload{
                        .invocation_id = progress.invocation_id.value(),
                        .attempt_id = progress.attempt_id.value(),
                        .workset_id = correlation.workset_id,
                        .item_id = correlation.item_id,
                        .item_ordinal = correlation.item_ordinal,
                        .ordinal = progress.ordinal,
                        .durable_job_id = progress.durable_job_id,
                        .library_id = progress.library_id,
                        .library_revision = progress.library_revision,
                        .progress_point_id = progress.progress_point_id,
                        .has_routed_provenance =
                            progress.routed_sequence.has_value(),
                        .routed_sequence = progress.routed_sequence
                            ? progress.routed_sequence->value()
                            : 0,
                        .sample_snapshot_id = progress.sample_snapshot_id
                            ? progress.sample_snapshot_id->value()
                            : 0,
                        .trigger_epoch = progress.trigger_epoch
                            ? progress.trigger_epoch->value()
                            : 0,
                        .schema_id = progress.schema.canonical_id,
                        .schema_revision = progress.schema.revision,
                        .schema_sha256 = progress.schema.sha256,
                        .typed_payload = progress.typed_payload,
                        .display_text = progress.display_text,
                    });
            },
            [&](const savor::runtime::WorkerWorksetStateEvent& state) {
                publisher.Publish(
                    MessageKind::WorksetState,
                    0,
                    savor::wrms::WorksetStatePayload{
                        .outbound_sequence =
                            state.outbound_sequence.value(),
                        .workset_id = state.workset_id.value(),
                        .state = MapWorksetState(state.state),
                        .next_item_ordinal = state.next_item_ordinal,
                        .rejection_code =
                            MapRejectionCode(state.error.code),
                        .message = state.error.message,
                    });
            },
            [&](const savor::runtime::WorkerWorksetItemStartedEvent& started) {
                metadata.RememberWorksetItem(started);
                publisher.Publish(
                    MessageKind::WorksetItemStarted,
                    0,
                    savor::wrms::WorksetItemStartedPayload{
                        .outbound_sequence =
                            started.outbound_sequence.value(),
                        .workset_id = started.workset_id.value(),
                        .item_id = started.item_id.value(),
                        .item_ordinal = started.item_ordinal,
                        .invocation_id = started.invocation_id.value(),
                        .attempt_id = started.attempt_id.value(),
                        .session_id = started.session_id.value(),
                        .workset_epoch = started.workset_epoch.value(),
                        .baseline_sha256 =
                            started.baseline.key.sha256,
                        .baseline_lineage =
                            started.baseline.lineage,
                        .baseline_state_established =
                            started.baseline.state_established,
                    });
            },
            [&](const savor::runtime::WorkerWorksetItemTerminalEvent& item) {
                const auto& correlation = item.correlation;
                const auto& terminal = item.terminal;
                publisher.Publish(
                    MessageKind::WorksetItemTerminal,
                    0,
                    savor::wrms::WorksetItemTerminalPayload{
                        .outbound_sequence =
                            item.outbound_sequence.value(),
                        .workset_id = correlation.workset_id.value(),
                        .item_id = correlation.item_id.value(),
                        .item_ordinal = correlation.item_ordinal,
                        .invocation_id =
                            correlation.invocation_id.value(),
                        .attempt_id = correlation.attempt_id.value(),
                        .terminal_id = correlation.terminal_id.value(),
                        .terminal_order =
                            correlation.terminal_order.value(),
                        .status = MapTerminalStatus(terminal.status),
                        .session_disposition =
                            MapSessionDisposition(
                                terminal.session_disposition),
                        .workset_epoch =
                            terminal.workset_epoch.value(),
                        .unstarted = item.unstarted,
                        .rejection_code =
                            MapRejectionCode(terminal.error.code),
                        .error_code =
                            ErrorCodeString(terminal.error.code),
                        .message = terminal.error.message,
                        .result = terminal.output_payload,
                        .workset_artifacts = [&] {
                            std::vector<savor::wrms::WorksetArtifactPayload>
                                artifacts;
                            artifacts.reserve(
                                terminal.workset_artifacts.size());
                            for (const auto& artifact :
                                 terminal.workset_artifacts) {
                                artifacts.push_back({
                                    .artifact_id = artifact.artifact_id,
                                    .schema_id =
                                        artifact.schema.canonical_id,
                                    .schema_version =
                                        artifact.schema.version,
                                    .schema_sha256 =
                                        artifact.schema.schema_hash.ToHex(),
                                    .content_sha256 =
                                        artifact.content_hash.ToHex(),
                                    .storage_reference =
                                        artifact.storage_reference,
                                    .complete = artifact.complete,
                                });
                            }
                            return artifacts;
                        }(),
                        .diagnostics = terminal.diagnostics,
                    });
                metadata.ForgetWorksetItem(
                    correlation.invocation_id);
            },
            [&](const savor::runtime::WorkerWorksetCreditEvent& credits) {
                publisher.Publish(
                    MessageKind::WorksetCredits,
                    0,
                    savor::wrms::WorksetCreditsPayload{
                        .outbound_sequence =
                            credits.outbound_sequence.value(),
                        .available_item_credits =
                            credits.available_item_credits,
                        .active_and_staged_items =
                            credits.active_and_staged_items,
                        .retained_terminals =
                            credits.retained_terminals,
                    });
            },
            [&](const savor::runtime::WorkerWorksetTerminalSummaryEvent& summary) {
                publisher.Publish(
                    MessageKind::WorksetSummary,
                    0,
                    savor::wrms::WorksetSummaryPayload{
                        .outbound_sequence =
                            summary.outbound_sequence.value(),
                        .workset_id = summary.workset_id.value(),
                        .item_count = summary.item_count,
                        .completed_count = summary.completed_count,
                        .unstarted_count = summary.unstarted_count,
                        .initially_suppressed_count =
                            summary.initially_suppressed_count,
                    });
            },
            [&](const savor::runtime::WorkerExecutionEvent& execution_event) {
                const auto& event = execution_event.event;
                const auto& snapshot = event.snapshot;
                const auto active_control =
                    MapExecutionOperation(snapshot.active_kind);
                const auto operation_id = event.terminal
                    ? event.terminal->operation_id.value()
                    : event.progress
                        ? event.progress->operation_id.value()
                    : snapshot.active_operation
                        ? snapshot.active_operation->value()
                        : 0;
                const auto error = event.terminal
                    ? event.terminal->error
                    : savor::runtime::ExecutionError{};
                const std::string warning_code =
                    event.health_warning
                    ? event.health_warning->code
                    : std::string{};
                const std::string warning_message =
                    event.health_warning
                    ? event.health_warning->message
                    : std::string{};
                publisher.Publish(
                    MessageKind::ExecutionState,
                    0,
                    savor::wrms::ExecutionStatePayload{
                        .session_id = execution_event.session_id.value(),
                        .workset_epoch = snapshot.workset_epoch.value(),
                        .operation_id = operation_id,
                        .activity =
                            MapExecutionActivity(snapshot.activity),
                        .has_active_control =
                            snapshot.active_operation.has_value() &&
                            active_control.has_value(),
                        .active_control = active_control.value_or(
                            savor::wrms::ExecutionControlKind::Pause),
                        .completed_count = event.terminal
                            ? event.terminal->completed_count
                            : event.progress
                                ? event.progress->completed_count
                                : 0,
                        .program_counter = snapshot.evidence.pc,
                        .rejection_code = MapRejectionCode(
                            MapExecutionError(error.code)),
                        .code = warning_code.empty()
                            ? ErrorCodeString(
                                  MapExecutionError(error.code))
                            : warning_code,
                        .message = warning_message.empty()
                            ? error.message
                            : warning_message,
                    });
            },
            [&](const savor::runtime::HostRuntimeEvent& host) {
                publisher.Publish(
                    MessageKind::HostEvent,
                    0,
                    savor::wrms::HostEventPayload{
                        .session_id = host.session_id.value(),
                        .workset_epoch = host.workset_epoch.value(),
                        .sequence = host.event_sequence.value(),
                        .name = host.name,
                        .event_data = host.encoded_payload,
                    });
            },
            [&](const savor::runtime::WorkerRuntimeDiagnosticEvent& diagnostic) {
                publisher.Publish(
                    MessageKind::RuntimeDiagnostic,
                    0,
                    savor::wrms::RuntimeDiagnosticPayload{
                        .rejection_code =
                            MapRejectionCode(diagnostic.code),
                        .invocation_id = diagnostic.invocation_id
                            ? diagnostic.invocation_id->value()
                            : 0,
                        .message = diagnostic.message,
                    });
            }},
        event);
}

void PublishMalformedCommand(
    OutboundPublisher& publisher,
    MessageKind kind,
    std::uint64_t request_id,
    const std::string& message) {
    if (kind == MessageKind::OpenSession) {
        publisher.Publish(
            MessageKind::OpenSessionResult,
            request_id,
            savor::wrms::OpenSessionResultPayload{
                .success = false,
                .rejection_code = savor::wrms::RejectionCode::InvalidArgument,
                .error_code = "MalformedPayload",
                .message = message,
            });
    } else if (kind == MessageKind::CaptureScreenshot) {
        publisher.Publish(
            MessageKind::ScreenshotResult,
            request_id,
            savor::wrms::ScreenshotResultPayload{
                .status = savor::wrms::ScreenshotStatus::Failed,
                .rejection_code = savor::wrms::RejectionCode::InvalidArgument,
                .error_code = "MalformedPayload",
                .message = message,
            });
    } else if (kind == MessageKind::Shutdown) {
        publisher.Publish(
            MessageKind::ShutdownResult,
            request_id,
            savor::wrms::ShutdownResultPayload{
                .status = savor::wrms::ShutdownStatus::CleanupFailed,
                .rejection_code = savor::wrms::RejectionCode::InvalidArgument,
                .error_code = "MalformedPayload",
                .message = message,
            });
    } else if (kind == MessageKind::ControlExecution) {
        publisher.Publish(
            MessageKind::ExecutionResult,
            request_id,
            savor::wrms::ExecutionResultPayload{
                .status = savor::wrms::CommandStatus::Rejected,
                .activity =
                    savor::wrms::ExecutionActivityCode::Failed,
                .rejection_code =
                    savor::wrms::RejectionCode::InvalidArgument,
                .error_code = "MalformedPayload",
                .message = message,
            });
    } else {
        publisher.Publish(
            MessageKind::CommandResult,
            request_id,
            savor::wrms::CommandResultPayload{
                .command_kind = kind,
                .status = savor::wrms::CommandStatus::Rejected,
                .rejection_code = savor::wrms::RejectionCode::InvalidArgument,
                .error_code = "MalformedPayload",
                .message = message,
            });
    }
}

bool SubmitFrame(
    savor::runtime::WorkerRuntime& runtime,
    OutboundPublisher& publisher,
    RequestMetadata& metadata,
    const savor::wrms::FrameView& frame,
    std::future<WorkerCommandResult>* shutdown_future) {
    if (frame.header.request_id == 0) {
        PublishMalformedCommand(
            publisher,
            frame.header.kind,
            frame.header.request_id,
            "parent commands require a nonzero request ID");
        return true;
    }

    using namespace savor::runtime;
    switch (frame.header.kind) {
    case MessageKind::OpenSession: {
        savor::wrms::OpenSessionPayload payload;
        if (!savor::wrms::DecodePayload(frame.payload, payload)) {
            PublishMalformedCommand(
                publisher,
                frame.header.kind,
                frame.header.request_id,
                "invalid OpenSession payload");
            return true;
        }
        std::error_code path_error;
        const auto absolute_user_directory = std::filesystem::absolute(
            std::filesystem::path(payload.user_directory),
            path_error);
        if (path_error || absolute_user_directory.parent_path().empty()) {
            PublishMalformedCommand(
                publisher,
                frame.header.kind,
                frame.header.request_id,
                "OpenSession requires a resolvable worker user directory");
            return true;
        }
        SessionOpenOptions options;
        // The installed worker/Dolphin image is shared and read-only.  All
        // mutable backend files belong beside this worker's private User tree
        // so concurrent workers cannot race on movie restore/checkpoint files.
        options.backend.runtime_root =
            absolute_user_directory.parent_path() / "Runtime";
        options.backend.user_directory = absolute_user_directory;
        options.backend.dolphin_base_directory = payload.runtime_root;
        options.backend.iso_path = payload.iso_path;
        options.backend.force_resync_from_base = true;
        options.worker_mode = MapWorkerMode(payload.worker_mode);
        options.backend.visual =
            options.worker_mode != savor::runtime::WorkerMode::Headless;
        options.backend.render_window_handle =
            static_cast<std::uintptr_t>(payload.render_window_handle);
        options.runtime_artifact_root = payload.runtime_artifact_root;
        (void)runtime.Submit(
            WireRequestId{frame.header.request_id},
            OpenSessionCommand{std::move(options)});
        return true;
    }
    case MessageKind::SubmitWorkset: {
        savor::wrms::SubmitWorksetPayload payload;
        if (!savor::wrms::DecodePayload(frame.payload, payload)) {
            PublishMalformedCommand(
                publisher,
                frame.header.kind,
                frame.header.request_id,
                "invalid SubmitWorkset payload");
            return true;
        }
        WorkerWorksetDefinition definition;
        const std::size_t actual_encoded_size =
            payload.encoded_workset.size();
        const auto decoded = DecodeWorkerWorksetV4(
            payload.encoded_workset,
            definition);
        if (!decoded) {
            PublishMalformedCommand(
                publisher,
                frame.header.kind,
                frame.header.request_id,
                decoded.message.empty()
                    ? "invalid encoded WorkerWorkset"
                    : decoded.message);
            return true;
        }
        // The transport is authoritative for residency accounting. Never
        // accept a sender-supplied byte count.
        definition.encoded_size_bytes = actual_encoded_size;
        savor::runtime::InitialWorksetCancellationSidecarV1 sidecar{
            .workset_id = definition.workset_id,
        };
        sidecar.item_ids.reserve(
            payload.initially_cancelled_item_ids.size());
        for (const auto item_id : payload.initially_cancelled_item_ids) {
            sidecar.item_ids.emplace_back(item_id);
        }
        if (payload.workset_sha256.size() != 64
            || payload.workset_sha256 != ::hash::sha256(
                payload.encoded_workset.data(),
                payload.encoded_workset.size())
            || payload.cancellation_sidecar_version
                != savor::runtime::
                    kInitialWorksetCancellationSidecarVersionV1
            || payload.cancellation_sidecar_sha256
                != savor::runtime::
                    ComputeInitialWorksetCancellationSidecarSha256(sidecar)) {
            PublishMalformedCommand(
                publisher,
                frame.header.kind,
                frame.header.request_id,
                "SubmitWorkset identity or cancellation sidecar digest is invalid");
            return true;
        }
        (void)runtime.Submit(
            WireRequestId{frame.header.request_id},
            SubmitWorksetCommand{
                std::move(definition),
                std::move(sidecar),
                std::move(payload.workset_sha256)});
        return true;
    }
    case MessageKind::CancelWorksetItem: {
        savor::wrms::CancelWorksetItemPayload payload;
        if (!savor::wrms::DecodePayload(frame.payload, payload)) {
            PublishMalformedCommand(
                publisher,
                frame.header.kind,
                frame.header.request_id,
                "invalid CancelWorksetItem payload");
            return true;
        }
        (void)runtime.Submit(
            WireRequestId{frame.header.request_id},
            CancelWorksetItemCommand{
                WorkerWorksetId{payload.workset_id},
                WorkerWorksetItemId{payload.item_id}});
        return true;
    }
    case MessageKind::CancelWorkset: {
        savor::wrms::CancelWorksetPayload payload;
        if (!savor::wrms::DecodePayload(frame.payload, payload)) {
            PublishMalformedCommand(
                publisher,
                frame.header.kind,
                frame.header.request_id,
                "invalid CancelWorkset payload");
            return true;
        }
        (void)runtime.Submit(
            WireRequestId{frame.header.request_id},
            CancelWorksetCommand{
                WorkerWorksetId{payload.workset_id}});
        return true;
    }
    case MessageKind::AcknowledgeTerminal: {
        savor::wrms::AcknowledgeTerminalPayload payload;
        if (!savor::wrms::DecodePayload(frame.payload, payload)) {
            PublishMalformedCommand(
                publisher,
                frame.header.kind,
                frame.header.request_id,
                "invalid AcknowledgeTerminal payload");
            return true;
        }
        (void)runtime.Submit(
            WireRequestId{frame.header.request_id},
            AcknowledgeTerminalCommand{
                WorkerItemTerminalCorrelation{
                    WorkerWorksetId{payload.workset_id},
                    WorkerWorksetItemId{payload.item_id},
                    payload.item_ordinal,
                    InvocationId{payload.invocation_id},
                    AttemptId{payload.attempt_id},
                    WorkerTerminalId{payload.terminal_id},
                    WorkerTerminalOrder{
                        payload.terminal_order}}});
        return true;
    }
    case MessageKind::LivenessProbe: {
        if (!frame.payload.empty()) {
            PublishMalformedCommand(
                publisher,
                frame.header.kind,
                frame.header.request_id,
                "LivenessProbe payload must be empty");
            return true;
        }
        const auto snapshot = runtime.snapshot();
        savor::wrms::WorksetResidenceSnapshotV1 residence{};
        const auto resident = snapshot.active_workset.has_value()
            ? snapshot.active_workset
            : snapshot.staged_workset;
        if (resident.has_value()) {
            residence.has_resident_workset = true;
            residence.workset_id = resident->value();
            residence.state = MapWorksetState(
                snapshot.resident_workset_state.value_or(
                    savor::runtime::WorkerWorksetState::Validating));
            residence.cancellation_sidecar_sha256 =
                snapshot.resident_cancellation_sidecar_sha256;
        }
        std::vector<std::uint8_t> encoded_residence;
        if (!savor::wrms::EncodePayload(
                residence,
                encoded_residence)) {
            PublishMalformedCommand(
                publisher,
                frame.header.kind,
                frame.header.request_id,
                "failed encoding LivenessProbe residence snapshot");
            return true;
        }
        publisher.Publish(
            MessageKind::CommandResult,
            frame.header.request_id,
            savor::wrms::CommandResultPayload{
                .command_kind = MessageKind::LivenessProbe,
                .status = savor::wrms::CommandStatus::Succeeded,
                .result = std::move(encoded_residence),
            });
        return true;
    }
    case MessageKind::CancelInvocation: {
        savor::wrms::CancelInvocationPayload payload;
        if (!savor::wrms::DecodePayload(frame.payload, payload)) {
            PublishMalformedCommand(
                publisher,
                frame.header.kind,
                frame.header.request_id,
                "invalid CancelInvocation payload");
            return true;
        }
        (void)runtime.Submit(
            WireRequestId{frame.header.request_id},
            CancelInvocationCommand{InvocationId{payload.invocation_id}});
        return true;
    }
    case MessageKind::CaptureScreenshot: {
        savor::wrms::CaptureScreenshotPayload payload;
        if (!savor::wrms::DecodePayload(frame.payload, payload)) {
            PublishMalformedCommand(
                publisher,
                frame.header.kind,
                frame.header.request_id,
                "invalid CaptureScreenshot payload");
            return true;
        }
        metadata.RememberScreenshot(
            frame.header.request_id,
            payload.output_path);
        (void)runtime.Submit(
            WireRequestId{frame.header.request_id},
            CaptureScreenshotCommand{
                .workset_id = WorkerWorksetId{payload.workset_id},
                .item_id = WorkerWorksetItemId{payload.item_id},
                .output_path = std::move(payload.output_path),
                .timeout = std::chrono::milliseconds{
                    payload.timeout_ms ? payload.timeout_ms : 3000},
            });
        return true;
    }
    case MessageKind::ControlExecution: {
        savor::wrms::ControlExecutionPayload payload;
        if (!savor::wrms::DecodePayload(frame.payload, payload)) {
            PublishMalformedCommand(
                publisher,
                frame.header.kind,
                frame.header.request_id,
                "invalid ControlExecution payload");
            return true;
        }
        WorkerExecutionControlKind control =
            WorkerExecutionControlKind::Pause;
        switch (payload.control) {
        case savor::wrms::ExecutionControlKind::Pause:
            control = WorkerExecutionControlKind::Pause;
            break;
        case savor::wrms::ExecutionControlKind::Resume:
            control = WorkerExecutionControlKind::Resume;
            break;
        case savor::wrms::ExecutionControlKind::ReservedGuestInstruction:
            // DecodePayload rejects the reserved discriminator before this
            // switch, so it can never reach the runtime.
            break;
        case savor::wrms::ExecutionControlKind::StepFrame:
            control = WorkerExecutionControlKind::StepFrame;
            break;
        }
        (void)runtime.Submit(
            WireRequestId{frame.header.request_id},
            ControlExecutionCommand{
                .control = control,
                .workset_id = WorkerWorksetId{payload.workset_id},
                .item_id = WorkerWorksetItemId{payload.item_id},
                .count = payload.count,
                .timeout = std::chrono::milliseconds{
                    payload.timeout_ms},
            });
        return true;
    }
    case MessageKind::Shutdown: {
        savor::wrms::ShutdownPayload payload;
        if (!savor::wrms::DecodePayload(frame.payload, payload)) {
            PublishMalformedCommand(
                publisher,
                frame.header.kind,
                frame.header.request_id,
                "invalid Shutdown payload");
            return true;
        }
        *shutdown_future = runtime.Submit(
            WireRequestId{frame.header.request_id},
            ShutdownCommand{});
        return false;
    }
    default:
        return false;
    }
}

} // namespace

int main(int argc, char** argv) {
    std::uint64_t worker_id = 0;
    std::filesystem::path log_directory;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--id")
            worker_id = ParseU64(NextArg(index, argc, argv));
        else if (argument == "--log-dir")
            log_directory = NextArg(index, argc, argv);
    }

    set_this_thread_name_utf8(
        ("WorkerMainV1-" + std::to_string(worker_id)).c_str());

    if (log_directory.empty()) {
        std::error_code temp_error;
        log_directory = std::filesystem::temp_directory_path(temp_error);
        if (temp_error)
            log_directory = ExecutableDirectory();
        log_directory /= "SavorWorker";
    }
    std::error_code directory_error;
    std::filesystem::create_directories(log_directory, directory_error);
    const auto log_path =
        log_directory /
        ("worker-" + std::to_string(worker_id) + ".log");
    auto& logger = savor::logger::Logger::get();
    logger.open_file(log_path.string().c_str(), false);
    logger.set_levels(
        savor::logger::Level::Off,
        savor::logger::Level::Debug);

    HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
    HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!input || input == INVALID_HANDLE_VALUE ||
        !output || output == INVALID_HANDLE_VALUE) {
        return static_cast<int>(WorkerExitCode::InvalidHandles);
    }

    OutboundPublisher publisher(output);
    RequestMetadata request_metadata;
    std::unique_ptr<savor::runtime::WorkerRuntime> runtime;
    runtime = savor::runtime::MakeProductionWorkerRuntime(
        savor::runtime::SessionId{},
        [&](const WorkerEvent& event) {
            PublishWorkerEvent(
                publisher,
                request_metadata,
                event);
            if (!publisher.healthy()) {
                throw std::runtime_error(
                    "WRMS outbound publisher rejected the worker event");
            }
        });
    savor::hoststubs::SetHostEventSink(
        [&](const savor::hoststubs::HostEvent& event) {
            std::vector<std::uint8_t> encoded(
                event.args_json.begin(),
                event.args_json.end());
            (void)runtime->EnqueueHostEvent(
                event.name,
                std::move(encoded));
        });

    std::vector<std::uint8_t> encoded_runtime_contract;
    const auto encoded_contract =
        savor::runtime::EncodeWorkerRuntimeContractV1(
            runtime->runtime_contract(),
            encoded_runtime_contract);
    if (!encoded_contract) {
        publisher.StopAndDrain();
        return static_cast<int>(WorkerExitCode::PublisherFailure);
    }
    publisher.Publish(
        MessageKind::ProcessHello,
        0,
        savor::wrms::ProcessHelloPayload{
            .worker_id = worker_id,
            .process_id = GetCurrentProcessId(),
            .encoded_runtime_contract =
                std::move(encoded_runtime_contract),
        });

    std::vector<std::uint8_t> buffered;
    buffered.reserve(64 * 1024);
    std::array<std::uint8_t, 64 * 1024> chunk{};
    std::future<WorkerCommandResult> shutdown_future;
    bool shutdown_requested = false;
    bool protocol_ok = true;

    while (!shutdown_requested && publisher.healthy()) {
        DWORD read = 0;
        if (!ReadFile(
                input,
                chunk.data(),
                static_cast<DWORD>(chunk.size()),
                &read,
                nullptr) ||
            read == 0) {
            protocol_ok = buffered.empty();
            break;
        }
        buffered.insert(
            buffered.end(),
            chunk.begin(),
            chunk.begin() + read);

        for (;;) {
            const auto decoded =
                savor::wrms::DecodeFrame(buffered, false);
            if (decoded.status ==
                savor::wrms::FrameDecodeStatus::NeedMoreData) {
                break;
            }
            if (decoded.status ==
                savor::wrms::FrameDecodeStatus::Error) {
                protocol_ok = false;
                shutdown_requested = true;
                break;
            }
            if (savor::wrms::DirectionOf(decoded.frame.header.kind) !=
                savor::wrms::MessageDirection::ParentToWorker) {
                protocol_ok = false;
                shutdown_requested = true;
                break;
            }

            const bool keep_reading = SubmitFrame(
                *runtime,
                publisher,
                request_metadata,
                decoded.frame,
                &shutdown_future);
            buffered.erase(
                buffered.begin(),
                buffered.begin() + decoded.consumed_size);
            if (!keep_reading) {
                shutdown_requested = true;
                break;
            }
        }
    }

    if (!buffered.empty() && !shutdown_requested) {
        const auto final_decode =
            savor::wrms::DecodeFrame(buffered, true);
        if (final_decode.status ==
            savor::wrms::FrameDecodeStatus::Error) {
            protocol_ok = false;
        }
    }

    if (!shutdown_future.valid()) {
        shutdown_future = runtime->Submit(
            savor::runtime::WireRequestId{},
            savor::runtime::ShutdownCommand{});
    }
    (void)shutdown_future.get();
    runtime->WaitStopped();
    savor::hoststubs::ClearHostEventSink();
    publisher.StopAndDrain();
    runtime.reset();

    if (!publisher.healthy())
        return static_cast<int>(WorkerExitCode::PublisherFailure);
    return protocol_ok
        ? static_cast<int>(WorkerExitCode::Success)
        : static_cast<int>(WorkerExitCode::ProtocolFailure);
}

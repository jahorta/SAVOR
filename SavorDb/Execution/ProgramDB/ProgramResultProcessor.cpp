#include "ProgramResultProcessor.h"

#include <algorithm>
#include <exception>
#include <sstream>
#include <utility>

#include "../../../SavorCore/Runner/IPC/DurableWorkerTerminalEnvelope.h"

namespace savor::db::execution::programdb {
namespace {

std::atomic<std::uint64_t> g_processor_sequence{1};

std::string MakeToken(std::string_view prefix, const void* owner) {
    std::ostringstream token;
    token << prefix << '-'
          << std::chrono::steady_clock::now().time_since_epoch().count()
          << '-' << reinterpret_cast<std::uintptr_t>(owner)
          << '-' << g_processor_sequence.fetch_add(1);
    return token.str();
}

const char* TerminalStatusName(wrms::InvocationTerminalStatus status) {
    switch (status) {
    case wrms::InvocationTerminalStatus::Succeeded:
        return "SUCCEEDED";
    case wrms::InvocationTerminalStatus::Failed:
        return "FAILED";
    case wrms::InvocationTerminalStatus::Cancelled:
        return "CANCELLED";
    case wrms::InvocationTerminalStatus::InfrastructureFailure:
        return "INFRASTRUCTURE_FAILURE";
    case wrms::InvocationTerminalStatus::CleanupFailure:
        return "CLEANUP_FAILURE";
    case wrms::InvocationTerminalStatus::TimedOut:
        return "TIMED_OUT";
    default:
        return "UNKNOWN";
    }
}

bool IsSchedulerOwnedState(std::string_view state) {
    return state == "PENDING_WORKSET" || state == "QUEUED"
        || state == "CLAIMED" || state == "RUNNING"
        || state == "EXECUTION_FINISHED";
}

bool IsExecutionFailureWithoutProgramOutcome(
    const ClaimedExecutionFinishedJob& claimed) {
    return claimed.worker_terminal_status == "FAILED"
        || claimed.worker_terminal_status == "INFRASTRUCTURE_FAILURE"
        || claimed.worker_terminal_status == "CLEANUP_FAILURE"
        || claimed.worker_terminal_status == "TIMED_OUT";
}

class ProcessingLeaseHeartbeat {
public:
    ProcessingLeaseHeartbeat(
        IExecutionDb* execution_db,
        std::int64_t job_id,
        std::string processor_token,
        std::chrono::milliseconds lease_duration)
        : execution_db_(execution_db)
        , job_id_(job_id)
        , processor_token_(std::move(processor_token))
        , lease_duration_(lease_duration)
        , interval_(std::max(
              std::chrono::milliseconds(250),
              lease_duration / 3)) {
    }

    ~ProcessingLeaseHeartbeat() {
        Stop();
    }

    void Start() {
        thread_ = std::thread([this]() {
            std::unique_lock lock(mutex_);
            while (!stop_) {
                if (cv_.wait_for(lock, interval_, [this]() { return stop_; })) {
                    break;
                }
                lock.unlock();
                ResultProcessingReceipt receipt{};
                std::string error;
                const bool ok = execution_db_->RenewResultProcessingLease(
                    {
                        .job_id = job_id_,
                        .processor_token = processor_token_,
                        .lease_duration_ms = lease_duration_.count(),
                    },
                    &receipt,
                    &error);
                if (!ok
                    || (receipt.disposition
                            != ExecutionDbOperationDisposition::Applied
                        && receipt.disposition
                            != ExecutionDbOperationDisposition::AlreadyApplied)) {
                    authoritative_.store(false);
                    {
                        std::lock_guard error_lock(error_mutex_);
                        error_ = error.empty()
                            ? "result-processing lease authority was lost"
                            : std::move(error);
                    }
                    break;
                }
                lock.lock();
            }
        });
    }

    void Stop() {
        {
            std::lock_guard lock(mutex_);
            stop_ = true;
        }
        cv_.notify_all();
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    [[nodiscard]] bool Authoritative() const noexcept {
        return authoritative_.load();
    }

    [[nodiscard]] std::string Error() const {
        std::lock_guard lock(error_mutex_);
        return error_;
    }

private:
    IExecutionDb* execution_db_ = nullptr;
    std::int64_t job_id_ = 0;
    std::string processor_token_;
    std::chrono::milliseconds lease_duration_{};
    std::chrono::milliseconds interval_{};
    std::atomic<bool> authoritative_{true};
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    bool stop_ = false;
    std::thread thread_;
    mutable std::mutex error_mutex_;
    std::string error_;
};

} // namespace

ProgramResultProcessor::ProgramResultProcessor(
    IExecutionDb* execution_db,
    const ProgramKindRegistry* program_kind_registry,
    WorkerResultBlobStore* blob_store,
    ProgramResultProcessorConfig config,
    FinalizationCallback finalization_callback,
    EventLineCallback event_line_callback)
    : execution_db_(execution_db)
    , program_kind_registry_(program_kind_registry)
    , blob_store_(blob_store)
    , config_(std::move(config))
    , finalization_callback_(std::move(finalization_callback))
    , event_line_callback_(std::move(event_line_callback))
    , processor_token_(MakeToken("program-result-processor", this))
    , max_total_processing_attempts_(
          std::max(1, config_.max_total_processing_attempts)) {
}

ProgramResultProcessor::~ProgramResultProcessor() {
    Stop();
}

bool ProgramResultProcessor::Start(std::string* error_out) {
    if (running_.load()) {
        return true;
    }
    if (!config_.enabled) {
        if (error_out != nullptr) {
            error_out->clear();
        }
        return true;
    }
    if (execution_db_ == nullptr || program_kind_registry_ == nullptr
        || blob_store_ == nullptr) {
        if (error_out != nullptr) {
            *error_out =
                "program result processor requires execution DB, registry, "
                "and private blob store";
        }
        return false;
    }
    if (config_.lease_duration <= std::chrono::milliseconds::zero()) {
        if (error_out != nullptr) {
            *error_out = "program result processor lease must be positive";
        }
        return false;
    }

    stop_.store(false);
    next_recovery_at_ = std::chrono::steady_clock::now();
    running_.store(true);
    thread_ = std::thread([this]() { Loop(); });
    if (error_out != nullptr) {
        error_out->clear();
    }
    return true;
}

void ProgramResultProcessor::Stop() {
    stop_.store(true);
    wait_cv_.notify_all();
    if (thread_.joinable()) {
        thread_.join();
    }
    running_.store(false);
}

void ProgramResultProcessor::Wake() {
    {
        std::lock_guard lock(wait_mutex_);
        ++wake_generation_;
    }
    wait_cv_.notify_all();
}

bool ProgramResultProcessor::IsRunning() const noexcept {
    return running_.load();
}

void ProgramResultProcessor::SetMaxTotalProcessingAttempts(
    int attempts) {
    max_total_processing_attempts_.store(std::max(1, attempts));
    Wake();
}

int ProgramResultProcessor::MaxTotalProcessingAttempts() const noexcept {
    return max_total_processing_attempts_.load();
}

ProgramResultProcessorTelemetry
ProgramResultProcessor::SnapshotTelemetry() const {
    ProgramResultProcessorTelemetry telemetry{};
    telemetry.max_total_processing_attempts =
        max_total_processing_attempts_.load();
    telemetry.claims = claims_.load();
    telemetry.finalized = finalized_.load();
    telemetry.execution_retries = execution_retries_.load();
    telemetry.processing_failures = processing_failures_.load();
    telemetry.descriptor_unavailable = descriptor_unavailable_.load();
    telemetry.lease_authority_lost = lease_authority_lost_.load();
    telemetry.recovered_leases = recovered_leases_.load();
    {
        std::lock_guard lock(error_mutex_);
        telemetry.last_error = last_error_;
    }
    return telemetry;
}

void ProgramResultProcessor::Loop() {
    while (!stop_.load()) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= next_recovery_at_) {
            RecoverExpiredLeases();
            next_recovery_at_ = now
                + std::max(
                    config_.recovery_interval,
                    std::chrono::milliseconds(1));
        }

        if (ProcessOne()) {
            continue;
        }

        std::unique_lock lock(wait_mutex_);
        const auto observed_generation = wake_generation_;
        wait_cv_.wait_for(
            lock,
            std::max(config_.poll_interval, std::chrono::milliseconds(1)),
            [this, observed_generation]() {
                return stop_.load()
                    || wake_generation_ != observed_generation;
            });
    }
}

bool ProgramResultProcessor::ProcessOne() {
    std::string error;
    const auto claimed = execution_db_->ClaimNextExecutionFinishedJob(
        {
            .processor_token = processor_token_,
            .lease_duration_ms = config_.lease_duration.count(),
            .max_total_processing_attempts =
                max_total_processing_attempts_.load(),
        },
        &error);
    if (!claimed.has_value()) {
        if (!error.empty()) {
            RecordError(std::move(error));
        }
        return false;
    }
    ++claims_;

    WorkerResultBlobReference blob_reference{
        .relative_path = claimed->result_blob.relative_path,
        .sha256 = claimed->result_blob.sha256,
        .size_bytes =
            static_cast<std::int64_t>(claimed->result_blob.size_bytes),
        .format = claimed->result_blob.format,
    };
    std::vector<std::uint8_t> envelope_bytes;
    if (blob_reference.format != kWorkerTerminalEnvelopeFormatV1
        || blob_reference.sha256
            != claimed->worker_terminal_fingerprint
        || !blob_store_->Read(
            blob_reference,
            &envelope_bytes,
            &error)) {
        return Park(
            *claimed,
            "WORKER_RESULT_BLOB_INVALID",
            error.empty() ? "worker terminal blob format is unsupported"
                          : std::move(error));
    }

    runtime::DurableWorkerTerminalEnvelope durable_terminal{};
    if (!runtime::DecodeDurableWorkerTerminalEnvelope(
            envelope_bytes,
            &durable_terminal,
            &error)
        || durable_terminal.terminal.workset_id
            != static_cast<std::uint64_t>(
                claimed->dispatch_attempt_id)
        || durable_terminal.terminal.item_ordinal
            != claimed->runtime_item_ordinal
        || durable_terminal.terminal.attempt_id
            != claimed->reserved_attempt_id
        || durable_terminal.terminal.terminal_id == 0
        || std::to_string(durable_terminal.terminal.terminal_id)
            != claimed->worker_terminal_id
        || TerminalStatusName(durable_terminal.terminal.status)
            != claimed->worker_terminal_status
        || durable_terminal.terminal.unstarted
            != claimed->worker_terminal_unstarted
        || (durable_terminal.terminal.error_code.empty()
                ? std::optional<std::string>{}
                : std::optional<std::string>{
                    durable_terminal.terminal.error_code})
            != claimed->worker_terminal_error_code
        || (durable_terminal.terminal.message.empty()
                ? std::optional<std::string>{}
                : std::optional<std::string>{
                    durable_terminal.terminal.message})
            != claimed->worker_terminal_error_text) {
        return Park(
            *claimed,
            "WORKER_RESULT_ENVELOPE_MISMATCH",
            error.empty()
                ? "worker terminal envelope does not match durable metadata"
                : std::move(error));
    }

    const auto* descriptor =
        program_kind_registry_->Find(claimed->job.program_kind);
    if (descriptor == nullptr || descriptor->result_handler == nullptr) {
        ++descriptor_unavailable_;
        return Park(
            *claimed,
            "DESCRIPTOR_UNAVAILABLE",
            "program result handler is not registered for program kind "
                + std::to_string(claimed->job.program_kind));
    }

    ProgramResultDecision decision{};
    ProcessingLeaseHeartbeat heartbeat(
        execution_db_,
        claimed->job.job_id,
        claimed->processor_token,
        config_.lease_duration);
    heartbeat.Start();
    try {
        decision = descriptor->result_handler->Process(
            ProgramResultProcessingContext{
                .job_id = claimed->job.job_id,
                .job_set_id = claimed->job.job_set_id,
                .program_kind = claimed->job.program_kind,
                .program_version = claimed->job.program_version,
                .program_ref_kind = claimed->job.program_ref_kind,
                .program_ref_id = claimed->job.program_ref_id,
                .fingerprint = claimed->job.fingerprint,
                .input_ini = claimed->job.input_ini,
                .terminal =
                    WorkerTerminalObservation{
                        .job_id = claimed->job.job_id,
                        .workset_id = claimed->workset_id,
                        .dispatch_attempt_id =
                            claimed->dispatch_attempt_id,
                        .reserved_attempt_id =
                            claimed->reserved_attempt_id,
                        .format = blob_reference.format,
                        .sha256 = blob_reference.sha256,
                        .envelope = envelope_bytes,
                    },
            });
    } catch (const std::exception& exception) {
        heartbeat.Stop();
        if (!heartbeat.Authoritative()) {
            ++lease_authority_lost_;
            RecordError(heartbeat.Error());
            return true;
        }
        return Park(
            *claimed,
            "RESULT_PROCESSOR_EXCEPTION",
            exception.what());
    } catch (...) {
        heartbeat.Stop();
        if (!heartbeat.Authoritative()) {
            ++lease_authority_lost_;
            RecordError(heartbeat.Error());
            return true;
        }
        return Park(
            *claimed,
            "RESULT_PROCESSOR_EXCEPTION",
            "program result handler threw an unknown exception");
    }
    heartbeat.Stop();
    if (!heartbeat.Authoritative()) {
        ++lease_authority_lost_;
        RecordError(heartbeat.Error());
        return true;
    }

    CommitResultFinalizationCommand command{};
    command.job_id = claimed->job.job_id;
    command.processor_token = claimed->processor_token;
    command.requested_by = "program_result_processor";
    command.error_code = decision.error_code;
    command.error_text = decision.error_text;
    command.event_lines = decision.event_lines;

    if (decision.disposition == ProgramResultDisposition::RetryExecution) {
        if (!IsExecutionFailureWithoutProgramOutcome(*claimed)) {
            return Park(
                *claimed,
                "INVALID_EXECUTION_RETRY_DECISION",
                "same-job retry requires an execution failure without a "
                "program-kind outcome");
        }
        command.disposition = ExecutionResultFinalizationDisposition::Retry;
    } else {
        if (decision.final_job_state.empty()
            || IsSchedulerOwnedState(decision.final_job_state)) {
            return Park(
                *claimed,
                "INVALID_FINAL_JOB_STATE",
                "program result handler returned an empty or scheduler-owned "
                "final job state");
        }
        command.disposition = ExecutionResultFinalizationDisposition::Final;
        command.final_state = decision.final_job_state;
    }

    command.outputs.reserve(decision.outputs.size());
    for (const auto& output : decision.outputs) {
        command.outputs.push_back(
            {
                .output_key = output.output_key,
                .data_kind = output.data_kind,
                .ref_kind = output.ref_kind,
                .ref_id = output.ref_id,
            });
    }
    command.cancellation_requests.reserve(decision.cancellations.size());
    for (const auto& cancellation : decision.cancellations) {
        command.cancellation_requests.push_back(
            {
                .job_id = cancellation.job_id,
                .request_key = cancellation.request_key,
                .reason_code = cancellation.reason_code,
                .reason_text = cancellation.reason_text.empty()
                    ? std::nullopt
                    : std::optional<std::string>(
                        cancellation.reason_text),
                .requested_by = "program_result_processor",
                .caused_by_job_id = claimed->job.job_id,
            });
    }

    ResultProcessingReceipt receipt{};
    if (!execution_db_->CommitResultFinalization(
            command,
            &receipt,
            &error)
        || (receipt.disposition != ExecutionDbOperationDisposition::Applied
            && receipt.disposition
                != ExecutionDbOperationDisposition::AlreadyApplied)) {
        RecordError(
            error.empty() ? "failed committing program result finalization"
                          : std::move(error));
        return true;
    }

    if (command.disposition
        == ExecutionResultFinalizationDisposition::Retry) {
        ++execution_retries_;
    } else {
        ++finalized_;
        if (finalization_callback_ && receipt.commit_sequence != 0
            && receipt.workflow_step_id > 0) {
            finalization_callback_(
                receipt.commit_sequence,
                receipt.workflow_step_id,
                claimed->job.job_id);
        }
    }
    if (event_line_callback_) {
        for (const auto& line : decision.event_lines) {
            event_line_callback_(line);
        }
    }
    return true;
}

void ProgramResultProcessor::RecoverExpiredLeases() {
    int recovered = 0;
    std::string error;
    if (!execution_db_->RecoverExpiredResultProcessingLeases(
            std::max(1, config_.recovery_batch_size),
            &recovered,
            &error)) {
        RecordError(
            error.empty()
                ? "failed recovering expired result-processing leases"
                : std::move(error));
        return;
    }
    if (recovered > 0) {
        recovered_leases_.fetch_add(
            static_cast<std::uint64_t>(recovered));
    }
}

bool ProgramResultProcessor::Park(
    const ClaimedExecutionFinishedJob& claimed,
    std::string error_code,
    std::string error_text) {
    ++processing_failures_;
    ResultProcessingReceipt receipt{};
    std::string db_error;
    if (!execution_db_->ParkResultProcessing(
            {
                .job_id = claimed.job.job_id,
                .processor_token = claimed.processor_token,
                .error_code = std::move(error_code),
                .error_text = error_text,
            },
            &receipt,
            &db_error)
        || (receipt.disposition
                != ExecutionDbOperationDisposition::Applied
            && receipt.disposition
                != ExecutionDbOperationDisposition::AlreadyApplied)) {
        if (receipt.disposition
                == ExecutionDbOperationDisposition::TokenMismatch
            || receipt.disposition
                == ExecutionDbOperationDisposition::LeaseExpired) {
            ++lease_authority_lost_;
        }
        RecordError(
            db_error.empty() ? "failed parking program result"
                             : std::move(db_error));
    } else {
        RecordError(std::move(error_text));
    }
    return true;
}

void ProgramResultProcessor::RecordError(std::string error) {
    if (error.empty()) {
        return;
    }
    std::lock_guard lock(error_mutex_);
    last_error_ = std::move(error);
}

} // namespace savor::db::execution::programdb

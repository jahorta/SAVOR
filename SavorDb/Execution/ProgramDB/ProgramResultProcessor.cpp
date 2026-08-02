#include "ProgramResultProcessor.h"

#include <algorithm>
#include <exception>
#include <limits>
#include <utility>

#include "../../../SavorCore/Runner/IPC/DurableWorkerTerminalEnvelope.h"

namespace savor::db::execution::programdb {
namespace {

const char* TerminalStatusName(wrms::InvocationTerminalStatus status) {
    switch (status) {
    case wrms::InvocationTerminalStatus::Succeeded: return "SUCCEEDED";
    case wrms::InvocationTerminalStatus::Failed: return "FAILED";
    case wrms::InvocationTerminalStatus::Cancelled: return "CANCELLED";
    case wrms::InvocationTerminalStatus::InfrastructureFailure:
        return "INFRASTRUCTURE_FAILURE";
    case wrms::InvocationTerminalStatus::CleanupFailure:
        return "CLEANUP_FAILURE";
    case wrms::InvocationTerminalStatus::TimedOut: return "TIMED_OUT";
    default: return "UNKNOWN";
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

void StoreMaximum(
    std::atomic<std::uint64_t>& target,
    std::uint64_t value) {
    auto current = target.load(std::memory_order_relaxed);
    while (value > current
        && !target.compare_exchange_weak(
            current, value, std::memory_order_relaxed)) {
    }
}

bool Applied(ExecutionDbOperationDisposition disposition) {
    return disposition == ExecutionDbOperationDisposition::Applied
        || disposition == ExecutionDbOperationDisposition::AlreadyApplied;
}

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
    , event_line_callback_(std::move(event_line_callback)) {
}

ProgramResultProcessor::~ProgramResultProcessor() {
    Stop();
}

void ProgramResultProcessor::SetCancellationCallbacks(
    CancellationPrecommitCallback precommit,
    CancellationCommittedCallback committed) {
    if (running_.load()) return;
    cancellation_precommit_callback_ = std::move(precommit);
    cancellation_committed_callback_ = std::move(committed);
}

bool ProgramResultProcessor::Start(std::string* error_out) {
    if (running_.load()) return true;
    if (!config_.enabled) {
        if (error_out != nullptr) error_out->clear();
        return true;
    }
    if (execution_db_ == nullptr || program_kind_registry_ == nullptr
        || blob_store_ == nullptr
        || config_.result_claim_batch_size == 0
        || config_.result_finalization_batch_size == 0
        || config_.result_finalization_delay
            < std::chrono::milliseconds::zero()) {
        if (error_out != nullptr) {
            *error_out = "program result processor configuration is invalid";
        }
        return false;
    }

    processing_canaries_.store(0);
    if (!ReconcileInterruptedProcessing(error_out)) return false;

    stop_.store(false);
    {
        std::lock_guard lock(finalization_mutex_);
        finalization_queue_.clear();
        finalization_active_ = false;
        finalization_force_flush_ = false;
        finalization_queue_high_water_ = 0;
    }
    availability_subscription_ =
        execution_db_->SubscribeExecutionWorkAvailability(
            [this](const ExecutionWorkAvailabilitySnapshot& snapshot) {
                {
                    std::lock_guard lock(wait_mutex_);
                    availability_ = snapshot;
                    ++wake_generation_;
                }
                ++signal_wakes_;
                wait_cv_.notify_all();
            });
    if (availability_subscription_ == 0) {
        if (error_out != nullptr) {
            *error_out = "execution-work availability subscription failed";
        }
        return false;
    }
    std::string availability_error;
    const auto availability =
        execution_db_->GetExecutionWorkAvailability(&availability_error);
    if (!availability.has_value()) {
        execution_db_->UnsubscribeExecutionWorkAvailability(
            availability_subscription_);
        availability_subscription_ = 0;
        if (error_out != nullptr) {
            *error_out = availability_error.empty()
                ? "execution-work availability snapshot failed"
                : std::move(availability_error);
        }
        return false;
    }
    {
        std::lock_guard lock(wait_mutex_);
        availability_ = *availability;
        ++wake_generation_;
    }

    running_.store(true);
    finalization_thread_ = std::thread([this]() { FinalizationLoop(); });
    thread_ = std::thread([this]() { Loop(); });
    if (error_out != nullptr) error_out->clear();
    return true;
}

void ProgramResultProcessor::Stop() {
    stop_.store(true);
    if (availability_subscription_ != 0 && execution_db_ != nullptr) {
        execution_db_->UnsubscribeExecutionWorkAvailability(
            availability_subscription_);
        availability_subscription_ = 0;
    }
    wait_cv_.notify_all();
    {
        std::lock_guard lock(finalization_mutex_);
        finalization_force_flush_ = true;
    }
    finalization_cv_.notify_all();
    if (thread_.joinable()) thread_.join();
    if (finalization_thread_.joinable()) finalization_thread_.join();
    running_.store(false);
}

bool ProgramResultProcessor::IsRunning() const noexcept {
    return running_.load();
}

ProgramResultProcessorTelemetry
ProgramResultProcessor::SnapshotTelemetry() const {
    ProgramResultProcessorTelemetry result{};
    result.claims = claims_.load();
    result.finalized = finalized_.load();
    result.execution_retries = execution_retries_.load();
    result.processing_failures = processing_failures_.load();
    result.descriptor_unavailable = descriptor_unavailable_.load();
    result.signal_wakes = signal_wakes_.load();
    result.drain_campaigns = drain_campaigns_.load();
    result.empty_claims = empty_claims_.load();
    result.processing_canaries = processing_canaries_.load();
    result.claim_batches = claim_batches_.load();
    result.claim_batch_items = claim_batch_items_.load();
    result.claim_batch_max_size = claim_batch_max_size_.load();
    if (result.claim_batches != 0) {
        result.claim_batch_average_size =
            static_cast<double>(result.claim_batch_items)
            / static_cast<double>(result.claim_batches);
    }
    result.finalization_batches = finalization_batches_.load();
    result.finalization_batch_items = finalization_batch_items_.load();
    result.finalization_batch_max_size = finalization_batch_max_size_.load();
    if (result.finalization_batches != 0) {
        result.finalization_batch_average_size =
            static_cast<double>(result.finalization_batch_items)
            / static_cast<double>(result.finalization_batches);
    }
    result.finalization_full_flushes = finalization_full_flushes_.load();
    result.finalization_deadline_flushes =
        finalization_deadline_flushes_.load();
    result.finalization_barrier_flushes =
        finalization_barrier_flushes_.load();
    result.finalization_rollbacks = finalization_rollbacks_.load();
    result.finalization_max_collection_age_ms =
        finalization_max_collection_age_ms_.load();
    result.startup_blob_resets = startup_blob_resets_.load();
    result.startup_program_recoveries = startup_program_recoveries_.load();
    result.startup_lost_blob_requeues = startup_lost_blob_requeues_.load();
    result.startup_recovery_canaries = startup_recovery_canaries_.load();
    {
        std::lock_guard lock(finalization_mutex_);
        result.finalization_queue_depth = finalization_queue_.size();
        result.finalization_queue_high_water = finalization_queue_high_water_;
    }
    {
        std::lock_guard lock(error_mutex_);
        result.last_error = last_error_;
    }
    return result;
}

void ProgramResultProcessor::Loop() {
    while (!stop_.load()) {
        bool available = false;
        std::uint64_t observed_generation = 0;
        {
            std::unique_lock lock(wait_mutex_);
            wait_cv_.wait(lock, [this]() {
                return stop_.load()
                    || availability_.has_execution_finished_results;
            });
            if (stop_.load()) break;
            available = availability_.has_execution_finished_results;
            observed_generation = availability_.generation;
        }
        if (!available) continue;
        ++drain_campaigns_;
        while (!stop_.load() && ProcessDrainCampaign()) {
            std::lock_guard lock(wait_mutex_);
            if (!availability_.has_execution_finished_results
                && availability_.generation == observed_generation) {
                break;
            }
            observed_generation = availability_.generation;
        }
    }
    (void)FlushFinalizationBarrier();
}

bool ProgramResultProcessor::ProcessDrainCampaign() {
    std::string error;
    auto claimed = execution_db_->ClaimExecutionFinishedJobsBatch(
        {.requested_job_count = config_.result_claim_batch_size},
        &error);
    ++claim_batches_;
    if (claimed.empty()) {
        ++empty_claims_;
        if (!error.empty()) RecordError(std::move(error));
        return false;
    }
    claims_.fetch_add(claimed.size());
    processing_canaries_.fetch_add(claimed.size());
    claim_batch_items_.fetch_add(claimed.size());
    StoreMaximum(claim_batch_max_size_, claimed.size());

    for (const auto& item : claimed) {
        auto pending = ProcessClaimed(item);
        if (!pending) continue;
        if (!EnqueueFinalization(pending)) return false;
        if (pending->force_flush && !WaitForFinalization(pending)) {
            return false;
        }
    }
    if (!FlushFinalizationBarrier()) return false;
    return claimed.size() == config_.result_claim_batch_size;
}

ProgramResultProcessor::PendingFinalizationPtr
ProgramResultProcessor::ProcessClaimed(
    const ClaimedExecutionFinishedJob& claimed) {
    WorkerResultBlobReference blob{
        .relative_path = claimed.result_blob.relative_path,
        .sha256 = claimed.result_blob.sha256,
        .size_bytes = static_cast<std::int64_t>(claimed.result_blob.size_bytes),
        .format = claimed.result_blob.format,
    };
    std::vector<std::uint8_t> bytes;
    std::string error;
    if (blob.format != kWorkerTerminalEnvelopeFormatV1
        || blob.sha256 != claimed.worker_terminal_fingerprint
        || !blob_store_->Read(blob, &bytes, &error)) {
        (void)RecordFailure(
            claimed.job.job_id,
            "WORKER_RESULT_BLOB_INVALID",
            error.empty() ? "worker result blob is invalid" : error);
        return {};
    }

    runtime::DurableWorkerTerminalEnvelope durable{};
    if (!runtime::DecodeDurableWorkerTerminalEnvelope(bytes, &durable, &error)
        || durable.terminal.workset_id
            != static_cast<std::uint64_t>(claimed.dispatch_attempt_id)
        || durable.terminal.item_ordinal != claimed.runtime_item_ordinal
        || durable.terminal.attempt_id != claimed.reserved_attempt_id
        || durable.terminal.terminal_id == 0
        || std::to_string(durable.terminal.terminal_id)
            != claimed.worker_terminal_id
        || TerminalStatusName(durable.terminal.status)
            != claimed.worker_terminal_status
        || durable.terminal.unstarted != claimed.worker_terminal_unstarted) {
        (void)RecordFailure(
            claimed.job.job_id,
            "WORKER_RESULT_ENVELOPE_MISMATCH",
            error.empty()
                ? "worker terminal envelope does not match durable metadata"
                : error);
        return {};
    }

    const auto* descriptor =
        program_kind_registry_->Find(claimed.job.program_kind);
    if (descriptor == nullptr || descriptor->result_handler == nullptr) {
        ++descriptor_unavailable_;
        (void)RecordFailure(
            claimed.job.job_id,
            "DESCRIPTOR_UNAVAILABLE",
            "program result handler is unavailable");
        return {};
    }

    ProgramResultDecision decision{};
    try {
        decision = descriptor->result_handler->Process(
            {
                .job_id = claimed.job.job_id,
                .job_set_id = claimed.job.job_set_id,
                .program_kind = claimed.job.program_kind,
                .program_version = claimed.job.program_version,
                .program_ref_kind = claimed.job.program_ref_kind,
                .program_ref_id = claimed.job.program_ref_id,
                .fingerprint = claimed.job.fingerprint,
                .input_ini = claimed.job.input_ini,
                .terminal = {
                    .job_id = claimed.job.job_id,
                    .workset_id = claimed.workset_id,
                    .dispatch_attempt_id = claimed.dispatch_attempt_id,
                    .reserved_attempt_id = claimed.reserved_attempt_id,
                    .format = blob.format,
                    .sha256 = blob.sha256,
                    .envelope = std::move(bytes),
                },
            });
    } catch (const std::exception& exception) {
        (void)RecordFailure(
            claimed.job.job_id,
            "RESULT_PROCESSOR_EXCEPTION",
            exception.what());
        return {};
    } catch (...) {
        (void)RecordFailure(
            claimed.job.job_id,
            "RESULT_PROCESSOR_EXCEPTION",
            "program result handler threw an unknown exception");
        return {};
    }

    if (decision.disposition == ProgramResultDisposition::RetryExecution) {
        if (!IsExecutionFailureWithoutProgramOutcome(claimed)) {
            (void)RecordFailure(
                claimed.job.job_id,
                "INVALID_EXECUTION_RETRY_DECISION",
                "same-job retry requires an execution failure without a program outcome");
            return {};
        }
    } else if (decision.final_job_state.empty()
        || IsSchedulerOwnedState(decision.final_job_state)) {
        (void)RecordFailure(
            claimed.job.job_id,
            "INVALID_FINAL_JOB_STATE",
            "program result handler returned an invalid final state");
        return {};
    }

    auto pending = std::make_shared<PendingFinalization>();
    pending->command =
        BuildFinalizationCommand(claimed.job, &claimed, decision);
    pending->event_lines = decision.event_lines;
    pending->enqueued_at = Clock::now();
    pending->force_flush = !decision.cancellations.empty();
    if (!pending->command.cancellation_requests.empty()) {
        pending->cancellation_hold_id =
            cancellation_hold_sequence_.fetch_add(1);
    }
    return pending;
}

CommitResultFinalizationCommand
ProgramResultProcessor::BuildFinalizationCommand(
    const ExecutionJobRecord& job,
    const ClaimedExecutionFinishedJob* claimed,
    const ProgramResultDecision& decision) const {
    CommitResultFinalizationCommand command{};
    command.job_id = job.job_id;
    command.requested_by = "program_result_processor";
    command.error_code = decision.error_code;
    command.error_text = decision.error_text;
    command.event_lines = decision.event_lines;
    if (decision.disposition == ProgramResultDisposition::RetryExecution) {
        command.disposition = ExecutionResultFinalizationDisposition::Retry;
    } else {
        command.disposition = ExecutionResultFinalizationDisposition::Final;
        command.final_state = decision.final_job_state;
    }
    command.outputs.reserve(decision.outputs.size());
    for (const auto& output : decision.outputs) {
        command.outputs.push_back({
            .output_key = output.output_key,
            .data_kind = output.data_kind,
            .ref_kind = output.ref_kind,
            .ref_id = output.ref_id,
        });
    }
    command.cancellation_requests.reserve(decision.cancellations.size());
    for (const auto& cancellation : decision.cancellations) {
        command.cancellation_requests.push_back({
            .job_id = cancellation.job_id,
            .request_key = cancellation.request_key,
            .reason_code = cancellation.reason_code,
            .reason_text = cancellation.reason_text.empty()
                ? std::nullopt
                : std::optional<std::string>(cancellation.reason_text),
            .requested_by = "program_result_processor",
            .caused_by_job_id = job.job_id,
        });
    }
    (void)claimed;
    return command;
}

bool ProgramResultProcessor::EnqueueFinalization(
    const PendingFinalizationPtr& pending) {
    if (!pending) return false;
    if (pending->cancellation_hold_id != 0
        && cancellation_precommit_callback_) {
        cancellation_precommit_callback_(
            pending->cancellation_hold_id,
            pending->command.cancellation_requests);
    }
    {
        std::lock_guard lock(finalization_mutex_);
        finalization_queue_.push_back(pending);
        finalization_queue_high_water_ = std::max(
            finalization_queue_high_water_, finalization_queue_.size());
        finalization_force_flush_ =
            finalization_force_flush_ || pending->force_flush;
    }
    finalization_cv_.notify_all();
    return true;
}

bool ProgramResultProcessor::WaitForFinalization(
    const PendingFinalizationPtr& pending) {
    std::unique_lock lock(pending->mutex);
    pending->cv.wait(lock, [this, &pending]() {
        return pending->completed || stop_.load();
    });
    if (!pending->completed || !pending->succeeded) {
        RecordError(pending->error.empty()
            ? "result finalization did not commit"
            : pending->error);
        return false;
    }
    return true;
}

bool ProgramResultProcessor::FlushFinalizationBarrier() {
    {
        std::lock_guard lock(finalization_mutex_);
        if (finalization_queue_.empty() && !finalization_active_) return true;
        finalization_force_flush_ = true;
    }
    finalization_cv_.notify_all();
    std::unique_lock lock(finalization_mutex_);
    finalization_drained_cv_.wait(lock, [this]() {
        return stop_.load()
            || (finalization_queue_.empty() && !finalization_active_);
    });
    return finalization_queue_.empty() && !finalization_active_;
}

void ProgramResultProcessor::FinalizationLoop() {
    for (;;) {
        std::vector<PendingFinalizationPtr> batch;
        bool full = false;
        bool deadline = false;
        bool barrier = false;
        {
            std::unique_lock lock(finalization_mutex_);
            finalization_cv_.wait(lock, [this]() {
                return stop_.load() || !finalization_queue_.empty();
            });
            if (stop_.load() && finalization_queue_.empty()) break;

            const auto batch_deadline = finalization_queue_.front()->enqueued_at
                + config_.result_finalization_delay;
            while (!stop_.load()
                && finalization_queue_.size()
                    < config_.result_finalization_batch_size
                && !finalization_force_flush_
                && Clock::now() < batch_deadline) {
                finalization_cv_.wait_until(lock, batch_deadline);
            }
            full = finalization_queue_.size()
                >= config_.result_finalization_batch_size;
            deadline = !full && !finalization_force_flush_
                && Clock::now() >= batch_deadline;
            barrier = finalization_force_flush_;
            const auto count = std::min(
                config_.result_finalization_batch_size,
                finalization_queue_.size());
            batch.reserve(count);
            for (std::size_t index = 0; index < count; ++index) {
                batch.push_back(std::move(finalization_queue_.front()));
                finalization_queue_.pop_front();
            }
            finalization_force_flush_ = std::any_of(
                finalization_queue_.begin(),
                finalization_queue_.end(),
                [](const auto& item) { return item->force_flush; });
            finalization_active_ = true;
        }
        if (full) ++finalization_full_flushes_;
        else if (deadline) ++finalization_deadline_flushes_;
        else if (barrier) ++finalization_barrier_flushes_;

        CommitResultFinalizationsBatchCommand command;
        command.finalizations.reserve(batch.size());
        for (const auto& item : batch) {
            command.finalizations.push_back(item->command);
        }
        std::vector<ResultProcessingReceipt> receipts;
        std::string error;
        bool committed = false;
        do {
            committed = execution_db_->CommitResultFinalizationsBatch(
                command, &receipts, &error)
                && receipts.size() == batch.size();
            if (committed) break;
            ++finalization_rollbacks_;
            RecordError(error.empty()
                ? "result finalization batch rolled back"
                : error);
            if (!stop_.load()) {
                std::unique_lock lock(finalization_mutex_);
                finalization_cv_.wait_for(
                    lock, std::chrono::milliseconds(5),
                    [this]() { return stop_.load(); });
            }
            receipts.clear();
            error.clear();
        } while (!stop_.load());

        ++finalization_batches_;
        finalization_batch_items_.fetch_add(batch.size());
        StoreMaximum(finalization_batch_max_size_, batch.size());
        const auto age = std::chrono::duration_cast<std::chrono::milliseconds>(
            Clock::now() - batch.front()->enqueued_at).count();
        StoreMaximum(
            finalization_max_collection_age_ms_,
            static_cast<std::uint64_t>(std::max<std::int64_t>(0, age)));

        for (std::size_t index = 0; index < batch.size(); ++index) {
            auto& pending = batch[index];
            const bool item_ok = committed && Applied(receipts[index].disposition);
            if (item_ok) {
                if (pending->cancellation_hold_id != 0
                    && cancellation_committed_callback_) {
                    cancellation_committed_callback_(
                        pending->cancellation_hold_id,
                        receipts[index].committed_cancellations);
                }
                processing_canaries_.fetch_sub(1);
                if (pending->command.disposition
                    == ExecutionResultFinalizationDisposition::Retry) {
                    ++execution_retries_;
                } else {
                    ++finalized_;
                    if (finalization_callback_
                        && receipts[index].commit_sequence != 0
                        && receipts[index].workflow_step_id > 0) {
                        finalization_callback_(
                            receipts[index].commit_sequence,
                            receipts[index].workflow_step_id,
                            pending->command.job_id);
                    }
                }
                if (event_line_callback_) {
                    for (const auto& line : pending->event_lines) {
                        event_line_callback_(line);
                    }
                }
            }
            {
                std::lock_guard lock(pending->mutex);
                pending->completed = true;
                pending->succeeded = item_ok;
                if (committed) pending->receipt = receipts[index];
                pending->error = item_ok ? std::string{} :
                    (error.empty()
                        ? "result finalization receipt rejected"
                        : error);
            }
            pending->cv.notify_all();
        }
        {
            std::lock_guard lock(finalization_mutex_);
            finalization_active_ = false;
            if (finalization_queue_.empty()) {
                finalization_force_flush_ = false;
                finalization_drained_cv_.notify_all();
            }
        }
        finalization_cv_.notify_all();
    }
    std::lock_guard lock(finalization_mutex_);
    finalization_active_ = false;
    finalization_drained_cv_.notify_all();
}

bool ProgramResultProcessor::ReconcileInterruptedProcessing(
    std::string* error_out) {
    std::string error;
    const auto interrupted =
        execution_db_->ListInterruptedResultProcessingJobs(&error);
    if (!error.empty()) {
        if (error_out != nullptr) *error_out = std::move(error);
        return false;
    }
    processing_canaries_.fetch_add(interrupted.size());
    for (const auto& item : interrupted) {
        const auto job_id = item.claimed.job.job_id;
        if (!item.structural_metadata_complete) {
            ++startup_recovery_canaries_;
            (void)RecordFailure(
                job_id,
                "RESULT_RECOVERY_METADATA_INCOMPLETE",
                item.structural_diagnostic.empty()
                    ? "PROCESSING job has incomplete structural metadata"
                    : item.structural_diagnostic);
            continue;
        }
        if (!item.has_result_blob_record) {
            ++startup_recovery_canaries_;
            (void)RecordFailure(
                job_id,
                "RESULT_RECOVERY_METADATA_MISSING",
                "PROCESSING job has no durable result blob record");
            continue;
        }
        WorkerResultBlobReference reference{
            .relative_path = item.claimed.result_blob.relative_path,
            .sha256 = item.claimed.result_blob.sha256,
            .size_bytes = static_cast<std::int64_t>(
                item.claimed.result_blob.size_bytes),
            .format = item.claimed.result_blob.format,
        };
        bool exists = false;
        if (!blob_store_->Exists(reference.relative_path, &exists, &error)) {
            if (error_out != nullptr) *error_out = std::move(error);
            return false;
        }
        if (exists) {
            std::vector<std::uint8_t> bytes;
            if (!blob_store_->Read(reference, &bytes, &error)) {
                ++startup_recovery_canaries_;
                (void)RecordFailure(
                    job_id,
                    "RESULT_RECOVERY_BLOB_CORRUPT",
                    error.empty() ? "result blob verification failed" : error);
                error.clear();
                continue;
            }
            ResultProcessingReceipt receipt{};
            if (!execution_db_->ResetInterruptedResultProcessing(
                    {.job_id = job_id,
                     .requested_by = "program_result_processor_startup"},
                    &receipt,
                    &error)
                || !Applied(receipt.disposition)) {
                if (error_out != nullptr) {
                    *error_out = error.empty()
                        ? "interrupted result reset failed"
                        : std::move(error);
                }
                return false;
            }
            ++startup_blob_resets_;
            processing_canaries_.fetch_sub(1);
            continue;
        }

        const auto* descriptor = program_kind_registry_->Find(
            item.claimed.job.program_kind);
        if (descriptor == nullptr || descriptor->result_handler == nullptr) {
            ++startup_recovery_canaries_;
            (void)RecordFailure(
                job_id,
                "RESULT_RECOVERY_DESCRIPTOR_UNAVAILABLE",
                "persisted-outcome recovery handler is unavailable");
            continue;
        }
        const auto recovered =
            descriptor->result_handler->RecoverPersistedOutcome({
                .job_id = job_id,
                .job_set_id = item.claimed.job.job_set_id,
                .program_kind = item.claimed.job.program_kind,
                .program_version = item.claimed.job.program_version,
                .program_ref_kind = item.claimed.job.program_ref_kind,
                .program_ref_id = item.claimed.job.program_ref_id,
                .fingerprint = item.claimed.job.fingerprint,
                .input_ini = item.claimed.job.input_ini,
                .terminal_sha256 = item.claimed.worker_terminal_fingerprint,
            });
        if (recovered.disposition
                == ProgramResultRecoveryDisposition::Recovered
            && recovered.decision.has_value()) {
            if (!CommitRecoveredDecision(
                    item, std::move(*recovered.decision), &error)) {
                if (error_out != nullptr) *error_out = std::move(error);
                return false;
            }
            ++startup_program_recoveries_;
            processing_canaries_.fetch_sub(1);
        } else if (recovered.disposition
                == ProgramResultRecoveryDisposition::NoPersistedOutcome) {
            ResultProcessingReceipt receipt{};
            if (!execution_db_->RequeueLostResultProcessing(
                    {.job_id = job_id,
                     .requested_by = "program_result_processor_startup"},
                    &receipt,
                    &error)
                || !Applied(receipt.disposition)) {
                if (error_out != nullptr) {
                    *error_out = error.empty()
                        ? "lost result requeue failed"
                        : std::move(error);
                }
                return false;
            }
            ++startup_lost_blob_requeues_;
            processing_canaries_.fetch_sub(1);
        } else {
            ++startup_recovery_canaries_;
            (void)RecordFailure(
                job_id,
                "RESULT_RECOVERY_INCONSISTENT",
                recovered.diagnostic.empty()
                    ? "persisted program outcome is inconsistent"
                    : recovered.diagnostic);
        }
    }
    if (error_out != nullptr) error_out->clear();
    return true;
}

bool ProgramResultProcessor::CommitRecoveredDecision(
    const InterruptedResultProcessingJob& interrupted,
    ProgramResultDecision decision,
    std::string* error_out) {
    if (decision.disposition == ProgramResultDisposition::RetryExecution
        || decision.final_job_state.empty()
        || IsSchedulerOwnedState(decision.final_job_state)) {
        if (error_out != nullptr) {
            *error_out = "persisted outcome returned an invalid recovery decision";
        }
        return false;
    }
    CommitResultFinalizationsBatchCommand batch;
    batch.finalizations.push_back(BuildFinalizationCommand(
        interrupted.claimed.job, nullptr, decision));
    const auto hold_id = batch.finalizations.front()
            .cancellation_requests.empty()
        ? 0
        : cancellation_hold_sequence_.fetch_add(1);
    if (hold_id != 0 && cancellation_precommit_callback_) {
        cancellation_precommit_callback_(
            hold_id,
            batch.finalizations.front().cancellation_requests);
    }
    std::vector<ResultProcessingReceipt> receipts;
    if (!execution_db_->CommitResultFinalizationsBatch(
            batch, &receipts, error_out)
        || receipts.size() != 1
        || !Applied(receipts.front().disposition)) {
        return false;
    }
    if (hold_id != 0 && cancellation_committed_callback_) {
        cancellation_committed_callback_(
            hold_id,
            receipts.front().committed_cancellations);
    }
    ++finalized_;
    if (finalization_callback_ && receipts.front().commit_sequence != 0
        && receipts.front().workflow_step_id > 0) {
        finalization_callback_(
            receipts.front().commit_sequence,
            receipts.front().workflow_step_id,
            interrupted.claimed.job.job_id);
    }
    if (event_line_callback_) {
        for (const auto& line : decision.event_lines) {
            event_line_callback_(line);
        }
    }
    return true;
}

bool ProgramResultProcessor::RecordFailure(
    std::int64_t job_id,
    std::string error_code,
    std::string error_text) {
    ++processing_failures_;
    ResultProcessingReceipt receipt{};
    std::string db_error;
    const bool recorded = execution_db_->RecordResultProcessingFailure(
        {
            .job_id = job_id,
            .error_code = std::move(error_code),
            .error_text = error_text,
        },
        &receipt,
        &db_error);
    RecordError(db_error.empty() ? std::move(error_text) : std::move(db_error));
    return recorded && Applied(receipt.disposition);
}

void ProgramResultProcessor::RecordError(std::string error) {
    if (error.empty()) return;
    std::lock_guard lock(error_mutex_);
    last_error_ = std::move(error);
}

} // namespace savor::db::execution::programdb

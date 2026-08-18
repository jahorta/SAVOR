#include "ScenarioAssessment.h"

#include <deque>
#include <iomanip>
#include <limits>
#include <sstream>
#include <unordered_set>

#include "Common/DbService.h"
#include "Execution/IExecutionDb.h"
#include "Runner/Runtime/ProgramRuntime/Capabilities/SourceCapabilityPacks.h"
#include "UIRead/IUiReadDb.h"

namespace savor::e2e {
namespace {

const char* ToString(
    savor::db::execution::workflow::WorkflowInstanceState state) {
    using savor::db::execution::workflow::WorkflowInstanceState;
    switch (state) {
    case WorkflowInstanceState::Pending: return "PENDING";
    case WorkflowInstanceState::Running: return "RUNNING";
    case WorkflowInstanceState::Completed: return "COMPLETED";
    case WorkflowInstanceState::Failed: return "FAILED";
    case WorkflowInstanceState::Canceled: return "CANCELED";
    }
    return "UNKNOWN";
}

const char* ToString(
    savor::db::execution::workflow::WorkflowStepState state) {
    using savor::db::execution::workflow::WorkflowStepState;
    switch (state) {
    case WorkflowStepState::Waiting: return "WAITING";
    case WorkflowStepState::Ready: return "READY";
    case WorkflowStepState::Materialized: return "MATERIALIZED";
    case WorkflowStepState::Running: return "RUNNING";
    case WorkflowStepState::Completed: return "COMPLETED";
    case WorkflowStepState::Failed: return "FAILED";
    case WorkflowStepState::Skipped: return "SKIPPED";
    }
    return "UNKNOWN";
}

std::string Quoted(std::string_view value) {
    std::ostringstream out;
    out << std::quoted(std::string(value));
    return out.str();
}

std::string HexBytes(std::span<const std::uint8_t> bytes) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string encoded;
    encoded.reserve(bytes.size() * 2);
    for (const auto byte : bytes) {
        encoded.push_back(kHex[(byte >> 4) & 0x0f]);
        encoded.push_back(kHex[byte & 0x0f]);
    }
    return encoded;
}

} // namespace

void ScenarioAssessment::Require(bool condition, std::string message) {
    if (!condition) invariant_failures.push_back(std::move(message));
}

void ScenarioAssessment::Warn(std::string message) {
    warnings.push_back(std::move(message));
}

std::string ScenarioAssessment::FailureSummary(
    std::string_view scenario) const {
    std::ostringstream out;
    out << scenario << " invariant assessment failed";
    for (const auto& failure : invariant_failures)
        out << "\n  - " << failure;
    return out.str();
}

void AssessCommonScenarioExecution(
    savor::db::IExecutionDb* execution_db,
    std::span<const savor::db::execution::workflow::WorkflowGraphSnapshot>
        workflows,
    const savor::runner::parallel::savordb::CoordinatorRuntimeTelemetry& telemetry,
    std::span<const savor::runner::parallel::savordb::
        ReadyWorkerDispatchSnapshot> ready_workers,
    ScenarioAssessment* assessment) {
    if (assessment == nullptr) return;
    assessment->Require(execution_db != nullptr,
                        "execution database is unavailable");
    assessment->Require(!workflows.empty(),
                        "no workflow snapshot was retained");
    if (execution_db == nullptr) return;

    using savor::db::execution::workflow::WorkflowInstanceState;
    using savor::db::execution::workflow::WorkflowStepState;
    std::unordered_set<std::int64_t> visited_job_sets;
    std::deque<std::int64_t> pending_job_sets;
    for (const auto& graph : workflows) {
        assessment->Require(
            graph.instance.state == WorkflowInstanceState::Completed,
            "workflow "
                + std::to_string(graph.instance.workflow_instance_id)
                + " did not reach COMPLETED");
        for (const auto& step : graph.steps) {
            assessment->Require(
                step.state == WorkflowStepState::Completed
                    || step.state == WorkflowStepState::Skipped,
                "workflow step " + step.step_key
                    + " is not terminal-success or guard-skipped");
            if (step.job_set_id
                && visited_job_sets.insert(*step.job_set_id).second) {
                pending_job_sets.push_back(*step.job_set_id);
            }
        }
    }

    while (!pending_job_sets.empty()) {
        const auto job_set_id = pending_job_sets.front();
        pending_job_sets.pop_front();
        for (const auto& child :
             execution_db->GetChildJobSetProgress(job_set_id)) {
            if (visited_job_sets.insert(child.job_set_id).second)
                pending_job_sets.push_back(child.job_set_id);
        }
        for (const auto& listed : execution_db->ListJobsInJobSet(job_set_id)) {
            const auto job = execution_db->GetExecutionJob(listed.job_id);
            assessment->Require(
                job.has_value(),
                "execution job " + std::to_string(listed.job_id)
                    + " disappeared from job set "
                    + std::to_string(job_set_id));
            if (!job) continue;
            const bool business_final =
                job->state != "PENDING_WORKSET" && job->state != "QUEUED"
                && job->state != "CLAIMED" && job->state != "RUNNING"
                && job->state != "EXECUTION_FINISHED";
            assessment->Require(
                business_final,
                "execution job " + std::to_string(job->job_id)
                    + " is not business-final; state=" + job->state);
            assessment->Require(
                job->state != "FAILED",
                "execution job " + std::to_string(job->job_id)
                    + " reached " + job->state);
            assessment->Require(
                job->result_processing_failures == 0,
                "execution job " + std::to_string(job->job_id)
                    + " recorded result-processing failures");
            if (job->worker_terminal_status) {
                assessment->Require(
                    job->result_processing_state
                        == std::optional<std::string>("PROCESSED"),
                    "execution job " + std::to_string(job->job_id)
                        + " has an unprocessed worker terminal");
            }
        }
    }

    assessment->Require(
        telemetry.workflow.materialization_failure_count == 0
            && telemetry.workflow.workflow_failed_count == 0,
        "workflow coordinator recorded a materialization or workflow failure");
    assessment->Require(
        telemetry.execution.reconstruction_invariant_failures == 0,
        "job execution coordinator recorded a reconstruction invariant failure");
    assessment->Require(
        telemetry.execution.sidecar_submit_receipts_mismatched == 0
            && telemetry.execution.cancellation_mutation_rollbacks == 0,
        "job execution coordinator recorded sidecar or cancellation persistence drift");
    assessment->Require(
        telemetry.execution.pending_worker_terminals == 0
            && telemetry.execution.draining_worksets == 0
            && !telemetry.execution.claims_paused_for_terminal_staging,
        "job execution coordinator retained terminal or draining state");
    assessment->Require(
        !telemetry.execution.invariant_paused,
        "job execution coordinator ended invariant-paused");
    assessment->Require(
        telemetry.execution.worker_terminal_acks
            == telemetry.execution.worker_terminals_staged
                + telemetry.execution
                      .worker_terminals_discarded_after_authority_release
            && telemetry.execution.worker_terminals_observed
                == telemetry.execution.worker_terminals_staged
                    + telemetry.execution
                          .worker_terminals_discarded_after_authority_release,
        "worker terminal observation, staging, and acknowledgement counts do not reconcile");
    assessment->Require(
        telemetry.execution
                .worker_terminals_discarded_after_authority_release == 0
            && telemetry.execution
                .worker_terminal_ack_abandoned_generation_loss == 0,
        "worker terminal authority was lost during reconciliation");
    assessment->Require(
        telemetry.execution.draining_transitions
            == telemetry.execution.worksets_submitted,
        "submitted worksets and draining transitions do not reconcile");
    assessment->Require(
        telemetry.worker.submit_rejected == 0
            && telemetry.worker.worker_losses == 0
            && telemetry.worker.liveness_quarantines == 0,
        "worker coordinator rejected work or lost/quarantined a worker");
    assessment->Require(
        telemetry.results.processing_failures == 0
            && telemetry.results.descriptor_unavailable == 0,
        "program result processor recorded a failure");
    assessment->Require(
        telemetry.cleanup.failed == 0 && telemetry.cleanup.orphan_failed == 0,
        "worker result blob cleanup recorded a failure");

    const auto expected_contract =
        savor::runtime::BuildProductionWorkerRuntimeContractV1();
    for (const auto& worker : ready_workers) {
        assessment->Require(
            savor::runtime::IsNormalWorkerMode(worker.mode)
                && worker.runtime_contract_sha256
                    == expected_contract.canonical_sha256,
            "ready worker " + std::to_string(worker.worker_id)
                + " does not match the homogeneous runtime contract");
    }
    if (ready_workers.empty())
        assessment->Warn("no worker remained ready at the final snapshot");
    if (telemetry.execution.worker_terminal_retry_attempts != 0)
        assessment->Warn("worker terminal persistence required retries");
    if (telemetry.execution.active_lease_renewal_retries != 0)
        assessment->Warn("active workset lease renewal required retries");
    if (telemetry.execution.startup_recovered_dispatches != 0
        || telemetry.execution.startup_requeued_jobs != 0) {
        assessment->Warn("execution coordination used startup recovery");
    }
    if (!telemetry.execution.last_error.empty())
        assessment->Warn("job execution coordinator retained diagnostic: "
                         + telemetry.execution.last_error);
    if (telemetry.worker.liveness_probe_failures != 0)
        assessment->Warn("worker liveness probes required recovery");
    if (!telemetry.results.last_error.empty())
        assessment->Warn("result processor retained diagnostic: "
                         + telemetry.results.last_error);
    if (!telemetry.cleanup.last_error.empty())
        assessment->Warn("result cleanup retained diagnostic: "
                         + telemetry.cleanup.last_error);
}

void EmitScenarioAssessment(
    std::string_view scenario,
    const ScenarioAssessment& assessment,
    const std::function<void(const std::string&)>& sink) {
    if (!sink) return;
    std::ostringstream summary;
    summary << "[scenario-assessment] scenario=" << scenario
            << " execution="
            << (assessment.Passed() ? "SUCCEEDED" : "FAILED")
            << " invariants="
            << (assessment.Passed() ? "PASS" : "FAIL")
            << " warnings=" << assessment.warnings.size()
            << " trajectory=HUMAN_REVIEW_REQUIRED";
    sink(summary.str());
    for (const auto& failure : assessment.invariant_failures)
        sink("[scenario-invariant-failure] scenario="
             + std::string(scenario) + " detail=" + failure);
    for (const auto& warning : assessment.warnings)
        sink("[scenario-warning] scenario=" + std::string(scenario)
             + " detail=" + warning);
}

void ReportCommonScenarioTrajectory(
    savor::db::core::DBService* db_service,
    std::span<const savor::db::execution::workflow::WorkflowGraphSnapshot>
        workflows,
    std::string_view scenario,
    const std::function<void(const std::string&)>& sink,
    ScenarioAssessment* assessment) {
    if (db_service == nullptr || !sink) return;
    std::string projection_error;
    const bool projection_current =
        db_service->RunUiReadProjectionOnce(&projection_error);
    if (assessment != nullptr) {
        assessment->Require(
            projection_current,
            projection_error.empty()
                ? "UI read projection could not be brought current for trajectory reporting"
                : "UI read projection failed: " + projection_error);
    }
    auto* execution_db = db_service->ExecutionDb();
    auto* ui_read_db = db_service->UiReadDb();
    if (execution_db == nullptr) return;

    std::unordered_set<std::int64_t> visited_job_sets;
    std::deque<std::int64_t> pending_job_sets;
    for (const auto& graph : workflows) {
        sink("[workflow-trajectory] scenario=" + std::string(scenario)
             + " workflow="
             + std::to_string(graph.instance.workflow_instance_id)
             + " kind=" + graph.instance.workflow_kind
             + " state=" + ToString(graph.instance.state)
             + " steps=" + std::to_string(graph.steps.size()));
        for (const auto& step : graph.steps) {
            std::ostringstream line;
            line << "[workflow-step-trajectory] scenario=" << scenario
                 << " workflow=" << graph.instance.workflow_instance_id
                 << " step=" << step.workflow_step_id
                 << " node=" << step.graph_node_key
                 << " key=" << step.step_key
                 << " kind=" << step.step_kind
                 << " state=" << ToString(step.state)
                 << " skip_reason="
                 << Quoted(step.blocked_reason.value_or(""))
                 << " job_set=";
            if (step.job_set_id) line << *step.job_set_id;
            else line << "none";
            line << " input="
                 << step.input_ref_kind.value_or("none") << ':';
            if (step.input_ref_id) line << *step.input_ref_id;
            else line << "none";
            line << " output="
                 << step.output_ref_kind.value_or("none") << ':';
            if (step.output_ref_id) line << *step.output_ref_id;
            else line << "none";
            sink(line.str());
            if (step.job_set_id
                && visited_job_sets.insert(*step.job_set_id).second)
                pending_job_sets.push_back(*step.job_set_id);
        }
        for (const auto& output : execution_db->WorkflowQueryService()
                                      ->ListStepOutputs(
                                          graph.instance.workflow_instance_id)) {
            sink("[workflow-output-trajectory] scenario="
                 + std::string(scenario) + " workflow="
                 + std::to_string(graph.instance.workflow_instance_id)
                 + " node=" + output.graph_node_key
                 + " key=" + output.output_key
                 + " data_kind=" + output.data_kind
                 + " ref=" + output.ref_kind + ':'
                 + std::to_string(output.ref_id));
        }
    }

    while (!pending_job_sets.empty()) {
        const auto job_set_id = pending_job_sets.front();
        pending_job_sets.pop_front();
        const auto progress = execution_db->GetJobSetProgress(job_set_id);
        if (progress) {
            std::ostringstream line;
            line << "[job-set-trajectory] scenario=" << scenario
                 << " job_set=" << job_set_id
                 << " total=" << progress->total_jobs
                 << " completed=" << progress->completed_jobs
                 << " succeeded=" << progress->succeeded_jobs
                 << " failed=" << progress->failed_jobs
                 << " canceled=" << progress->canceled_jobs
                 << " expected=";
            if (progress->expected_total) line << *progress->expected_total;
            else line << "none";
            sink(line.str());
        }
        for (const auto& child :
             execution_db->GetChildJobSetProgress(job_set_id)) {
            sink("[job-set-child-trajectory] scenario="
                 + std::string(scenario) + " parent="
                 + std::to_string(job_set_id) + " child="
                 + std::to_string(child.job_set_id) + " purpose="
                 + Quoted(child.purpose));
            if (visited_job_sets.insert(child.job_set_id).second)
                pending_job_sets.push_back(child.job_set_id);
        }
        for (const auto& listed : execution_db->ListJobsInJobSet(job_set_id)) {
            const auto job = execution_db->GetExecutionJob(listed.job_id);
            if (!job) continue;
            std::ostringstream line;
            line << "[job-trajectory] scenario=" << scenario
                 << " job_set=" << job_set_id
                 << " job=" << job->job_id
                 << " program=" << job->program_kind << ':'
                 << job->program_version
                 << " ref=" << job->program_ref_kind << ':'
                 << job->program_ref_id
                 << " state=" << job->state
                 << " attempts=" << job->attempts
                 << " worker_terminal="
                 << job->worker_terminal_status.value_or("none")
                 << " result_processing="
                 << job->result_processing_state.value_or("none")
                 << " workset=";
            if (job->workset_id) line << *job->workset_id;
            else line << "none";
            sink(line.str());

            for (const auto& event :
                 execution_db->ListJobEvents(
                     job->job_id,
                     (std::numeric_limits<int>::max)())) {
                sink("[job-event-trajectory] scenario="
                     + std::string(scenario) + " job="
                     + std::to_string(job->job_id) + " kind="
                     + event.event_kind + " artifact="
                     + (event.artifact_id
                            ? std::to_string(*event.artifact_id) : "none")
                     + " message=" + Quoted(event.message));
            }
            if (ui_read_db == nullptr) continue;
            for (const auto& artifact :
                 ui_read_db->ListJobArtifacts(job->job_id)) {
                sink("[job-artifact-trajectory] scenario="
                     + std::string(scenario) + " job="
                     + std::to_string(job->job_id) + " artifact="
                     + std::to_string(artifact.artifact_id) + " role="
                     + artifact.role_kind + " kind="
                     + artifact.artifact_kind + " size="
                     + std::to_string(artifact.size_bytes) + " file="
                     + Quoted(artifact.filename));
            }
            for (const auto& progress_event :
                 ui_read_db->ListJobProgress(
                     job->job_id,
                     (std::numeric_limits<int>::max)())) {
                std::ostringstream progress_line;
                progress_line << "[job-progress-trajectory] scenario="
                    << scenario << " job=" << job->job_id
                    << " attempt=" << progress_event.attempt_id
                    << " ordinal=" << progress_event.ordinal
                    << " workset=" << progress_event.workset_id
                    << " item=" << progress_event.item_id
                    << " invocation=" << progress_event.invocation_id
                    << " library=" << progress_event.library_id << '/'
                    << progress_event.library_revision
                    << " point=" << progress_event.progress_point_id
                    << " schema=" << progress_event.schema_id << '/'
                    << progress_event.schema_revision
                    << " payload_bytes="
                    << progress_event.typed_payload.size()
                    << " payload_hex="
                    << HexBytes(progress_event.typed_payload)
                    << " text=" << Quoted(progress_event.display_text);
                sink(progress_line.str());
            }
        }
    }
}

} // namespace savor::e2e

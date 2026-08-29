#include <gtest/gtest.h>

#include "Phases/Programs/SeedProbe/SeedProbeModule.h"
#include "Runner/Runtime/EmulationSession.h"
#include "Runner/Runtime/IProgramRuntimePort.h"
#include "Runner/Runtime/ProgramRuntime/Codec/ProgramCodecV1.h"
#include "Runner/Runtime/WorkerRuntime.h"
#include "Runner/Runtime/Worksets/ProgramBaseline.h"
#include "Utils/Hash.h"
#include "common/FakePhysicalStopBackend.h"
#include "common/ScriptedDolphinBackend.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <barrier>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <future>
#include <iterator>
#include <latch>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;
using namespace savor::runtime;
using savor::test_support::ScriptedDolphinBackend;
using savor::test_support::ScriptedDolphinBackendControl;
using savor::test_support::FakePhysicalStopBackend;
using savor::test_support::FakePhysicalStopBackendControl;

thread_local bool g_fake_runtime_start_active = false;
thread_local bool g_fake_action_dispatch_active = false;

static_assert(!std::is_same_v<WorksetEpoch, WorkerCommandSequence>);
static_assert(!std::is_convertible_v<WorksetEpoch, WorkerCommandSequence>);
static_assert(!std::is_convertible_v<WorkerCommandSequence, WorksetEpoch>);
static_assert(!std::is_convertible_v<WorksetEpoch, std::uint64_t>);
static_assert(!std::is_convertible_v<std::uint64_t, WorksetEpoch>);

ExecutionRequestPolicy SessionExecutionPolicy(WorksetEpoch epoch)
{
    ExecutionRequestPolicy policy;
    policy.expected_epoch = epoch;
    return policy;
}

SubmitWorksetCommand SubmitWithoutInitialCancellations(
    WorkerWorksetDefinition definition)
{
    const auto workset_id = definition.workset_id;
    return {
        .definition = std::move(definition),
        .initial_cancellations = {
            .workset_id = workset_id,
        },
    };
}

StopSubscriptionGroupDefinition WorkerWakeGroup(std::uint32_t pc)
{
    return {
        .id = StopSubscriptionGroupId(900),
        .source = {
            .id = StopSourceId(900),
            .stable_name = "test.worker-runtime.wake",
            .diagnostic_label = "worker runtime ingress ordering",
        },
        .subscriptions = {{
            .id = StopSubscriptionId(900),
            .point = PcStopPointSpec{pc},
            .route = ForegroundStopWait{},
        }},
    };
}

std::optional<ExecutionTerminalResult> DrainSessionExecution(
    EmulationSession& session,
    int maximum_pumps = 32)
{
    for (int pump = 0; pump < maximum_pumps; ++pump)
    {
        session.PumpExecution();
        for (ExecutionEvent& event : session.DrainExecutionEvents())
        {
            if (event.kind == ExecutionEventKind::Terminal &&
                event.terminal)
            {
                return std::move(event.terminal);
            }
        }
    }
    return std::nullopt;
}

class TemporaryRuntimeDirectory
{
public:
    TemporaryRuntimeDirectory()
    {
        static std::atomic<std::uint64_t> next{1};
        path_ = std::filesystem::temp_directory_path() /
            ("savor-worker-runtime-" +
             std::to_string(
                 std::chrono::steady_clock::now()
                     .time_since_epoch()
                     .count()) +
             "-" +
             std::to_string(
                 next.fetch_add(1, std::memory_order_relaxed)));
        std::filesystem::create_directories(path_);
    }

    ~TemporaryRuntimeDirectory()
    {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    [[nodiscard]] std::filesystem::path File(
        std::string_view name) const
    {
        return path_ / name;
    }

private:
    std::filesystem::path path_;
};

std::vector<std::uint8_t> EncodedCompletedProgramResult(
    InvocationId invocation_id,
    AttemptId attempt_id)
{
    const auto phase = seedprobe::SeedProbeFullPhaseDefinitionV2();
    const auto hash = program::ContentHash256::FromHex(
        phase->runtime_contract().module.canonical_hash);
    if (!hash)
        throw std::runtime_error("test module hash is invalid");

    program::ProgramResult result{
        .invocation_id = invocation_id,
        .attempt_id = attempt_id,
        .module = {
            phase->runtime_contract().module.canonical_id,
            phase->runtime_contract().module.revision,
            *hash},
        .entrypoint = phase->runtime_contract().entrypoint,
        .infrastructure =
            program::ProgramInfrastructureStatus::Completed,
        .cleanup = program::ProgramCleanupStatus::Clean,
        .session_disposition = SessionDisposition::Clean,
        .provenance = {
            .requesting_component =
                "test.execution-worker-runtime"},
    };
    program::EncodeResult encoded =
        program::EncodeProgramResultV1(result);
    if (!encoded)
    {
        throw std::runtime_error(
            "test ProgramResult encoding failed: " +
            encoded.status.message);
    }
    return std::move(encoded.bytes);
}

struct FakeProgramRuntimeControl
{
    struct ObservedInvocation
    {
        WorkerCommandSequence command_sequence;
        EncodedInvocationEnvelope invocation;
        bool state_already_prepared = false;
        std::string baseline_sha256;
    };

    mutable std::mutex mutex;
    std::condition_variable changed;
    std::shared_ptr<IProgramRuntimeEventSink> event_sink;
    std::optional<ObservedInvocation> last_invocation;
    CancellationToken last_token;
    int prepare_count = 0;
    int start_count = 0;
    int cancellation_count = 0;
    int shutdown_count = 0;
    int action_dispatch_count = 0;
    int action_completion_count = 0;
    bool throw_prepare = false;
    bool throw_start = false;
    bool throw_action_dispatch = false;
    bool emit_action_on_start = false;
    bool complete_action_from_execution_terminal = false;
    bool complete_invocation_on_action_completion = false;
    bool execution_finished_by_action_completion = false;
    bool action_dispatched_inline = false;
    bool action_completed_inline = false;
    std::uint64_t maximum_artifacts = 4096;
    program::ProgramActionResolutionStatus
        last_action_resolution_status =
            program::ProgramActionResolutionStatus::Completed;
    std::string last_action_resolution_code;
    program::InvocationStatePolicy prepared_state_policy =
        program::InvocationStatePolicy::RestoreBaseline;
    std::uint64_t next_template_id = 1;
    std::unordered_map<
        std::uint64_t,
        EncodedInvocationEnvelope>
        prepared_templates;
    std::shared_ptr<program::IProgramActionRequestSink> action_sink;
    std::function<std::vector<program::StagedProgramOutput>()>
        pending_state_artifact_factory;
    std::optional<ProgramExecutionFinished> finished_execution;
    std::vector<std::uint8_t> terminal_output_payload;
    std::vector<std::string> action_order;
    ProgramRuntimeSubmission prepare_submission =
        ProgramRuntimeSubmission::Accepted();
    ProgramRuntimeSubmission start_submission =
        ProgramRuntimeSubmission::Accepted();
    ProgramRuntimeSubmission cancellation_submission =
        ProgramRuntimeSubmission::Accepted();
    ProgramRuntimeSubmission action_completion_submission =
        ProgramRuntimeSubmission::Accepted();

    [[nodiscard]] bool WaitForStarts(int expected)
    {
        std::unique_lock lock(mutex);
        return changed.wait_for(lock, 5s, [&] { return start_count >= expected; });
    }

    [[nodiscard]] bool WaitForCancellations(int expected)
    {
        std::unique_lock lock(mutex);
        return changed.wait_for(
            lock,
            5s,
            [&] { return cancellation_count >= expected; });
    }

    [[nodiscard]] bool WaitForShutdowns(int expected)
    {
        std::unique_lock lock(mutex);
        return changed.wait_for(lock, 5s, [&] { return shutdown_count >= expected; });
    }

    [[nodiscard]] bool WaitForActionCompletions(int expected)
    {
        std::unique_lock lock(mutex);
        return changed.wait_for(
            lock,
            5s,
            [&] { return action_completion_count >= expected; });
    }

    [[nodiscard]] bool WaitForActionDispatches(int expected)
    {
        std::unique_lock lock(mutex);
        return changed.wait_for(
            lock,
            5s,
            [&] { return action_dispatch_count >= expected; });
    }

    [[nodiscard]] int StartCount() const
    {
        std::lock_guard lock(mutex);
        return start_count;
    }

    [[nodiscard]] int ShutdownCount() const
    {
        std::lock_guard lock(mutex);
        return shutdown_count;
    }

    [[nodiscard]] int CancellationCount() const
    {
        std::lock_guard lock(mutex);
        return cancellation_count;
    }

    [[nodiscard]] CancellationToken LastToken() const
    {
        std::lock_guard lock(mutex);
        return last_token;
    }

    bool EmitTerminal(
        InvocationTerminalStatus status,
        CleanupStatus cleanup = CleanupStatus::Clean,
        SessionDisposition disposition = SessionDisposition::Clean,
        std::optional<WorksetEpoch> epoch_override = std::nullopt,
        RuntimeError error = {})
    {
        ObservedInvocation invocation;
        std::vector<std::uint8_t> output_payload;
        {
            std::lock_guard lock(mutex);
            if (!event_sink || !last_invocation)
                return false;
            invocation = *last_invocation;
            output_payload = terminal_output_payload;
            finished_execution = ProgramExecutionFinished{
                invocation.invocation.invocation_id,
                invocation.invocation.attempt_id,
                status,
                cleanup,
                disposition,
                epoch_override.value_or(
                    invocation.invocation.expected_workset_epoch),
                std::move(output_payload),
                std::move(error)};
        }
        event_sink->Publish(ProgramInvocationCompletionAvailableEvent{
            invocation.invocation.invocation_id,
            invocation.invocation.attempt_id});
        return true;
    }
};

class FakeProgramRuntimePort final : public IProgramRuntimePort
{
public:
    explicit FakeProgramRuntimePort(
        std::shared_ptr<FakeProgramRuntimeControl> control)
        : control_(std::move(control))
    {
    }

    ProgramRuntimeSubmission StartObservedInvocation(
        FakeProgramRuntimeControl::ObservedInvocation request,
        CancellationToken cancellation,
        std::shared_ptr<IProgramRuntimeEventSink> events)
    {
        bool should_throw = false;
        bool emit_action = false;
        std::shared_ptr<program::IProgramActionRequestSink>
            action_sink;
        ProgramRuntimeSubmission submission;
        {
            std::lock_guard lock(control_->mutex);
            ++control_->start_count;
            control_->last_invocation = request;
            control_->last_token = std::move(cancellation);
            control_->event_sink = std::move(events);
            should_throw = control_->throw_start;
            emit_action = control_->emit_action_on_start;
            action_sink = control_->action_sink;
            submission = control_->start_submission;
            control_->changed.notify_all();
        }
        if (should_throw)
            throw std::runtime_error("fake invocation failure after admission");
        if (submission.accepted && emit_action && action_sink)
        {
            {
                std::lock_guard lock(control_->mutex);
                control_->action_order.push_back(
                    "runtime_start_publish");
            }
            g_fake_runtime_start_active = true;
            action_sink->Publish(program::ProgramActionRequest{
                .request_id =
                    program::ProgramActionRequestId(1),
                .invocation_id =
                    request.invocation.invocation_id,
                .attempt_id = request.invocation.attempt_id,
                .operation =
                    program::ProgramHostOperation::InvokeAction,
                .expected_epoch =
                    request.invocation.expected_workset_epoch,
                .input = {
                    program::ProgramValueId(1),
                    {program::ProgramValue{
                        program::ProgramValueId(1),
                        program::TypeRef::Builtin(
                            program::BuiltinType::Unit),
                        program::UnitValue{}}}},
                .scope = program::ProgramScopeId(1),
            });
            g_fake_runtime_start_active = false;
        }
        return submission;
    }

    ProgramRuntimeSubmission AdmitModuleClosure(
        ModuleClosureAdmissionRequest request,
        ModuleClosureAdmissionReceipt& receipt) override
    {
        bool should_throw = false;
        ProgramRuntimeSubmission submission;
        {
            std::lock_guard lock(control_->mutex);
            ++control_->prepare_count;
            should_throw = control_->throw_prepare;
            submission = control_->prepare_submission;
            control_->changed.notify_all();
        }
        if (should_throw)
            throw std::runtime_error("fake closure admission failure");
        if (!submission.accepted)
            return submission;
        if (request.modules.empty())
        {
            return ProgramRuntimeSubmission::Rejected(
                WorkerRejectionCode::InvalidArgument,
                "fake module closure is empty");
        }
        receipt = {
            .root = std::move(request.root),
            .dependency_lock_sha256 =
                std::move(request.expected_dependency_lock_sha256),
            .admitted_module_count = request.modules.size(),
        };
        return ProgramRuntimeSubmission::Accepted();
    }

    ProgramRuntimeSubmission PrepareInvocationTemplate(
        InvocationTemplatePreparationRequest request,
        PreparedInvocationTemplateReceipt& receipt) override
    {
        std::lock_guard lock(control_->mutex);
        const PreparedInvocationTemplateId id(
            control_->next_template_id++);
        control_->prepared_templates.emplace(
            id.value(),
            request.invocation_template);
        const auto phase =
            seedprobe::SeedProbeFullPhaseDefinitionV2();
        receipt = {
            id,
            request.invocation_template.invocation_id,
            request.invocation_template.attempt_id,
            request.invocation_template.module,
            request.invocation_template.entrypoint,
            phase->runtime_contract().verified_dependency_sha256,
            control_->prepared_state_policy,
            control_->maximum_artifacts};
        return ProgramRuntimeSubmission::Accepted();
    }

    ProgramRuntimeSubmission ReleaseInvocationTemplate(
        PreparedInvocationTemplateId template_id) override
    {
        std::lock_guard lock(control_->mutex);
        return control_->prepared_templates.erase(
                   template_id.value()) == 1
            ? ProgramRuntimeSubmission::Accepted()
            : ProgramRuntimeSubmission::Rejected(
                  WorkerRejectionCode::InvalidArgument,
                  "fake template missing");
    }

    ProgramRuntimeSubmission StartPreparedInvocation(
        PreparedInvocationStartRequest request,
        CancellationToken cancellation,
        std::shared_ptr<IProgramRuntimeEventSink> events) override
    {
        EncodedInvocationEnvelope envelope;
        {
            std::lock_guard lock(control_->mutex);
            const auto found =
                control_->prepared_templates.find(
                    request.template_id.value());
            if (found == control_->prepared_templates.end())
            {
                return ProgramRuntimeSubmission::Rejected(
                    WorkerRejectionCode::InvalidArgument,
                    "fake template missing");
            }
            envelope = found->second;
        }
        envelope.expected_workset_epoch = request.workset_epoch;
        ProgramRuntimeSubmission started = StartObservedInvocation(
            {
                request.command_sequence,
                std::move(envelope),
                request.state_already_prepared,
                request.baseline_sha256,
            },
            std::move(cancellation),
            std::move(events));
        if (started.accepted)
        {
            std::lock_guard lock(control_->mutex);
            control_->prepared_templates.erase(
                request.template_id.value());
        }
        return started;
    }

    ProgramRuntimeSubmission RequestCancellation(
        InvocationId) override
    {
        std::lock_guard lock(control_->mutex);
        ++control_->cancellation_count;
        control_->action_order.push_back("runtime_cancel");
        control_->changed.notify_all();
        if (control_->execution_finished_by_action_completion)
            return ProgramRuntimeSubmission::ExecutionAlreadyFinished();
        return control_->cancellation_submission;
    }

    void BindActionSink(
        std::shared_ptr<program::IProgramActionRequestSink> sink)
        override
    {
        std::lock_guard lock(control_->mutex);
        control_->action_sink = std::move(sink);
    }

    ProgramRuntimeSubmission DeliverActionResolution(
        program::ProgramActionResolution completion) override
    {
        std::optional<FakeProgramRuntimeControl::ObservedInvocation> invocation;
        std::vector<std::uint8_t> output_payload;
        ProgramRuntimeSubmission submission;
        {
            std::lock_guard lock(control_->mutex);
            ++control_->action_completion_count;
            control_->last_action_resolution_status =
                completion.status;
            control_->last_action_resolution_code =
                completion.code;
            control_->action_completed_inline =
                g_fake_action_dispatch_active;
            control_->action_order.push_back("runtime_completion");
            submission = control_->action_completion_submission;
            if (submission.accepted &&
                control_->complete_invocation_on_action_completion)
            {
                control_->execution_finished_by_action_completion = true;
                invocation = control_->last_invocation;
                output_payload =
                    control_->terminal_output_payload;
                control_->finished_execution = ProgramExecutionFinished{
                    invocation->invocation.invocation_id,
                    invocation->invocation.attempt_id,
                    InvocationTerminalStatus::Completed,
                    CleanupStatus::Clean,
                    SessionDisposition::Clean,
                    invocation->invocation.expected_workset_epoch,
                    std::move(output_payload),
                    {}};
            }
            control_->changed.notify_all();
        }
        return submission;
    }

    ProgramExecutionTakeResult TakeFinishedExecution(
        InvocationId invocation_id,
        AttemptId attempt_id) override
    {
        std::lock_guard lock(control_->mutex);
        if (!control_->finished_execution)
            return {};
        if (control_->finished_execution->invocation_id != invocation_id ||
            control_->finished_execution->attempt_id != attempt_id)
        {
            return {false, std::nullopt, {
                WorkerRejectionCode::InvocationMismatch,
                "fake finished execution mismatch"}};
        }
        auto finished = std::move(control_->finished_execution);
        control_->finished_execution.reset();
        return {true, std::move(finished), {}};
    }

    void Shutdown() noexcept override
    {
        std::lock_guard lock(control_->mutex);
        ++control_->shutdown_count;
        control_->changed.notify_all();
    }

private:
    std::shared_ptr<FakeProgramRuntimeControl> control_;
};

class FakeProgramActionHost final
    : public program::IProgramActionHost
{
public:
    explicit FakeProgramActionHost(
        std::shared_ptr<FakeProgramRuntimeControl> control)
        : control_(std::move(control))
    {
    }

    program::ProgramActionDispatchResult Dispatch(
        program::ProgramActionRequest request) override
    {
        g_fake_action_dispatch_active = true;
        bool should_throw = false;
        {
            std::lock_guard lock(control_->mutex);
            ++control_->action_dispatch_count;
            control_->action_dispatched_inline =
                g_fake_runtime_start_active;
            control_->action_order.push_back(
                "action_host_dispatch");
            should_throw = control_->throw_action_dispatch;
        }
        if (should_throw)
        {
            g_fake_action_dispatch_active = false;
            throw std::runtime_error(
                "fake program action dispatch failure");
        }
        {
            std::lock_guard lock(control_->mutex);
            if (control_->complete_action_from_execution_terminal)
            {
                pending_request_ = std::move(request);
                g_fake_action_dispatch_active = false;
                control_->changed.notify_all();
                return {
                    true,
                    std::nullopt,
                    {}};
            }
        }
        program::ProgramActionResolution completion{
            .request_id = request.request_id,
            .invocation_id = request.invocation_id,
            .attempt_id = request.attempt_id,
            .operation = request.operation,
            .status =
                program::ProgramActionResolutionStatus::Completed,
            .workset_epoch = request.expected_epoch,
            .output = {
                program::ProgramValueId(1),
                {program::ProgramValue{
                    program::ProgramValueId(1),
                    program::TypeRef::Builtin(
                        program::BuiltinType::Unit),
                    program::UnitValue{}}}},
            .cleanup = program::ProgramCleanupStatus::Clean,
            .session_disposition = SessionDisposition::Clean,
        };
        std::function<std::vector<program::StagedProgramOutput>()>
            pending_factory;
        {
            std::lock_guard lock(control_->mutex);
            pending_factory =
                control_->pending_state_artifact_factory;
        }
        if (pending_factory)
        {
            try
            {
                auto staged_outputs = pending_factory();
                g_fake_action_dispatch_active = false;
                return {
                    true,
                    program::ActorActionResult{
                        std::move(completion),
                        std::move(staged_outputs)},
                    {}};
            }
            catch (...)
            {
                g_fake_action_dispatch_active = false;
                throw;
            }
        }
        g_fake_action_dispatch_active = false;
        return {
            true,
            program::ActorActionResult{
                std::move(completion), {}},
            {}};
    }

    void RequestCancellation(
        InvocationId,
        CancellationReason) noexcept override
    {
    }
    void HandleExecutionEvent(ExecutionEvent event) override
    {
        if (!pending_request_ || !event.terminal)
            return;
        const program::ProgramActionRequest request =
            std::move(*pending_request_);
        pending_request_.reset();
        completions_.push_back(program::ActorActionResult{{
            .request_id = request.request_id,
            .invocation_id = request.invocation_id,
            .attempt_id = request.attempt_id,
            .operation = request.operation,
            .status =
                program::ProgramActionResolutionStatus::Completed,
            .workset_epoch = event.terminal->workset_epoch,
            .output = {
                program::ProgramValueId(1),
                {program::ProgramValue{
                    program::ProgramValueId(1),
                    program::TypeRef::Builtin(
                        program::BuiltinType::Unit),
                    program::UnitValue{}}}},
            .cleanup = program::ProgramCleanupStatus::Clean,
            .session_disposition = SessionDisposition::Clean,
        }, {}});
    }
    void Pump() override {}
    std::vector<program::ActorActionResult>
    DrainResults() override
    {
        return std::exchange(
            completions_,
            std::vector<program::ActorActionResult>{});
    }
    std::vector<program::ForegroundSemanticStopObservationV1>
    DrainForegroundSemanticStops() override
    {
        return {};
    }
    void Shutdown() noexcept override {}

private:
    std::shared_ptr<FakeProgramRuntimeControl> control_;
    std::optional<program::ProgramActionRequest> pending_request_;
    std::vector<program::ActorActionResult> completions_;
};

class WorkerEventLog
{
public:
    void ThrowNextWorksetTerminal()
    {
        throw_next_workset_terminal_.store(
            true,
            std::memory_order_release);
    }

    void Record(const WorkerEvent& event)
    {
        if (std::holds_alternative<
                WorkerWorksetItemTerminalEvent>(event) &&
            throw_next_workset_terminal_.exchange(
                false,
                std::memory_order_acq_rel))
        {
            throw std::runtime_error(
                "injected terminal publisher failure");
        }
        std::lock_guard lock(mutex_);
        events_.push_back(event);
        changed_.notify_all();
    }

    [[nodiscard]] bool WaitForTerminalCount(std::size_t count)
    {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, 5s, [&] {
            return TerminalCountLocked() >= count;
        });
    }

    [[nodiscard]] bool WaitForCommandCount(
        WorkerCommandKind kind,
        std::size_t count)
    {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, 5s, [&] {
            return CommandResultsLocked(kind).size() >= count;
        });
    }

    [[nodiscard]] bool WaitForExecutionTerminalCount(std::size_t count)
    {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, 5s, [&] {
            return ExecutionTerminalCountLocked() >= count;
        });
    }

    [[nodiscard]] bool WaitForExecutionHealthWarningCount(
        std::size_t count)
    {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, 5s, [&] {
            return static_cast<std::size_t>(std::count_if(
                events_.begin(),
                events_.end(),
                [](const WorkerEvent& event) {
                    const auto* execution =
                        std::get_if<WorkerExecutionEvent>(&event);
                    return execution &&
                        execution->event.health_warning.has_value();
                })) >= count;
        });
    }

    [[nodiscard]] bool WaitForWorksetTerminalCount(
        std::size_t count)
    {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, 5s, [&] {
            return WorksetTerminalsLocked().size() >= count;
        });
    }

    [[nodiscard]] bool WaitForWorksetSummaryCount(
        std::size_t count)
    {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, 5s, [&] {
            return WorksetSummariesLocked().size() >= count;
        });
    }

    [[nodiscard]] std::size_t TerminalCount() const
    {
        std::lock_guard lock(mutex_);
        return TerminalCountLocked();
    }

    [[nodiscard]] std::vector<ProgramInvocationTerminalEvent> Terminals() const
    {
        std::lock_guard lock(mutex_);
        std::vector<ProgramInvocationTerminalEvent> terminals;
        for (const WorkerEvent& event : events_)
        {
            if (const auto* terminal =
                    std::get_if<WorkerWorksetItemTerminalEvent>(&event))
            {
                terminals.push_back(terminal->terminal);
            }
        }
        return terminals;
    }

    [[nodiscard]] std::vector<ExecutionTerminalResult>
    ExecutionTerminals() const
    {
        std::lock_guard lock(mutex_);
        std::vector<ExecutionTerminalResult> terminals;
        for (const WorkerEvent& event : events_)
        {
            const auto* execution =
                std::get_if<WorkerExecutionEvent>(&event);
            if (execution && execution->event.terminal)
                terminals.push_back(*execution->event.terminal);
        }
        return terminals;
    }

    [[nodiscard]] std::vector<ExecutionHealthWarning>
    ExecutionHealthWarnings() const
    {
        std::lock_guard lock(mutex_);
        std::vector<ExecutionHealthWarning> warnings;
        for (const WorkerEvent& event : events_)
        {
            const auto* execution =
                std::get_if<WorkerExecutionEvent>(&event);
            if (execution && execution->event.health_warning)
            {
                warnings.push_back(
                    *execution->event.health_warning);
            }
        }
        return warnings;
    }

    [[nodiscard]] std::vector<WorkerWorksetItemTerminalEvent>
    WorksetTerminals() const
    {
        std::lock_guard lock(mutex_);
        return WorksetTerminalsLocked();
    }

    [[nodiscard]] std::vector<WorkerWorksetTerminalSummaryEvent>
    WorksetSummaries() const
    {
        std::lock_guard lock(mutex_);
        return WorksetSummariesLocked();
    }

    [[nodiscard]] std::vector<WorkerCommandResult> CommandResults(
        WorkerCommandKind kind) const
    {
        std::lock_guard lock(mutex_);
        return CommandResultsLocked(kind);
    }

    [[nodiscard]] std::vector<WorkerEvent> Events() const
    {
        std::lock_guard lock(mutex_);
        return events_;
    }

private:
    [[nodiscard]] std::size_t TerminalCountLocked() const
    {
        return WorksetTerminalsLocked().size();
    }

    [[nodiscard]] std::size_t ExecutionTerminalCountLocked() const
    {
        return static_cast<std::size_t>(std::count_if(
            events_.begin(),
            events_.end(),
            [](const WorkerEvent& event) {
                const auto* execution =
                    std::get_if<WorkerExecutionEvent>(&event);
                return execution && execution->event.terminal.has_value();
            }));
    }

    [[nodiscard]] std::vector<WorkerCommandResult> CommandResultsLocked(
        WorkerCommandKind kind) const
    {
        std::vector<WorkerCommandResult> results;
        for (const WorkerEvent& event : events_)
        {
            if (const auto* completed =
                    std::get_if<WorkerCommandCompletedEvent>(&event);
                completed && completed->result.command_kind == kind)
            {
                results.push_back(completed->result);
            }
        }
        return results;
    }

    [[nodiscard]] std::vector<WorkerWorksetItemTerminalEvent>
    WorksetTerminalsLocked() const
    {
        std::vector<WorkerWorksetItemTerminalEvent> terminals;
        for (const WorkerEvent& event : events_)
        {
            if (const auto* terminal =
                    std::get_if<
                        WorkerWorksetItemTerminalEvent>(&event))
            {
                terminals.push_back(*terminal);
            }
        }
        return terminals;
    }

    [[nodiscard]] std::vector<WorkerWorksetTerminalSummaryEvent>
    WorksetSummariesLocked() const
    {
        std::vector<WorkerWorksetTerminalSummaryEvent> summaries;
        for (const WorkerEvent& event : events_)
        {
            if (const auto* summary =
                    std::get_if<WorkerWorksetTerminalSummaryEvent>(
                        &event))
            {
                summaries.push_back(*summary);
            }
        }
        return summaries;
    }

    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::vector<WorkerEvent> events_;
    std::atomic<bool> throw_next_workset_terminal_{false};
};

struct RuntimeHarness
{
    SessionId session_id{77};
    TemporaryRuntimeDirectory baseline_files;
    std::filesystem::path baseline_path =
        baseline_files.File("baseline.sav");
    std::shared_ptr<ScriptedDolphinBackendControl> backend =
        std::make_shared<ScriptedDolphinBackendControl>();
    std::shared_ptr<FakeProgramRuntimeControl> program =
        std::make_shared<FakeProgramRuntimeControl>();
    WorkerEventLog events;
    std::unique_ptr<WorkerRuntime> runtime;
    std::uint64_t next_request = 1;

    explicit RuntimeHarness(
        std::unique_ptr<IPhysicalStopPointBackendPort> physical_stop_points = {},
        std::shared_ptr<const WorkerRuntimeTestHooks> test_hooks = {},
        bool supports_worksets = true,
        std::shared_ptr<ProgramBaselineComponentRegistry>
            baseline_components = {},
        ExecutionControlCoreConfig execution_control_core_config = {})
    {
        {
            std::ofstream output(baseline_path, std::ios::binary);
            output << "seedprobe-test-baseline";
        }
        (void)supports_worksets;
        program->prepared_state_policy =
            program::InvocationStatePolicy::RestoreBaseline;
        runtime = std::make_unique<WorkerRuntime>(
            std::make_unique<EmulationSession>(
                session_id,
                std::make_unique<ScriptedDolphinBackend>(
                    backend,
                    std::move(physical_stop_points)),
                std::move(execution_control_core_config)),
            std::make_unique<FakeProgramRuntimePort>(program),
            [this](const WorkerEvent& event) { events.Record(event); },
            std::move(test_hooks),
            std::make_unique<FakeProgramActionHost>(program),
            std::move(baseline_components));
    }

    [[nodiscard]] WorkerWorksetDefinition Workset(
        std::uint64_t workset_id,
        std::uint32_t item_count) const
    {
        WorkerWorksetDefinition definition;
        definition.workset_id = WorkerWorksetId(workset_id);
        const auto phase =
            seedprobe::SeedProbeFullPhaseDefinitionV2();
        definition.phase_invocation = {
            .invocation_id = {1, workset_id},
            .program_package =
                fullphase::BuildFullPhaseProgramPackage(*phase),
            .common_input = fullphase::MakeFullPhaseCommonInput(
                "soa.seed_probe.CommonInput", 1),
        };
        definition.baseline.artifact = ProgramBaselineArtifact{
            .kind = ProgramBaselineArtifactKind::Savestate,
            .state_path = baseline_path,
            .state_sha256 = hash::sha256_of_file(
                baseline_path.string()),
            .compatibility = {
                .game_id = "TEST00",
                .iso_sha256 = std::string(64, '0'),
                .emulator_build = "scripted-dolphin-backend",
                .runtime_revision = "slice4",
            },
            .lineage = {
                .edge = "test-fixture",
                .producer = "RuntimeHarness",
            },
        };
        definition.baseline.lineage =
            phase->runtime_contract().baseline_lineage;
        definition.execution_key.module =
            phase->runtime_contract().module;
        definition.execution_key.entrypoint =
            phase->runtime_contract().entrypoint;
        definition.execution_key.verified_dependency_sha256 =
            phase->runtime_contract().verified_dependency_sha256;
        definition.execution_key.runtime_profile_sha256 =
            phase->runtime_contract().runtime_profile_sha256;
        definition.execution_key.baseline =
            ComputeProgramBaselineKey(definition.baseline);
        definition.execution_key.movie_policy_sha256 =
            phase->runtime_contract().movie_policy_sha256;
        definition.execution_key.service_policy_sha256 =
            phase->runtime_contract().service_policy_sha256;
        definition.execution_key.program_package_sha256 =
            definition.phase_invocation.program_package.canonical_sha256;
        definition.execution_key.common_input_sha256 =
            definition.phase_invocation.common_input.content_sha256;
        definition.execution_key.derived_state_binding_sha256 =
            definition.derived_state.content_sha256;
        definition.execution_key.capture_binding_sha256 =
            EmptyWorksetCaptureBindingHashV1();
        definition.execution_key.progress_plan_sha256 =
            definition.progress_plan.content_sha256;
        definition.execution_key.canonical_sha256 =
            ComputeWorkerWorksetExecutionKeyHash(
                definition.execution_key);
        for (std::uint32_t ordinal = 0;
             ordinal < item_count;
             ++ordinal)
        {
            WorksetItemTemplate item;
            item.item_id =
                WorkerWorksetItemId(ordinal + 1);
            item.ordinal = ordinal;
            item.execution.execution_id =
                ProgramExecutionId(workset_id * 100 + ordinal + 1);
            item.execution.attempt_id = AttemptId(1);
            item.execution.input_payload =
                seedprobe::EncodeSeedProbeExecutionInputV2(
                    {savor::GCInputFrame{}});
            item.correlation.durable_job_id =
                "job-" + std::to_string(ordinal + 1);
            item.correlation.claim_token =
                "claim-" + std::to_string(ordinal + 1);
            definition.items.push_back(std::move(item));
        }
        definition.encoded_size_bytes = 128;
        return definition;
    }

    [[nodiscard]] WireRequestId NextRequest()
    {
        return WireRequestId(next_request++);
    }

    [[nodiscard]] SessionOpenOptions OpenOptions() const
    {
        SessionOpenOptions options;
        options.worker_mode = WorkerMode::VisualDebug;
        options.backend.runtime_root = "fake-runtime";
        options.backend.user_directory = "fake-user";
        options.backend.dolphin_base_directory = "fake-dolphin";
        options.backend.iso_path = "fake.iso";
        return options;
    }

    [[nodiscard]] WorkerCommandResult Open(
        std::optional<SessionOpenOptions> options = std::nullopt)
    {
        return runtime->Submit(
            NextRequest(),
            OpenSessionCommand{options.value_or(OpenOptions())}).get();
    }

    [[nodiscard]] EncodedInvocationEnvelope Invocation(
        std::uint64_t invocation_id,
        std::uint64_t attempt_id = 1) const
    {
        EncodedInvocationEnvelope invocation;
        invocation.invocation_id = InvocationId(invocation_id);
        invocation.attempt_id = AttemptId(attempt_id);
        invocation.module.canonical_id = "test.module/1";
        invocation.module.revision = 1;
        invocation.module.canonical_hash = "test-hash";
        invocation.entrypoint = "main";
        invocation.expected_workset_epoch =
            runtime->snapshot().session.workset_epoch;
        invocation.input_payload = {0x01, 0x02};
        return invocation;
    }

    [[nodiscard]] WorkerCommandResult Invoke(
        std::uint64_t invocation_id,
        std::uint64_t attempt_id = 1,
        std::uint64_t workset_id = 0)
    {
        WorkerWorksetDefinition workset = Workset(
            workset_id == 0 ? invocation_id : workset_id,
            1);
        workset.items.front().execution.execution_id =
            ProgramExecutionId(invocation_id);
        workset.items.front().execution.attempt_id = AttemptId(attempt_id);
        {
            std::lock_guard lock(program->mutex);
            program->terminal_output_payload =
                EncodedCompletedProgramResult(
                    workset.items.front().execution.execution_id,
                    workset.items.front().execution.attempt_id);
        }
        return runtime->Submit(
            NextRequest(),
            SubmitWithoutInitialCancellations(workset)).get();
    }

    [[nodiscard]] WorkerCommandResult Shutdown()
    {
        auto future = runtime->Submit(NextRequest(), ShutdownCommand{});
        WorkerCommandResult result = future.get();
        runtime->WaitStopped();
        return result;
    }
};

[[nodiscard]] bool CompleteAndAcknowledgeActiveWorkset(
    RuntimeHarness& harness,
    InvocationTerminalStatus status = InvocationTerminalStatus::Completed)
{
    const std::size_t before = harness.events.WorksetTerminals().size();
    if (!harness.program->EmitTerminal(status) ||
        !harness.events.WaitForWorksetTerminalCount(before + 1))
    {
        return false;
    }
    const auto terminals = harness.events.WorksetTerminals();
    if (terminals.size() <= before)
        return false;
    const WorkerCommandResult acknowledged = harness.runtime->Submit(
        harness.NextRequest(),
        AcknowledgeTerminalCommand{terminals[before].correlation}).get();
    return acknowledged.outcome == WorkerCommandOutcome::Completed;
}

[[nodiscard]] bool AcknowledgeLatestWorksetTerminal(
    RuntimeHarness& harness)
{
    const auto terminals = harness.events.WorksetTerminals();
    if (terminals.empty())
        return false;
    const WorkerCommandResult acknowledged = harness.runtime->Submit(
        harness.NextRequest(),
        AcknowledgeTerminalCommand{terminals.back().correlation}).get();
    return acknowledged.outcome == WorkerCommandOutcome::Completed;
}

TEST(
    ExecutionWorkerRuntime,
    AdmissionInitializationReadyAndRunningAreDistinctOrderedBoundaries)
{
    RuntimeHarness harness;
    ASSERT_EQ(harness.Open().outcome, WorkerCommandOutcome::Completed);
    ASSERT_EQ(harness.Invoke(440).outcome, WorkerCommandOutcome::Accepted);
    ASSERT_TRUE(harness.program->WaitForStarts(1));

    const std::vector<WorkerEvent> events = harness.events.Events();
    const auto find_index = [&](const auto& predicate) {
        for (std::size_t index = 0; index < events.size(); ++index)
        {
            if (predicate(events[index]))
                return std::optional<std::size_t>(index);
        }
        return std::optional<std::size_t>{};
    };
    const auto admission = find_index([](const WorkerEvent& event) {
        const auto* completed =
            std::get_if<WorkerCommandCompletedEvent>(&event);
        return completed &&
            completed->result.command_kind ==
                WorkerCommandKind::SubmitWorkset &&
            completed->result.outcome == WorkerCommandOutcome::Accepted;
    });
    const auto workset_state = [&](WorkerWorksetState expected) {
        return find_index([expected](const WorkerEvent& event) {
            const auto* state =
                std::get_if<WorkerWorksetStateEvent>(&event);
            return state && state->state == expected;
        });
    };
    const auto admitted = workset_state(WorkerWorksetState::Admitted);
    const auto initializing = workset_state(
        WorkerWorksetState::Initializing);
    const auto ready = workset_state(WorkerWorksetState::Ready);
    const auto running = workset_state(WorkerWorksetState::Running);
    const auto item_started = find_index([](const WorkerEvent& event) {
        return std::holds_alternative<
            WorkerWorksetItemStartedEvent>(event);
    });

    ASSERT_TRUE(admission);
    ASSERT_TRUE(admitted);
    ASSERT_TRUE(initializing);
    ASSERT_TRUE(ready);
    ASSERT_TRUE(running);
    ASSERT_TRUE(item_started);
    EXPECT_LT(*admission, *admitted);
    EXPECT_LT(*admitted, *initializing);
    EXPECT_LT(*initializing, *ready);
    EXPECT_LT(*ready, *running);
    EXPECT_LT(*running, *item_started);

    const auto ready_worker = find_index([](const WorkerEvent& event) {
        const auto* state = std::get_if<WorkerStateChangedEvent>(&event);
        return state && state->current.state == WorkerState::Ready &&
            state->current.resident_workset_state ==
                WorkerWorksetState::Ready;
    });
    const auto running_worker = find_index([](const WorkerEvent& event) {
        const auto* state = std::get_if<WorkerStateChangedEvent>(&event);
        return state && state->current.state == WorkerState::Running &&
            state->current.resident_workset_state ==
                WorkerWorksetState::Running;
    });
    ASSERT_TRUE(ready_worker);
    ASSERT_TRUE(running_worker);
    const auto& ready_event = std::get<WorkerStateChangedEvent>(
        events[*ready_worker]);
    const auto& running_event = std::get<WorkerStateChangedEvent>(
        events[*running_worker]);
    EXPECT_FALSE(ready_event.current.execution);
    EXPECT_TRUE(running_event.current.execution);

    ASSERT_TRUE(CompleteAndAcknowledgeActiveWorkset(harness));
    EXPECT_EQ(harness.Shutdown().outcome, WorkerCommandOutcome::Completed);
}

TEST(
    ExecutionWorkerRuntime,
    FinalizesMultipleStateArtifactsAndReleasesServiceOwnershipBetweenWorksets)
{
    TemporaryRuntimeDirectory temporary;
    EmulationSession* actor_session = nullptr;
    auto hooks = std::make_shared<WorkerRuntimeTestHooks>();
    hooks->session_opened = [&](EmulationSession& session) {
        actor_session = &session;
    };
    RuntimeHarness harness({}, hooks, true);
    ASSERT_EQ(
        harness.Open().outcome,
        WorkerCommandOutcome::Completed);
    ASSERT_NE(actor_session, nullptr);

    {
        std::lock_guard lock(harness.program->mutex);
        harness.program->emit_action_on_start = true;
        harness.program
            ->complete_invocation_on_action_completion = true;
        harness.program->pending_state_artifact_factory =
            [&]() {
                std::vector<program::StagedProgramOutput>
                    publications;
                const auto capture =
                    [&](std::string artifact_id,
                        const std::filesystem::path& path)
                    {
                        ImmutableSavestateArtifactCaptureReceipt
                            receipt =
                                actor_session
                                    ->CaptureImmutableSavestateArtifact({
                                        .path = path,
                                        .lineage = {
                                            .edge =
                                                "test-output",
                                            .producer =
                                                "worker-runtime-test"},
                                    });
                        if (!receipt.result.ok)
                        {
                            throw std::runtime_error(
                                "test state capture failed: " +
                                receipt.result.message);
                        }
                        publications.emplace_back(
                            program::StagedSavestateOutput{
                                std::move(artifact_id),
                                std::move(receipt)});
                    };
                capture(
                    "test-state-a",
                    temporary.File("state-a.sav"));
                capture(
                    "test-state-b",
                    temporary.File("state-b.sav"));
                return publications;
            };
    }

    WorkerWorksetDefinition first =
        harness.Workset(82, 1);
    {
        std::lock_guard lock(harness.program->mutex);
        harness.program->terminal_output_payload =
            EncodedCompletedProgramResult(
                first.items.front()
                    .execution.execution_id,
                first.items.front()
                    .execution.attempt_id);
    }
    ASSERT_EQ(
        harness.runtime
            ->Submit(
                harness.NextRequest(),
                SubmitWithoutInitialCancellations(first))
            .get()
            .outcome,
        WorkerCommandOutcome::Accepted);
    ASSERT_TRUE(
        harness.events.WaitForWorksetTerminalCount(1));

    auto terminals = harness.events.WorksetTerminals();
    ASSERT_EQ(terminals.size(), 1u);
    auto first_result =
        program::DecodeProgramResultV1(
            terminals.front().terminal.output_payload);
    ASSERT_TRUE(first_result)
        << first_result.status.message;
    ASSERT_EQ(first_result.value->artifacts.size(), 2u);
    EXPECT_EQ(
        first_result.value->artifacts[0]
            .artifact.artifact_id,
        "test-state-a");
    EXPECT_EQ(
        first_result.value->artifacts[1]
            .artifact.artifact_id,
        "test-state-b");
    EXPECT_TRUE(
        first_result.value->artifacts[0]
            .artifact.complete);
    EXPECT_TRUE(
        first_result.value->artifacts[1]
            .artifact.complete);
    EXPECT_TRUE(
        std::filesystem::exists(
            temporary.File("state-a.sav")));
    EXPECT_TRUE(
        std::filesystem::exists(
            temporary.File("state-b.sav")));
    ASSERT_EQ(
        harness.runtime
            ->Submit(
                harness.NextRequest(),
                AcknowledgeTerminalCommand{
                    terminals.front().correlation})
            .get()
            .outcome,
        WorkerCommandOutcome::Completed);

    // Reusing the exact output paths proves WorkerRuntime released the
    // committed SavestateService records after assembling the authoritative
    // references. The finalizer should validate and reuse the immutable
    // files rather than treating them as still session-owned.
    WorkerWorksetDefinition second =
        harness.Workset(83, 1);
    {
        std::lock_guard lock(harness.program->mutex);
        harness.program->terminal_output_payload =
            EncodedCompletedProgramResult(
                second.items.front()
                    .execution.execution_id,
                second.items.front()
                    .execution.attempt_id);
    }
    ASSERT_EQ(
        harness.runtime
            ->Submit(
                harness.NextRequest(),
                SubmitWithoutInitialCancellations(second))
            .get()
            .outcome,
        WorkerCommandOutcome::Accepted);
    ASSERT_TRUE(
        harness.events.WaitForWorksetTerminalCount(2));
    terminals = harness.events.WorksetTerminals();
    ASSERT_EQ(terminals.size(), 2u);
    auto second_result =
        program::DecodeProgramResultV1(
            terminals.back().terminal.output_payload);
    ASSERT_TRUE(second_result)
        << second_result.status.message;
    ASSERT_EQ(second_result.value->artifacts.size(), 2u);
    EXPECT_EQ(
        second_result.value->artifacts[0]
            .artifact.artifact_id,
        "test-state-a");
    EXPECT_EQ(
        second_result.value->artifacts[1]
            .artifact.artifact_id,
        "test-state-b");
    ASSERT_EQ(
        harness.runtime
            ->Submit(
                harness.NextRequest(),
                AcknowledgeTerminalCommand{
                    terminals.back().correlation})
            .get()
            .outcome,
        WorkerCommandOutcome::Completed);
    EXPECT_EQ(
        harness.runtime->snapshot()
            .retained_terminal_count,
        0u);
    EXPECT_EQ(
        harness.Shutdown().outcome,
        WorkerCommandOutcome::Completed);
}

TEST(
    ExecutionWorkerRuntime,
    FinalizesMovieBackedStateAtItsSavestateSidecarBeforeTerminal)
{
    TemporaryRuntimeDirectory temporary;
    EmulationSession* actor_session = nullptr;
    auto hooks = std::make_shared<WorkerRuntimeTestHooks>();
    hooks->session_opened = [&](EmulationSession& session) {
        actor_session = &session;
    };
    RuntimeHarness harness({}, hooks, true);
    ASSERT_EQ(
        harness.Open().outcome,
        WorkerCommandOutcome::Completed);
    ASSERT_NE(actor_session, nullptr);

    std::vector<std::uint8_t> dtm_bytes(256, 0);
    const std::array<std::uint8_t, 4> magic{
        'D', 'T', 'M', 0x1a};
    std::ranges::copy(magic, dtm_bytes.begin());
    const std::string game_id = "TEST00";
    std::ranges::copy(game_id, dtm_bytes.begin() + 4);
    const std::string expected_dtm_sha256 =
        hash::sha256(dtm_bytes.data(), dtm_bytes.size());
    const std::filesystem::path state_path =
        temporary.File("movie-checkpoint.sav");
    const std::filesystem::path sidecar_path =
        SavestateDtmSidecarPath(state_path);

    {
        std::lock_guard lock(harness.program->mutex);
        harness.program->emit_action_on_start = true;
        harness.program
            ->complete_invocation_on_action_completion = true;
        harness.program->pending_state_artifact_factory =
            [&, dtm_bytes]() mutable {
                ImmutableSavestateArtifactCaptureReceipt receipt =
                    actor_session->CaptureImmutableSavestateArtifact({
                        .path = state_path,
                        .movie = MovieCheckpointMetadata{
                            .mode =
                                MovieCheckpointMode::ReadOnlyPlayback,
                            .dtm_sha256 =
                                expected_dtm_sha256,
                            .game_id = "TEST00",
                            .dtm_bytes = std::move(dtm_bytes),
                            // This is the active movie's source identity,
                            // not the output sidecar destination.
                            .dtm_path =
                                temporary.File("source-movie.dtm"),
                        },
                        .lineage = {
                            .edge = "test-movie-output",
                            .producer = "worker-runtime-test"},
                    });
                if (!receipt.result.ok)
                {
                    throw std::runtime_error(
                        "test movie state capture failed: " +
                        receipt.result.message);
                }
                std::vector<program::StagedProgramOutput> outputs;
                outputs.emplace_back(
                    program::StagedSavestateOutput{
                        "test-movie-state",
                        std::move(receipt)});
                return outputs;
            };
    }

    WorkerWorksetDefinition workset = harness.Workset(89, 1);
    {
        std::lock_guard lock(harness.program->mutex);
        harness.program->terminal_output_payload =
            EncodedCompletedProgramResult(
                workset.items.front().execution.execution_id,
                workset.items.front().execution.attempt_id);
    }
    ASSERT_EQ(
        harness.runtime
            ->Submit(
                harness.NextRequest(),
                SubmitWithoutInitialCancellations(workset))
            .get()
            .outcome,
        WorkerCommandOutcome::Accepted);
    ASSERT_TRUE(
        harness.events.WaitForWorksetTerminalCount(1));
    const auto terminal =
        harness.events.WorksetTerminals().front();
    ASSERT_EQ(
        terminal.terminal.status,
        InvocationTerminalStatus::Completed)
        << terminal.terminal.error.message;
    const auto result = program::DecodeProgramResultV1(
        terminal.terminal.output_payload);
    ASSERT_TRUE(result) << result.status.message;
    ASSERT_EQ(result.value->artifacts.size(), 1u);
    EXPECT_EQ(
        result.value->artifacts.front().artifact.artifact_id,
        "test-movie-state");
    EXPECT_TRUE(std::filesystem::is_regular_file(state_path));
    EXPECT_TRUE(std::filesystem::is_regular_file(sidecar_path));
    EXPECT_EQ(
        hash::sha256_of_file(sidecar_path.string()),
        expected_dtm_sha256);
    EXPECT_FALSE(std::filesystem::exists(
        temporary.File("source-movie.dtm")));

    ASSERT_EQ(
        harness.runtime
            ->Submit(
                harness.NextRequest(),
                AcknowledgeTerminalCommand{
                    terminal.correlation})
            .get()
            .outcome,
        WorkerCommandOutcome::Completed);
    EXPECT_EQ(
        harness.Shutdown().outcome,
        WorkerCommandOutcome::Completed);
}

TEST(
    ExecutionWorkerRuntime,
    DelayedOutputFinalizerBlocksTheNextWorksetItem)
{
    TemporaryRuntimeDirectory temporary;
    EmulationSession* actor_session = nullptr;
    std::promise<void> finalizer_entered_promise;
    std::future<void> finalizer_entered =
        finalizer_entered_promise.get_future();
    std::promise<void> release_finalizer_promise;
    std::shared_future<void> release_finalizer =
        release_finalizer_promise.get_future().share();
    std::atomic<bool> announced{false};
    auto hooks = std::make_shared<WorkerRuntimeTestHooks>();
    hooks->session_opened = [&](EmulationSession& session) {
        actor_session = &session;
    };
    hooks->before_output_finalization = [&] {
        if (!announced.exchange(true))
            finalizer_entered_promise.set_value();
        release_finalizer.wait();
    };

    RuntimeHarness harness({}, hooks, true);
    ASSERT_EQ(
        harness.Open().outcome,
        WorkerCommandOutcome::Completed);
    ASSERT_NE(actor_session, nullptr);
    WorkerWorksetDefinition workset =
        harness.Workset(87, 2);
    std::atomic<std::uint32_t> captures{0};
    {
        std::lock_guard lock(harness.program->mutex);
        harness.program->emit_action_on_start = true;
        harness.program
            ->complete_invocation_on_action_completion = true;
        harness.program->terminal_output_payload =
            EncodedCompletedProgramResult(
                workset.items.front().execution.execution_id,
                workset.items.front().execution.attempt_id);
        harness.program->pending_state_artifact_factory =
            [&]() {
                std::vector<program::StagedProgramOutput> outputs;
                if (captures.fetch_add(1) != 0)
                    return outputs;
                ImmutableSavestateArtifactCaptureReceipt receipt =
                    actor_session->CaptureImmutableSavestateArtifact({
                        .path = temporary.File("delayed-finalizer.sav"),
                        .lineage = {
                            .edge = "test-delayed-finalizer",
                            .producer = "worker-runtime-test"},
                    });
                if (!receipt.result.ok)
                {
                    throw std::runtime_error(
                        "test state capture failed: " +
                        receipt.result.message);
                }
                outputs.emplace_back(
                    program::StagedSavestateOutput{
                        "delayed-finalizer-state",
                        std::move(receipt)});
                return outputs;
            };
    }

    ASSERT_EQ(
        harness.runtime
            ->Submit(
                harness.NextRequest(),
                SubmitWithoutInitialCancellations(workset))
            .get()
            .outcome,
        WorkerCommandOutcome::Accepted);
    const bool entered =
        finalizer_entered.wait_for(5s) ==
        std::future_status::ready;
    EXPECT_TRUE(entered);
    EXPECT_EQ(harness.program->StartCount(), 1);
    EXPECT_EQ(harness.events.WorksetTerminals().size(), 0u);
    EXPECT_TRUE(
        harness.runtime->snapshot().active_workset_item.has_value());

    release_finalizer_promise.set_value();
    ASSERT_TRUE(harness.program->WaitForStarts(2));
    ASSERT_TRUE(
        harness.events.WaitForWorksetTerminalCount(2));
    EXPECT_TRUE(std::filesystem::exists(
        temporary.File("delayed-finalizer.sav")));
    const auto terminals =
        harness.events.WorksetTerminals();
    ASSERT_EQ(terminals.size(), 2u);
    EXPECT_LT(
        terminals[0].correlation.terminal_order,
        terminals[1].correlation.terminal_order);
    for (const auto& terminal : terminals)
    {
        EXPECT_EQ(
            harness.runtime
                ->Submit(
                    harness.NextRequest(),
                    AcknowledgeTerminalCommand{
                        terminal.correlation})
                .get()
                .outcome,
            WorkerCommandOutcome::Completed);
    }
    EXPECT_EQ(
        harness.Shutdown().outcome,
        WorkerCommandOutcome::Completed);
}

TEST(
    ExecutionWorkerRuntime,
    RejectsStagedOutputsBeyondVerifiedAllowanceAndAbandonsCapture)
{
    TemporaryRuntimeDirectory temporary;
    EmulationSession* actor_session = nullptr;
    auto hooks = std::make_shared<WorkerRuntimeTestHooks>();
    hooks->session_opened = [&](EmulationSession& session) {
        actor_session = &session;
    };
    RuntimeHarness harness({}, hooks, true);
    ASSERT_EQ(
        harness.Open().outcome,
        WorkerCommandOutcome::Completed);
    ASSERT_NE(actor_session, nullptr);

    const std::filesystem::path output =
        temporary.File("allowance-rejected.sav");
    {
        std::lock_guard lock(harness.program->mutex);
        harness.program->maximum_artifacts = 0;
        harness.program->emit_action_on_start = true;
        harness.program->pending_state_artifact_factory =
            [&]() {
                ImmutableSavestateArtifactCaptureReceipt receipt =
                    actor_session->CaptureImmutableSavestateArtifact({
                        .path = output,
                        .lineage = {
                            .edge = "test-output-allowance",
                            .producer = "worker-runtime-test"},
                    });
                if (!receipt.result.ok)
                {
                    throw std::runtime_error(
                        "test state capture failed: " +
                        receipt.result.message);
                }
                std::vector<program::StagedProgramOutput> outputs;
                outputs.emplace_back(
                    program::StagedSavestateOutput{
                        "allowance-rejected-state",
                        std::move(receipt)});
                return outputs;
            };
    }

    const WorkerWorksetDefinition workset =
        harness.Workset(86, 1);
    ASSERT_EQ(
        harness.runtime
            ->Submit(
                harness.NextRequest(),
                SubmitWithoutInitialCancellations(workset))
            .get()
            .outcome,
        WorkerCommandOutcome::Accepted);
    ASSERT_TRUE(
        harness.program->WaitForActionCompletions(1));
    {
        std::lock_guard lock(harness.program->mutex);
        EXPECT_EQ(
            harness.program->last_action_resolution_status,
            program::ProgramActionResolutionStatus::Failed);
        EXPECT_EQ(
            harness.program->last_action_resolution_code,
            "staged_output_adoption_failed");
    }
    EXPECT_FALSE(std::filesystem::exists(output));

    ASSERT_TRUE(harness.program->EmitTerminal(
        InvocationTerminalStatus::InfrastructureFailure,
        CleanupStatus::Clean,
        SessionDisposition::Clean,
        std::nullopt,
        {WorkerRejectionCode::BackendFailure,
         "program observed rejected output adoption"}));
    ASSERT_TRUE(
        harness.events.WaitForWorksetTerminalCount(1));
    const auto terminal =
        harness.events.WorksetTerminals().front();
    EXPECT_EQ(
        terminal.terminal.status,
        InvocationTerminalStatus::InfrastructureFailure);
    EXPECT_FALSE(std::filesystem::exists(output));
    ASSERT_EQ(
        harness.runtime
            ->Submit(
                harness.NextRequest(),
                AcknowledgeTerminalCommand{
                    terminal.correlation})
            .get()
            .outcome,
        WorkerCommandOutcome::Completed);
    EXPECT_EQ(
        harness.Shutdown().outcome,
        WorkerCommandOutcome::Completed);
}

TEST(
    ExecutionWorkerRuntime,
    DefersCapturedStateUntilSuccessfulTerminalAndAbandonsItOnTailFailure)
{
    TemporaryRuntimeDirectory temporary;
    EmulationSession* actor_session = nullptr;
    auto hooks = std::make_shared<WorkerRuntimeTestHooks>();
    hooks->session_opened = [&](EmulationSession& session) {
        actor_session = &session;
    };
    RuntimeHarness harness({}, hooks, true);
    ASSERT_EQ(
        harness.Open().outcome,
        WorkerCommandOutcome::Completed);
    ASSERT_NE(actor_session, nullptr);

    const std::filesystem::path output =
        temporary.File("deferred-tail-failure.sav");
    WorkerWorksetDefinition workset =
        harness.Workset(85, 1);
    {
        std::lock_guard lock(harness.program->mutex);
        harness.program->emit_action_on_start = true;
        harness.program->pending_state_artifact_factory =
            [&]() {
                ImmutableSavestateArtifactCaptureReceipt receipt =
                    actor_session->CaptureImmutableSavestateArtifact({
                        .path = output,
                        .lineage = {
                            .edge = "test-deferred-tail",
                            .producer = "worker-runtime-test"},
                    });
                if (!receipt.result.ok)
                {
                    throw std::runtime_error(
                        "test state capture failed: " +
                        receipt.result.message);
                }
                std::vector<program::StagedProgramOutput>
                    publications;
                publications.emplace_back(
                    program::StagedSavestateOutput{
                        "test-deferred-tail-state",
                        std::move(receipt)});
                return publications;
            };
    }

    ASSERT_EQ(
        harness.runtime
            ->Submit(
                harness.NextRequest(),
                SubmitWithoutInitialCancellations(workset))
            .get()
            .outcome,
        WorkerCommandOutcome::Accepted);
    ASSERT_TRUE(
        harness.program->WaitForActionCompletions(1));
    EXPECT_FALSE(std::filesystem::exists(output));

    ASSERT_TRUE(harness.program->EmitTerminal(
        InvocationTerminalStatus::InfrastructureFailure,
        CleanupStatus::Clean,
        SessionDisposition::Clean,
        std::nullopt,
        {WorkerRejectionCode::BackendFailure,
         "tail playback failed"}));
    ASSERT_TRUE(
        harness.events.WaitForWorksetTerminalCount(1));
    const auto terminal =
        harness.events.WorksetTerminals().front();
    EXPECT_EQ(
        terminal.terminal.status,
        InvocationTerminalStatus::InfrastructureFailure);
    EXPECT_FALSE(std::filesystem::exists(output));

    ASSERT_EQ(
        harness.runtime
            ->Submit(
                harness.NextRequest(),
                AcknowledgeTerminalCommand{
                    terminal.correlation})
            .get()
            .outcome,
        WorkerCommandOutcome::Completed);

    // Reusing the exact path from a second actor-owned action proves the
    // failed invocation abandoned its pending SavestateService ownership rather
    // than merely leaving it unpublished.
    WorkerWorksetDefinition retry =
        harness.Workset(89, 1);
    {
        std::lock_guard lock(harness.program->mutex);
        harness.program
            ->complete_invocation_on_action_completion = true;
        harness.program->terminal_output_payload =
            EncodedCompletedProgramResult(
                retry.items.front()
                    .execution.execution_id,
                retry.items.front()
                    .execution.attempt_id);
    }
    ASSERT_EQ(
        harness.runtime
            ->Submit(
                harness.NextRequest(),
                SubmitWithoutInitialCancellations(retry))
            .get()
            .outcome,
        WorkerCommandOutcome::Accepted);
    ASSERT_TRUE(
        harness.events.WaitForWorksetTerminalCount(2));
    EXPECT_TRUE(std::filesystem::exists(output));
    const auto retry_terminal =
        harness.events.WorksetTerminals().back();
    EXPECT_EQ(
        retry_terminal.terminal.status,
        InvocationTerminalStatus::Completed);
    ASSERT_EQ(
        harness.runtime
            ->Submit(
                harness.NextRequest(),
                AcknowledgeTerminalCommand{
                    retry_terminal.correlation})
            .get()
            .outcome,
        WorkerCommandOutcome::Completed);
    EXPECT_EQ(
        harness.Shutdown().outcome,
        WorkerCommandOutcome::Completed);
}

TEST(
    ExecutionWorkerRuntime,
    ShutdownDrainsAcceptedArtifactsButRejectsUnacknowledgedTerminal)
{
    TemporaryRuntimeDirectory temporary;
    EmulationSession* actor_session = nullptr;
    auto hooks = std::make_shared<WorkerRuntimeTestHooks>();
    hooks->session_opened = [&](EmulationSession& session) {
        actor_session = &session;
    };
    RuntimeHarness harness({}, hooks, true);
    ASSERT_EQ(
        harness.Open().outcome,
        WorkerCommandOutcome::Completed);
    ASSERT_NE(actor_session, nullptr);

    WorkerWorksetDefinition workset =
        harness.Workset(84, 1);
    {
        std::lock_guard lock(harness.program->mutex);
        harness.program->emit_action_on_start = true;
        harness.program
            ->complete_invocation_on_action_completion = true;
        harness.program->terminal_output_payload =
            EncodedCompletedProgramResult(
                workset.items.front()
                    .execution.execution_id,
                workset.items.front()
                    .execution.attempt_id);
        harness.program->pending_state_artifact_factory =
            [&]() {
                ImmutableSavestateArtifactCaptureReceipt receipt =
                    actor_session->CaptureImmutableSavestateArtifact({
                        .path =
                            temporary.File("shutdown-state.sav"),
                        .lineage = {
                            .edge = "test-shutdown",
                            .producer =
                                "worker-runtime-test"},
                    });
                if (!receipt.result.ok)
                {
                    throw std::runtime_error(
                        "test state capture failed: " +
                        receipt.result.message);
                }
                std::vector<program::StagedProgramOutput>
                    publications;
                publications.emplace_back(
                    program::StagedSavestateOutput{
                        "test-shutdown-state",
                        std::move(receipt)});
                return publications;
            };
    }
    ASSERT_EQ(
        harness.runtime
            ->Submit(
                harness.NextRequest(),
                SubmitWithoutInitialCancellations(workset))
            .get()
            .outcome,
        WorkerCommandOutcome::Accepted);

    // Publication is now intentionally deferred until the invocation has
    // reached a successful, clean terminal. Wait for that authoritative
    // terminal before exercising the unacknowledged-terminal shutdown
    // barrier.
    ASSERT_TRUE(
        harness.events.WaitForWorksetTerminalCount(1));

    // Shutdown cannot honestly report graceful completion while the
    // coordinator has not acknowledged that authoritative result.
    const WorkerCommandResult shutdown =
        harness.Shutdown();
    EXPECT_EQ(
        shutdown.outcome,
        WorkerCommandOutcome::Rejected);
    EXPECT_EQ(
        shutdown.error.code,
        WorkerRejectionCode::BackendFailure);
    EXPECT_NE(
        shutdown.error.message.find(
            "unacknowledged authoritative terminal"),
        std::string::npos);
    const auto terminal =
        harness.events.WorksetTerminals().front();
    auto result =
        program::DecodeProgramResultV1(
            terminal.terminal.output_payload);
    ASSERT_TRUE(result) << result.status.message;
    ASSERT_EQ(result.value->artifacts.size(), 1u);
    EXPECT_EQ(
        result.value->artifacts.front()
            .artifact.artifact_id,
        "test-shutdown-state");
    EXPECT_TRUE(
        std::filesystem::exists(
            temporary.File("shutdown-state.sav")));
    EXPECT_EQ(
        harness.runtime->snapshot().state,
        WorkerState::Stopped);
}

TEST(
    ExecutionWorkerRuntime,
    RejectsWholeWorksetAtomicallyBeforeBaselineMutation)
{
    RuntimeHarness harness({}, {}, true);
    ASSERT_EQ(
        harness.Open().outcome,
        WorkerCommandOutcome::Completed);
    const WorksetEpoch original_epoch =
        harness.runtime->snapshot().session.workset_epoch;
    WorkerWorksetDefinition invalid =
        harness.Workset(42, 2);
    invalid.phase_invocation.program_package.identity.canonical_sha256 =
        std::string(64, '0');
    invalid.phase_invocation.program_package.canonical_sha256 =
        fullphase::ComputeFullPhaseProgramPackageHash(
            invalid.phase_invocation.program_package);
    invalid.execution_key.program_package_sha256 =
        invalid.phase_invocation.program_package.canonical_sha256;
    invalid.execution_key.canonical_sha256 =
        ComputeWorkerWorksetExecutionKeyHash(invalid.execution_key);
    const WorkerCommandResult rejected =
        harness.runtime
            ->Submit(
                harness.NextRequest(),
                SubmitWithoutInitialCancellations(std::move(invalid)))
            .get();
    EXPECT_EQ(
        rejected.outcome,
        WorkerCommandOutcome::Rejected);
    EXPECT_EQ(
        harness.program->StartCount(),
        0);
    EXPECT_EQ(
        harness.runtime->snapshot().session.workset_epoch,
        original_epoch);
    EXPECT_FALSE(
        harness.runtime->snapshot().active_workset);
    EXPECT_EQ(
        harness.Shutdown().outcome,
        WorkerCommandOutcome::Completed);
}

TEST(
    ExecutionWorkerRuntime,
    RejectsCorruptArtifactAndTemplatePolicyMismatch)
{
    RuntimeHarness harness({}, {}, true);
    ASSERT_EQ(
        harness.Open().outcome,
        WorkerCommandOutcome::Completed);

    WorkerWorksetDefinition corrupt =
        harness.Workset(50, 1);
    corrupt.baseline.artifact.state_sha256 =
        std::string(64, '0');
    corrupt.execution_key.baseline =
        ComputeProgramBaselineKey(corrupt.baseline);
    corrupt.execution_key.canonical_sha256 =
        ComputeWorkerWorksetExecutionKeyHash(corrupt.execution_key);
    EXPECT_EQ(
        harness.runtime
            ->Submit(
                harness.NextRequest(),
                SubmitWithoutInitialCancellations(corrupt))
            .get()
            .error.code,
        WorkerRejectionCode::InvalidArgument);

    harness.program->prepared_state_policy =
        program::InvocationStatePolicy::EstablishBaseline;
    WorkerWorksetDefinition wrong_policy =
        harness.Workset(52, 1);
    EXPECT_EQ(
        harness.runtime
            ->Submit(
                harness.NextRequest(),
                SubmitWithoutInitialCancellations(wrong_policy))
            .get()
            .outcome,
        WorkerCommandOutcome::Rejected);
    EXPECT_EQ(harness.program->StartCount(), 0);
    EXPECT_EQ(
        harness.Shutdown().outcome,
        WorkerCommandOutcome::Completed);
}

TEST(
    ExecutionWorkerRuntime,
    RejectsResidentIdentityReuseAndLateTerminalCancellation)
{
    RuntimeHarness harness({}, {}, true);
    ASSERT_EQ(
        harness.Open().outcome,
        WorkerCommandOutcome::Completed);
    WorkerWorksetDefinition active =
        harness.Workset(60, 2);
    ASSERT_EQ(
        harness.runtime
            ->Submit(
                harness.NextRequest(),
                SubmitWithoutInitialCancellations(active))
            .get()
            .outcome,
        WorkerCommandOutcome::Accepted);
    ASSERT_TRUE(harness.program->WaitForStarts(1));

    WorkerWorksetDefinition conflicting =
        harness.Workset(61, 1);
    EXPECT_EQ(
        harness.runtime
            ->Submit(
                harness.NextRequest(),
                SubmitWithoutInitialCancellations(conflicting))
            .get()
            .error.code,
        WorkerRejectionCode::InvalidArgument);

    ASSERT_TRUE(harness.program->EmitTerminal(
        InvocationTerminalStatus::Completed));
    ASSERT_TRUE(harness.program->WaitForStarts(2));
    EXPECT_EQ(
        harness.runtime
            ->Submit(
                harness.NextRequest(),
                CancelWorksetItemCommand{
                    WorkerWorksetId(60),
                    WorkerWorksetItemId(1)})
            .get()
            .error.code,
        WorkerRejectionCode::WorksetItemAlreadyTerminal);
    ASSERT_TRUE(harness.program->EmitTerminal(
        InvocationTerminalStatus::Completed));
    ASSERT_TRUE(
        harness.events.WaitForWorksetTerminalCount(2));
    for (const auto& terminal :
         harness.events.WorksetTerminals())
    {
        EXPECT_EQ(
            harness.runtime
                ->Submit(
                    harness.NextRequest(),
                    AcknowledgeTerminalCommand{
                        terminal.correlation})
                .get()
                .outcome,
            WorkerCommandOutcome::Completed);
    }
    EXPECT_EQ(
        harness.Shutdown().outcome,
        WorkerCommandOutcome::Completed);
}

TEST(
    ExecutionWorkerRuntime,
    AppliesInitialCancellationSidecarBeforePreparingInvocations)
{
    RuntimeHarness harness({}, {}, true);
    ASSERT_EQ(
        harness.Open().outcome,
        WorkerCommandOutcome::Completed);
    const WorkerWorksetDefinition definition =
        harness.Workset(65, 3);
    const InitialWorksetCancellationSidecarV1 sidecar{
        .workset_id = definition.workset_id,
        .item_ids = {WorkerWorksetItemId(2)},
    };
    const auto sidecar_sha =
        ComputeInitialWorksetCancellationSidecarSha256(sidecar);

    const WorkerCommandResult accepted =
        harness.runtime
            ->Submit(
                harness.NextRequest(),
                SubmitWorksetCommand{definition, sidecar})
            .get();
    ASSERT_EQ(accepted.outcome, WorkerCommandOutcome::Accepted)
        << accepted.error.message;
    ASSERT_TRUE(accepted.workset_submission.has_value());
    EXPECT_EQ(
        accepted.workset_submission->disposition,
        WorksetSubmissionDispositionV1::Admitted);
    EXPECT_EQ(accepted.workset_submission->applied_item_count, 1u);
    EXPECT_EQ(
        accepted.workset_submission->applied_sidecar_sha256,
        sidecar_sha);
    EXPECT_EQ(
        accepted.snapshot.resident_cancellation_sidecar_sha256,
        sidecar_sha);
    ASSERT_TRUE(harness.program->WaitForStarts(1));
    {
        std::lock_guard lock(harness.program->mutex);
        // The start notification can win the race with removal of item 1's
        // consumed fake template. Item 3 may therefore be the only retained
        // template or item 1 and item 3 may both still be visible. The
        // invariant is that suppressed item 2 never acquired a template.
        EXPECT_LE(harness.program->prepared_templates.size(), 2u);
        EXPECT_TRUE(std::ranges::none_of(
            harness.program->prepared_templates,
            [&](const auto& entry) {
                return entry.second.invocation_id.value() ==
                    definition.items[1]
                        .execution.execution_id.value();
            }));
    }

    const WorkerCommandResult repeated =
        harness.runtime
            ->Submit(
                harness.NextRequest(),
                SubmitWorksetCommand{definition, sidecar})
            .get();
    ASSERT_EQ(repeated.outcome, WorkerCommandOutcome::Accepted);
    ASSERT_TRUE(repeated.workset_submission.has_value());
    EXPECT_EQ(
        repeated.workset_submission->disposition,
        WorksetSubmissionDispositionV1::AlreadyAdmitted);
    EXPECT_EQ(
        repeated.workset_submission->applied_sidecar_sha256,
        sidecar_sha);

    auto mismatched = sidecar;
    mismatched.item_ids = {WorkerWorksetItemId(1)};
    const WorkerCommandResult rejected =
        harness.runtime
            ->Submit(
                harness.NextRequest(),
                SubmitWorksetCommand{definition, mismatched})
            .get();
    EXPECT_EQ(rejected.outcome, WorkerCommandOutcome::Rejected);
    EXPECT_EQ(rejected.error.code, WorkerRejectionCode::InvalidArgument);

    ASSERT_TRUE(harness.program->EmitTerminal(
        InvocationTerminalStatus::Completed));
    ASSERT_TRUE(harness.program->WaitForStarts(2));
    ASSERT_TRUE(harness.program->EmitTerminal(
        InvocationTerminalStatus::Completed));
    ASSERT_TRUE(harness.events.WaitForWorksetTerminalCount(2));
    const auto terminals = harness.events.WorksetTerminals();
    ASSERT_EQ(terminals.size(), 2u);
    for (const auto& terminal : terminals)
    {
        EXPECT_EQ(
            harness.runtime
                ->Submit(
                    harness.NextRequest(),
                    AcknowledgeTerminalCommand{terminal.correlation})
                .get()
                .outcome,
            WorkerCommandOutcome::Completed);
    }
    ASSERT_TRUE(harness.events.WaitForWorksetSummaryCount(1));
    const auto summaries = harness.events.WorksetSummaries();
    ASSERT_EQ(summaries.size(), 1u);
    EXPECT_EQ(summaries.front().item_count, 3u);
    EXPECT_EQ(summaries.front().initially_suppressed_count, 1u);
    EXPECT_EQ(summaries.front().completed_count, 2u);
    EXPECT_EQ(summaries.front().unstarted_count, 0u);
    EXPECT_EQ(
        harness.Shutdown().outcome,
        WorkerCommandOutcome::Completed);
}

TEST(
    ExecutionWorkerRuntime,
    RetriesAThrownTerminalPublicationWithoutLosingTheTerminal)
{
    RuntimeHarness harness({}, {}, true);
    ASSERT_EQ(
        harness.Open().outcome,
        WorkerCommandOutcome::Completed);
    ASSERT_EQ(
        harness.runtime
            ->Submit(
                harness.NextRequest(),
                SubmitWithoutInitialCancellations(
                    harness.Workset(70, 1)))
            .get()
            .outcome,
        WorkerCommandOutcome::Accepted);
    ASSERT_TRUE(harness.program->WaitForStarts(1));
    harness.events.ThrowNextWorksetTerminal();
    ASSERT_TRUE(harness.program->EmitTerminal(
        InvocationTerminalStatus::Completed));

    ASSERT_TRUE(
        harness.events.WaitForWorksetTerminalCount(1));
    const auto terminal =
        harness.events.WorksetTerminals().front();
    EXPECT_EQ(
        harness.runtime
            ->Submit(
                harness.NextRequest(),
                AcknowledgeTerminalCommand{
                    terminal.correlation})
            .get()
            .outcome,
        WorkerCommandOutcome::Completed);
    EXPECT_EQ(
        harness.runtime
            ->Submit(
                harness.NextRequest(),
                AcknowledgeTerminalCommand{
                    terminal.correlation})
            .get()
            .outcome,
        WorkerCommandOutcome::Completed);
    EXPECT_EQ(
        harness.Shutdown().outcome,
        WorkerCommandOutcome::Completed);
}

TEST(
    ExecutionWorkerRuntime,
    PreparedStartFailureRetainsExactWorksetTerminalsBeforeTaint)
{
    RuntimeHarness harness({}, {}, true);
    ASSERT_EQ(
        harness.Open().outcome,
        WorkerCommandOutcome::Completed);
    harness.program->throw_start = true;
    ASSERT_EQ(
        harness.runtime
            ->Submit(
                harness.NextRequest(),
                SubmitWithoutInitialCancellations(
                    harness.Workset(80, 2)))
            .get()
            .outcome,
        WorkerCommandOutcome::Accepted);
    ASSERT_TRUE(
        harness.events.WaitForWorksetTerminalCount(2));
    const auto terminals =
        harness.events.WorksetTerminals();
    ASSERT_EQ(terminals.size(), 2u);
    EXPECT_FALSE(terminals[0].unstarted);
    EXPECT_EQ(
        terminals[0].terminal.status,
        InvocationTerminalStatus::CleanupFailure);
    EXPECT_TRUE(terminals[1].unstarted);
    EXPECT_EQ(
        harness.runtime->snapshot().state,
        WorkerState::Tainted);
    {
        std::lock_guard lock(harness.program->mutex);
        EXPECT_TRUE(harness.program->prepared_templates.empty());
    }
}

TEST(
    ExecutionWorkerRuntime,
    ActiveWorksetFaultRetainsActiveAndUnstartedTerminalsBeforeTaint)
{
    RuntimeHarness harness({}, {}, true);
    ASSERT_EQ(
        harness.Open().outcome,
        WorkerCommandOutcome::Completed);
    harness.program->emit_action_on_start = true;
    harness.program->throw_action_dispatch = true;
    ASSERT_EQ(
        harness.runtime
            ->Submit(
                harness.NextRequest(),
                SubmitWithoutInitialCancellations(
                    harness.Workset(81, 2)))
            .get()
            .outcome,
        WorkerCommandOutcome::Accepted);
    ASSERT_TRUE(
        harness.events.WaitForWorksetTerminalCount(2));
    const auto terminals =
        harness.events.WorksetTerminals();
    ASSERT_EQ(terminals.size(), 2u);
    EXPECT_FALSE(terminals[0].unstarted);
    EXPECT_EQ(
        terminals[0].terminal.status,
        InvocationTerminalStatus::InfrastructureFailure);
    EXPECT_TRUE(terminals[1].unstarted);
    EXPECT_EQ(harness.runtime->snapshot().state, WorkerState::Tainted);
}

TEST(EmulationSession, BootFailureDoesNotAdvanceEpochAndShutdownIsIdempotent)
{
    auto control = std::make_shared<ScriptedDolphinBackendControl>();
    control->SetOpenResult(
        BackendResult::Failure(
            BackendErrorCode::BootFailed,
            "expected boot failure"),
        BackendCoreState::Closed);
    EmulationSession session(
        SessionId(1),
        std::make_unique<ScriptedDolphinBackend>(control));

    SessionOpenOptions options;
    const SessionOperationReceipt open = session.Open(options);
    EXPECT_FALSE(open.ok);
    EXPECT_EQ(open.workset_epoch, WorksetEpoch{});
    EXPECT_EQ(open.disposition, SessionDisposition::Closed);

    const SessionOperationReceipt first_shutdown = session.Shutdown();
    const SessionOperationReceipt second_shutdown = session.Shutdown();
    EXPECT_TRUE(first_shutdown.ok);
    EXPECT_EQ(first_shutdown.ok, second_shutdown.ok);
    EXPECT_EQ(first_shutdown.disposition, second_shutdown.disposition);
    EXPECT_EQ(first_shutdown.backend.code, second_shutdown.backend.code);
    EXPECT_EQ(control->OpenCount(), 1);
    EXPECT_EQ(control->CloseCount(), 1);
}

TEST(EmulationSession, UnknownIntegrityAndStoppedCoreTaintWithoutAdvancingEpoch)
{
    {
        auto control = std::make_shared<ScriptedDolphinBackendControl>();
        control->SetOpenResult(
            BackendResult::Success(),
            BackendCoreState::Stopped);
        EmulationSession session(
            SessionId(3),
            std::make_unique<ScriptedDolphinBackend>(control));

        const SessionOperationReceipt open = session.Open({});
        EXPECT_FALSE(open.ok);
        EXPECT_EQ(open.workset_epoch, WorksetEpoch{});
        EXPECT_EQ(open.disposition, SessionDisposition::Tainted);
        EXPECT_TRUE(session.Shutdown().ok);
    }

    auto control = std::make_shared<ScriptedDolphinBackendControl>();
    EmulationSession session(
        SessionId(4),
        std::make_unique<ScriptedDolphinBackend>(control));
    ASSERT_TRUE(session.Open({}).ok);
    const WorkerWorksetId workset_id(1);
    const SessionOperationReceipt initialization =
        session.OpenWorksetInitialization(workset_id);
    ASSERT_TRUE(initialization.ok) << initialization.backend.message;
    ASSERT_FALSE(session.execution_snapshot());
    const SessionOperationReceipt committed =
        session.CommitWorksetInitialization(workset_id);
    ASSERT_TRUE(committed.ok) << committed.backend.message;
    ASSERT_TRUE(session.execution_snapshot());

    control->SetStepFrameResult(BackendResult::Failure(
        BackendErrorCode::Timeout,
        "frame completion is uncertain",
        BackendIntegrity::Unknown));
    const ExecutionSubmissionReceipt step =
        session.SubmitExecution(StepFramesRequest{
            .policy = SessionExecutionPolicy(WorksetEpoch{1}),
            .count = 1,
        });
    ASSERT_TRUE(step.accepted) << step.error.message;
    const auto step_terminal = DrainSessionExecution(session);
    ASSERT_TRUE(step_terminal.has_value());
    EXPECT_EQ(
        step_terminal->status,
        ExecutionTerminalStatus::BackendFailure);
    EXPECT_EQ(
        step_terminal->integrity,
        BackendIntegrity::Unknown);
    EXPECT_EQ(
        session.snapshot().workset_epoch,
        WorksetEpoch{1});
    EXPECT_EQ(
        session.snapshot().disposition,
        SessionDisposition::Tainted);
    EXPECT_TRUE(session.Shutdown().ok);
}

TEST(EmulationSession, RejectsOffOwnerCallsBeforeBackendMutation)
{
    auto control = std::make_shared<ScriptedDolphinBackendControl>();
    EmulationSession session(
        SessionId(5),
        std::make_unique<ScriptedDolphinBackend>(control));
    ASSERT_TRUE(session.Open({}).ok);

    std::promise<SessionOperationReceipt> attempted;
    std::thread other([&] {
        attempted.set_value(
            session.OpenWorksetInitialization(WorkerWorksetId(1)));
    });
    other.join();

    const SessionOperationReceipt receipt = attempted.get_future().get();
    EXPECT_FALSE(receipt.ok);
    EXPECT_EQ(receipt.backend.code, BackendErrorCode::InvalidState);
    EXPECT_TRUE(session.Shutdown().ok);
    EXPECT_FALSE(control->HasOwnerViolation());
}

TEST(EmulationSession, WorksetEpochIsNotACommandOrDispatchEpoch)
{
    const WorksetEpoch workset_epoch{9};
    const WorkerCommandSequence command_sequence{9};
    const std::uint64_t dispatch_epoch = 9;

    EXPECT_EQ(workset_epoch.value(), dispatch_epoch);
    EXPECT_EQ(command_sequence.value(), dispatch_epoch);
    EXPECT_FALSE((std::is_same_v<WorksetEpoch, WorkerCommandSequence>));
}

TEST(ExecutionWorkerRuntime, EnforcesOneSessionAndOneActiveInvocation)
{
    RuntimeHarness harness;
    ASSERT_EQ(harness.Open().outcome, WorkerCommandOutcome::Completed);

    const WorkerCommandResult second_open = harness.runtime->Submit(
        harness.NextRequest(),
        OpenSessionCommand{harness.OpenOptions()}).get();
    EXPECT_EQ(second_open.outcome, WorkerCommandOutcome::Rejected);
    EXPECT_EQ(second_open.error.code, WorkerRejectionCode::InvalidState);
    EXPECT_EQ(harness.backend->OpenCount(), 1);

    const WorkerCommandResult first = harness.Invoke(101);
    ASSERT_EQ(first.outcome, WorkerCommandOutcome::Accepted);
    ASSERT_TRUE(harness.program->WaitForStarts(1));

    const WorkerCommandResult second = harness.Invoke(102);
    EXPECT_EQ(second.outcome, WorkerCommandOutcome::Rejected)
        << second.error.message;
    EXPECT_EQ(second.error.code, WorkerRejectionCode::InvalidArgument)
        << second.error.message;
    EXPECT_EQ(harness.program->StartCount(), 1);

    ASSERT_TRUE(CompleteAndAcknowledgeActiveWorkset(harness));
    EXPECT_EQ(harness.runtime->snapshot().state, WorkerState::Ready);
    EXPECT_EQ(harness.events.TerminalCount(), 1);

    EXPECT_EQ(harness.Shutdown().outcome, WorkerCommandOutcome::Completed);
}

TEST(
    ExecutionWorkerRuntime,
    ImmediateProgramActionsStillRoundTripThroughTheActorMailbox)
{
    RuntimeHarness harness;
    {
        std::lock_guard lock(harness.program->mutex);
        harness.program->emit_action_on_start = true;
    }
    ASSERT_EQ(
        harness.Open().outcome,
        WorkerCommandOutcome::Completed);
    ASSERT_EQ(
        harness.Invoke(150).outcome,
        WorkerCommandOutcome::Accepted);
    ASSERT_TRUE(harness.program->WaitForActionCompletions(1));

    {
        std::lock_guard lock(harness.program->mutex);
        EXPECT_EQ(harness.program->action_dispatch_count, 1);
        EXPECT_EQ(harness.program->action_completion_count, 1);
        EXPECT_FALSE(harness.program->action_dispatched_inline);
        EXPECT_FALSE(harness.program->action_completed_inline);
        EXPECT_EQ(
            harness.program->action_order,
            (std::vector<std::string>{
                "runtime_start_publish",
                "action_host_dispatch",
                "runtime_completion"}));
    }

    ASSERT_TRUE(CompleteAndAcknowledgeActiveWorkset(harness));
    EXPECT_EQ(
        harness.Shutdown().outcome,
        WorkerCommandOutcome::Completed);
}

TEST(
    ExecutionWorkerRuntime,
    ProgramActionDispatchExceptionTaintsAndTerminatesOnce)
{
    RuntimeHarness harness;
    {
        std::lock_guard lock(harness.program->mutex);
        harness.program->emit_action_on_start = true;
        harness.program->throw_action_dispatch = true;
    }
    ASSERT_EQ(
        harness.Open().outcome,
        WorkerCommandOutcome::Completed);
    ASSERT_EQ(
        harness.Invoke(152).outcome,
        WorkerCommandOutcome::Accepted);
    ASSERT_TRUE(harness.events.WaitForTerminalCount(1));

    EXPECT_EQ(
        harness.runtime->snapshot().state,
        WorkerState::Tainted);
    EXPECT_EQ(harness.events.TerminalCount(), 1u);
    EXPECT_EQ(harness.backend->CloseCount(), 1);
    EXPECT_EQ(harness.program->ShutdownCount(), 1);

    const WorkerCommandResult shutdown = harness.Shutdown();
    EXPECT_EQ(shutdown.outcome, WorkerCommandOutcome::Rejected);
    EXPECT_EQ(harness.events.TerminalCount(), 1u);
}

TEST(
    ExecutionWorkerRuntime,
    RejectedAwaitedActionCompletionTaintsAndTerminatesOnce)
{
    RuntimeHarness harness;
    {
        std::lock_guard lock(harness.program->mutex);
        harness.program->emit_action_on_start = true;
        harness.program->action_completion_submission =
            ProgramRuntimeSubmission::Rejected(
                WorkerRejectionCode::InvalidArgument,
                "fake runtime rejected the awaited completion");
    }
    ASSERT_EQ(
        harness.Open().outcome,
        WorkerCommandOutcome::Completed);
    ASSERT_EQ(
        harness.Invoke(153).outcome,
        WorkerCommandOutcome::Accepted);
    ASSERT_TRUE(harness.program->WaitForActionCompletions(1));
    ASSERT_TRUE(harness.events.WaitForTerminalCount(1));

    EXPECT_EQ(
        harness.runtime->snapshot().state,
        WorkerState::Tainted);
    EXPECT_EQ(harness.events.TerminalCount(), 1u);
    EXPECT_EQ(harness.backend->CloseCount(), 1);
    EXPECT_EQ(harness.program->ShutdownCount(), 1);

    const WorkerCommandResult shutdown = harness.Shutdown();
    EXPECT_EQ(shutdown.outcome, WorkerCommandOutcome::Rejected);
    EXPECT_EQ(harness.events.TerminalCount(), 1u);
}

TEST(ExecutionWorkerRuntime, CompletionWinningCancelRaceHasOneTerminal)
{
    RuntimeHarness harness;
    ASSERT_EQ(harness.Open().outcome, WorkerCommandOutcome::Completed);
    ASSERT_EQ(harness.Invoke(201).outcome, WorkerCommandOutcome::Accepted);
    ASSERT_TRUE(harness.program->WaitForStarts(1));

    ASSERT_TRUE(harness.program->EmitTerminal(
        InvocationTerminalStatus::Completed));
    const WorkerCommandResult cancel = harness.runtime->Submit(
        harness.NextRequest(),
        CancelInvocationCommand{InvocationId(201)}).get();
    EXPECT_EQ(cancel.outcome, WorkerCommandOutcome::Rejected);
    EXPECT_EQ(cancel.error.code, WorkerRejectionCode::InvocationNotActive);
    ASSERT_TRUE(harness.events.WaitForTerminalCount(1));

    ASSERT_TRUE(harness.program->EmitTerminal(
        InvocationTerminalStatus::Completed));
    ASSERT_TRUE(AcknowledgeLatestWorksetTerminal(harness));
    EXPECT_EQ(harness.events.TerminalCount(), 1);

    EXPECT_EQ(harness.Shutdown().outcome, WorkerCommandOutcome::Completed);
}

TEST(
    ExecutionWorkerRuntime,
    PublishedCanonicalTerminalWinsBeforeItsMailboxEventIsConsumed)
{
    RuntimeHarness harness;
    ASSERT_EQ(harness.Open().outcome, WorkerCommandOutcome::Completed);
    ASSERT_EQ(harness.Invoke(251).outcome, WorkerCommandOutcome::Accepted);
    ASSERT_TRUE(harness.program->WaitForStarts(1));

    {
        std::lock_guard lock(harness.program->mutex);
        harness.program->cancellation_submission =
            ProgramRuntimeSubmission::ExecutionAlreadyFinished();
    }
    ASSERT_TRUE(harness.program->EmitTerminal(
        InvocationTerminalStatus::Completed));
    const WorkerCommandResult cancel = harness.runtime->Submit(
        harness.NextRequest(),
        CancelInvocationCommand{InvocationId(251)}).get();
    EXPECT_EQ(cancel.outcome, WorkerCommandOutcome::Rejected);
    EXPECT_EQ(
        cancel.error.code,
        WorkerRejectionCode::InvocationNotActive);
    EXPECT_FALSE(
        harness.program->LastToken().is_cancellation_requested());

    ASSERT_TRUE(harness.events.WaitForTerminalCount(1));
    const auto terminals = harness.events.Terminals();
    ASSERT_EQ(terminals.size(), 1u);
    EXPECT_EQ(
        terminals.front().status,
        InvocationTerminalStatus::Completed);
    ASSERT_TRUE(AcknowledgeLatestWorksetTerminal(harness));
    EXPECT_EQ(
        harness.runtime->snapshot().state,
        WorkerState::Ready);
    EXPECT_EQ(harness.Shutdown().outcome, WorkerCommandOutcome::Completed);
}

TEST(
    ExecutionWorkerRuntime,
    CancelWinningRaceNormalizesLaterCompletionAndRejectsDuplicateMismatchAndStale)
{
    RuntimeHarness harness;
    ASSERT_EQ(harness.Open().outcome, WorkerCommandOutcome::Completed);
    ASSERT_EQ(harness.Invoke(301, 9).outcome, WorkerCommandOutcome::Accepted);
    ASSERT_TRUE(harness.program->WaitForStarts(1));

    const WorkerCommandResult cancel = harness.runtime->Submit(
        harness.NextRequest(),
        CancelInvocationCommand{InvocationId(301)}).get();
    EXPECT_EQ(cancel.outcome, WorkerCommandOutcome::Accepted);
    ASSERT_TRUE(harness.program->WaitForCancellations(1));
    EXPECT_TRUE(harness.program->LastToken().is_cancellation_requested());
    EXPECT_EQ(
        harness.program->LastToken().reason(),
        CancellationReason::ExternalRequest);

    const WorkerCommandResult duplicate = harness.runtime->Submit(
        harness.NextRequest(),
        CancelInvocationCommand{InvocationId(301)}).get();
    EXPECT_EQ(duplicate.outcome, WorkerCommandOutcome::Rejected);
    EXPECT_EQ(
        duplicate.error.code,
        WorkerRejectionCode::DuplicateCancellation);

    const WorkerCommandResult mismatch = harness.runtime->Submit(
        harness.NextRequest(),
        CancelInvocationCommand{InvocationId(999)}).get();
    EXPECT_EQ(mismatch.outcome, WorkerCommandOutcome::Rejected);
    EXPECT_EQ(mismatch.error.code, WorkerRejectionCode::InvocationMismatch);

    // The fake deliberately reports success after the actor has accepted the
    // exact cancellation. Actor queue order owns the race and normalizes it.
    ASSERT_TRUE(harness.program->EmitTerminal(
        InvocationTerminalStatus::Completed));
    ASSERT_TRUE(harness.events.WaitForTerminalCount(1));
    const auto terminals = harness.events.Terminals();
    ASSERT_EQ(terminals.size(), 1);
    EXPECT_EQ(
        terminals.front().status,
        InvocationTerminalStatus::Cancelled);
    ASSERT_TRUE(AcknowledgeLatestWorksetTerminal(harness));

    const WorkerCommandResult stale = harness.runtime->Submit(
        harness.NextRequest(),
        CancelInvocationCommand{InvocationId(301)}).get();
    EXPECT_EQ(stale.outcome, WorkerCommandOutcome::Rejected);
    EXPECT_EQ(stale.error.code, WorkerRejectionCode::InvocationNotActive);
    EXPECT_EQ(harness.runtime->snapshot().state, WorkerState::Ready);
    EXPECT_EQ(harness.events.TerminalCount(), 1);

    EXPECT_EQ(harness.Shutdown().outcome, WorkerCommandOutcome::Completed);
}

TEST(
    ExecutionWorkerRuntime,
    CancellationDeliveryFailureTaintsAndSynthesizesExactlyOneTerminal)
{
    RuntimeHarness harness;
    ASSERT_EQ(harness.Open().outcome, WorkerCommandOutcome::Completed);
    ASSERT_EQ(harness.Invoke(351).outcome, WorkerCommandOutcome::Accepted);
    ASSERT_TRUE(harness.program->WaitForStarts(1));
    {
        std::lock_guard lock(harness.program->mutex);
        harness.program->cancellation_submission =
            ProgramRuntimeSubmission::Rejected(
                WorkerRejectionCode::InternalFailure,
                "injected cancellation delivery failure");
    }

    const WorkerCommandResult cancel = harness.runtime->Submit(
        harness.NextRequest(),
        CancelInvocationCommand{InvocationId(351)}).get();

    EXPECT_EQ(cancel.outcome, WorkerCommandOutcome::Rejected);
    EXPECT_EQ(cancel.error.code, WorkerRejectionCode::InternalFailure);
    EXPECT_EQ(harness.program->CancellationCount(), 1);
    EXPECT_EQ(harness.program->ShutdownCount(), 1);
    EXPECT_EQ(harness.backend->CloseCount(), 1);
    EXPECT_EQ(harness.runtime->snapshot().state, WorkerState::Tainted);
    ASSERT_TRUE(harness.events.WaitForTerminalCount(1));

    const auto terminals = harness.events.Terminals();
    ASSERT_EQ(terminals.size(), 1);
    EXPECT_EQ(
        terminals.front().status,
        InvocationTerminalStatus::InfrastructureFailure);
    EXPECT_EQ(terminals.front().cleanup, CleanupStatus::Failed);
    EXPECT_EQ(
        terminals.front().session_disposition,
        SessionDisposition::Tainted);

    // Retired ingress cannot publish a second terminal after the fault.
    ASSERT_TRUE(harness.program->EmitTerminal(
        InvocationTerminalStatus::Completed));
    const WorkerCommandResult shutdown = harness.Shutdown();
    EXPECT_EQ(shutdown.outcome, WorkerCommandOutcome::Rejected);
    EXPECT_EQ(harness.runtime->snapshot().state, WorkerState::Stopped);
    EXPECT_EQ(harness.events.TerminalCount(), 1);
    EXPECT_EQ(harness.program->CancellationCount(), 1);
    EXPECT_EQ(harness.program->ShutdownCount(), 1);
    EXPECT_EQ(harness.backend->CloseCount(), 1);
}

TEST(
    ExecutionWorkerRuntime,
    ShutdownCancellationDeliveryFailureStopsDeterministically)
{
    RuntimeHarness harness;
    ASSERT_EQ(harness.Open().outcome, WorkerCommandOutcome::Completed);
    ASSERT_EQ(harness.Invoke(352).outcome, WorkerCommandOutcome::Accepted);
    ASSERT_TRUE(harness.program->WaitForStarts(1));
    {
        std::lock_guard lock(harness.program->mutex);
        harness.program->cancellation_submission =
            ProgramRuntimeSubmission::Rejected(
                WorkerRejectionCode::InternalFailure,
                "injected shutdown cancellation failure");
    }

    auto shutdown = harness.runtime->Submit(
        harness.NextRequest(),
        ShutdownCommand{});
    ASSERT_EQ(shutdown.wait_for(5s), std::future_status::ready);
    const WorkerCommandResult result = shutdown.get();
    harness.runtime->WaitStopped();

    EXPECT_EQ(result.outcome, WorkerCommandOutcome::Rejected);
    EXPECT_EQ(result.error.code, WorkerRejectionCode::SessionTainted);
    EXPECT_EQ(harness.runtime->snapshot().state, WorkerState::Stopped);
    EXPECT_EQ(harness.program->CancellationCount(), 1);
    EXPECT_EQ(harness.program->ShutdownCount(), 1);
    EXPECT_EQ(harness.backend->CloseCount(), 1);
    ASSERT_TRUE(harness.events.WaitForTerminalCount(1));
    const auto terminals = harness.events.Terminals();
    ASSERT_EQ(terminals.size(), 1);
    EXPECT_EQ(
        terminals.front().status,
        InvocationTerminalStatus::InfrastructureFailure);
    EXPECT_EQ(terminals.front().cleanup, CleanupStatus::Failed);
    EXPECT_EQ(
        terminals.front().session_disposition,
        SessionDisposition::Tainted);
}

TEST(ExecutionWorkerRuntime, ShutdownWinningRaceWaitsForTerminalAndClosesOnce)
{
    RuntimeHarness harness;
    ASSERT_EQ(harness.Open().outcome, WorkerCommandOutcome::Completed);
    ASSERT_EQ(harness.Invoke(401).outcome, WorkerCommandOutcome::Accepted);
    ASSERT_TRUE(harness.program->WaitForStarts(1));

    auto shutdown = harness.runtime->Submit(
        harness.NextRequest(),
        ShutdownCommand{});
    ASSERT_TRUE(harness.program->WaitForCancellations(1));
    EXPECT_EQ(shutdown.wait_for(0ms), std::future_status::timeout);

    ASSERT_TRUE(harness.program->EmitTerminal(
        InvocationTerminalStatus::Cancelled));
    const WorkerCommandResult result = shutdown.get();
    harness.runtime->WaitStopped();

    EXPECT_EQ(result.outcome, WorkerCommandOutcome::Rejected);
    EXPECT_EQ(result.error.code, WorkerRejectionCode::BackendFailure);
    EXPECT_EQ(harness.backend->CloseCount(), 1);
    EXPECT_EQ(harness.program->ShutdownCount(), 1);
    EXPECT_EQ(harness.events.TerminalCount(), 1);
    EXPECT_EQ(harness.runtime->snapshot().state, WorkerState::Stopped);
}

TEST(ExecutionWorkerRuntime, CompletionWinningShutdownRaceStillClosesCleanly)
{
    RuntimeHarness harness;
    ASSERT_EQ(harness.Open().outcome, WorkerCommandOutcome::Completed);
    ASSERT_EQ(harness.Invoke(402).outcome, WorkerCommandOutcome::Accepted);
    ASSERT_TRUE(harness.program->WaitForStarts(1));

    ASSERT_TRUE(harness.program->EmitTerminal(
        InvocationTerminalStatus::Completed));
    const WorkerCommandResult shutdown = harness.Shutdown();

    EXPECT_EQ(shutdown.outcome, WorkerCommandOutcome::Rejected);
    EXPECT_EQ(shutdown.error.code, WorkerRejectionCode::BackendFailure);
    EXPECT_EQ(harness.backend->CloseCount(), 1);
    EXPECT_EQ(harness.program->ShutdownCount(), 1);
    EXPECT_EQ(harness.events.TerminalCount(), 1);
}

TEST(
    ExecutionWorkerRuntime,
    RetainedTerminalAlsoWinsAQueuedShutdownCancellation)
{
    RuntimeHarness harness;
    ASSERT_EQ(harness.Open().outcome, WorkerCommandOutcome::Completed);
    ASSERT_EQ(harness.Invoke(403).outcome, WorkerCommandOutcome::Accepted);
    ASSERT_TRUE(harness.program->WaitForStarts(1));
    ASSERT_TRUE(harness.program->EmitTerminal(
        InvocationTerminalStatus::Completed));
    auto shutdown = harness.runtime->Submit(
        harness.NextRequest(),
        ShutdownCommand{});
    const WorkerCommandResult result = shutdown.get();
    harness.runtime->WaitStopped();
    EXPECT_EQ(result.outcome, WorkerCommandOutcome::Rejected);
    EXPECT_EQ(result.error.code, WorkerRejectionCode::BackendFailure);
    const auto terminals = harness.events.Terminals();
    ASSERT_EQ(terminals.size(), 1u);
    EXPECT_EQ(
        terminals.front().status,
        InvocationTerminalStatus::Completed);
    EXPECT_EQ(harness.backend->CloseCount(), 1);
}

TEST(ExecutionWorkerRuntime, CleanDiagnosticsReuseButTaintRejectsFurtherWork)
{
    RuntimeHarness harness;
    ASSERT_EQ(harness.Open().outcome, WorkerCommandOutcome::Completed);

    ASSERT_EQ(harness.Invoke(501).outcome, WorkerCommandOutcome::Accepted);
    ASSERT_TRUE(harness.program->WaitForStarts(1));
    ASSERT_TRUE(harness.program->EmitTerminal(
        InvocationTerminalStatus::Completed,
        CleanupStatus::CleanWithDiagnostics,
        SessionDisposition::CleanWithDiagnostics));
    ASSERT_TRUE(harness.events.WaitForTerminalCount(1));
    ASSERT_TRUE(AcknowledgeLatestWorksetTerminal(harness));

    ASSERT_EQ(harness.Invoke(502).outcome, WorkerCommandOutcome::Accepted);
    ASSERT_TRUE(harness.program->WaitForStarts(2));
    for (int attempt = 0;
         attempt < 100 &&
             harness.runtime->snapshot().state != WorkerState::Running;
         ++attempt)
    {
        std::this_thread::sleep_for(1ms);
    }
    EXPECT_EQ(harness.runtime->snapshot().state, WorkerState::Running);
    EXPECT_EQ(
        harness.runtime->snapshot().session.disposition,
        SessionDisposition::CleanWithDiagnostics);
    ASSERT_TRUE(harness.program->EmitTerminal(
        InvocationTerminalStatus::Completed,
        CleanupStatus::Failed,
        SessionDisposition::Tainted,
        std::nullopt,
        RuntimeError{
            WorkerRejectionCode::SessionTainted,
            "cleanup proof failed"}));
    ASSERT_TRUE(harness.events.WaitForTerminalCount(2));
    EXPECT_EQ(harness.runtime->snapshot().state, WorkerState::Tainted);
    EXPECT_EQ(
        harness.runtime->snapshot().session.disposition,
        SessionDisposition::Tainted);
    EXPECT_EQ(harness.backend->CloseCount(), 1);

    const WorkerCommandResult rejected = harness.Invoke(503);
    EXPECT_EQ(rejected.outcome, WorkerCommandOutcome::Rejected);
    EXPECT_EQ(rejected.error.code, WorkerRejectionCode::SessionTainted);

    const WorkerCommandResult shutdown = harness.Shutdown();
    EXPECT_EQ(shutdown.outcome, WorkerCommandOutcome::Rejected);
    EXPECT_EQ(shutdown.error.code, WorkerRejectionCode::SessionTainted);

    const auto terminals = harness.events.Terminals();
    ASSERT_EQ(terminals.size(), 2);
    EXPECT_EQ(
        terminals.back().status,
        InvocationTerminalStatus::CleanupFailure);
}

TEST(
    ExecutionWorkerRuntime,
    GenuineCoreStallMarksSessionCleanWithDiagnosticsAndAllowsReuse)
{
    std::atomic<std::int64_t> clock_offset_milliseconds{0};
    ExecutionControlCoreConfig execution_config;
    execution_config.now = [&clock_offset_milliseconds] {
        return std::chrono::steady_clock::now() +
            std::chrono::milliseconds(
                clock_offset_milliseconds.load(
                    std::memory_order_acquire));
    };

    EmulationSession* actor_session = nullptr;
    std::atomic<bool> arm_execution{false};
    std::atomic<bool> execution_accepted{false};
    std::string execution_error;
    auto hooks = std::make_shared<WorkerRuntimeTestHooks>();
    hooks->session_opened = [&](EmulationSession& session) {
        actor_session = &session;
    };
    hooks->before_ingress_stability_check = [&] {
        if (!arm_execution.exchange(
                false,
                std::memory_order_acq_rel))
        {
            return;
        }
        const ExecutionSubmissionReceipt submitted =
            actor_session->SubmitExecution(ContinueUntilRequest{
                .policy = SessionExecutionPolicy(
                    actor_session->snapshot().workset_epoch),
                .wake_group = WorkerWakeGroup(0x801DC288u),
            });
        execution_accepted.store(
            submitted.accepted,
            std::memory_order_release);
        execution_error = submitted.error.message;
    };

    RuntimeHarness harness(
        {},
        std::move(hooks),
        false,
        {},
        std::move(execution_config));
    ASSERT_EQ(
        harness.Open().outcome,
        WorkerCommandOutcome::Completed);
    ASSERT_NE(actor_session, nullptr);
    ASSERT_EQ(
        harness.Invoke(504).outcome,
        WorkerCommandOutcome::Accepted);
    ASSERT_TRUE(harness.program->WaitForStarts(1));

    arm_execution.store(true, std::memory_order_release);
    (void)harness.runtime
        ->Submit(
            harness.NextRequest(),
            CancelInvocationCommand{
                InvocationId(900001)})
        .get();
    (void)harness.runtime
        ->Submit(
            harness.NextRequest(),
            CancelInvocationCommand{
                InvocationId(900000)})
        .get();
    ASSERT_TRUE(
        execution_accepted.load(std::memory_order_acquire))
        << execution_error;
    ASSERT_EQ(
        harness.runtime->snapshot().execution->activity,
        ExecutionActivity::Continuing);

    clock_offset_milliseconds.fetch_add(
        10'001,
        std::memory_order_acq_rel);
    (void)harness.runtime
        ->Submit(
            harness.NextRequest(),
            CancelInvocationCommand{
                InvocationId(900002)})
        .get();
    ASSERT_TRUE(
        harness.events.WaitForExecutionHealthWarningCount(1));
    EXPECT_EQ(
        harness.events.ExecutionTerminals().size(),
        0u);

    clock_offset_milliseconds.fetch_add(
        10'001,
        std::memory_order_acq_rel);
    (void)harness.runtime
        ->Submit(
            harness.NextRequest(),
            CancelInvocationCommand{
                InvocationId(900003)})
        .get();
    ASSERT_EQ(
        harness.events.ExecutionHealthWarnings().size(),
        1u);
    // Core-stall confirmation begins the bounded safe-pause terminal path.
    // Drive one more actor turn so the newly paused backend can be confirmed
    // without relying on a real-time sleep.
    (void)harness.runtime
        ->Submit(
            harness.NextRequest(),
            CancelInvocationCommand{
                InvocationId(900004)})
        .get();
    ASSERT_TRUE(
        harness.events.WaitForExecutionTerminalCount(1));

    const auto execution_terminals =
        harness.events.ExecutionTerminals();
    ASSERT_EQ(execution_terminals.size(), 1u);
    EXPECT_EQ(
        execution_terminals.front().status,
        ExecutionTerminalStatus::CoreStalled);
    EXPECT_EQ(
        execution_terminals.front().integrity,
        BackendIntegrity::Preserved);
    EXPECT_EQ(
        harness.runtime->snapshot().session.disposition,
        SessionDisposition::CleanWithDiagnostics);
    EXPECT_EQ(
        harness.runtime->snapshot().state,
        WorkerState::Running);
    ASSERT_TRUE(harness.program->EmitTerminal(
        InvocationTerminalStatus::Completed));
    ASSERT_TRUE(harness.events.WaitForTerminalCount(1));
    ASSERT_TRUE(AcknowledgeLatestWorksetTerminal(harness));
    EXPECT_EQ(
        harness.runtime->snapshot().state,
        WorkerState::Ready);
    EXPECT_EQ(
        harness.runtime->snapshot().session.disposition,
        SessionDisposition::CleanWithDiagnostics);

    EXPECT_EQ(
        harness.Shutdown().outcome,
        WorkerCommandOutcome::Completed);
}

TEST(
    ExecutionWorkerRuntime,
    UnprovenCoreStallPauseTaintsSessionAndTerminatesResidentWorkset)
{
    std::atomic<std::int64_t> clock_offset_milliseconds{0};
    ExecutionControlCoreConfig execution_config;
    execution_config.now = [&clock_offset_milliseconds] {
        return std::chrono::steady_clock::now() +
            std::chrono::milliseconds(
                clock_offset_milliseconds.load(
                    std::memory_order_acquire));
    };

    EmulationSession* actor_session = nullptr;
    std::atomic<bool> arm_execution{false};
    std::atomic<bool> execution_accepted{false};
    std::string execution_error;
    auto hooks = std::make_shared<WorkerRuntimeTestHooks>();
    hooks->session_opened = [&](EmulationSession& session) {
        actor_session = &session;
    };
    hooks->before_ingress_stability_check = [&] {
        if (!arm_execution.exchange(
                false,
                std::memory_order_acq_rel))
        {
            return;
        }
        const ExecutionSubmissionReceipt submitted =
            actor_session->SubmitExecution(ContinueUntilRequest{
                .policy = SessionExecutionPolicy(
                    actor_session->snapshot().workset_epoch),
                .wake_group = WorkerWakeGroup(0x801DC288u),
            });
        execution_accepted.store(
            submitted.accepted,
            std::memory_order_release);
        execution_error = submitted.error.message;
    };

    RuntimeHarness harness(
        {},
        std::move(hooks),
        true,
        {},
        std::move(execution_config));
    ASSERT_EQ(
        harness.Open().outcome,
        WorkerCommandOutcome::Completed);
    ASSERT_NE(actor_session, nullptr);

    const WorkerCommandResult accepted =
        harness.runtime
            ->Submit(
                harness.NextRequest(),
                SubmitWithoutInitialCancellations(
                    harness.Workset(86, 2)))
            .get();
    ASSERT_EQ(
        accepted.outcome,
        WorkerCommandOutcome::Accepted)
        << accepted.error.message;
    ASSERT_TRUE(harness.program->WaitForStarts(1));

    arm_execution.store(true, std::memory_order_release);
    (void)harness.runtime
        ->Submit(
            harness.NextRequest(),
            CancelInvocationCommand{
                InvocationId(910001)})
        .get();
    (void)harness.runtime
        ->Submit(
            harness.NextRequest(),
            CancelInvocationCommand{
                InvocationId(910000)})
        .get();
    ASSERT_TRUE(
        execution_accepted.load(std::memory_order_acquire));

    harness.backend->SetPauseResult(BackendResult::Failure(
        BackendErrorCode::OperationFailed,
        "pause proof failed",
        BackendIntegrity::Unknown));
    clock_offset_milliseconds.fetch_add(
        10'001,
        std::memory_order_acq_rel);
    (void)harness.runtime
        ->Submit(
            harness.NextRequest(),
            CancelInvocationCommand{
                InvocationId(910002)})
        .get();
    ASSERT_TRUE(
        harness.events.WaitForExecutionHealthWarningCount(1));

    clock_offset_milliseconds.fetch_add(
        10'001,
        std::memory_order_acq_rel);
    (void)harness.runtime
        ->Submit(
            harness.NextRequest(),
            CancelInvocationCommand{
                InvocationId(910003)})
        .get();
    (void)harness.runtime
        ->Submit(
            harness.NextRequest(),
            CancelInvocationCommand{
                InvocationId(910004)})
        .get();

    ASSERT_TRUE(
        harness.events.WaitForExecutionTerminalCount(1));
    const auto execution_terminals =
        harness.events.ExecutionTerminals();
    ASSERT_EQ(execution_terminals.size(), 1u);
    EXPECT_EQ(
        execution_terminals.front().status,
        ExecutionTerminalStatus::CleanupFailure);
    EXPECT_EQ(
        execution_terminals.front().integrity,
        BackendIntegrity::Unknown);

    ASSERT_TRUE(
        harness.events.WaitForWorksetTerminalCount(2));
    const auto workset_terminals =
        harness.events.WorksetTerminals();
    ASSERT_EQ(workset_terminals.size(), 2u);
    EXPECT_FALSE(workset_terminals[0].unstarted);
    EXPECT_EQ(
        workset_terminals[0].terminal.status,
        InvocationTerminalStatus::InfrastructureFailure);
    EXPECT_TRUE(workset_terminals[1].unstarted);
    EXPECT_EQ(
        harness.runtime->snapshot().state,
        WorkerState::Tainted);
    EXPECT_EQ(
        harness.runtime->snapshot().session.disposition,
        SessionDisposition::Tainted);
    EXPECT_FALSE(
        harness.runtime->snapshot().active_workset.has_value());
    EXPECT_EQ(harness.backend->CloseCount(), 1);

    const WorkerCommandResult rejected =
        harness.runtime
            ->Submit(
                harness.NextRequest(),
                SubmitWithoutInitialCancellations(
                    harness.Workset(87, 1)))
            .get();
    EXPECT_EQ(
        rejected.outcome,
        WorkerCommandOutcome::Rejected);
    EXPECT_EQ(
        rejected.error.code,
        WorkerRejectionCode::SessionTainted);

    const WorkerCommandResult shutdown =
        harness.Shutdown();
    EXPECT_EQ(
        shutdown.outcome,
        WorkerCommandOutcome::Rejected);
    EXPECT_EQ(
        shutdown.error.code,
        WorkerRejectionCode::SessionTainted);
    EXPECT_EQ(
        harness.runtime->snapshot().state,
        WorkerState::Stopped);
}

TEST(ExecutionWorkerRuntime, VisualActiveItemControlsCompleteExactlyOnce)
{
    RuntimeHarness harness;
    SessionOpenOptions options = harness.OpenOptions();
    options.backend.visual = true;
    ASSERT_EQ(
        harness.Open(std::move(options)).outcome,
        WorkerCommandOutcome::Completed);
    ASSERT_EQ(
        harness.Invoke(603).outcome,
        WorkerCommandOutcome::Accepted);
    ASSERT_TRUE(harness.program->WaitForStarts(1));

    const WorkerCommandResult resumed = harness.runtime->Submit(
        harness.NextRequest(),
        ControlExecutionCommand{
            .control = WorkerExecutionControlKind::Resume,
            .workset_id = WorkerWorksetId(603),
            .item_id = WorkerWorksetItemId(1),
        }).get();
    EXPECT_EQ(resumed.outcome, WorkerCommandOutcome::Accepted);
    ASSERT_TRUE(resumed.execution_operation_id.has_value());
    EXPECT_FALSE(resumed.execution_terminal.has_value());
    EXPECT_EQ(resumed.snapshot.state, WorkerState::Running);
    EXPECT_EQ(
        resumed.snapshot.execution->activity,
        ExecutionActivity::InteractiveRunning);

    const WorkerCommandResult paused = harness.runtime->Submit(
        harness.NextRequest(),
        ControlExecutionCommand{
            .control = WorkerExecutionControlKind::Pause,
            .workset_id = WorkerWorksetId(603),
            .item_id = WorkerWorksetItemId(1),
            .timeout = 1s,
        }).get();
    EXPECT_EQ(paused.outcome, WorkerCommandOutcome::Completed);
    ASSERT_TRUE(paused.execution_terminal.has_value());
    EXPECT_EQ(
        paused.execution_terminal->status,
        ExecutionTerminalStatus::Paused);
    EXPECT_EQ(paused.snapshot.state, WorkerState::Running);
    EXPECT_EQ(
        paused.snapshot.execution->activity,
        ExecutionActivity::IdlePaused);

    const WorkerCommandResult stepped = harness.runtime->Submit(
        harness.NextRequest(),
        ControlExecutionCommand{
            .control = WorkerExecutionControlKind::StepFrame,
            .workset_id = WorkerWorksetId(603),
            .item_id = WorkerWorksetItemId(1),
            .count = 2,
            .timeout = 1s,
        }).get();
    EXPECT_EQ(stepped.outcome, WorkerCommandOutcome::Completed);
    ASSERT_TRUE(stepped.execution_terminal.has_value());
    EXPECT_EQ(
        stepped.execution_terminal->status,
        ExecutionTerminalStatus::StepsCompleted);
    EXPECT_EQ(stepped.execution_terminal->completed_count, 2u);
    EXPECT_EQ(stepped.snapshot.state, WorkerState::Running);
    EXPECT_EQ(
        stepped.snapshot.execution->activity,
        ExecutionActivity::IdlePaused);

    ASSERT_TRUE(harness.events.WaitForCommandCount(
        WorkerCommandKind::ControlExecution,
        3));
    const auto completions =
        harness.events.CommandResults(WorkerCommandKind::ControlExecution);
    ASSERT_EQ(completions.size(), 3u);
    EXPECT_EQ(completions[0].request_id, resumed.request_id);
    EXPECT_EQ(completions[1].request_id, paused.request_id);
    EXPECT_EQ(completions[2].request_id, stepped.request_id);

    ASSERT_TRUE(CompleteAndAcknowledgeActiveWorkset(harness));

    EXPECT_EQ(
        harness.Shutdown().outcome,
        WorkerCommandOutcome::Completed);
}

TEST(ExecutionWorkerRuntime, ShutdownUnwindsInteractiveResume)
{
    RuntimeHarness harness;
    SessionOpenOptions options = harness.OpenOptions();
    options.backend.visual = true;
    ASSERT_EQ(
        harness.Open(std::move(options)).outcome,
        WorkerCommandOutcome::Completed);
    ASSERT_EQ(
        harness.Invoke(604).outcome,
        WorkerCommandOutcome::Accepted);
    ASSERT_TRUE(harness.program->WaitForStarts(1));

    const WorkerCommandResult resumed = harness.runtime->Submit(
        harness.NextRequest(),
        ControlExecutionCommand{
            .control = WorkerExecutionControlKind::Resume,
            .workset_id = WorkerWorksetId(604),
            .item_id = WorkerWorksetItemId(1),
        }).get();
    ASSERT_EQ(resumed.outcome, WorkerCommandOutcome::Accepted);
    ASSERT_EQ(
        harness.runtime->snapshot().execution->activity,
        ExecutionActivity::InteractiveRunning);

    auto shutdown = harness.runtime->Submit(
        harness.NextRequest(),
        ShutdownCommand{});
    ASSERT_TRUE(harness.program->WaitForCancellations(1));
    ASSERT_TRUE(harness.program->EmitTerminal(
        InvocationTerminalStatus::Cancelled));
    const WorkerCommandResult shutdown_result = shutdown.get();
    harness.runtime->WaitStopped();
    EXPECT_EQ(shutdown_result.outcome, WorkerCommandOutcome::Rejected);
    EXPECT_EQ(
        shutdown_result.error.code,
        WorkerRejectionCode::BackendFailure);
    EXPECT_EQ(harness.runtime->snapshot().state, WorkerState::Stopped);
    EXPECT_EQ(harness.backend->CloseCount(), 1);
}

TEST(
    ExecutionWorkerRuntime,
    RoutedStopInjectedAtCommandBoundaryIsNotLost)
{
    constexpr std::uint32_t kWakePc = 0x801dc288u;
    auto physical_control =
        std::make_shared<FakePhysicalStopBackendControl>();
    auto physical_backend =
        std::make_unique<FakePhysicalStopBackend>(physical_control);
    FakePhysicalStopBackend* physical_backend_raw =
        physical_backend.get();

    std::atomic<bool> continue_accepted{false};
    std::atomic<bool> arm_boundary_injection{false};
    std::atomic<bool> coordination_timed_out{false};
    std::promise<void> boundary_open_signal;
    std::shared_future<void> boundary_open =
        boundary_open_signal.get_future().share();
    std::promise<void> injection_complete_signal;
    std::shared_future<void> injection_complete =
        injection_complete_signal.get_future().share();
    EmulationSession* actor_session = nullptr;
    auto hooks = std::make_shared<WorkerRuntimeTestHooks>();
    hooks->session_opened = [&](EmulationSession& session) {
        actor_session = &session;
    };
    hooks->before_ingress_stability_check = [&]() {
        if (!arm_boundary_injection.exchange(
                false,
                std::memory_order_acq_rel))
        {
            return;
        }
        const ExecutionSubmissionReceipt submission =
            actor_session->SubmitExecution(ContinueUntilRequest{
                .policy = SessionExecutionPolicy(
                    actor_session->snapshot().workset_epoch),
                .wake_group = WorkerWakeGroup(kWakePc),
            });
        continue_accepted.store(
            submission.accepted,
            std::memory_order_release);
        boundary_open_signal.set_value();
        if (injection_complete.wait_for(5s) !=
            std::future_status::ready)
        {
            coordination_timed_out.store(
                true,
                std::memory_order_release);
        }
    };

    RuntimeHarness harness(std::move(physical_backend), hooks);
    const WorkerCommandResult open = harness.Open();
    ASSERT_EQ(open.outcome, WorkerCommandOutcome::Completed);
    ASSERT_NE(actor_session, nullptr);
    ASSERT_EQ(
        harness.Invoke(450).outcome,
        WorkerCommandOutcome::Accepted);
    ASSERT_TRUE(harness.program->WaitForStarts(1));
    WorkerSnapshot active = harness.runtime->snapshot();
    for (int attempt = 0;
         attempt < 100 && !active.active_workset_item;
         ++attempt)
    {
        std::this_thread::sleep_for(1ms);
        active = harness.runtime->snapshot();
    }
    ASSERT_TRUE(active.active_workset);
    ASSERT_TRUE(active.active_workset_item);

    std::atomic<bool> requested_break{false};
    std::thread injector([&]() {
        if (boundary_open.wait_for(5s) !=
            std::future_status::ready)
        {
            coordination_timed_out.store(
                true,
                std::memory_order_release);
            injection_complete_signal.set_value();
            return;
        }
        const auto decision =
            physical_backend_raw->InjectJitPcStop(kWakePc);
        requested_break.store(
            decision.request_break,
            std::memory_order_release);
        injection_complete_signal.set_value();
    });

    const WireRequestId boundary_request = harness.NextRequest();
    arm_boundary_injection.store(true, std::memory_order_release);
    auto boundary_future = harness.runtime->Submit(
        boundary_request,
        CancelInvocationCommand{InvocationId(999999)});
    const std::future_status boundary_status =
        boundary_future.wait_for(5s);
    injector.join();
    ASSERT_EQ(boundary_status, std::future_status::ready);
    const WorkerCommandResult boundary = boundary_future.get();

    EXPECT_FALSE(
        coordination_timed_out.load(std::memory_order_acquire));
    EXPECT_TRUE(
        continue_accepted.load(std::memory_order_acquire));
    EXPECT_TRUE(requested_break.load(std::memory_order_acquire));
    EXPECT_EQ(boundary.outcome, WorkerCommandOutcome::Rejected);
    ASSERT_TRUE(harness.events.WaitForExecutionTerminalCount(1));

    const std::vector<WorkerEvent> events = harness.events.Events();
    std::optional<std::size_t> routed_terminal_index;
    for (std::size_t index = 0; index < events.size(); ++index)
    {
        if (const auto* execution =
                std::get_if<WorkerExecutionEvent>(&events[index]);
            execution && execution->event.terminal &&
            execution->event.terminal->status ==
                ExecutionTerminalStatus::RequestedCompletion)
        {
            routed_terminal_index = index;
        }
    }

    ASSERT_TRUE(routed_terminal_index.has_value());

    ASSERT_TRUE(CompleteAndAcknowledgeActiveWorkset(harness));

    EXPECT_EQ(
        harness.Shutdown().outcome,
        WorkerCommandOutcome::Completed);
}

TEST(
    ExecutionWorkerRuntime,
    RoutedActionCompletionPrecedesQueuedInvocationCancellation)
{
    constexpr std::uint32_t kWakePc = 0x801dc288u;
    auto physical_control =
        std::make_shared<FakePhysicalStopBackendControl>();
    auto physical_backend =
        std::make_unique<FakePhysicalStopBackend>(physical_control);
    FakePhysicalStopBackend* physical_backend_raw =
        physical_backend.get();

    EmulationSession* actor_session = nullptr;
    std::atomic<bool> continue_accepted{false};
    std::atomic<bool> arm_boundary_injection{false};
    std::atomic<bool> coordination_timed_out{false};
    std::promise<void> boundary_open_signal;
    std::shared_future<void> boundary_open =
        boundary_open_signal.get_future().share();
    std::promise<void> injection_complete_signal;
    std::shared_future<void> injection_complete =
        injection_complete_signal.get_future().share();
    auto hooks = std::make_shared<WorkerRuntimeTestHooks>();
    hooks->session_opened = [&](EmulationSession& session) {
        actor_session = &session;
    };
    hooks->before_ingress_stability_check = [&]() {
        if (!arm_boundary_injection.exchange(
                false,
                std::memory_order_acq_rel))
        {
            return;
        }
        const ExecutionSubmissionReceipt submission =
            actor_session->SubmitExecution(ContinueUntilRequest{
                .policy = SessionExecutionPolicy(
                    actor_session->snapshot().workset_epoch),
                .wake_group = WorkerWakeGroup(kWakePc),
            });
        continue_accepted.store(
            submission.accepted,
            std::memory_order_release);
        boundary_open_signal.set_value();
        if (injection_complete.wait_for(5s) !=
            std::future_status::ready)
        {
            coordination_timed_out.store(
                true,
                std::memory_order_release);
        }
    };

    RuntimeHarness harness(std::move(physical_backend), hooks);
    {
        std::lock_guard lock(harness.program->mutex);
        harness.program->emit_action_on_start = true;
        harness.program->complete_action_from_execution_terminal = true;
        harness.program->complete_invocation_on_action_completion = true;
    }
    ASSERT_EQ(
        harness.Open().outcome,
        WorkerCommandOutcome::Completed);
    ASSERT_NE(actor_session, nullptr);
    ASSERT_EQ(
        harness.Invoke(451).outcome,
        WorkerCommandOutcome::Accepted);
    ASSERT_TRUE(harness.program->WaitForActionDispatches(1));

    std::atomic<bool> requested_break{false};
    std::thread injector([&]() {
        if (boundary_open.wait_for(5s) !=
            std::future_status::ready)
        {
            coordination_timed_out.store(
                true,
                std::memory_order_release);
            injection_complete_signal.set_value();
            return;
        }
        harness.backend->SetCoreState(BackendCoreState::Paused);
        harness.backend->pc = kWakePc;
        const auto decision =
            physical_backend_raw->InjectJitPcStop(kWakePc);
        requested_break.store(
            decision.request_break,
            std::memory_order_release);
        injection_complete_signal.set_value();
    });

    arm_boundary_injection.store(true, std::memory_order_release);
    auto cancel_future = harness.runtime->Submit(
        harness.NextRequest(),
        CancelInvocationCommand{InvocationId(451)});
    const std::future_status cancel_status =
        cancel_future.wait_for(5s);
    injector.join();
    ASSERT_EQ(cancel_status, std::future_status::ready);
    const WorkerCommandResult cancel = cancel_future.get();

    EXPECT_FALSE(
        coordination_timed_out.load(std::memory_order_acquire));
    EXPECT_TRUE(
        continue_accepted.load(std::memory_order_acquire));
    EXPECT_TRUE(requested_break.load(std::memory_order_acquire));
    EXPECT_EQ(cancel.outcome, WorkerCommandOutcome::Rejected);
    EXPECT_EQ(
        cancel.error.code,
        WorkerRejectionCode::InvocationNotActive);
    ASSERT_TRUE(harness.program->WaitForActionCompletions(1));
    ASSERT_TRUE(harness.events.WaitForTerminalCount(1));

    {
        std::lock_guard lock(harness.program->mutex);
        const auto completion = std::ranges::find(
            harness.program->action_order,
            "runtime_completion");
        const auto cancellation = std::ranges::find(
            harness.program->action_order,
            "runtime_cancel");
        ASSERT_NE(completion, harness.program->action_order.end());
        EXPECT_EQ(cancellation, harness.program->action_order.end());
    }
    const auto terminals = harness.events.Terminals();
    ASSERT_EQ(terminals.size(), 1u);
    EXPECT_EQ(
        terminals.front().status,
        InvocationTerminalStatus::Completed);
    ASSERT_TRUE(AcknowledgeLatestWorksetTerminal(harness));

    EXPECT_EQ(
        harness.Shutdown().outcome,
        WorkerCommandOutcome::Completed);
}

TEST(
    ExecutionWorkerRuntime,
    AuthoritativeWakePrecedesQueuedPauseAndCoreHealthConfirmation)
{
    constexpr std::uint32_t kWakePc = 0x801dc288u;
    std::atomic<std::int64_t> clock_offset_milliseconds{0};
    ExecutionControlCoreConfig execution_config;
    execution_config.now = [&clock_offset_milliseconds] {
        return std::chrono::steady_clock::now() +
            std::chrono::milliseconds(
                clock_offset_milliseconds.load(
                    std::memory_order_acquire));
    };

    auto physical_control =
        std::make_shared<FakePhysicalStopBackendControl>();
    auto physical_backend =
        std::make_unique<FakePhysicalStopBackend>(physical_control);
    FakePhysicalStopBackend* physical_backend_raw =
        physical_backend.get();

    EmulationSession* actor_session = nullptr;
    std::atomic<bool> arm_execution{false};
    std::atomic<bool> execution_accepted{false};
    std::string execution_error;
    std::atomic<bool> arm_boundary_injection{false};
    std::atomic<bool> coordination_timed_out{false};
    std::promise<void> boundary_open_signal;
    std::shared_future<void> boundary_open =
        boundary_open_signal.get_future().share();
    std::promise<void> injection_complete_signal;
    std::shared_future<void> injection_complete =
        injection_complete_signal.get_future().share();
    auto hooks = std::make_shared<WorkerRuntimeTestHooks>();
    hooks->session_opened = [&](EmulationSession& session) {
        actor_session = &session;
    };
    hooks->before_ingress_stability_check = [&] {
        if (arm_execution.exchange(
                false,
                std::memory_order_acq_rel))
        {
            const ExecutionSubmissionReceipt submitted =
                actor_session->SubmitExecution(ContinueUntilRequest{
                    .policy = SessionExecutionPolicy(
                        actor_session->snapshot().workset_epoch),
                    .wake_group = WorkerWakeGroup(kWakePc),
                });
            execution_accepted.store(
                submitted.accepted,
                std::memory_order_release);
            execution_error = submitted.error.message;
        }
        if (!arm_boundary_injection.exchange(
                false,
                std::memory_order_acq_rel))
        {
            return;
        }
        boundary_open_signal.set_value();
        if (injection_complete.wait_for(5s) !=
            std::future_status::ready)
        {
            coordination_timed_out.store(
                true,
                std::memory_order_release);
        }
    };

    RuntimeHarness harness(
        std::move(physical_backend),
        std::move(hooks),
        false,
        {},
        std::move(execution_config));
    SessionOpenOptions options = harness.OpenOptions();
    options.backend.visual = true;
    ASSERT_EQ(
        harness.Open(std::move(options)).outcome,
        WorkerCommandOutcome::Completed);
    ASSERT_NE(actor_session, nullptr);
    ASSERT_EQ(
        harness.Invoke(1).outcome,
        WorkerCommandOutcome::Accepted);
    ASSERT_TRUE(harness.program->WaitForStarts(1));

    arm_execution.store(true, std::memory_order_release);
    (void)harness.runtime
        ->Submit(
            harness.NextRequest(),
            CancelInvocationCommand{InvocationId(999999)})
        .get();
    ASSERT_TRUE(
        execution_accepted.load(std::memory_order_acquire))
        << execution_error;

    std::atomic<bool> requested_break{false};
    std::thread injector([&] {
        if (boundary_open.wait_for(5s) !=
            std::future_status::ready)
        {
            coordination_timed_out.store(
                true,
                std::memory_order_release);
            injection_complete_signal.set_value();
            return;
        }
        harness.backend->SetCoreState(
            BackendCoreState::Paused);
        {
            std::lock_guard lock(harness.backend->mutex);
            harness.backend->pc = kWakePc;
        }
        const auto decision =
            physical_backend_raw->InjectJitPcStop(kWakePc);
        requested_break.store(
            decision.request_break,
            std::memory_order_release);
        clock_offset_milliseconds.store(
            20'001,
            std::memory_order_release);
        injection_complete_signal.set_value();
    });

    arm_boundary_injection.store(
        true,
        std::memory_order_release);
    auto pause_future = harness.runtime->Submit(
        harness.NextRequest(),
        ControlExecutionCommand{
            .control = WorkerExecutionControlKind::Pause,
            .workset_id = WorkerWorksetId(1),
            .item_id = WorkerWorksetItemId(1),
        });
    const std::future_status pause_status =
        pause_future.wait_for(5s);
    injector.join();
    ASSERT_EQ(pause_status, std::future_status::ready);
    const WorkerCommandResult pause = pause_future.get();

    EXPECT_FALSE(
        coordination_timed_out.load(std::memory_order_acquire));
    EXPECT_TRUE(requested_break.load(std::memory_order_acquire));
    EXPECT_EQ(
        pause.outcome,
        WorkerCommandOutcome::Rejected);
    ASSERT_TRUE(
        harness.events.WaitForExecutionTerminalCount(1));
    const auto terminals =
        harness.events.ExecutionTerminals();
    ASSERT_EQ(terminals.size(), 1u);
    EXPECT_EQ(
        terminals.front().status,
        ExecutionTerminalStatus::RequestedCompletion);
    EXPECT_EQ(
        terminals.front().evidence.pc,
        kWakePc);
    EXPECT_EQ(
        harness.runtime->snapshot().session.disposition,
        SessionDisposition::Clean);

    ASSERT_TRUE(CompleteAndAcknowledgeActiveWorkset(harness));

    EXPECT_EQ(
        harness.Shutdown().outcome,
        WorkerCommandOutcome::Completed);
}

TEST(ExecutionWorkerRuntime, RuntimeSubmissionExceptionTaintsAndTerminatesOnce)
{
    RuntimeHarness harness;
    ASSERT_EQ(harness.Open().outcome, WorkerCommandOutcome::Completed);
    {
        std::lock_guard lock(harness.program->mutex);
        harness.program->throw_start = true;
    }

    const WorkerCommandResult result = harness.Invoke(801);
    EXPECT_EQ(result.outcome, WorkerCommandOutcome::Accepted);
    ASSERT_TRUE(harness.events.WaitForWorksetTerminalCount(1));
    const auto terminals = harness.events.WorksetTerminals();
    ASSERT_EQ(terminals.size(), 1u);
    EXPECT_EQ(
        terminals.front().terminal.status,
        InvocationTerminalStatus::CleanupFailure);
    ASSERT_TRUE(AcknowledgeLatestWorksetTerminal(harness));
    EXPECT_EQ(harness.runtime->snapshot().state, WorkerState::Tainted);
    EXPECT_EQ(harness.backend->CloseCount(), 1);
    EXPECT_EQ(harness.program->ShutdownCount(), 1);

    const WorkerCommandResult shutdown = harness.Shutdown();
    EXPECT_EQ(shutdown.outcome, WorkerCommandOutcome::Rejected);
    EXPECT_EQ(harness.backend->CloseCount(), 1);
    EXPECT_EQ(harness.program->ShutdownCount(), 1);
}

} // namespace

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <functional>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <sqlite3.h>

#include "Common/Events/EventPayloadDispatch.h"
#include "Common/Events/EventPayloadValidation.h"
#include "Common/Events/OutboxRelay.h"
#include "Common/Migrations/MigrationRunner.h"
#include "Execution/ProgramDB/SeedProbe/SeedProbeNeutralAdapters.h"
#include "Execution/ProgramDB/SeedProbe/SeedProbeUniqueAdapters.h"
#include "Execution/Workflow/AdapterChainOrchestrator.h"
#include "Execution/Workflow/ExecutionDb.h"

namespace {

using simcore::db::events::EventEnvelope;
using simcore::db::events::OutboxRelay;
using simcore::db::events::OutboxRelayDispatchBinding;
using simcore::db::events::OutboxRelayResult;

struct ValidationResult {
    std::string name;
    bool passed = false;
    std::string message;
};

bool ExecSql(sqlite3* db, const char* sql, std::string* error_out) {
    char* err = nullptr;
    const int rc = sqlite3_exec(db, sql, nullptr, nullptr, &err);
    if (rc == SQLITE_OK) {
        return true;
    }

    if (error_out != nullptr) {
        *error_out = err != nullptr ? err : "sqlite3_exec failed";
    }
    sqlite3_free(err);
    return false;
}

std::filesystem::path ResolveMigrationRoot(std::optional<std::filesystem::path> explicit_root) {
    if (explicit_root.has_value() && !explicit_root->empty()) {
        auto provided = explicit_root.value();
        if (std::filesystem::exists(provided / "Execution")) {
            return provided;
        }

        // Accept Windows-style separators even when running on POSIX hosts.
        std::string normalized = provided.generic_string();
        std::replace(normalized.begin(), normalized.end(), '\\', '/');
        provided = std::filesystem::path(normalized);
        if (std::filesystem::exists(provided / "Execution")) {
            return provided;
        }
    }

    const std::vector<std::filesystem::path> candidates{
        std::filesystem::path("SimCoreDB") / "migration",
        std::filesystem::path("..") / "SimCoreDB" / "migration",
        std::filesystem::path("..") / ".." / "SimCoreDB" / "migration",
        std::filesystem::path("..") / ".." / ".." / "SimCoreDB" / "migration",
        std::filesystem::path("migration"),
    };

    for (const auto& candidate : candidates) {
        if (std::filesystem::exists(candidate / "Execution")) {
            return candidate;
        }
    }

    return candidates.front();
}

ValidationResult ValidatePhase0EventContracts() {
    using namespace simcore::db::events;

    ValidationResult result{ .name = "phase0.event_contracts" };
    constexpr std::array<std::string_view, 3> kExpectedInputEvents{ {
        "Execution.WorkflowStepInputRequested.v1",
        "Execution.WorkflowStepInputFragmentReady.v1",
        "Execution.WorkflowStepInputComplete.v1",
    } };

    for (const auto event_type : kExpectedInputEvents) {
        const auto contract = ResolvePayloadResolverContract(event_type, 1);
        if (!contract.has_value() || contract.value() != PayloadResolverContract::ExecutionWorkflowJobV1) {
            result.message = "dispatch binding missing for " + std::string(event_type);
            return result;
        }

        EventEnvelope valid{};
        valid.event_type = std::string(event_type);
        valid.event_version = 1;
        valid.context_name = "Execution";
        valid.aggregate_kind = "workflow_step";
        valid.aggregate_id = "123";
        valid.payload_ref_kind = "workflow_input_event";
        valid.payload_ref_id = 123;

        std::string error;
        if (!ValidateExecutionWorkflowJobPayloadV1(valid, &error)) {
            result.message = "validation failed for " + std::string(event_type) + ": " + error;
            return result;
        }

        valid.payload_ref_kind = "workflow_event";
        if (ValidateExecutionWorkflowJobPayloadV1(valid, &error)) {
            result.message = "expected workflow_input_event payload family for " + std::string(event_type);
            return result;
        }
    }

    result.passed = true;
    result.message = "all workflow input event contracts route and validate correctly";
    return result;
}

ValidationResult ValidatePhase0ReplayBackfill(const std::filesystem::path& migration_root) {
    using namespace simcore::db;
    using namespace simcore::db::events;
    using namespace simcore::db::execution::workflow;
    using namespace simcore::db::migrations;

    ValidationResult result{ .name = "phase0.replay_backfill" };

    sqlite3* db = nullptr;
    if (sqlite3_open(":memory:", &db) != SQLITE_OK) {
        result.message = "failed to open sqlite memory db";
        if (db != nullptr) {
            sqlite3_close(db);
        }
        return result;
    }

    auto close_db = [&]() {
        if (db != nullptr) {
            sqlite3_close(db);
            db = nullptr;
        }
    };

    const MigrationSourceOptions options{
        .source_kind = MigrationSourceKind::Filesystem,
        .filesystem_root = migration_root,
    };

    std::string err;
    if (!ApplyContextMigrations(db, MigrationContext::Execution, options, &err)) {
        result.message = "failed applying execution migrations from " + migration_root.string() + ": " + err;
        close_db();
        return result;
    }

    if (!ExecSql(db, R"SQL(
INSERT INTO exec_workflow_instance(
    workflow_instance_id, workflow_kind, state, root_scope_kind, created_by, created_at_utc, started_at_utc
)
VALUES(301, 'SEED_PROBE_CHAIN', 'RUNNING', 'manual', 'validation', unixepoch()*1000, unixepoch()*1000);
INSERT INTO exec_workflow_step(
    workflow_step_id, workflow_instance_id, step_key, step_kind, state, created_at_utc, ready_at_utc
)
VALUES(302, 301, 'seedprobe.neutral', 'seedprobe.neutral', 'READY', unixepoch()*1000, unixepoch()*1000);
INSERT INTO exec_workflow_event(workflow_event_id, workflow_instance_id, workflow_step_id, event_kind, event_ts_utc, message)
VALUES(303, 301, 302, 'Execution.WorkflowStepReady.v1', unixepoch()*1000, 'ready');
INSERT INTO exec_workflow_input_event(workflow_input_event_id, workflow_instance_id, workflow_step_id, event_kind, event_ts_utc, source_key, request_id)
VALUES(304, 301, 302, 'Execution.WorkflowStepInputRequested.v1', unixepoch()*1000, 'state', 'req-304');
INSERT INTO exec_outbox_message(
    outbox_id,event_id,event_type,event_version,context_name,aggregate_kind,aggregate_id,correlation_id,causation_id,occurred_at_utc,payload_ref_kind,payload_ref_id
)
VALUES
    (305,'evt-v-1','Execution.WorkflowStepReady.v1',1,'Execution','workflow_step','302','corr-301','cause-301',unixepoch()*1000,'workflow_event',303),
    (306,'evt-v-2','Execution.WorkflowStepInputRequested.v1',1,'Execution','workflow_step','302','corr-301','cause-301',unixepoch()*1000,'workflow_input_event',304);
)SQL", &err)) {
        result.message = "failed seeding replay fixtures: " + err;
        close_db();
        return result;
    }

    ExecutionDb execution_db(db);

    const auto legacy_payload = execution_db.ResolveExecutionWorkflowJobPayload(
        "Execution.WorkflowStepReady.v1", 1, "workflow_event", 303);
    if (!legacy_payload.has_value() || legacy_payload->workflow_instance_id != 301 || legacy_payload->workflow_step_id != 302) {
        result.message = "legacy workflow_event payload resolution failed";
        close_db();
        return result;
    }

    int unresolved_count = 0;
    OutboxRelay relay({
        .db = db,
        .outbox_table = "exec_outbox_message",
        .context_name = "Execution",
    });

    std::vector<OutboxRelayDispatchBinding> bindings;
    bindings.push_back({
        .key = { .event_type = "Execution.WorkflowStepReady.v1", .event_version = 1 },
        .handler = [&](const EventEnvelope& envelope, std::string* handler_error) {
            const auto payload = execution_db.ResolveExecutionWorkflowJobPayload(envelope);
            if (!payload.has_value()) {
                ++unresolved_count;
                if (handler_error) *handler_error = "unresolved payload";
                return false;
            }
            return true;
        },
    });
    bindings.push_back({
        .key = { .event_type = "Execution.WorkflowStepInputRequested.v1", .event_version = 1 },
        .handler = [&](const EventEnvelope& envelope, std::string* handler_error) {
            const auto payload = execution_db.ResolveExecutionWorkflowJobPayload(envelope);
            if (!payload.has_value()) {
                ++unresolved_count;
                if (handler_error) *handler_error = "unresolved payload";
                return false;
            }
            return true;
        },
    });

    OutboxRelayResult relay_result{};
    if (!relay.RelayBatch(0, 10, bindings, &relay_result, &err)) {
        result.message = "relay batch failed: " + err;
        close_db();
        return result;
    }

    if (relay_result.failure_count != 0 || unresolved_count != 0) {
        result.message = "replay unresolved payloads detected (failures=" + std::to_string(relay_result.failure_count)
            + ", unresolved=" + std::to_string(unresolved_count) + ")";
        close_db();
        return result;
    }

    if (relay_result.published_count != 2) {
        result.message = "unexpected published count: " + std::to_string(relay_result.published_count);
        close_db();
        return result;
    }

    result.passed = true;
    result.message = "replay/backfill payload resolution passed with zero unresolved rows";
    close_db();
    return result;
}

ValidationResult ValidatePhase1AggregationGatingContracts(const std::filesystem::path& migration_root) {
    using namespace simcore::db;
    using namespace simcore::db::execution::workflow;
    using namespace simcore::db::migrations;

    ValidationResult result{ .name = "phase1.aggregation_gating" };
    sqlite3* db = nullptr;
    if (sqlite3_open(":memory:", &db) != SQLITE_OK) {
        result.message = "failed to open sqlite memory db";
        if (db != nullptr) sqlite3_close(db);
        return result;
    }
    auto close_db = [&]() { if (db != nullptr) sqlite3_close(db); db = nullptr; };

    std::string err;
    const MigrationSourceOptions options{ .source_kind = MigrationSourceKind::Filesystem, .filesystem_root = migration_root };
    if (!ApplyContextMigrations(db, MigrationContext::Execution, options, &err)) {
        result.message = "failed applying execution migrations: " + err;
        close_db();
        return result;
    }
    if (!ExecSql(db, R"SQL(
INSERT INTO exec_workflow_instance(workflow_instance_id, workflow_kind, state, root_scope_kind, created_by, created_at_utc, started_at_utc)
VALUES(1301, 'SEED_PROBE_CHAIN', 'RUNNING', 'manual', 'validation', unixepoch()*1000, unixepoch()*1000);
INSERT INTO exec_workflow_step(workflow_step_id, workflow_instance_id, step_key, step_kind, state, priority, attempts, max_attempts, created_at_utc, ready_at_utc)
VALUES(1302, 1301, 'Neutral', 'seedprobe.neutral', 'READY', 1, 0, 2, unixepoch()*1000, unixepoch()*1000);
)SQL", &err)) {
        result.message = "seed setup failed: " + err;
        close_db();
        return result;
    }

    ExecutionDb execution_db(db);
    auto* commands = execution_db.WorkflowCommandService();
    const auto append = [&](const char* kind, const std::optional<std::string>& source, const std::optional<std::string>& request_id) {
        return commands && commands->AppendStepInputEvent(
            {
                .workflow_instance_id = 1301,
                .workflow_step_id = 1302,
                .event_kind = kind,
                .source_key = source,
                .request_id = request_id,
                .message = std::optional<std::string>("phase1-validation"),
                .requested_by = "SimCoreDBValidation",
            },
            &err);
    };
    if (!append("Execution.WorkflowStepInputRequested.v1", std::optional<std::string>("sync"), std::optional<std::string>("req-sync-1"))
        || !append("Execution.WorkflowStepInputFragmentReady.v1", std::optional<std::string>("sync"), std::optional<std::string>("req-sync-1"))
        || !append("Execution.WorkflowStepInputComplete.v1", std::nullopt, std::nullopt)) {
        result.message = "append input events failed: " + err;
        close_db();
        return result;
    }

    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db, "SELECT COUNT(1) FROM exec_outbox_message WHERE payload_ref_kind='workflow_input_event' AND aggregate_kind='workflow_step' AND aggregate_id='1302';", -1, &st, nullptr) != SQLITE_OK) {
        result.message = "failed preparing outbox validation query";
        close_db();
        return result;
    }
    if (sqlite3_step(st) != SQLITE_ROW) {
        sqlite3_finalize(st);
        result.message = "failed reading outbox validation row";
        close_db();
        return result;
    }
    const int outbox_rows = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    if (outbox_rows != 3) {
        result.message = "expected 3 workflow_input_event outbox rows, got " + std::to_string(outbox_rows);
        close_db();
        return result;
    }

    result.passed = true;
    result.message = "aggregation gating contract rows emit expected workflow_step-scoped outbox payloads";
    close_db();
    return result;
}

ValidationResult ValidatePhase1TimeoutRetryPolicy(const std::filesystem::path& migration_root) {
    using namespace simcore::db;
    using namespace simcore::db::execution::workflow;
    using namespace simcore::db::migrations;

    ValidationResult result{ .name = "phase1.timeout_retry_once" };
    sqlite3* db = nullptr;
    if (sqlite3_open(":memory:", &db) != SQLITE_OK) {
        result.message = "failed to open sqlite memory db";
        if (db != nullptr) sqlite3_close(db);
        return result;
    }
    auto close_db = [&]() { if (db != nullptr) sqlite3_close(db); db = nullptr; };

    std::string err;
    const MigrationSourceOptions options{ .source_kind = MigrationSourceKind::Filesystem, .filesystem_root = migration_root };
    if (!ApplyContextMigrations(db, MigrationContext::Execution, options, &err)) {
        result.message = "failed applying execution migrations: " + err;
        close_db();
        return result;
    }
    if (!ExecSql(db, R"SQL(
INSERT INTO exec_workflow_instance(workflow_instance_id, workflow_kind, state, root_scope_kind, created_by, created_at_utc, started_at_utc)
VALUES(1401, 'SEED_PROBE_CHAIN', 'RUNNING', 'manual', 'validation', unixepoch()*1000, unixepoch()*1000);
INSERT INTO exec_workflow_step(workflow_step_id, workflow_instance_id, step_key, step_kind, state, priority, attempts, max_attempts, created_at_utc, ready_at_utc)
VALUES(1402, 1401, 'Grid', 'seedprobe.grid', 'READY', 1, 0, 2, unixepoch()*1000, unixepoch()*1000);
)SQL", &err)) {
        result.message = "seed setup failed: " + err;
        close_db();
        return result;
    }

    ExecutionDb execution_db(db);
    auto* commands = execution_db.WorkflowCommandService();
    for (int retry = 0; retry < 2; ++retry) {
        if (commands == nullptr || !commands->AppendStepInputEvent(
                {
                    .workflow_instance_id = 1401,
                    .workflow_step_id = 1402,
                    .event_kind = "Execution.WorkflowStepInputRequested.v1",
                    .source_key = std::optional<std::string>("async"),
                    .request_id = std::optional<std::string>("req-async-retry-" + std::to_string(retry)),
                    .message = std::optional<std::string>("retry-on-timeout"),
                    .requested_by = "SimCoreDBValidation",
                },
                &err)) {
            result.message = "failed appending retry request event: " + err;
            close_db();
            return result;
        }
    }
    if (commands == nullptr || !commands->MarkStepTerminal(
            {
                .workflow_step_id = 1402,
                .terminal_state = "FAILED",
                .requested_by = "SimCoreDBValidation-timeout",
            },
            &err)) {
        result.message = "failed marking step terminal after retry exhaustion: " + err;
        close_db();
        return result;
    }

    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db, "SELECT state FROM exec_workflow_step WHERE workflow_step_id=1402;", -1, &st, nullptr) != SQLITE_OK) {
        result.message = "failed preparing terminal-state query";
        close_db();
        return result;
    }
    if (sqlite3_step(st) != SQLITE_ROW) {
        sqlite3_finalize(st);
        result.message = "missing step row after terminal mark";
        close_db();
        return result;
    }
    const auto* state_text = sqlite3_column_text(st, 0);
    const std::string state = state_text ? reinterpret_cast<const char*>(state_text) : "";
    sqlite3_finalize(st);
    if (state != "FAILED") {
        result.message = "expected FAILED terminal state after timeout retry exhaustion";
        close_db();
        return result;
    }

    result.passed = true;
    result.message = "single-retry timeout policy represented by two request events followed by failed terminal transition";
    close_db();
    return result;
}

ValidationResult ValidatePhase2AdapterChainShape() {
    using namespace simcore::db::execution::programdb;
    using namespace simcore::db::execution::workflow;

    class ValidationPersistence final : public IJobPersistenceAdapter {
    public:
        JobPersistenceRecord EncodeForQueueing(std::int64_t domain_ref_id) const override {
            return JobPersistenceRecord{ .program_ref_kind = "validation", .program_ref_id = domain_ref_id, .fingerprint = "fp", .program_version = 1 };
        }
        std::int64_t DecodeDomainRefId(const JobPersistenceRecord& persisted) const override { return persisted.program_ref_id; }
    };
    class ValidationRuntimeInit final : public IRuntimeInitAdapter {
    public:
        RuntimeInitRequest BuildRuntimeInit(std::int64_t job_id) const override {
            return RuntimeInitRequest{ .savestate_ref_kind = "savestate", .savestate_ref_id = job_id, .bootstrap_profile = "validation.runtime" };
        }
    };
    class ValidationMapper final : public IResultMapper {
    public:
        std::string BuildResultIniFromPrResult(std::int64_t, const simcore::PRResult&) const override {
            return "[Validation.Results]\nok=1\n";
        }
        ResultMapPayload MapPrimaryResult(std::int64_t job_id, const std::string&) const override {
            return ResultMapPayload{ .result_kind = "validation.result", .result_ref_id = job_id };
        }
        std::optional<ResultArtifactRef> MapPrimaryArtifact(std::int64_t) const override { return std::nullopt; }
    };
    class ValidationTransition final : public IWorkflowTransitionHandler {
    public:
        WorkflowTransitionDecision EvaluateTransition(const WorkflowTransitionContext&) const override {
            return WorkflowTransitionDecision{ .should_advance = true, .blocked_reason = std::nullopt, .next_step_key = std::optional<std::string>("Unique") };
        }
    };
    class ValidationWriter final : public IResultPayloadWriter {
    public:
        bool Persist(const ResultMapPayload& payload, std::string*) override {
            writes.push_back(payload.result_kind + ":" + std::to_string(payload.result_ref_id));
            return true;
        }
        std::vector<std::string> writes;
    };

    ValidationResult result{ .name = "phase2.adapter_chain_shape" };
    ProgramKindDescriptor descriptor{};
    descriptor.program_kind = 321;
    descriptor.program_name = "validation";
    descriptor.job_persistence = std::make_shared<ValidationPersistence>();
    descriptor.runtime_init = std::make_shared<ValidationRuntimeInit>();
    descriptor.result_mapper = std::make_shared<ValidationMapper>();
    descriptor.workflow_transition = std::make_shared<ValidationTransition>();
    descriptor.supports_workflow_orchestration = true;

    ProgramKindRegistry registry;
    if (!registry.Register(descriptor) || !registry.RegisterForStepKind("validation.step", descriptor)) {
        result.message = "failed to register validation descriptor";
        return result;
    }

    auto writer = std::make_shared<ValidationWriter>();
    ResultPayloadWriterRegistry writers;
    writers.Register("validation.result", writer);
    StepCompletionGateService gate;
    AdapterChainOrchestrator orchestrator(&registry, &writers, &gate);

    AdapterChainTrace trace{};
    simcore::PRResult pr{};
    pr.job_id = 3;
    if (!orchestrator.OnInputComplete("validation.step", 1, &trace).has_value()
        || !orchestrator.OnJobClaimed("validation.step", 2, &trace).has_value()
        || !orchestrator.OnJobTerminal("validation.step", 3, pr, &trace, nullptr).has_value()) {
        result.message = "adapter chain failed before transition stage";
        return result;
    }
    const auto terminal = orchestrator.OnStepTerminal(
        "validation.step",
        WorkflowTransitionContext{ .workflow_instance_id = 1, .workflow_step_id = 11, .job_set_id = 12, .workflow_kind = "VALIDATION", .step_key = "Validation" },
        StepCompletionSnapshot{ .workflow_step_id = 11, .job_set_id = 12, .expected_total = 1, .discovered_total = 1, .terminal_total = 1 },
        &trace);
    if (!terminal.gate.can_transition || !terminal.transition.has_value() || !terminal.transition->should_advance) {
        result.message = "adapter chain did not reach transition-approve stage";
        return result;
    }
    if (!trace.job_persistence_invoked || !trace.runtime_init_invoked || !trace.result_mapper_invoked || !trace.result_writer_invoked || !trace.transition_handler_invoked) {
        result.message = "adapter invocation order/coverage did not execute all canonical stages";
        return result;
    }
    if (writer->writes.size() != 1u) {
        result.message = "context-owned writer did not persist mapped payload";
        return result;
    }

    result.passed = true;
    result.message = "adapter chain executed through persistence/runtime/map/writer/transition stages";
    return result;
}

ValidationResult ValidatePhase2CompletionGateMismatchPolicy() {
    using namespace simcore::db::execution::workflow;

    ValidationResult result{ .name = "phase2.completion_gate_mismatch" };
    StepCompletionGateService gate;
    const StepCompletionSnapshot mismatch{
        .workflow_step_id = 20,
        .job_set_id = 200,
        .expected_total = 4,
        .discovered_total = 3,
        .terminal_total = 3,
    };

    const auto first = gate.Evaluate(mismatch);
    const auto second = gate.Evaluate(mismatch);
    if (first.can_transition || first.terminal_fail || first.blocked_reason.value_or("") != "STEP_BLOCKED_COUNT_MISMATCH") {
        result.message = "first mismatch attempt should block with STEP_BLOCKED_COUNT_MISMATCH";
        return result;
    }
    if (second.can_transition || !second.terminal_fail || second.blocked_reason.value_or("") != "STEP_BLOCKED_COUNT_MISMATCH_TERMINAL_FAIL") {
        result.message = "second mismatch attempt should escalate to terminal fail fallback";
        return result;
    }

    const StepCompletionSnapshot incomplete{
        .workflow_step_id = 20,
        .job_set_id = 201,
        .expected_total = 3,
        .discovered_total = 3,
        .terminal_total = 2,
    };
    const auto incomplete_decision = gate.Evaluate(incomplete);
    if (incomplete_decision.can_transition || incomplete_decision.blocked_reason.value_or("") != "STEP_BLOCKED_JOBS_NON_TERMINAL") {
        result.message = "non-terminal job-set gate should block transition";
        return result;
    }

    result.passed = true;
    result.message = "completion gate enforces mismatch reconcile/fail semantics and non-terminal blocking";
    return result;
}

void PrintUsage(const std::map<std::string, std::string>& validations) {
    std::cout << "SimCoreDBValidation - SimCoreDB workflow migration validation tool\n\n";
    std::cout << "Usage:\n";
    std::cout << "  SimCoreDBValidation --list\n";
    std::cout << "  SimCoreDBValidation --run <validation-name|all> [--migration-root <path>]\n\n";
    std::cout << "Available validations:\n";
    for (const auto& [name, desc] : validations) {
        std::cout << "  - " << name << ": " << desc << "\n";
    }
}

} // namespace

int main(int argc, char** argv) {
    const std::map<std::string, std::string> validation_descriptions{
        { "phase0.event_contracts", "Validate Phase 0 workflow input event dispatch and payload-family contract checks." },
        { "phase0.replay_backfill", "Replay representative outbox rows and verify legacy/new payload refs resolve with zero unresolved rows." },
        { "phase1.aggregation_gating", "Validate workflow_step-scoped workflow_input_event outbox rows for requested/fragment/complete aggregation flow." },
        { "phase1.timeout_retry_once", "Validate timeout-retry-once policy shape (two input-request retries then FAILED terminal transition)." },
        { "phase2.adapter_chain_shape", "Validate canonical adapter-chain members are present and invocable for seedprobe neutral step." },
        { "phase2.completion_gate_mismatch", "Validate mismatch semantics block with STEP_BLOCKED_COUNT_MISMATCH then terminal-fail fallback reason." },
    };

    bool list_only = false;
    std::string run_target = "all";
    std::optional<std::filesystem::path> migration_root_override;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--list") {
            list_only = true;
        } else if (arg == "--run") {
            if (i + 1 >= argc) {
                std::cerr << "missing value for --run\n";
                PrintUsage(validation_descriptions);
                return 2;
            }
            run_target = argv[++i];
        } else if (arg == "--migration-root") {
            if (i + 1 >= argc) {
                std::cerr << "missing value for --migration-root\n";
                PrintUsage(validation_descriptions);
                return 2;
            }
            migration_root_override = std::filesystem::path(argv[++i]);
        } else if (arg == "--help" || arg == "-h") {
            PrintUsage(validation_descriptions);
            return 0;
        } else {
            std::cerr << "unknown argument: " << arg << "\n";
            PrintUsage(validation_descriptions);
            return 2;
        }
    }

    if (list_only) {
        PrintUsage(validation_descriptions);
        return 0;
    }

    const auto migration_root = ResolveMigrationRoot(migration_root_override);

    std::vector<ValidationResult> results;
    const auto run_one = [&](const std::string& name) {
        if (name == "phase0.event_contracts") {
            results.push_back(ValidatePhase0EventContracts());
            return true;
        }
        if (name == "phase0.replay_backfill") {
            results.push_back(ValidatePhase0ReplayBackfill(migration_root));
            return true;
        }
        if (name == "phase1.aggregation_gating") {
            results.push_back(ValidatePhase1AggregationGatingContracts(migration_root));
            return true;
        }
        if (name == "phase1.timeout_retry_once") {
            results.push_back(ValidatePhase1TimeoutRetryPolicy(migration_root));
            return true;
        }
        if (name == "phase2.adapter_chain_shape") {
            results.push_back(ValidatePhase2AdapterChainShape());
            return true;
        }
        if (name == "phase2.completion_gate_mismatch") {
            results.push_back(ValidatePhase2CompletionGateMismatchPolicy());
            return true;
        }
        return false;
    };

    if (run_target == "all") {
        run_one("phase0.event_contracts");
        run_one("phase0.replay_backfill");
        run_one("phase1.aggregation_gating");
        run_one("phase1.timeout_retry_once");
        run_one("phase2.adapter_chain_shape");
        run_one("phase2.completion_gate_mismatch");
    } else if (!run_one(run_target)) {
        std::cerr << "unknown validation: " << run_target << "\n";
        PrintUsage(validation_descriptions);
        return 2;
    }

    std::cout << "Running validations (migration root: " << migration_root.string() << ")\n";
    bool all_passed = true;
    for (const auto& r : results) {
        std::cout << (r.passed ? "[PASS] " : "[FAIL] ") << r.name << " - " << r.message << "\n";
        all_passed = all_passed && r.passed;
    }

    return all_passed ? 0 : 1;
}

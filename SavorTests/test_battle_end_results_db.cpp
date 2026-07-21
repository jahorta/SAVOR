#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>
#include <sqlite3.h>

#include "Analysis/SqliteAnalysisDb.h"
#include "Execution/Jobs/JobEventOrchestration.h"
#include "Execution/ProgramDB/BattleEndResults/BattleEndResultsPhaseRegistration.h"
#include "Execution/ProgramDB/ProgramKindRegistry.h"
#include "Execution/Workflow/WorkflowComposition.h"
#include "Execution/Workflow/WorkflowCoordinatorService.h"
#include "Execution/Workflow/WorkflowOrchestration.h"
#include "Execution/Workflow/WorkflowUnitActivationFactory.h"
#include "Phases/Programs/BattleCompletion/BattleCompletionManifest.h"
#include "Phases/Programs/BattleEndResults/BattleEndResultsReport.h"
#include "Runner/IPC/Wire.h"
#include "State/SqliteStateDb.h"
#include "Utils/Base64.h"
#include "Utils/Hash.h"
#include "Utils/IniDoc.h"
#include "common/RecordingExecutionDb.h"
#include "common/SqliteDbFixture.h"

namespace {

class ScopedSqliteDb {
public:
    ScopedSqliteDb() { EXPECT_EQ(sqlite3_open(":memory:", &db_), SQLITE_OK); }
    ~ScopedSqliteDb() { if (db_ != nullptr) sqlite3_close(db_); }
    sqlite3* get() const { return db_; }

    void Exec(const char* sql) const {
        char* error = nullptr;
        const auto rc = sqlite3_exec(db_, sql, nullptr, nullptr, &error);
        const std::string message = error == nullptr ? "" : error;
        if (error != nullptr) sqlite3_free(error);
        ASSERT_EQ(rc, SQLITE_OK) << message;
    }

private:
    sqlite3* db_ = nullptr;
};

struct SeededVictorySource {
    std::filesystem::path savestate_path;
    std::int64_t artifact_id = 0;
    std::int64_t input_savestate_id = 0;
    std::int64_t savestate_id = 0;
    std::int64_t battle_set_id = 0;
    std::int64_t wave_id = 0;
    std::int64_t turn_job_id = 0;
    std::int64_t exec_job_id = 0;
};

void SetError(std::string* error_out, std::string error) {
    if (error_out != nullptr) {
        *error_out = std::move(error);
    }
}

std::optional<SeededVictorySource> SeedVictorySource(
    savor::db::core::DBService* db_service,
    const std::filesystem::path& root,
    const std::string& label,
    std::int32_t source_program_kind,
    std::string* error_out) {
    using namespace savor::db;
    using namespace savor::db::execution::jobs;

    if (db_service == nullptr) {
        SetError(error_out, "db service unavailable");
        return std::nullopt;
    }
    auto* state_db = db_service->StateDb();
    auto* authoring_db = db_service->AuthoringDb();
    auto* analysis_db = db_service->AnalysisDb();
    auto* execution_db = db_service->ExecutionDb();
    if (state_db == nullptr || authoring_db == nullptr || analysis_db == nullptr || execution_db == nullptr) {
        SetError(error_out, "required database unavailable");
        return std::nullopt;
    }

    const auto now = types::UtcNow();
    SeededVictorySource out{};
    out.savestate_path = root / (label + "-victory.sav");
    {
        std::ofstream source(out.savestate_path, std::ios::binary | std::ios::trunc);
        source << "battle-victory-source:" << label;
        if (!source.good()) {
            SetError(error_out, "failed writing source savestate");
            return std::nullopt;
        }
    }

    std::string error;
    if (!state_db->StoreArtifact(
            {
                .sha256 = hash::sha256_of_file(out.savestate_path.string()),
                .size_bytes = static_cast<std::int64_t>(std::filesystem::file_size(out.savestate_path)),
                .compression_kind = 0,
                .filename = std::filesystem::absolute(out.savestate_path).string(),
                .file_ext = ".sav",
                .artifact_kind = "SAV",
                .created_at_utc = now,
                .correlation_id = "test.battle_end_results." + label,
                .causation_id = "test",
            },
            &out.artifact_id,
            &error)) {
        SetError(error_out, "store source artifact: " + error);
        return std::nullopt;
    }
    if (!state_db->CreateSavestate(
            {
                .artifact_id = out.artifact_id,
                .savestate_type = "BATTLE",
                .note = "Victory",
                .is_complete = true,
                .created_at_utc = now,
                .correlation_id = "test.battle_end_results." + label,
                .causation_id = "artifact-" + std::to_string(out.artifact_id),
            },
            &out.savestate_id,
            &error)) {
        SetError(error_out, "create source savestate: " + error);
        return std::nullopt;
    }
    if (!state_db->CreateSavestate(
            {
                .artifact_id = out.artifact_id,
                .savestate_type = "BATTLE",
                .note = "Turn Input",
                .is_complete = true,
                .created_at_utc = now,
                .correlation_id = "test.battle_end_results." + label,
                .causation_id = "artifact-" + std::to_string(out.artifact_id),
            },
            &out.input_savestate_id,
            &error)) {
        SetError(error_out, "create turn-input savestate: " + error);
        return std::nullopt;
    }
    if (out.input_savestate_id <= 0 || out.input_savestate_id == out.savestate_id) {
        SetError(error_out, "turn input and victory output savestates were not distinct");
        return std::nullopt;
    }

    std::int64_t battle_run_spec_id = 0;
    if (!authoring_db->SaveBattleRunSpec(
            {
                .name = label + "-run",
                .priority = 1,
                .run_ms = 60000,
                .vi_stall_ms = 0,
                .use_single_turn_runner = true,
                .created_at_utc = now,
                .correlation_id = "test.battle_end_results." + label,
                .causation_id = "test",
            },
            &battle_run_spec_id,
            &error)) {
        SetError(error_out, "save battle run spec: " + error);
        return std::nullopt;
    }
    std::int64_t explorer_settings_id = 0;
    if (!authoring_db->SaveExplorerSettings(
            {
                .name = label + "-settings",
                .created_at_utc = now,
                .correlation_id = "test.battle_end_results." + label,
                .causation_id = "test",
            },
            &explorer_settings_id,
            &error)) {
        SetError(error_out, "save explorer settings: " + error);
        return std::nullopt;
    }
    if (!analysis_db->CreateBattleSet(
            {
                .name = label + "-battle-set",
                .entry_savestate_id = out.input_savestate_id,
                .battle_run_spec_id = battle_run_spec_id,
                .explorer_settings_id = explorer_settings_id,
                .status = BattleSetStatus::Victory,
                .created_at_utc = now,
                .correlation_id = "test.battle_end_results." + label,
                .causation_id = "test",
            },
            &out.battle_set_id,
            &error)) {
        SetError(error_out, "create battle set: " + error);
        return std::nullopt;
    }
    std::int64_t seed_candidate_id = 0;
    if (!analysis_db->AddBattleSeedCandidate(
            {
                .battle_set_id = out.battle_set_id,
                .seed_value = 12345,
                .source_kind = BattleSeedCandidateSourceKind::Synthetic,
                .candidate_status = BattleSeedCandidateStatus::Ready,
                .created_at_utc = now,
                .correlation_id = "test.battle_end_results." + label,
                .causation_id = "test",
            },
            &seed_candidate_id,
            &error)) {
        SetError(error_out, "create seed candidate: " + error);
        return std::nullopt;
    }
    if (!analysis_db->CreateBattleTurnWave(
            {
                .battle_set_id = out.battle_set_id,
                .turn_index = 1,
                .seed_candidate_id = seed_candidate_id,
                .status = BattleTurnWaveStatus::Completed,
                .created_at_utc = now,
                .completed_at_utc = now,
                .correlation_id = "test.battle_end_results." + label,
                .causation_id = "test",
            },
            &out.wave_id,
            &error)) {
        SetError(error_out, "create turn wave: " + error);
        return std::nullopt;
    }
    if (!analysis_db->RecordBattleTurnJob(
            {
                .wave_id = out.wave_id,
                .plan_id = 1,
                .source_savestate_id = out.input_savestate_id,
                .seed_candidate_id = seed_candidate_id,
                .fake_attacks_this_turn = 0,
                .fake_attacks_used_before = 0,
                .job_state = BattleTurnJobState::Succeeded,
                .started_at_utc = now,
                .ended_at_utc = now,
                .has_results = true,
                .battle_outcome = savor::battle::Outcome::Victory,
                .output_savestate_id = out.savestate_id,
                .recorded_at_utc = now,
                .correlation_id = "test.battle_end_results." + label,
                .causation_id = "test",
            },
            &out.turn_job_id,
            &error)) {
        SetError(error_out, "record turn job: " + error);
        return std::nullopt;
    }

    std::int64_t source_job_set_id = 0;
    if (!execution_db->CreateJobSet(
            {
                .program_kind = source_program_kind,
                .purpose = "source battle turn",
                .created_by = std::string("battle-end-results-test"),
                .expected_total = 1,
                .domain_ref_kind = std::string("analysis_battle.turn_job"),
                .domain_ref_id = out.turn_job_id,
            },
            &source_job_set_id,
            &error)) {
        SetError(error_out, "create source job set: " + error);
        return std::nullopt;
    }
    IniDoc source_input;
    source_input.set("BattleSingleTurn.Job", "run_ms_override", "60000");
    if (!execution_db->EnqueueJob(
            {
                .job_set_id = source_job_set_id,
                .program_kind = source_program_kind,
                .program_version = 1,
                .program_ref_kind = "analysis_battle.turn_job",
                .program_ref_id = out.turn_job_id,
                .savestate_id = out.input_savestate_id,
                .fingerprint = "source-turn-" + label,
                .input_ini = source_input.to_string_sorted(),
            },
            &out.exec_job_id,
            &error)) {
        SetError(error_out, "enqueue source job: " + error);
        return std::nullopt;
    }
    if (!execution_db->JobCommandService()->AppendLifecycleEvent(
            {
                .kind = JobLifecycleEventKind::JobCompleted,
                .job_id = out.exec_job_id,
                .terminal_state = std::string("SUCCEEDED"),
                .requested_by = "battle-end-results-test",
            },
            &error)) {
        SetError(error_out, "complete source job: " + error);
        return std::nullopt;
    }
    if (!analysis_db->SetBattleTurnJobExecJobId(out.turn_job_id, out.exec_job_id, &error)) {
        SetError(error_out, "link turn job to source exec job: " + error);
        return std::nullopt;
    }
    if (error_out != nullptr) error_out->clear();
    return out;
}

std::optional<std::int64_t> SaveBattleEndResultsGraph(
    savor::db::IAuthoringDb* authoring_db,
    const std::string& label,
    std::string* error_out) {
    if (authoring_db == nullptr) {
        SetError(error_out, "authoring db unavailable");
        return std::nullopt;
    }
    std::int64_t seed_probe_spec_id = 0;
    if (!authoring_db->SaveSeedProbeSpec(
            {
                .name = label + " field-return seed probe",
                .priority = 1,
                .run_ms = 60000,
                .vi_stall_ms = 2000,
                .min_value = 47,
                .max_value = 207,
                .cap_trigger_top = true,
                .ignore_trigger_minmax = true,
                .combo_attempts_per_target = 20,
                .combo_sampler_tries = 4,
                .created_at_utc = savor::db::types::UtcNow(),
                .correlation_id = "test.battle_end.seedprobe",
                .causation_id = "test",
            },
            &seed_probe_spec_id,
            error_out)) {
        return std::nullopt;
    }
    savor::db::SaveWorkflowGraphResult saved{};
    if (!authoring_db->SaveWorkflowGraph(
            {
                .name = label,
                .description = "battle-end/results coordinator acceptance",
                .hidden = true,
                .graph_version = 2,
                .graph_hash = label + "-hash",
                .nodes = {
                    {
                        .node_key = "completion",
                        .unit_kind = "battle_completion",
                        .display_name = "Battle Completion",
                        .inputs = {
                            {
                                .input_key = "entry_savestate",
                                .data_kind = "state.savestate_id",
                                .display_name = "Battle Victory savestate",
                            },
                        },
                        .possible_outputs = {{.output_key="completion",.data_kind="analysis_battle.battle_completion_id",.display_name="Completion"}},
                    },
                    {
                        .node_key = "seed",
                        .unit_kind = "field_return_seed_probe",
                        .display_name = "Field Return Seed Probe",
                        .authored_ref_kind = std::string("seed_probe_spec"),
                        .authored_ref_id = seed_probe_spec_id,
                        .inputs = {{.input_key="completion",.data_kind="analysis_battle.battle_completion_id",.display_name="Completion"}},
                        .possible_outputs = {{.output_key="seeded_savestate",.data_kind="state.savestate_id",.display_name="Seeded state"}},
                    },
                    {
                        .node_key = "results",
                        .unit_kind = "battle_results_screen",
                        .display_name = "Battle Results Screen",
                        .inputs = {
                            {.input_key="completion",.data_kind="analysis_battle.battle_completion_id",.display_name="Completion"},
                            {.input_key="seeded_savestate",.data_kind="state.savestate_id",.display_name="Seeded state"},
                        },
                        .possible_outputs = {{.output_key="terminal_savestate",.data_kind="state.savestate_id",.display_name="Terminal state"}},
                    },
                },
                .edges = {
                    {.from_node_key="completion",.output_key="completion",.to_node_key="seed",.input_key="completion"},
                    {.from_node_key="completion",.output_key="completion",.to_node_key="results",.input_key="completion"},
                    {.from_node_key="seed",.output_key="seeded_savestate",.to_node_key="results",.input_key="seeded_savestate"},
                },
                .created_at_utc = savor::db::types::UtcNow(),
                .correlation_id = "test.battle_end_results.graph",
                .causation_id = "test",
            },
            &saved,
            error_out)) {
        return std::nullopt;
    }
    return saved.workflow_graph_revision_id;
}

std::optional<std::int64_t> CreateBattleEndResultsWorkflow(
    savor::db::IAuthoringDb* authoring_db,
    savor::db::IExecutionDb* execution_db,
    std::int64_t graph_revision_id,
    std::int64_t source_savestate_id,
    const std::string& created_by,
    std::string* error_out) {
    using namespace savor::db::execution::workflow;
    if (authoring_db == nullptr || execution_db == nullptr) {
        SetError(error_out, "authoring or execution db unavailable");
        return std::nullopt;
    }
    const auto graph = authoring_db->GetWorkflowGraphRevision(graph_revision_id);
    if (!graph.has_value()) {
        SetError(error_out, "workflow graph unavailable");
        return std::nullopt;
    }
    const auto seed_node = std::find_if(graph->nodes.begin(), graph->nodes.end(), [](const auto& node) {
        return node.node_key == "seed" && node.unit_kind == "field_return_seed_probe";
    });
    if (seed_node == graph->nodes.end() || seed_node->authored_ref_kind != std::optional<std::string>("seed_probe_spec")
        || !seed_node->authored_ref_id.has_value() || *seed_node->authored_ref_id <= 0) {
        SetError(error_out, "field-return seed node is missing its authored spec");
        return std::nullopt;
    }
    const auto unit_registry = BuildDefaultWorkflowUnitRegistry();
    std::string activation_error;
    auto completion = BuildUnitActivationSpecFromDefinition(
        unit_registry,
        "completion", "completion", "battle_completion", "Battle Completion",
        std::nullopt,
        std::nullopt,
        {},
        &activation_error);
    if (!completion.has_value()) {
        SetError(error_out, "build activation: " + activation_error);
        return std::nullopt;
    }
    auto seed = BuildUnitActivationSpecFromDefinition(
        unit_registry,
        "seed", "seed", "field_return_seed_probe", "Field Return Seed Probe",
        seed_node->authored_ref_kind,
        seed_node->authored_ref_id,
        {"completion"},
        &activation_error);
    if (!seed.has_value()) {
        SetError(error_out, "build seed activation: " + activation_error);
        return std::nullopt;
    }
    auto results = BuildUnitActivationSpecFromDefinition(
        unit_registry,
        "results", "results", "battle_results_screen", "Battle Results Screen",
        std::nullopt,
        std::nullopt,
        {"completion", "seed"},
        &activation_error);
    if (!results.has_value()) {
        SetError(error_out, "build results activation: " + activation_error);
        return std::nullopt;
    }

    WorkflowCreateInstanceCommand command{};
    command.workflow_kind = "workflow_graph";
    command.root_scope_kind = "manual";
    command.root_scope_id = source_savestate_id;
    command.workflow_graph_revision_id = graph_revision_id;
    command.created_by = created_by;
    command.created_at_utc = savor::db::types::UtcNow().time_since_epoch().count();
    command.unit_activations.push_back(std::move(*completion));
    command.unit_activations.push_back(std::move(*seed));
    command.unit_activations.push_back(std::move(*results));
    command.input_bindings.push_back({
        .node_key = "completion",
        .input_key = "entry_savestate",
        .data_kind = "state.savestate_id",
        .ref_kind = "state.savestate",
        .ref_id = source_savestate_id,
        .source_kind = "external",
    });
    command.arguments.push_back({
        .node_key = "seed",
        .argument_key = "seed_selector",
        .value_type = "text",
        .text_value = std::string("neutral"),
        .source_kind = "test",
    });
    std::int64_t workflow_instance_id = 0;
    if (!execution_db->CreateWorkflowInstance(command, &workflow_instance_id, error_out)) {
        return std::nullopt;
    }
    return workflow_instance_id;
}

std::int64_t QueryInt64(sqlite3* db, const std::string& sql) {
    sqlite3_stmt* statement = nullptr;
    if (db == nullptr || sqlite3_prepare_v2(db, sql.c_str(), -1, &statement, nullptr) != SQLITE_OK) {
        return -1;
    }
    const auto rc = sqlite3_step(statement);
    const auto value = rc == SQLITE_ROW ? sqlite3_column_int64(statement, 0) : -1;
    sqlite3_finalize(statement);
    return value;
}

std::string QueryText(sqlite3* db, const std::string& sql) {
    sqlite3_stmt* statement = nullptr;
    if (db == nullptr || sqlite3_prepare_v2(db, sql.c_str(), -1, &statement, nullptr) != SQLITE_OK) {
        return {};
    }
    std::string value;
    if (sqlite3_step(statement) == SQLITE_ROW) {
        const auto* text = sqlite3_column_text(statement, 0);
        if (text != nullptr) value = reinterpret_cast<const char*>(text);
    }
    sqlite3_finalize(statement);
    return value;
}

bool WaitUntil(const std::function<bool()>& predicate, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return predicate();
}

TEST(BattleEndResultsDb, WorkflowUnitsExposeTheThreeHiddenBattleEndPhases) {
    const auto registry = savor::db::execution::workflow::BuildDefaultWorkflowUnitRegistry();
    const auto* completion = registry.Find("battle_completion");
    const auto* seed = registry.Find("field_return_seed_probe");
    const auto* results = registry.Find("battle_results_screen");
    ASSERT_NE(completion, nullptr);
    ASSERT_NE(seed, nullptr);
    ASSERT_NE(results, nullptr);
    EXPECT_EQ(registry.Find("battle_end_results"), nullptr);

    EXPECT_TRUE(completion->hidden);
    ASSERT_EQ(completion->required_inputs.size(), 1u);
    EXPECT_EQ(completion->required_inputs[0].key, "entry_savestate");
    ASSERT_EQ(completion->possible_outputs.size(), 1u);
    EXPECT_EQ(completion->possible_outputs[0].key, "completion");
    EXPECT_EQ(completion->possible_outputs[0].data_kind, "analysis_battle.battle_completion_id");
    EXPECT_EQ(completion->internal_step_kinds, std::vector<std::string>({"battle.completion"}));

    EXPECT_TRUE(seed->hidden);
    ASSERT_EQ(seed->required_inputs.size(), 1u);
    EXPECT_EQ(seed->required_inputs[0].data_kind, "analysis_battle.battle_completion_id");
    ASSERT_EQ(seed->possible_outputs.size(), 1u);
    EXPECT_EQ(seed->possible_outputs[0].key, "seeded_savestate");
    EXPECT_EQ(seed->internal_step_kinds, std::vector<std::string>({
        "battle.field_return_seed_probe",
        "battle.field_return_seed_probe.grid",
        "battle.field_return_seed_probe.unique",
        "battle.field_return_seed_probe.materialize",
    }));

    EXPECT_TRUE(results->hidden);
    ASSERT_EQ(results->required_inputs.size(), 2u);
    EXPECT_EQ(results->required_inputs[0].data_kind, "analysis_battle.battle_completion_id");
    EXPECT_EQ(results->required_inputs[1].data_kind, "state.savestate_id");
    ASSERT_EQ(results->possible_outputs.size(), 1u);
    EXPECT_EQ(results->possible_outputs[0].key, "terminal_savestate");
    EXPECT_EQ(results->internal_step_kinds, std::vector<std::string>({"battle.results_screen"}));
}

TEST(BattleEndResultsDb, DescriptorsSeparateGraphEntrypointsFromDynamicSeedSteps) {
    using namespace savor::db::execution::programdb;
    using namespace savor::db::execution::programdb::battleend;

    ProgramKindRegistry registry;
    RegisterBattleEndWorkflowPhaseDescriptors(&registry, nullptr, nullptr, nullptr);
    const auto* completion = registry.FindForStepKind("battle.completion");
    const auto* neutral = registry.FindForStepKind("battle.field_return_seed_probe");
    const auto* grid = registry.FindForStepKind("battle.field_return_seed_probe.grid");
    const auto* unique = registry.FindForStepKind("battle.field_return_seed_probe.unique");
    const auto* materialize = registry.FindForStepKind("battle.field_return_seed_probe.materialize");
    const auto* results = registry.FindForStepKind("battle.results_screen");
    ASSERT_NE(completion, nullptr);
    ASSERT_NE(neutral, nullptr);
    ASSERT_NE(grid, nullptr);
    ASSERT_NE(unique, nullptr);
    ASSERT_NE(materialize, nullptr);
    ASSERT_NE(results, nullptr);
    EXPECT_EQ(registry.FindForStepKind("battle.end_results"), nullptr);
    EXPECT_EQ(completion->program_name, "BattleCompletionRunner");
    EXPECT_EQ(results->program_name, "BattleResultsScreenRunner");
    for (const auto* descriptor : {completion, neutral, results}) {
        EXPECT_EQ(descriptor->job_persistence, nullptr);
        EXPECT_NE(descriptor->graph_job_persistence, nullptr);
        EXPECT_TRUE(descriptor->supports_workflow_orchestration);
    }
    for (const auto* descriptor : {grid, unique, materialize}) {
        EXPECT_NE(descriptor->job_persistence, nullptr);
        EXPECT_EQ(descriptor->graph_job_persistence, nullptr);
        EXPECT_TRUE(descriptor->supports_workflow_orchestration);
    }

    const auto transition = completion->workflow_transition->EvaluateTransition({});
    EXPECT_TRUE(transition.should_advance);
    EXPECT_FALSE(transition.terminal_failure);
    EXPECT_FALSE(transition.next_step_key.has_value());
    EXPECT_TRUE(transition.spawn_steps.empty());
    savor::db::execution::programdb::WorkflowTransitionContext failed_context{};
    failed_context.failed_total = 1;
    const auto failed = results->workflow_transition->EvaluateTransition(failed_context);
    EXPECT_FALSE(failed.should_advance);
    EXPECT_TRUE(failed.terminal_failure);
}

TEST(BattleEndResultsDb, RuntimeMaterializationRejectsDirectlyEnqueuedJobWithoutWorkflowProvenance) {
    using namespace savor::db::execution::programdb::battleend;

    const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto source_path = std::filesystem::temp_directory_path()
        / ("savor-battle-end-results-provenance-" + std::to_string(unique) + ".sav");
    {
        std::ofstream source(source_path, std::ios::binary | std::ios::trunc);
        source << "victory-savestate";
        ASSERT_TRUE(source.good());
    }
    struct RemoveOnExit {
        std::filesystem::path path;
        ~RemoveOnExit() { std::error_code ignored; std::filesystem::remove(path, ignored); }
    } cleanup{source_path};

    ScopedSqliteDb state_handle;
    state_handle.Exec(
        "CREATE TABLE state_artifact(artifact_id INTEGER,sha256 TEXT,size_bytes INTEGER,filename TEXT,file_ext TEXT,artifact_kind TEXT);"
        "CREATE TABLE state_savestate(savestate_id INTEGER,artifact_id INTEGER,savestate_type TEXT,note TEXT,is_complete INTEGER,created_at_utc INTEGER);");
    const auto source_hash = hash::sha256_of_file(source_path.string());
    const auto filename = source_path.generic_string();
    state_handle.Exec((
        "INSERT INTO state_artifact VALUES(5,'" + source_hash + "',16,'" + filename + "','.sav','SAV');"
        "INSERT INTO state_savestate VALUES(9,5,'BATTLE','Victory',1,1000);").c_str());
    savor::db::state::SqliteStateDb state_db(state_handle.get());

    RecordingExecutionDb execution_db;
    std::int64_t job_set_id = 0;
    ASSERT_TRUE(execution_db.CreateJobSet(
        {
            .program_kind = static_cast<std::int32_t>(savor::PK_BattleEndResultsRunner),
            .purpose = "forged direct launch",
        },
        &job_set_id));

    IniDoc input;
    input.set("BattleEndResults.Job", "source_turn_job_id", "77");
    input.set("BattleEndResults.Job", "source_exec_job_id", "88");
    input.set("BattleEndResults.Job", "source_wave_id", "99");
    input.set("BattleEndResults.Job", "source_battle_set_id", "111");
    input.set("BattleEndResults.Job", "source_savestate_id", "9");
    input.set("BattleEndResults.Job", "source_artifact_id", "5");
    input.set("BattleEndResults.Job", "source_artifact_sha256", source_hash);
    input.set("BattleEndResults.Job", "run_timeout_ms", "60000");
    input.set("BattleEndResults.Job", "acceleration_policy", "1");

    std::int64_t job_id = 0;
    ASSERT_TRUE(execution_db.EnqueueJob(
        {
            .job_set_id = job_set_id,
            .program_kind = static_cast<std::int32_t>(savor::PK_BattleEndResultsRunner),
            .program_version = 1,
            .program_ref_kind = "analysis_battle.turn_job",
            .program_ref_id = 77,
            .savestate_id = 9,
            .fingerprint = "forged-direct-fingerprint",
            .input_ini = input.to_string_sorted(),
        },
        &job_id));

    const auto descriptor = BuildBattleEndResultsDescriptor(
        &execution_db,
        &state_db,
        nullptr,
        BattleEndResultsPhaseRegistrationConfig{
            .working_dir_root = std::filesystem::temp_directory_path(),
        });
    ASSERT_NE(descriptor.runtime_init, nullptr);
    const auto init = descriptor.runtime_init->BuildRuntimeInit(job_id);
    EXPECT_FALSE(descriptor.runtime_init->MaterializePsJob(job_id, init).has_value());
}

TEST(BattleEndResultsDb, AnalysisLookupReturnsEveryJobWithTheChosenOutputSavestate) {
    ScopedSqliteDb db;
    db.Exec(
        "CREATE TABLE ab_turn_job("
        "turn_job_id INTEGER,wave_id INTEGER,exec_job_id INTEGER,plan_id INTEGER,source_savestate_id INTEGER,"
        "seed_candidate_id INTEGER,authored_plan_id INTEGER,authored_turn_index INTEGER,resolved_turn_commands_blob TEXT,"
        "resolved_turn_variant_key TEXT,fake_attacks_this_turn INTEGER,fake_attacks_used_before INTEGER,job_state TEXT,"
        "started_at_utc INTEGER,ended_at_utc INTEGER,has_results INTEGER,vi_start INTEGER,vi_end INTEGER,delta_vi INTEGER,"
        "rng_seed INTEGER,battle_outcome INTEGER,plan_materialize_err INTEGER,pred_passed INTEGER,pred_total INTEGER,"
        "pred_abort_run INTEGER,output_savestate_id INTEGER,applied_input_artifact_id INTEGER,input_trace_artifact_id INTEGER,"
        "result_context_blob_base64 TEXT,result_context_version INTEGER,recorded_at_utc INTEGER);"
        "INSERT INTO ab_turn_job VALUES"
        "(1,10,100,20,30,40,50,1,'commands','variant',0,0,'SUCCEEDED',1,2,1,3,4,1,5,2,0,1,1,0,77,NULL,NULL,NULL,NULL,2),"
        "(2,11,101,21,31,41,51,1,'commands2','variant2',0,0,'SUCCEEDED',1,2,1,3,4,1,5,2,0,1,1,0,77,NULL,NULL,NULL,NULL,2),"
        "(3,12,102,22,32,42,52,1,'commands3','variant3',0,0,'SUCCEEDED',1,2,1,3,4,1,5,2,0,1,1,0,88,NULL,NULL,NULL,NULL,2);");

    savor::db::analysis::SqliteAnalysisDb analysis(db.get());
    const auto matches = analysis.ListBattleTurnJobsByOutputSavestateId(77);
    ASSERT_EQ(matches.size(), 2u);
    EXPECT_EQ(matches[0].turn_job_id, 1);
    EXPECT_EQ(matches[1].turn_job_id, 2);
    EXPECT_EQ(matches[0].job_state, savor::db::BattleTurnJobState::Succeeded);
    EXPECT_EQ(matches[0].output_savestate_id, 77);
    EXPECT_TRUE(analysis.ListBattleTurnJobsByOutputSavestateId(0).empty());
}

TEST(BattleEndResultsDb, StateLookupIncludesCompletionAndFrozenArtifactIdentity) {
    ScopedSqliteDb db;
    db.Exec(
        "CREATE TABLE state_artifact(artifact_id INTEGER,sha256 TEXT,size_bytes INTEGER,filename TEXT,file_ext TEXT,artifact_kind TEXT);"
        "CREATE TABLE state_savestate(savestate_id INTEGER,artifact_id INTEGER,savestate_type TEXT,note TEXT,is_complete INTEGER,created_at_utc INTEGER);"
        "INSERT INTO state_artifact VALUES(5,'abc123',1234,'C:/state/victory.sav','.sav','SAV');"
        "INSERT INTO state_savestate VALUES(9,5,'BATTLE','Victory',1,1000);");

    savor::db::state::SqliteStateDb state(db.get());
    const auto row = state.GetSavestate(9);
    ASSERT_TRUE(row.has_value());
    EXPECT_EQ(row->savestate_id, 9);
    EXPECT_EQ(row->artifact_id, 5);
    EXPECT_TRUE(row->is_complete);
    EXPECT_EQ(row->artifact_sha256, "abc123");
    EXPECT_EQ(row->artifact_size_bytes, 1234);
    EXPECT_EQ(row->artifact_filename, "C:/state/victory.sav");
    EXPECT_EQ(row->artifact_kind, "SAV");
    EXPECT_FALSE(state.GetSavestate(10).has_value());
}

class BattleEndResultsSqliteDbFixture : public SqliteDbFixture {
};

TEST_F(BattleEndResultsSqliteDbFixture, AggregateBindingAndTerminalTransitionsAreIdempotentAndFailClosed) {
    using namespace savor::db;
    using namespace savor::db::execution::workflow;

    std::string error;
    const auto source = SeedVictorySource(
        db_service_.get(), temp_root_, "aggregate-matrix", savor::PK_BattleSingleTurnRunner, &error);
    ASSERT_TRUE(source.has_value()) << error;
    const auto graph_revision_id = SaveBattleEndResultsGraph(
        db_service_->AuthoringDb(), "aggregate-matrix", &error);
    ASSERT_TRUE(graph_revision_id.has_value()) << error;
    const auto workflow_instance_id = CreateBattleEndResultsWorkflow(
        db_service_->AuthoringDb(),
        db_service_->ExecutionDb(),
        *graph_revision_id,
        source->savestate_id,
        "aggregate-matrix",
        &error);
    ASSERT_TRUE(workflow_instance_id.has_value()) << error;

    ASSERT_TRUE(db_service_->ExecutionDb()->WorkflowCommandService()->AppendDynamicSteps(
        {
            .workflow_instance_id = *workflow_instance_id,
            .steps = {
                {.step_key="matrix/completion-fail",.step_kind="battle.completion",.max_attempts=1},
                {.step_key="matrix/completion-success",.step_kind="battle.completion",.max_attempts=1},
                {.step_key="matrix/results-success",.step_kind="battle.results_screen",.max_attempts=1},
                {.step_key="matrix/results-bad-codec",.step_kind="battle.results_screen",.max_attempts=1},
                {.step_key="matrix/results-bad-flavor",.step_kind="battle.results_screen",.max_attempts=1},
            },
            .requested_by = "aggregate-matrix",
        },
        &error)) << error;
    const auto step_id = [&](const std::string& key) {
        return QueryInt64(
            db_,
            "SELECT workflow_step_id FROM exec_workflow_step WHERE workflow_instance_id="
                + std::to_string(*workflow_instance_id) + " AND step_key='" + key + "';");
    };
    const auto original_completion_step = step_id("completion");
    const auto original_results_step = step_id("results");
    const auto completion_fail_step = step_id("matrix/completion-fail");
    const auto completion_success_step = step_id("matrix/completion-success");
    const auto results_success_step = step_id("matrix/results-success");
    const auto results_bad_codec_step = step_id("matrix/results-bad-codec");
    const auto results_bad_flavor_step = step_id("matrix/results-bad-flavor");
    ASSERT_GT(original_completion_step, 0);
    ASSERT_GT(original_results_step, 0);
    ASSERT_GT(completion_fail_step, 0);
    ASSERT_GT(completion_success_step, 0);
    ASSERT_GT(results_success_step, 0);
    ASSERT_GT(results_bad_codec_step, 0);
    ASSERT_GT(results_bad_flavor_step, 0);

    auto* analysis = db_service_->AnalysisDb();
    ASSERT_NE(analysis, nullptr);
    const auto now = savor::db::types::UtcNow();
    const auto create_completion = [&](std::int64_t workflow_step_id) {
        std::int64_t id = 0;
        EXPECT_TRUE(analysis->CreateBattleCompletion(
            {
                .workflow_instance_id = *workflow_instance_id,
                .workflow_step_id = workflow_step_id,
                .entry_savestate_id = source->savestate_id,
                .status = "QUEUED",
                .created_at_utc = now,
                .correlation_id = "aggregate-matrix",
                .causation_id = "test",
            },
            &id,
            &error)) << error;
        return id;
    };

    const auto failed_completion_id = create_completion(completion_fail_step);
    ASSERT_GT(failed_completion_id, 0);
    BindBattleCompletionExecutionJobCommand completion_binding{
        .battle_completion_id = failed_completion_id,
        .workflow_instance_id = *workflow_instance_id,
        .workflow_step_id = completion_fail_step,
        .exec_job_id = 700001,
    };
    EXPECT_TRUE(analysis->BindBattleCompletionExecutionJob(completion_binding, &error)) << error;
    EXPECT_TRUE(analysis->BindBattleCompletionExecutionJob(completion_binding, &error)) << error;
    auto conflicting_completion_binding = completion_binding;
    conflicting_completion_binding.exec_job_id += 1;
    EXPECT_FALSE(analysis->BindBattleCompletionExecutionJob(conflicting_completion_binding, &error));
    auto wrong_completion_identity = completion_binding;
    wrong_completion_identity.workflow_step_id = original_completion_step;
    EXPECT_FALSE(analysis->BindBattleCompletionExecutionJob(wrong_completion_identity, &error));

    FailBattleCompletionCommand fail_completion{
        .battle_completion_id = failed_completion_id,
        .manifest_version = 1,
        .manifest_blob = std::string("failed-manifest"),
        .mismatch_count = 2,
        .invariant_failure_count = 3,
        .completed_at_utc = now,
        .correlation_id = "aggregate-matrix",
        .causation_id = "test",
    };
    EXPECT_TRUE(analysis->FailBattleCompletion(fail_completion, &error)) << error;
    EXPECT_TRUE(analysis->FailBattleCompletion(fail_completion, &error)) << error;
    EXPECT_FALSE(analysis->BindBattleCompletionExecutionJob(completion_binding, &error));
    EXPECT_FALSE(analysis->CompleteBattleCompletion(
        {
            .battle_completion_id = failed_completion_id,
            .completion_savestate_id = source->savestate_id,
            .manifest_version = 1,
            .manifest_blob = "success-manifest",
            .status = "COMPLETED",
            .completed_at_utc = now,
            .correlation_id = "aggregate-matrix",
            .causation_id = "test",
        },
        &error));

    const auto unbound_failed_completion_id = create_completion(original_completion_step);
    ASSERT_GT(unbound_failed_completion_id, 0);
    auto unbound_fail_completion = fail_completion;
    unbound_fail_completion.battle_completion_id = unbound_failed_completion_id;
    unbound_fail_completion.manifest_blob = std::string("unbound-failure");
    EXPECT_TRUE(analysis->FailBattleCompletion(unbound_fail_completion, &error)) << error;
    EXPECT_TRUE(analysis->FailBattleCompletion(unbound_fail_completion, &error)) << error;

    const auto successful_completion_id = create_completion(completion_success_step);
    ASSERT_GT(successful_completion_id, 0);
    CompleteBattleCompletionCommand complete_completion{
        .battle_completion_id = successful_completion_id,
        .completion_savestate_id = source->savestate_id,
        .manifest_version = 1,
        .manifest_blob = "completed-manifest",
        .status = "COMPLETED",
        .completed_at_utc = now,
        .correlation_id = "aggregate-matrix",
        .causation_id = "test",
    };
    EXPECT_FALSE(analysis->CompleteBattleCompletion(complete_completion, &error));
    BindBattleCompletionExecutionJobCommand successful_completion_binding{
        .battle_completion_id = successful_completion_id,
        .workflow_instance_id = *workflow_instance_id,
        .workflow_step_id = completion_success_step,
        .exec_job_id = 700010,
    };
    ASSERT_TRUE(analysis->BindBattleCompletionExecutionJob(successful_completion_binding, &error)) << error;
    ASSERT_TRUE(analysis->CompleteBattleCompletion(complete_completion, &error)) << error;
    EXPECT_TRUE(analysis->CompleteBattleCompletion(complete_completion, &error)) << error;
    auto fail_after_completion = fail_completion;
    fail_after_completion.battle_completion_id = successful_completion_id;
    EXPECT_FALSE(analysis->FailBattleCompletion(fail_after_completion, &error));
    EXPECT_FALSE(analysis->BindBattleCompletionExecutionJob(successful_completion_binding, &error));

    const auto seed_spec = db_service_->AuthoringDb()->GetWorkflowGraphRevision(*graph_revision_id);
    ASSERT_TRUE(seed_spec.has_value());
    const auto seed_node = std::find_if(seed_spec->nodes.begin(), seed_spec->nodes.end(), [](const auto& node) {
        return node.node_key == "seed";
    });
    ASSERT_NE(seed_node, seed_spec->nodes.end());
    ASSERT_TRUE(seed_node->authored_ref_id.has_value());
    struct SeedRef {
        std::int64_t neutral_seed_id = 0;
        std::int64_t value = 0;
    };
    const auto create_seed_ref = [&](const std::string& flavor, int codec, const std::string& label) {
        SeedRef ref{};
        std::int64_t probe_set_id = 0;
        EXPECT_TRUE(analysis->CreateSeedProbeSet(
            {
                .name = "aggregate-matrix-" + label,
                .probe_flavor = flavor,
                .breakpoint_policy_name = "test",
                .segment_source_kind = "test",
                .created_at_utc = now,
                .correlation_id = "aggregate-matrix",
                .causation_id = "test",
            },
            &probe_set_id,
            &error)) << error;
        std::int64_t probe_run_id = 0;
        EXPECT_TRUE(analysis->RequestSeedProbeRun(
            {
                .probe_set_id = probe_set_id,
                .entry_savestate_id = source->savestate_id,
                .seed_probe_spec_id = *seed_node->authored_ref_id,
                .launch_samples_per_axis = 1,
                .codec_version = codec,
                .status = "REQUESTED",
                .requested_at_utc = now,
                .correlation_id = "aggregate-matrix",
                .causation_id = "test",
            },
            &probe_run_id,
            &error)) << error;
        const auto probe_result_id = analysis->LookupSeedProbeResultId(probe_run_id);
        EXPECT_TRUE(probe_result_id.has_value());
        bool inserted = false;
        ref.value = 12345;
        EXPECT_TRUE(analysis->EnsureSeedProbeNeutralSeed(
            {
                .probe_result_id = probe_result_id.value_or(0),
                .neutral_seed_value = ref.value,
                .source_kind = "CALCULATED",
                .recorded_at_utc = now,
                .correlation_id = "aggregate-matrix",
                .causation_id = "test",
            },
            &inserted,
            &ref.neutral_seed_id,
            &error)) << error;
        return ref;
    };
    const auto valid_seed = create_seed_ref("FIELD_RETURN", 2, "valid");
    const auto bad_codec_seed = create_seed_ref("FIELD_RETURN", 1, "bad-codec");
    const auto bad_flavor_seed = create_seed_ref("BATTLE_PRE", 2, "bad-flavor");
    ASSERT_GT(valid_seed.neutral_seed_id, 0);
    ASSERT_GT(bad_codec_seed.neutral_seed_id, 0);
    ASSERT_GT(bad_flavor_seed.neutral_seed_id, 0);

    const auto make_results_command = [&](std::int64_t workflow_step_id, const SeedRef& seed) {
        CreateBattleResultsCommand command{};
        command.battle_completion_id = successful_completion_id;
        command.workflow_instance_id = *workflow_instance_id;
        command.workflow_step_id = workflow_step_id;
        command.selected_seed_ref_kind = "analysisseedprobe.neutral_seed";
        command.selected_seed_ref_id = seed.neutral_seed_id;
        command.entry_savestate_id = source->savestate_id;
        command.selected_seed_value = seed.value;
        command.rng_effect_kind = RngEffectKind::Preserve;
        command.fixed_draw_count = 0;
        command.status = "QUEUED";
        command.created_at_utc = now;
        command.correlation_id = "aggregate-matrix";
        command.causation_id = "test";
        return command;
    };
    std::int64_t ignored_results_id = 0;
    EXPECT_FALSE(analysis->CreateBattleResults(
        make_results_command(results_bad_codec_step, bad_codec_seed), &ignored_results_id, &error));
    EXPECT_FALSE(analysis->CreateBattleResults(
        make_results_command(results_bad_flavor_step, bad_flavor_seed), &ignored_results_id, &error));

    std::int64_t failed_results_id = 0;
    ASSERT_TRUE(analysis->CreateBattleResults(
        make_results_command(original_results_step, valid_seed), &failed_results_id, &error)) << error;
    CompleteBattleResultsCommand complete_results{
        .battle_results_id = failed_results_id,
        .final_savestate_id = source->savestate_id,
        .entry_rng_seed = valid_seed.value,
        .final_rng_seed = valid_seed.value,
        .status = "COMPLETED",
        .completed_at_utc = now,
        .correlation_id = "aggregate-matrix",
        .causation_id = "test",
    };
    EXPECT_FALSE(analysis->CompleteBattleResults(complete_results, &error));
    BindBattleResultsExecutionJobCommand results_binding{
        .battle_results_id = failed_results_id,
        .workflow_instance_id = *workflow_instance_id,
        .workflow_step_id = original_results_step,
        .exec_job_id = 800001,
    };
    EXPECT_TRUE(analysis->BindBattleResultsExecutionJob(results_binding, &error)) << error;
    EXPECT_TRUE(analysis->BindBattleResultsExecutionJob(results_binding, &error)) << error;
    auto conflicting_results_binding = results_binding;
    conflicting_results_binding.exec_job_id += 1;
    EXPECT_FALSE(analysis->BindBattleResultsExecutionJob(conflicting_results_binding, &error));
    auto wrong_results_identity = results_binding;
    wrong_results_identity.workflow_step_id = results_success_step;
    EXPECT_FALSE(analysis->BindBattleResultsExecutionJob(wrong_results_identity, &error));
    FailBattleResultsCommand fail_results{
        .battle_results_id = failed_results_id,
        .entry_rng_seed = valid_seed.value,
        .final_rng_seed = valid_seed.value,
        .mismatch_count = 4,
        .invariant_failure_count = 5,
        .completed_at_utc = now,
        .correlation_id = "aggregate-matrix",
        .causation_id = "test",
    };
    EXPECT_TRUE(analysis->FailBattleResults(fail_results, &error)) << error;
    EXPECT_TRUE(analysis->FailBattleResults(fail_results, &error)) << error;
    EXPECT_FALSE(analysis->CompleteBattleResults(complete_results, &error));
    EXPECT_FALSE(analysis->BindBattleResultsExecutionJob(results_binding, &error));

    std::int64_t unbound_failed_results_id = 0;
    ASSERT_TRUE(analysis->CreateBattleResults(
        make_results_command(results_bad_codec_step, valid_seed),
        &unbound_failed_results_id,
        &error)) << error;
    auto unbound_fail_results = fail_results;
    unbound_fail_results.battle_results_id = unbound_failed_results_id;
    unbound_fail_results.mismatch_count = 6;
    EXPECT_TRUE(analysis->FailBattleResults(unbound_fail_results, &error)) << error;
    EXPECT_TRUE(analysis->FailBattleResults(unbound_fail_results, &error)) << error;

    std::int64_t successful_results_id = 0;
    ASSERT_TRUE(analysis->CreateBattleResults(
        make_results_command(results_success_step, valid_seed), &successful_results_id, &error)) << error;
    BindBattleResultsExecutionJobCommand successful_results_binding{
        .battle_results_id = successful_results_id,
        .workflow_instance_id = *workflow_instance_id,
        .workflow_step_id = results_success_step,
        .exec_job_id = 800010,
    };
    ASSERT_TRUE(analysis->BindBattleResultsExecutionJob(successful_results_binding, &error)) << error;
    auto successful_results = complete_results;
    successful_results.battle_results_id = successful_results_id;
    ASSERT_TRUE(analysis->CompleteBattleResults(successful_results, &error)) << error;
    EXPECT_TRUE(analysis->CompleteBattleResults(successful_results, &error)) << error;
    auto fail_after_results = fail_results;
    fail_after_results.battle_results_id = successful_results_id;
    EXPECT_FALSE(analysis->FailBattleResults(fail_after_results, &error));
    EXPECT_FALSE(analysis->BindBattleResultsExecutionJob(successful_results_binding, &error));

    EXPECT_EQ(
        QueryInt64(
            db_,
            "SELECT COUNT(1) FROM ab_outbox_message "
            "WHERE event_type='AnalysisBattle.BattleCompletionFailed.v1';"),
        2);
    EXPECT_EQ(
        QueryInt64(
            db_,
            "SELECT COUNT(1) FROM ab_outbox_message "
            "WHERE event_type='AnalysisBattle.BattleResultsFailed.v1';"),
        2);
}

TEST_F(BattleEndResultsSqliteDbFixture, CoordinatorQueuesValidGraphJobsRejectsWrongSourceKindAndScopesRepeatFingerprints) {
    using namespace savor::db::execution::programdb;
    using namespace savor::db::execution::programdb::battleend;
    using namespace savor::db::execution::workflow;

    std::string error;
    const auto valid_source = SeedVictorySource(
        db_service_.get(),
        temp_root_,
        "valid-source",
        savor::PK_BattleSingleTurnRunner,
        &error);
    ASSERT_TRUE(valid_source.has_value()) << error;
    const auto invalid_source = SeedVictorySource(
        db_service_.get(),
        temp_root_,
        "wrong-kind-source",
        savor::PK_TasMovie,
        &error);
    ASSERT_TRUE(invalid_source.has_value()) << error;
    const auto valid_exec_job = db_service_->ExecutionDb()->GetJob(valid_source->exec_job_id);
    ASSERT_TRUE(valid_exec_job.has_value());
    ASSERT_EQ(valid_exec_job->program_kind, savor::PK_BattleSingleTurnRunner);
    ASSERT_EQ(valid_exec_job->program_ref_kind, "analysis_battle.turn_job");
    ASSERT_EQ(valid_exec_job->program_ref_id, valid_source->turn_job_id);
    ASSERT_EQ(valid_exec_job->savestate_id, valid_source->input_savestate_id);
    ASSERT_NE(valid_exec_job->savestate_id, std::optional<std::int64_t>(valid_source->savestate_id));
    ASSERT_EQ(valid_exec_job->state, "SUCCEEDED");
    const auto valid_turn_jobs = db_service_->AnalysisDb()->ListBattleTurnJobsByOutputSavestateId(
        valid_source->savestate_id);
    ASSERT_EQ(valid_turn_jobs.size(), 1u);
    ASSERT_EQ(valid_turn_jobs.front().exec_job_id, valid_source->exec_job_id);
    ASSERT_EQ(valid_turn_jobs.front().source_savestate_id, valid_source->input_savestate_id);
    ASSERT_EQ(valid_turn_jobs.front().output_savestate_id, valid_source->savestate_id);

    const auto graph_revision_id = SaveBattleEndResultsGraph(
        db_service_->AuthoringDb(),
        "battle-end-results-coordinator-acceptance",
        &error);
    ASSERT_TRUE(graph_revision_id.has_value()) << error;
    const auto first_workflow = CreateBattleEndResultsWorkflow(
        db_service_->AuthoringDb(),
        db_service_->ExecutionDb(),
        *graph_revision_id,
        valid_source->savestate_id,
        "battle-end-results-repeat-1",
        &error);
    ASSERT_TRUE(first_workflow.has_value()) << error;
    const auto second_workflow = CreateBattleEndResultsWorkflow(
        db_service_->AuthoringDb(),
        db_service_->ExecutionDb(),
        *graph_revision_id,
        valid_source->savestate_id,
        "battle-end-results-repeat-2",
        &error);
    ASSERT_TRUE(second_workflow.has_value()) << error;
    const auto rejected_workflow = CreateBattleEndResultsWorkflow(
        db_service_->AuthoringDb(),
        db_service_->ExecutionDb(),
        *graph_revision_id,
        invalid_source->savestate_id,
        "battle-end-results-wrong-source-kind",
        &error);
    ASSERT_TRUE(rejected_workflow.has_value()) << error;

    ProgramKindRegistry registry;
    RegisterBattleEndResultsPhaseDescriptor(
        &registry,
        db_service_->ExecutionDb(),
        db_service_->StateDb(),
        db_service_->AnalysisDb(),
        BattleEndResultsPhaseRegistrationConfig{
            .authoring_db = db_service_->AuthoringDb(),
            .working_dir_root = temp_root_ / "runtime",
        });
    std::mutex event_mutex;
    std::vector<std::string> event_lines;
    WorkflowCoordinatorService coordinator(
        db_service_->ExecutionDb(),
        &registry,
        WorkflowCoordinatorConfig{
            .workflow_enabled = true,
            .strict_smoke_terminal_on_failure = true,
            .poll_interval = std::chrono::milliseconds(1),
            .ready_scan_limit = 32,
            .terminal_scan_limit = 32,
        },
        [&](const std::string& line) {
            std::lock_guard<std::mutex> lock(event_mutex);
            event_lines.push_back(line);
        },
        nullptr,
        db_service_->AuthoringDb());
    ASSERT_TRUE(coordinator.Start(&error)) << error;
    const auto completed = WaitUntil(
        [&]() {
            return QueryInt64(
                       db_,
                       "SELECT COUNT(1) FROM exec_job WHERE program_kind="
                           + std::to_string(savor::PK_BattleCompletionRunner) + ";") == 2
                && QueryText(
                       db_,
                       "SELECT state FROM exec_workflow_step WHERE workflow_instance_id="
                           + std::to_string(*rejected_workflow)
                           + " AND step_kind='battle.completion';") == "FAILED"
                && QueryText(
                       db_,
                       "SELECT state FROM exec_workflow_instance WHERE workflow_instance_id="
                           + std::to_string(*rejected_workflow) + ";") == "FAILED";
        },
        std::chrono::seconds(3));
    coordinator.Stop();
    std::vector<std::string> captured_event_lines;
    {
        std::lock_guard<std::mutex> lock(event_mutex);
        captured_event_lines = event_lines;
    }
    std::string event_diagnostics;
    for (const auto& line : captured_event_lines) {
        event_diagnostics += line + "\n";
    }
    ASSERT_TRUE(completed)
        << "target_jobs=" << QueryInt64(
               db_,
               "SELECT COUNT(1) FROM exec_job WHERE program_kind="
                   + std::to_string(savor::PK_BattleCompletionRunner) + ";")
        << " first_state=" << QueryText(
               db_,
               "SELECT state FROM exec_workflow_step WHERE workflow_instance_id="
                   + std::to_string(*first_workflow) + " AND step_kind='battle.completion';")
        << " second_state=" << QueryText(
               db_,
               "SELECT state FROM exec_workflow_step WHERE workflow_instance_id="
                   + std::to_string(*second_workflow) + " AND step_kind='battle.completion';")
        << " rejected_state=" << QueryText(
               db_,
               "SELECT state FROM exec_workflow_step WHERE workflow_instance_id="
                   + std::to_string(*rejected_workflow) + " AND step_kind='battle.completion';")
        << " rejected_workflow_state=" << QueryText(
               db_,
               "SELECT state FROM exec_workflow_instance WHERE workflow_instance_id="
                   + std::to_string(*rejected_workflow) + ";")
        << " events=\n" << event_diagnostics;

    EXPECT_EQ(
        QueryInt64(
            db_,
            "SELECT COUNT(1) FROM exec_job WHERE program_kind="
                + std::to_string(savor::PK_BattleCompletionRunner) + ";"),
        2);
    EXPECT_EQ(
        QueryInt64(
            db_,
            "SELECT COUNT(DISTINCT fingerprint) FROM exec_job WHERE program_kind="
                + std::to_string(savor::PK_BattleCompletionRunner) + ";"),
        2);
    EXPECT_EQ(
        QueryInt64(
            db_,
            "SELECT COUNT(1) FROM exec_job WHERE program_kind="
                + std::to_string(savor::PK_BattleCompletionRunner)
                + " AND program_ref_kind='analysis_battle.battle_completion' AND state='QUEUED';"),
        2);
    EXPECT_EQ(
        QueryText(
            db_,
            "SELECT state FROM exec_workflow_step WHERE workflow_instance_id="
                + std::to_string(*first_workflow) + " AND step_kind='battle.completion';"),
        "MATERIALIZED");
    EXPECT_EQ(
        QueryText(
            db_,
            "SELECT state FROM exec_workflow_step WHERE workflow_instance_id="
                + std::to_string(*second_workflow) + " AND step_kind='battle.completion';"),
        "MATERIALIZED");
    EXPECT_EQ(
        QueryText(
            db_,
            "SELECT state FROM exec_workflow_instance WHERE workflow_instance_id="
                + std::to_string(*rejected_workflow) + ";"),
        "FAILED");
    EXPECT_EQ(
        QueryText(
            db_,
            "SELECT failure_code FROM exec_workflow_instance WHERE workflow_instance_id="
                + std::to_string(*rejected_workflow) + ";"),
        "WORKFLOW_STEP_MATERIALIZATION_FAILED");
    EXPECT_EQ(
        QueryText(
            db_,
            "SELECT failure_text FROM exec_workflow_instance WHERE workflow_instance_id="
                + std::to_string(*rejected_workflow) + ";"),
        "workflow step 'completion' materialization failed: schedule result did not include a job set");
    EXPECT_EQ(
        QueryInt64(
            db_,
            "SELECT COUNT(1) FROM ab_battle_completion WHERE workflow_instance_id="
                + std::to_string(*rejected_workflow) + ";"),
        0);
    EXPECT_EQ(
        QueryInt64(
            db_,
            "SELECT COUNT(1) FROM ab_battle_completion WHERE exec_job_id IS NOT NULL"
            " AND status='QUEUED' AND workflow_instance_id IN ("
                + std::to_string(*first_workflow) + "," + std::to_string(*second_workflow) + ");"),
        2);

    EXPECT_NE(
        std::find_if(
            captured_event_lines.begin(),
            captured_event_lines.end(),
            [](const std::string& line) {
                return line.find("source_provenance_or_artifact_invalid") != std::string::npos;
            }),
            captured_event_lines.end());
}

TEST_F(BattleEndResultsSqliteDbFixture, TerminalTransitionFailureSuppressesCompletionMapperSuccessOutput) {
    using namespace savor::db::execution::jobs;
    using namespace savor::db::execution::programdb;
    using namespace savor::db::execution::programdb::battleend;
    using namespace savor::db::execution::workflow;
    namespace completion = phase::battle::completion;

    std::string error;
    const auto source = SeedVictorySource(
        db_service_.get(),
        temp_root_,
        "terminal-transition",
        savor::PK_BattleSingleTurnRunner,
        &error);
    ASSERT_TRUE(source.has_value()) << error;
    const auto graph_revision_id = SaveBattleEndResultsGraph(
        db_service_->AuthoringDb(),
        "terminal-transition",
        &error);
    ASSERT_TRUE(graph_revision_id.has_value()) << error;
    const auto workflow_instance_id = CreateBattleEndResultsWorkflow(
        db_service_->AuthoringDb(),
        db_service_->ExecutionDb(),
        *graph_revision_id,
        source->savestate_id,
        "terminal-transition",
        &error);
    ASSERT_TRUE(workflow_instance_id.has_value()) << error;

    ProgramKindRegistry registry;
    const auto working_root = temp_root_ / "terminal-transition-runtime";
    RegisterBattleEndResultsPhaseDescriptor(
        &registry,
        db_service_->ExecutionDb(),
        db_service_->StateDb(),
        db_service_->AnalysisDb(),
        BattleEndResultsPhaseRegistrationConfig{
            .authoring_db = db_service_->AuthoringDb(),
            .working_dir_root = working_root,
        });
    WorkflowCoordinatorService coordinator(
        db_service_->ExecutionDb(),
        &registry,
        WorkflowCoordinatorConfig{
            .workflow_enabled = true,
            .strict_smoke_terminal_on_failure = true,
            .poll_interval = std::chrono::milliseconds(1),
            .ready_scan_limit = 16,
            .terminal_scan_limit = 16,
        },
        {},
        nullptr,
        db_service_->AuthoringDb());
    ASSERT_TRUE(coordinator.Start(&error)) << error;
    const auto queued = WaitUntil(
        [&]() {
            return QueryInt64(
                       db_,
                       "SELECT COUNT(1) FROM exec_job WHERE program_kind="
                           + std::to_string(savor::PK_BattleCompletionRunner) + ";") == 1;
        },
        std::chrono::seconds(3));
    coordinator.Stop();
    ASSERT_TRUE(queued);

    const auto job_id = QueryInt64(
        db_,
        "SELECT job_id FROM exec_job WHERE program_kind="
            + std::to_string(savor::PK_BattleCompletionRunner) + ";");
    const auto completion_id = QueryInt64(
        db_,
        "SELECT battle_completion_id FROM ab_battle_completion WHERE workflow_instance_id="
            + std::to_string(*workflow_instance_id) + ";");
    ASSERT_GT(job_id, 0);
    ASSERT_GT(completion_id, 0);
    ASSERT_TRUE(db_service_->ExecutionDb()->JobCommandService()->AppendLifecycleEvent(
        {
            .kind = JobLifecycleEventKind::JobCompleted,
            .job_id = job_id,
            .terminal_state = std::string("FAILED"),
            .requested_by = "terminal-transition-regression",
        },
        &error)) << error;
    ASSERT_EQ(
        QueryText(db_, "SELECT state FROM exec_job WHERE job_id=" + std::to_string(job_id) + ";"),
        "FAILED");

    const auto output_path = working_root / "completion" / ("job-" + std::to_string(job_id))
        / "output" / "battle_completion.sav";
    ASSERT_TRUE(std::filesystem::create_directories(output_path.parent_path()));
    {
        std::ofstream output(output_path, std::ios::binary | std::ios::trunc);
        output << "valid-completion-with-terminal-conflict";
        ASSERT_TRUE(output.good());
    }
    completion::Manifest manifest{};
    manifest.invariant_flags = completion::RequiredManifestInvariants;
    std::string manifest_blob;
    ASSERT_TRUE(completion::EncodeManifest(manifest, manifest_blob));
    IniDoc results;
    results.set("BattleCompletion.Results", "w_err", "0");
    results.set(
        "BattleCompletion.Results",
        "dw_err",
        std::to_string(static_cast<std::uint32_t>(savor::RunToBpOutcome::Hit)));
    results.set(
        "BattleCompletion.Results",
        "outcome",
        std::to_string(static_cast<std::uint32_t>(phase::battle::endresults::Outcome::Completed)));
    results.set("BattleCompletion.Results", "provider_failure", "0");
    results.set("BattleCompletion.Results", "runtime_failure", "0");
    results.set("BattleCompletion.Results", "macro_result", "0");
    results.set("BattleCompletion.Results", "invariant_flags", std::to_string(manifest.invariant_flags));
    results.set("BattleCompletion.Results", "savestate_path", output_path.string());
    results.set(
        "BattleCompletion.Results",
        "manifest_base64",
        savor::utils::Base64Encode(manifest_blob));

    const auto* descriptor = registry.FindForStepKind("battle.completion");
    ASSERT_NE(descriptor, nullptr);
    ASSERT_NE(descriptor->result_mapper, nullptr);
    const auto mapped = descriptor->result_mapper->MapPrimaryResult(job_id, results.to_string_sorted());
    EXPECT_EQ(mapped.result_kind, "analysis_battle.battle_completion.failed");
    EXPECT_EQ(mapped.result_ref_id, 0);
    EXPECT_TRUE(mapped.output_key.empty());
    EXPECT_TRUE(mapped.output_data_kind.empty());
    EXPECT_TRUE(mapped.output_ref_kind.empty());
    EXPECT_EQ(mapped.output_ref_id, 0);
    EXPECT_NE(
        std::find_if(mapped.event_lines.begin(), mapped.event_lines.end(), [](const std::string& line) {
            return line.find("reason=terminal_transition_failed") != std::string::npos;
        }),
        mapped.event_lines.end());
    EXPECT_EQ(
        QueryText(db_, "SELECT state FROM exec_job WHERE job_id=" + std::to_string(job_id) + ";"),
        "FAILED");
    const auto aggregate = db_service_->AnalysisDb()->GetBattleCompletion(completion_id);
    ASSERT_TRUE(aggregate.has_value());
    EXPECT_EQ(aggregate->status, "COMPLETED");
    EXPECT_TRUE(aggregate->completion_savestate_id.has_value());
}

TEST_F(BattleEndResultsSqliteDbFixture, CompletionResultStoresBcmbAndAggregateScopedDerivation) {
    using namespace savor::db::execution::programdb;
    using namespace savor::db::execution::programdb::battleend;
    using namespace savor::db::execution::workflow;
    namespace completion = phase::battle::completion;

    std::string error;
    const auto source = SeedVictorySource(
        db_service_.get(),
        temp_root_,
        "result-source",
        savor::PK_BattleSingleTurnRunner,
        &error);
    ASSERT_TRUE(source.has_value()) << error;
    const auto graph_revision_id = SaveBattleEndResultsGraph(
        db_service_->AuthoringDb(),
        "battle-end-results-mapper-acceptance",
        &error);
    ASSERT_TRUE(graph_revision_id.has_value()) << error;
    const auto workflow_instance_id = CreateBattleEndResultsWorkflow(
        db_service_->AuthoringDb(),
        db_service_->ExecutionDb(),
        *graph_revision_id,
        source->savestate_id,
        "battle-end-results-mapper",
        &error);
    ASSERT_TRUE(workflow_instance_id.has_value()) << error;

    ProgramKindRegistry registry;
    const auto working_root = temp_root_ / "mapper-runtime";
    RegisterBattleEndResultsPhaseDescriptor(
        &registry,
        db_service_->ExecutionDb(),
        db_service_->StateDb(),
        db_service_->AnalysisDb(),
        BattleEndResultsPhaseRegistrationConfig{
            .authoring_db = db_service_->AuthoringDb(),
            .working_dir_root = working_root,
        });
    std::mutex event_mutex;
    std::vector<std::string> event_lines;
    WorkflowCoordinatorService coordinator(
        db_service_->ExecutionDb(),
        &registry,
        WorkflowCoordinatorConfig{
            .workflow_enabled = true,
            .strict_smoke_terminal_on_failure = true,
            .poll_interval = std::chrono::milliseconds(1),
            .ready_scan_limit = 16,
            .terminal_scan_limit = 16,
        },
        [&](const std::string& line) {
            std::lock_guard<std::mutex> lock(event_mutex);
            event_lines.push_back(line);
        },
        nullptr,
        db_service_->AuthoringDb());
    ASSERT_TRUE(coordinator.Start(&error)) << error;
    const auto queued = WaitUntil(
        [&]() {
            return QueryInt64(
                       db_,
                       "SELECT COUNT(1) FROM exec_job WHERE program_kind="
                           + std::to_string(savor::PK_BattleCompletionRunner) + ";") == 1;
        },
        std::chrono::seconds(3));
    coordinator.Stop();
    std::string event_diagnostics;
    {
        std::lock_guard<std::mutex> lock(event_mutex);
        for (const auto& line : event_lines) {
            event_diagnostics += line + "\n";
        }
    }
    ASSERT_TRUE(queued)
        << "workflow_state=" << QueryText(
               db_,
               "SELECT state FROM exec_workflow_step WHERE workflow_instance_id="
                   + std::to_string(*workflow_instance_id) + ";")
        << " events=\n" << event_diagnostics;

    const auto job_id = QueryInt64(
        db_,
        "SELECT job_id FROM exec_job WHERE program_kind="
            + std::to_string(savor::PK_BattleCompletionRunner) + ";");
    ASSERT_GT(job_id, 0);
    const auto completion_id = QueryInt64(
        db_,
        "SELECT battle_completion_id FROM ab_battle_completion WHERE workflow_instance_id="
            + std::to_string(*workflow_instance_id) + ";");
    ASSERT_GT(completion_id, 0);
    EXPECT_EQ(
        QueryInt64(db_, "SELECT exec_job_id FROM ab_battle_completion WHERE battle_completion_id="
            + std::to_string(completion_id) + ";"),
        job_id);
    const auto* descriptor = registry.FindForStepKind("battle.completion");
    ASSERT_NE(descriptor, nullptr);
    ASSERT_NE(descriptor->result_mapper, nullptr);

    const auto output_path = working_root / "completion" / ("job-" + std::to_string(job_id))
        / "output" / "battle_completion.sav";
    ASSERT_TRUE(std::filesystem::create_directories(output_path.parent_path()));
    {
        std::ofstream output(output_path, std::ios::binary | std::ios::trunc);
        output << "battle-completion-complete";
        ASSERT_TRUE(output.good());
    }

    completion::Manifest manifest{};
    manifest.invariant_flags = completion::RequiredManifestInvariants;
    manifest.start_vi = 100;
    manifest.end_vi = 200;
    std::string expected_view_error;
    ASSERT_TRUE(completion::DeriveExpectedView(manifest, &expected_view_error))
        << expected_view_error;
    std::string manifest_blob;
    ASSERT_TRUE(completion::EncodeManifest(manifest, manifest_blob));
    ASSERT_GE(manifest_blob.size(), 4u);
    EXPECT_EQ(manifest_blob.substr(0, 4), "BCMB");
    completion::Manifest canonical_manifest{};
    ASSERT_TRUE(completion::DecodeManifest(manifest_blob, canonical_manifest));

    IniDoc results;
    results.set("BattleCompletion.Results", "w_err", "0");
    results.set(
        "BattleCompletion.Results",
        "dw_err",
        std::to_string(static_cast<std::uint32_t>(savor::RunToBpOutcome::Hit)));
    results.set(
        "BattleCompletion.Results",
        "outcome",
        std::to_string(static_cast<std::uint32_t>(phase::battle::endresults::Outcome::Completed)));
    results.set("BattleCompletion.Results", "provider_failure", "0");
    results.set("BattleCompletion.Results", "runtime_failure", "0");
    results.set("BattleCompletion.Results", "macro_result", "0");
    results.set("BattleCompletion.Results", "invariant_flags", std::to_string(manifest.invariant_flags));
    results.set("BattleCompletion.Results", "savestate_path", output_path.string());
    results.set(
        "BattleCompletion.Results",
        "manifest_base64",
        savor::utils::Base64Encode(manifest_blob));

    const auto mapped = descriptor->result_mapper->MapPrimaryResult(
        job_id,
        results.to_string_sorted());
    ASSERT_EQ(mapped.result_kind, "analysis_battle.battle_completion");
    ASSERT_EQ(mapped.result_ref_id, completion_id);
    EXPECT_EQ(mapped.output_key, "completion");
    EXPECT_EQ(mapped.output_data_kind, "analysis_battle.battle_completion_id");
    EXPECT_EQ(mapped.output_ref_kind, "analysis_battle.battle_completion");
    EXPECT_EQ(mapped.output_ref_id, completion_id);
    EXPECT_FALSE(descriptor->result_mapper->MapPrimaryArtifact(job_id).has_value());

    EXPECT_EQ(
        QueryInt64(
            db_,
            "SELECT COUNT(1) FROM state_artifact WHERE artifact_kind='OTHER' AND file_ext='.bcmb';"),
        1);
    const auto stored_manifest_path = QueryText(
        db_,
        "SELECT filename FROM state_artifact WHERE artifact_kind='OTHER' AND file_ext='.bcmb';");
    ASSERT_FALSE(stored_manifest_path.empty());
    std::ifstream stored_manifest(stored_manifest_path, std::ios::binary);
    std::string stored_magic(4, '\0');
    stored_manifest.read(stored_magic.data(), static_cast<std::streamsize>(stored_magic.size()));
    ASSERT_TRUE(stored_manifest.good());
    EXPECT_EQ(stored_magic, "BCMB");

    const auto completed = db_service_->AnalysisDb()->GetBattleCompletion(completion_id);
    ASSERT_TRUE(completed.has_value());
    ASSERT_EQ(completed->status, "COMPLETED");
    ASSERT_TRUE(completed->completion_savestate_id.has_value());
    const auto completion_savestate_id = *completed->completion_savestate_id;
    EXPECT_EQ(
        QueryText(
            db_,
            "SELECT savestate_type FROM state_savestate WHERE savestate_id="
                + std::to_string(completion_savestate_id) + ";"),
        "BATTLE_COMPLETION");
    EXPECT_EQ(
        QueryText(
            db_,
            "SELECT a.artifact_kind FROM state_savestate s JOIN state_artifact a ON a.artifact_id=s.artifact_id "
            "WHERE s.savestate_id=" + std::to_string(completion_savestate_id) + ";"),
        "SAV");
    EXPECT_EQ(
        QueryInt64(
            db_,
            "SELECT COUNT(1) FROM state_savestate_derivation WHERE from_savestate_id="
                + std::to_string(source->savestate_id)
                + " AND to_savestate_id=" + std::to_string(completion_savestate_id)
                + " AND method_kind='battle_completion'"
                  " AND source_context_kind='analysis_battle.battle_completion'"
                  " AND source_context_id=" + std::to_string(completion_id) + ";"),
        1);

    const auto incoming = db_service_->StateDb()->ListIncomingSavestateDerivations(completion_savestate_id);
    ASSERT_EQ(incoming.size(), 1u);
    std::int64_t replayed_derivation_id = 0;
    ASSERT_TRUE(db_service_->StateDb()->DeriveSavestate(
        {
            .from_savestate_id = source->savestate_id,
            .to_savestate_id = completion_savestate_id,
            .method_kind = "battle_completion",
            .source_context_kind = "analysis_battle.battle_completion",
            .source_context_id = completion_id,
            .created_at_utc = savor::db::types::UtcNow(),
            .correlation_id = "replayed-completion-derivation",
            .causation_id = "replayed-test",
        },
        &replayed_derivation_id,
        &error)) << error;
    EXPECT_EQ(replayed_derivation_id, incoming.front().derivation_id);
    EXPECT_EQ(db_service_->StateDb()->ListIncomingSavestateDerivations(completion_savestate_id).size(), 1u);

    const auto authored_graph = db_service_->AuthoringDb()->GetWorkflowGraphRevision(*graph_revision_id);
    ASSERT_TRUE(authored_graph.has_value());
    const auto seed_node = std::find_if(authored_graph->nodes.begin(), authored_graph->nodes.end(), [](const auto& node) {
        return node.node_key == "seed";
    });
    ASSERT_NE(seed_node, authored_graph->nodes.end());
    ASSERT_TRUE(seed_node->authored_ref_id.has_value());
    const auto completion_state = db_service_->StateDb()->GetSavestate(completion_savestate_id);
    ASSERT_TRUE(completion_state.has_value());

    struct EnqueuedFieldRun {
        std::int64_t job_id = 0;
        std::int64_t probe_run_id = 0;
        std::int64_t neutral_seed_id = 0;
    };
    const auto enqueue_field_run = [&](const std::string& flavor, int codec, const std::string& label)
        -> std::optional<EnqueuedFieldRun> {
        std::int64_t probe_set_id = 0;
        if (!db_service_->AnalysisDb()->CreateSeedProbeSet(
                {
                    .name = "invalid-field-run-" + label,
                    .probe_flavor = flavor,
                    .breakpoint_policy_name = "battle-end-results-test",
                    .segment_source_kind = "test",
                    .created_at_utc = savor::db::types::UtcNow(),
                    .correlation_id = "invalid-field-run-" + label,
                    .causation_id = "test",
                },
                &probe_set_id,
                &error)) return std::nullopt;
        std::int64_t probe_run_id = 0;
        if (!db_service_->AnalysisDb()->RequestSeedProbeRun(
                {
                    .probe_set_id = probe_set_id,
                    .entry_savestate_id = completion_savestate_id,
                    .seed_probe_spec_id = *seed_node->authored_ref_id,
                    .launch_samples_per_axis = 1,
                    .codec_version = codec,
                    .status = "REQUESTED",
                    .requested_at_utc = savor::db::types::UtcNow(),
                    .correlation_id = "invalid-field-run-" + label,
                    .causation_id = "test",
                },
                &probe_run_id,
                &error)) return std::nullopt;
        const auto probe_result_id = db_service_->AnalysisDb()->LookupSeedProbeResultId(probe_run_id);
        if (!probe_result_id.has_value()) return std::nullopt;
        bool inserted = false;
        std::int64_t neutral_seed_id = 0;
        if (!db_service_->AnalysisDb()->EnsureSeedProbeNeutralSeed(
                {
                    .probe_result_id = *probe_result_id,
                    .neutral_seed_value = 12345,
                    .source_kind = "CALCULATED",
                    .recorded_at_utc = savor::db::types::UtcNow(),
                    .correlation_id = "invalid-field-run-" + label,
                    .causation_id = "test",
                },
                &inserted,
                &neutral_seed_id,
                &error)) return std::nullopt;
        savor::GCInputFrame neutral_frame{};
        IniDoc input;
        input.set("FieldReturnSeed.Job", "workflow_instance_id", std::to_string(*workflow_instance_id));
        input.set("FieldReturnSeed.Job", "workflow_step_id", std::to_string(completed->workflow_step_id));
        input.set("FieldReturnSeed.Job", "completion_id", std::to_string(completion_id));
        input.set("FieldReturnSeed.Job", "probe_run_id", std::to_string(probe_run_id));
        input.set("FieldReturnSeed.Job", "selector", "neutral");
        input.set("FieldReturnSeed.Job", "selector_value", "0");
        input.set("FieldReturnSeed.Job", "seed_ref_kind", "analysisseedprobe.neutral_seed");
        input.set("FieldReturnSeed.Job", "seed_ref_id", std::to_string(neutral_seed_id));
        input.set("FieldReturnSeed.Job", "expected_seed", "12345");
        input.set("FieldReturnSeed.Job", "frame_hex", neutral_frame.to_frame_hex());
        input.set("FieldReturnSeed.Job", "entry_savestate_id", std::to_string(completion_savestate_id));
        input.set("FieldReturnSeed.Job", "entry_artifact_id", std::to_string(completion_state->artifact_id));
        input.set("FieldReturnSeed.Job", "entry_artifact_sha256", completion_state->artifact_sha256);
        input.set("FieldReturnSeed.Job", "run_timeout_ms", "60000");
        std::int64_t job_set_id = 0;
        if (!db_service_->ExecutionDb()->CreateJobSet(
                {
                    .program_kind = savor::PK_SeedProbe,
                    .purpose = "invalid field-return provenance",
                    .created_by = std::string("battle-end-results-test"),
                    .expected_total = 1,
                    .domain_ref_kind = std::string("analysisseedprobe.neutral_seed"),
                    .domain_ref_id = neutral_seed_id,
                },
                &job_set_id,
                &error)) return std::nullopt;
        std::int64_t invalid_job_id = 0;
        if (!db_service_->ExecutionDb()->EnqueueJob(
                {
                    .job_set_id = job_set_id,
                    .program_kind = savor::PK_SeedProbe,
                    .program_version = 2,
                    .program_ref_kind = "analysisseedprobe.neutral_seed",
                    .program_ref_id = neutral_seed_id,
                    .savestate_id = completion_savestate_id,
                    .fingerprint = "invalid-field-run-" + label,
                    .input_ini = input.to_string_sorted(),
                },
                &invalid_job_id,
                &error)) return std::nullopt;
        return EnqueuedFieldRun{
            .job_id = invalid_job_id,
            .probe_run_id = probe_run_id,
            .neutral_seed_id = neutral_seed_id,
        };
    };

    const auto invalid_codec_job = enqueue_field_run("FIELD_RETURN", 1, "codec-v1");
    ASSERT_TRUE(invalid_codec_job.has_value()) << error;
    const auto invalid_flavor_job = enqueue_field_run("BATTLE_PRE", 2, "battle-pre-flavor");
    ASSERT_TRUE(invalid_flavor_job.has_value()) << error;
    const auto materialize_descriptor = BuildFieldReturnSeedMaterializeDescriptor(
        db_service_->ExecutionDb(),
        db_service_->StateDb(),
        db_service_->AnalysisDb(),
        BattleEndWorkflowPhaseRegistrationConfig{.working_dir_root = working_root});
    ASSERT_NE(materialize_descriptor.runtime_init, nullptr);
    for (const auto invalid_job_id : {invalid_codec_job->job_id, invalid_flavor_job->job_id}) {
        const auto init = materialize_descriptor.runtime_init->BuildRuntimeInit(invalid_job_id);
        EXPECT_EQ(init.savestate_ref_id, 0);
        savor::db::execution::programdb::RuntimeInitRequest forged{};
        forged.savestate_ref_kind = "state_savestate";
        forged.savestate_ref_id = completion_savestate_id;
        forged.bootstrap_profile = "battle.field_return_seed_probe.materialize";
        forged.default_timeout_ms = 60000;
        EXPECT_FALSE(materialize_descriptor.runtime_init->MaterializePsJob(invalid_job_id, forged).has_value());
    }

    const auto valid_field_run = enqueue_field_run("FIELD_RETURN", 2, "valid");
    ASSERT_TRUE(valid_field_run.has_value()) << error;
    const auto field_output_path = working_root / "field-return"
        / ("job-" + std::to_string(valid_field_run->job_id))
        / "output" / "field_return_seeded.sav";
    ASSERT_TRUE(std::filesystem::create_directories(field_output_path.parent_path()));
    {
        std::ofstream output(field_output_path, std::ios::binary | std::ios::trunc);
        output << "field-return-seeded";
        ASSERT_TRUE(output.good());
    }
    IniDoc field_results;
    field_results.set("FieldReturnSeed.Results", "w_err", "0");
    field_results.set(
        "FieldReturnSeed.Results",
        "dw_err",
        std::to_string(static_cast<std::uint32_t>(savor::RunToBpOutcome::Hit)));
    field_results.set("FieldReturnSeed.Results", "rng_seed", "12345");
    field_results.set("FieldReturnSeed.Results", "savestate_path", field_output_path.string());
    const auto seeded = materialize_descriptor.result_mapper->MapPrimaryResult(
        valid_field_run->job_id,
        field_results.to_string_sorted());
    ASSERT_EQ(seeded.result_kind, "state.field_return_seeded.savestate");
    ASSERT_EQ(seeded.output_key, "seeded_savestate");
    ASSERT_EQ(seeded.output_data_kind, "state.savestate_id");
    ASSERT_EQ(seeded.output_ref_kind, "state.savestate");
    ASSERT_GT(seeded.output_ref_id, 0);
    const auto seeded_savestate_id = seeded.output_ref_id;
    EXPECT_EQ(
        QueryText(
            db_,
            "SELECT savestate_type FROM state_savestate WHERE savestate_id="
                + std::to_string(seeded_savestate_id) + ";"),
        "FIELD_RETURN_SEEDED");
    EXPECT_EQ(
        QueryInt64(
            db_,
            "SELECT COUNT(1) FROM state_savestate_derivation WHERE from_savestate_id="
                + std::to_string(completion_savestate_id)
                + " AND to_savestate_id=" + std::to_string(seeded_savestate_id)
                + " AND method_kind='field_return_seed_materialization'"
                  " AND source_context_kind='analysisseedprobe.neutral_seed'"
                  " AND source_context_id=" + std::to_string(valid_field_run->neutral_seed_id) + ";"),
        1);

    const auto* results_descriptor = registry.FindForStepKind("battle.results_screen");
    ASSERT_NE(results_descriptor, nullptr);
    ASSERT_NE(results_descriptor->graph_job_persistence, nullptr);
    ASSERT_NE(results_descriptor->result_mapper, nullptr);
    const auto results_step_id = QueryInt64(
        db_,
        "SELECT workflow_step_id FROM exec_workflow_step WHERE workflow_instance_id="
            + std::to_string(*workflow_instance_id)
            + " AND step_kind='battle.results_screen';");
    ASSERT_GT(results_step_id, 0);

    const auto schedule_results = [&](std::int64_t workflow_step_id, const std::string& step_key) {
        WorkflowGraphStepScheduleContext context{};
        context.workflow_instance_id = *workflow_instance_id;
        context.workflow_step_id = workflow_step_id;
        context.workflow_graph_revision_id = *graph_revision_id;
        context.step_key = step_key;
        context.step_kind = "battle.results_screen";
        context.activation_key = "results";
        context.activation_graph_node_key = "results";
        context.unit_kind = "battle_results_screen";
        context.step_priority = 1;
        context.input_bindings = {
            {
                .node_key = "results",
                .input_key = "completion",
                .data_kind = "analysis_battle.battle_completion_id",
                .ref_kind = "analysis_battle.battle_completion",
                .ref_id = completion_id,
                .source_kind = "upstream",
            },
            {
                .node_key = "results",
                .input_key = "seeded_savestate",
                .data_kind = "state.savestate_id",
                .ref_kind = "state.savestate",
                .ref_id = seeded_savestate_id,
                .source_kind = "upstream",
            },
        };
        context.arguments = {
            {
                .node_key = "results",
                .argument_key = "run_ms",
                .value_type = "integer",
                .integer_value = 60000,
                .source_kind = "test",
            },
        };
        return results_descriptor->graph_job_persistence->EncodeForGraphQueueing(context);
    };

    const auto results_schedule = schedule_results(results_step_id, "results");
    ASSERT_GT(results_schedule.root_job_set_id, 0);
    const auto battle_results_id = results_schedule.persistence.program_ref_id;
    ASSERT_GT(battle_results_id, 0);
    const auto queued_results = db_service_->AnalysisDb()->GetBattleResults(battle_results_id);
    ASSERT_TRUE(queued_results.has_value());
    ASSERT_TRUE(queued_results->exec_job_id.has_value());
    const auto results_job_id = *queued_results->exec_job_id;
    ASSERT_TRUE(db_service_->ExecutionDb()->JobCommandService()->AppendLifecycleEvent(
        {
            .kind = savor::db::execution::jobs::JobLifecycleEventKind::JobQueued,
            .job_id = results_job_id,
            .requested_by = "battle-end-results-test",
        },
        &error)) << error;
    EXPECT_EQ(queued_results->selected_seed_ref_kind, "analysisseedprobe.neutral_seed");
    EXPECT_EQ(queued_results->selected_seed_ref_id, valid_field_run->neutral_seed_id);
    EXPECT_EQ(queued_results->selected_seed_value, 12345);
    EXPECT_EQ(queued_results->rng_effect_kind, savor::db::RngEffectKind::Preserve);
    EXPECT_EQ(queued_results->fixed_draw_count, std::optional<std::int64_t>(0));
    EXPECT_EQ(queued_results->status, "QUEUED");

    constexpr std::uint32_t results_invariants = phase::battle::endresults::InvariantSourcePc
        | phase::battle::endresults::InvariantLifecycleState
        | phase::battle::endresults::InvariantCompletionState
        | phase::battle::endresults::InvariantCompletionPublished
        | phase::battle::endresults::InvariantResultPointerCleared
        | phase::battle::endresults::InvariantFieldMode
        | phase::battle::endresults::InvariantRngPreserved;
    phase::battle::endresults::Report report{};
    report.policy = phase::battle::endresults::AccelerationPolicy::FullAdaptive;
    report.outcome = phase::battle::endresults::Outcome::Completed;
    report.failure = phase::battle::endresults::FailureCode::None;
    report.expected = canonical_manifest.expected;
    report.invariant_flags = results_invariants;
    std::string report_blob;
    ASSERT_TRUE(phase::battle::endresults::EncodeReport(report, report_blob));
    ASSERT_GE(report_blob.size(), 4u);
    EXPECT_EQ(report_blob.substr(0, 4), "BERB");

    const auto map_results = [&](std::int64_t target_job_id, bool include_entry_seed) {
        const auto output_path = working_root / "results"
            / ("job-" + std::to_string(target_job_id)) / "output" / "battle_end.sav";
        std::filesystem::create_directories(output_path.parent_path());
        {
            std::ofstream output(output_path, std::ios::binary | std::ios::trunc);
            output << "battle-results-complete:" << target_job_id;
        }
        IniDoc result;
        result.set("BattleResultsScreen.Results", "w_err", "0");
        result.set(
            "BattleResultsScreen.Results",
            "dw_err",
            std::to_string(static_cast<std::uint32_t>(savor::RunToBpOutcome::Hit)));
        result.set(
            "BattleResultsScreen.Results",
            "outcome",
            std::to_string(static_cast<std::uint32_t>(phase::battle::endresults::Outcome::Completed)));
        result.set("BattleResultsScreen.Results", "provider_failure", "0");
        result.set("BattleResultsScreen.Results", "runtime_failure", "0");
        result.set("BattleResultsScreen.Results", "macro_result", "0");
        result.set("BattleResultsScreen.Results", "mismatch_flags", "0");
        result.set("BattleResultsScreen.Results", "invariant_flags", std::to_string(results_invariants));
        if (include_entry_seed) result.set("BattleResultsScreen.Results", "entry_seed", "12345");
        result.set("BattleResultsScreen.Results", "final_seed", "12345");
        result.set("BattleResultsScreen.Results", "effect_kind", "0");
        result.set("BattleResultsScreen.Results", "advance_count", "0");
        result.set("BattleResultsScreen.Results", "savestate_path", output_path.string());
        result.set(
            "BattleResultsScreen.Results",
            "report_base64",
            savor::utils::Base64Encode(report_blob));
        return results_descriptor->result_mapper->MapPrimaryResult(
            target_job_id,
            result.to_string_sorted());
    };

    const auto final_result = map_results(results_job_id, true);
    std::string final_result_diagnostics;
    for (const auto& line : final_result.event_lines) final_result_diagnostics += line + "\n";
    ASSERT_EQ(final_result.result_kind, "state.battle_end.savestate")
        << final_result_diagnostics;
    ASSERT_EQ(final_result.output_key, "terminal_savestate");
    ASSERT_EQ(final_result.output_data_kind, "state.savestate_id");
    ASSERT_EQ(final_result.output_ref_kind, "state.savestate");
    ASSERT_GT(final_result.output_ref_id, 0);
    const auto final_savestate_id = final_result.output_ref_id;
    EXPECT_EQ(
        QueryText(
            db_,
            "SELECT savestate_type FROM state_savestate WHERE savestate_id="
                + std::to_string(final_savestate_id) + ";"),
        "BATTLE_END");
    const auto completed_results = db_service_->AnalysisDb()->GetBattleResults(battle_results_id);
    ASSERT_TRUE(completed_results.has_value());
    EXPECT_EQ(completed_results->status, "COMPLETED");
    EXPECT_EQ(completed_results->final_savestate_id, std::optional<std::int64_t>(final_savestate_id));
    EXPECT_EQ(completed_results->entry_rng_seed, std::optional<std::int64_t>(12345));
    EXPECT_EQ(completed_results->final_rng_seed, std::optional<std::int64_t>(12345));
    ASSERT_TRUE(completed_results->result_artifact_id.has_value());
    EXPECT_EQ(
        QueryInt64(
            db_,
            "SELECT COUNT(1) FROM state_savestate_derivation WHERE from_savestate_id="
                + std::to_string(seeded_savestate_id)
                + " AND to_savestate_id=" + std::to_string(final_savestate_id)
                + " AND method_kind='battle_results_screen'"
                  " AND source_context_kind='analysis_battle.battle_results'"
                  " AND source_context_id=" + std::to_string(battle_results_id) + ";"),
        1);

    ASSERT_TRUE(db_service_->ExecutionDb()->WorkflowCommandService()->AppendDynamicSteps(
        {
            .workflow_instance_id = *workflow_instance_id,
            .steps = {{
                .step_key = "results/tampered",
                .step_kind = "battle.results_screen",
                .priority = 1,
                .max_attempts = 1,
            }},
            .requested_by = "battle-end-results-test",
        },
        &error)) << error;
    const auto tampered_step_id = QueryInt64(
        db_,
        "SELECT workflow_step_id FROM exec_workflow_step WHERE workflow_instance_id="
            + std::to_string(*workflow_instance_id)
            + " AND step_key='results/tampered';");
    ASSERT_GT(tampered_step_id, 0);
    const auto tampered_schedule = schedule_results(tampered_step_id, "results/tampered");
    ASSERT_GT(tampered_schedule.root_job_set_id, 0);
    const auto tampered_results_id = tampered_schedule.persistence.program_ref_id;
    const auto tampered_aggregate = db_service_->AnalysisDb()->GetBattleResults(tampered_results_id);
    ASSERT_TRUE(tampered_aggregate.has_value());
    ASSERT_TRUE(tampered_aggregate->exec_job_id.has_value());
    ASSERT_TRUE(db_service_->ExecutionDb()->JobCommandService()->AppendLifecycleEvent(
        {
            .kind = savor::db::execution::jobs::JobLifecycleEventKind::JobQueued,
            .job_id = *tampered_aggregate->exec_job_id,
            .requested_by = "battle-end-results-test",
        },
        &error)) << error;
    const auto tampered_result = map_results(*tampered_aggregate->exec_job_id, false);
    EXPECT_EQ(tampered_result.result_kind, "state.battle_end.failed");
    EXPECT_TRUE(tampered_result.output_key.empty());
    const auto failed_results = db_service_->AnalysisDb()->GetBattleResults(tampered_results_id);
    ASSERT_TRUE(failed_results.has_value());
    EXPECT_EQ(failed_results->status, "FAILED");
    EXPECT_FALSE(failed_results->final_savestate_id.has_value());
    EXPECT_TRUE(failed_results->completed_at_utc.has_value());

    EXPECT_EQ(
        QueryInt64(
            db_,
            "SELECT COUNT(1) FROM exec_job_output WHERE job_id=" + std::to_string(job_id) + ";"),
        0);
    EXPECT_EQ(
        QueryText(db_, "SELECT state FROM exec_job WHERE job_id=" + std::to_string(job_id) + ";"),
        "SUCCEEDED");
}

} // namespace

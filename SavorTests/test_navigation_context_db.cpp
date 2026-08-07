#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>
#include <sqlite3.h>

#include "Common/DbService.h"
#include "Core/Memory/Soa/Navigation/NavigationContext.h"
#include "Core/Memory/Soa/Navigation/NavigationContextCodec.h"
#include "Execution/DBWorkflowWorkerCoordinator.h"
#include "Execution/IExecutionDb.h"
#include "Execution/ProgramDB/NavigationContext/NavigationContextAdapters.h"
#include "Execution/ProgramDB/NavigationContext/NavigationContextPhaseRegistration.h"
#include "Execution/ProgramDB/ProgramKindRegistry.h"
#include "Execution/Workflow/WorkflowComposition.h"
#include "Execution/Workflow/WorkflowOrchestration.h"
#include "Execution/Workflow/WorkflowUnitActivationFactory.h"
#include "Phases/Programs/NavigationContext/NavigationContextPayload.h"
#include "Phases/Programs/NavigationContext/NavigationContextResult.h"
#include "Runner/Breakpoints/BpRegistry.h"
#include "Runner/IPC/Wire.h"
#include "Runner/Parallel/PRTypes.h"
#include "Runner/Script/CtxRegistry.h"
#include "State/IStateDb.h"
#include "Utils/Hash.h"
#include "Utils/IniDoc.h"
#include "common/SqliteDbFixture.h"

#ifdef GetJob
#undef GetJob
#endif

namespace {

using savor::db::execution::programdb::ProgramKindRegistry;
using savor::db::execution::programdb::navigationcontext::
    NavigationContextPhaseRegistrationConfig;
using savor::db::execution::programdb::navigationcontext::
    RegisterNavigationContextProbePhaseDescriptor;

struct SourceSavestate {
    std::filesystem::path path;
    std::int64_t artifact_id = 0;
    std::int64_t savestate_id = 0;
    std::int64_t size_bytes = 0;
    std::string sha256;
};

struct ScheduledProbe {
    std::int64_t workflow_instance_id = 0;
    std::int64_t workflow_step_id = 0;
    std::int64_t workflow_unit_activation_id = 0;
    std::int64_t workflow_graph_revision_id = 0;
    std::int64_t job_set_id = 0;
    std::int64_t job_id = 0;
};

void SetError(std::string* error_out, std::string error) {
    if (error_out != nullptr) {
        *error_out = std::move(error);
    }
}

std::int64_t QueryInt64(sqlite3* db, const std::string& sql) {
    sqlite3_stmt* statement = nullptr;
    if (db == nullptr
        || sqlite3_prepare_v2(
            db,
            sql.c_str(),
            -1,
            &statement,
            nullptr) != SQLITE_OK) {
        return -1;
    }
    const auto rc = sqlite3_step(statement);
    const auto value =
        rc == SQLITE_ROW ? sqlite3_column_int64(statement, 0) : -1;
    sqlite3_finalize(statement);
    return value;
}

std::string QueryText(sqlite3* db, const std::string& sql) {
    sqlite3_stmt* statement = nullptr;
    if (db == nullptr
        || sqlite3_prepare_v2(
            db,
            sql.c_str(),
            -1,
            &statement,
            nullptr) != SQLITE_OK) {
        return {};
    }
    std::string value;
    if (sqlite3_step(statement) == SQLITE_ROW) {
        const auto* text = sqlite3_column_text(statement, 0);
        if (text != nullptr) {
            value = reinterpret_cast<const char*>(text);
        }
    }
    sqlite3_finalize(statement);
    return value;
}

std::optional<SourceSavestate> SeedSourceSavestate(
    savor::db::core::DBService* db_service,
    const std::filesystem::path& root,
    const std::string& label,
    std::string* error_out) {
    if (db_service == nullptr || db_service->StateDb() == nullptr) {
        SetError(error_out, "state db unavailable");
        return std::nullopt;
    }

    SourceSavestate out{};
    out.path = root / (label + ".sav");
    {
        std::ofstream file(out.path, std::ios::binary | std::ios::trunc);
        file << "navigation-context-source:" << label;
        if (!file.good()) {
            SetError(error_out, "failed to write source savestate");
            return std::nullopt;
        }
    }
    out.size_bytes =
        static_cast<std::int64_t>(std::filesystem::file_size(out.path));
    out.sha256 = hash::sha256_of_file(out.path.string());

    const auto now = savor::db::types::UtcNow();
    std::string error;
    if (!db_service->StateDb()->StoreArtifact(
            {
                .sha256 = out.sha256,
                .size_bytes = out.size_bytes,
                .compression_kind = 0,
                .filename = std::filesystem::absolute(out.path).string(),
                .file_ext = ".sav",
                .artifact_kind = "SAV",
                .created_at_utc = now,
                .correlation_id = "test.navigation-context." + label,
                .causation_id = "test",
            },
            &out.artifact_id,
            &error)) {
        SetError(error_out, "failed to store source artifact: " + error);
        return std::nullopt;
    }
    if (!db_service->StateDb()->CreateSavestate(
            {
                .artifact_id = out.artifact_id,
                .savestate_type = "FIELD",
                .note = "General navigation-context source",
                .is_complete = true,
                .created_at_utc = now,
                .correlation_id = "test.navigation-context." + label,
                .causation_id =
                    "artifact-" + std::to_string(out.artifact_id),
            },
            &out.savestate_id,
            &error)) {
        SetError(error_out, "failed to create source savestate: " + error);
        return std::nullopt;
    }
    if (error_out != nullptr) {
        error_out->clear();
    }
    return out;
}

std::optional<std::int64_t> SaveNavigationContextGraph(
    savor::db::IAuthoringDb* authoring_db,
    const std::string& label,
    std::string* error_out) {
    if (authoring_db == nullptr) {
        SetError(error_out, "authoring db unavailable");
        return std::nullopt;
    }

    savor::db::SaveWorkflowGraphResult saved{};
    if (!authoring_db->SaveWorkflowGraph(
            {
                .name = "navigation-context-" + label,
                .description = "Navigation Context coordinator integration",
                .hidden = true,
                .graph_version = 1,
                .graph_hash = "navigation-context-" + label + "-hash",
                .nodes = {
                    {
                        .node_key = "navigation",
                        .unit_kind = "navigation.context_probe",
                        .display_name = "Navigation Context Probe",
                        .inputs = {
                            {
                                .input_key = "entry_savestate",
                                .data_kind = "state.movie_inactive_savestate_id",
                                .display_name = "Entry savestate",
                            },
                        },
                        .possible_outputs = {
                            {
                                .output_key = "navigation_context",
                                .data_kind =
                                    "state_artifact.navigation_context_id",
                                .display_name = "Navigation context",
                            },
                        },
                    },
                },
                .created_at_utc = savor::db::types::UtcNow(),
                .correlation_id = "test.navigation-context.graph." + label,
                .causation_id = "test",
            },
            &saved,
            error_out)) {
        return std::nullopt;
    }
    return saved.workflow_graph_revision_id;
}

std::optional<ScheduledProbe> CreateAndScheduleProbe(
    savor::db::core::DBService* db_service,
    sqlite3* db,
    ProgramKindRegistry* program_registry,
    std::int64_t graph_revision_id,
    std::int64_t source_savestate_id,
    int priority,
    std::string* error_out) {
    using namespace savor::db::execution::workflow;
    using namespace savor::runner::parallel::savordb;

    if (db_service == nullptr
        || db_service->ExecutionDb() == nullptr
        || program_registry == nullptr
        || graph_revision_id <= 0
        || source_savestate_id <= 0) {
        SetError(error_out, "invalid schedule inputs");
        return std::nullopt;
    }

    const auto unit_registry = BuildDefaultWorkflowUnitRegistry();
    std::string activation_error;
    auto activation = BuildUnitActivationSpecFromDefinition(
        unit_registry,
        "navigation-activation",
        "navigation",
        "navigation.context_probe",
        "Navigation Context Probe",
        std::nullopt,
        std::nullopt,
        {},
        &activation_error);
    if (!activation.has_value()) {
        SetError(
            error_out,
            "failed to build navigation activation: " + activation_error);
        return std::nullopt;
    }
    if (activation->steps.size() != 1u) {
        SetError(error_out, "navigation activation did not contain one step");
        return std::nullopt;
    }
    activation->steps.front().priority = priority;

    WorkflowCreateInstanceCommand command{};
    command.workflow_kind = "workflow_graph";
    command.root_scope_kind = "manual";
    command.root_scope_id = source_savestate_id;
    command.workflow_graph_revision_id = graph_revision_id;
    command.created_by = "navigation-context-db-test";
    command.created_at_utc =
        savor::db::types::UtcNow().time_since_epoch().count();
    command.unit_activations.push_back(std::move(*activation));
    command.input_bindings.push_back({
        .node_key = "navigation",
        .input_key = "entry_savestate",
        .data_kind = "state.movie_inactive_savestate_id",
        .ref_kind = "state.savestate",
        .ref_id = source_savestate_id,
        .source_kind = "external",
    });

    ScheduledProbe out{};
    std::string error;
    if (!db_service->ExecutionDb()->WorkflowCommandService()->
            CreateWorkflowInstance(
                command,
                &out.workflow_instance_id,
                &error)) {
        SetError(error_out, "failed to create workflow: " + error);
        return std::nullopt;
    }
    const auto graph =
        db_service->ExecutionDb()->WorkflowQueryService()->
            GetWorkflowGraph(out.workflow_instance_id);
    if (!graph.has_value()
        || graph->steps.size() != 1
        || graph->unit_activations.size() != 1
        || !graph->instance.workflow_graph_revision_id.has_value()) {
        SetError(error_out, "created workflow graph is incomplete");
        return std::nullopt;
    }
    out.workflow_step_id = graph->steps.front().workflow_step_id;
    out.workflow_unit_activation_id =
        graph->unit_activations.front().workflow_unit_activation_id;
    out.workflow_graph_revision_id =
        *graph->instance.workflow_graph_revision_id;

    DBWorkflowWorkerCoordinator coordinator(
        db_service->ExecutionDb(),
        DBWorkflowWorkerCoordinatorConfig{},
        CoordinatorIntegrationConfig{.workflow_enabled = true},
        program_registry);
    const auto scheduled = coordinator.MaterializeWorkflowStep({
        .workflow_instance_id = out.workflow_instance_id,
        .workflow_step_id = out.workflow_step_id,
        .step_key = graph->steps.front().step_key,
        .step_kind = graph->steps.front().step_kind,
        .priority = priority,
    });
    if (!scheduled.has_value() || scheduled->job_set_id <= 0) {
        SetError(error_out, "coordinator did not schedule navigation probe");
        return std::nullopt;
    }
    out.job_set_id = scheduled->job_set_id;
    out.job_id = QueryInt64(
        db,
        "SELECT job_id FROM exec_job WHERE job_set_id="
            + std::to_string(out.job_set_id)
            + " ORDER BY job_id LIMIT 1;");
    if (out.job_id <= 0) {
        SetError(error_out, "scheduled job row is missing");
        return std::nullopt;
    }
    if (error_out != nullptr) {
        error_out->clear();
    }
    return out;
}

soa::navigation::ctx::NavigationContext MakeContext() {
    soa::navigation::ctx::NavigationContext context{};
    context.capture_pc = soa::navigation::ctx::CapturePc;
    context.player_worksheet = 0x80350000u;
    context.area = 111u;
    context.subarea = 2u;
    context.motion_state = 1u;
    context.motion_substate = 7u;
    context.post_input_movement_suppress = 0u;
    context.position_x = 1.25f;
    context.position_y = -2.5f;
    context.position_z = 3.75f;
    context.rotation_x_raw = 0x3f000000u;
    context.rotation_y_raw = 0x3f800000u;
    context.rotation_z_raw = 0x40000000u;
    context.previous_position_x = 1.0f;
    context.previous_position_y = -2.0f;
    context.previous_position_z = 3.0f;
    context.previous_rotation_x_raw = 0x3e800000u;
    context.previous_rotation_y_raw = 0x3f400000u;
    context.previous_rotation_z_raw = 0x3fc00000u;
    context.step_distance_carry_in = 0.5f;
    context.has_ground = true;
    context.ground_tbl_id = 0x3456u;
    return context;
}

savor::PRResult MakeSuccessfulResult(
    const std::filesystem::path& output_savestate,
    std::uint32_t entry_pc,
    std::string_view blob) {
    savor::PRResult result{};
    result.accepted = true;
    result.ps.ok = true;
    result.ps.w_err = savor::WERR_None;
    result.ps.ctx.emplace(
        savor::context::key::core::DW_RUN_OUTCOME_CODE,
        static_cast<std::uint32_t>(savor::RunToBpOutcome::Hit));
    result.ps.ctx.emplace(
        savor::context::key::navigation::ENTRY_PC,
        entry_pc);
    result.ps.ctx.emplace(
        savor::context::key::core::RUN_HIT_PC,
        soa::navigation::ctx::CapturePc);
    result.ps.ctx.emplace(
        savor::context::key::core::RUN_HIT_BP_KEY,
        static_cast<std::uint32_t>(
            bp::navigation::NavigationContextInitialPlayerInputReady));
    result.ps.ctx.emplace(
        savor::context::key::navigation::OUTCOME,
        static_cast<std::uint32_t>(
            phase::navigation::ctx::Outcome::Completed));
    result.ps.ctx.emplace(
        savor::context::key::navigation::FAILURE,
        static_cast<std::uint32_t>(
            phase::navigation::ctx::FailureCode::None));
    result.ps.ctx.emplace(
        savor::context::key::core::LAST_SAVESTATE_PATH,
        output_savestate.string());
    result.ps.ctx.emplace(
        savor::context::key::navigation::CTX_BLOB,
        std::string(blob));
    result.ps.ctx.emplace(
        savor::context::key::navigation::DIAGNOSTIC,
        std::string("capture-ready"));
    return result;
}

std::filesystem::path DecodeOutputPath(
    const savor::db::execution::programdb::ProgramKindDescriptor& descriptor,
    std::int64_t job_id) {
    const auto init = descriptor.runtime_init->BuildRuntimeInit(job_id);
    const auto job = descriptor.runtime_init->MaterializePsJob(job_id, init);
    if (!job.has_value()) {
        return {};
    }
    savor::PSContext context;
    if (!phase::navigation::ctx::decode_payload(job->payload, context)) {
        return {};
    }
    std::string path;
    if (!context.get(
            savor::context::key::navigation::OUTPUT_SAVESTATE_PATH,
            path)) {
        return {};
    }
    return path;
}

TEST(NavigationContextDb, HiddenUnitExposesOnlyTheCaptureContract) {
    const auto registry =
        savor::db::execution::workflow::BuildDefaultWorkflowUnitRegistry();
    const auto* unit = registry.Find("navigation.context_probe");
    ASSERT_NE(unit, nullptr);
    EXPECT_TRUE(unit->hidden);
    ASSERT_EQ(unit->required_inputs.size(), 1u);
    EXPECT_EQ(unit->required_inputs.front().key, "entry_savestate");
    EXPECT_EQ(
        unit->required_inputs.front().data_kind,
        "state.movie_inactive_savestate_id");
    ASSERT_EQ(unit->possible_outputs.size(), 1u);
    EXPECT_EQ(unit->possible_outputs.front().key, "navigation_context");
    EXPECT_EQ(
        unit->possible_outputs.front().data_kind,
        "state_artifact.navigation_context_id");
    ASSERT_EQ(unit->internal_step_kinds.size(), 1u);
    EXPECT_EQ(
        unit->internal_step_kinds.front(),
        "navigation.context_probe");
}

TEST_F(
    SqliteDbFixture,
    NavigationContextCoordinatorSchedulesFrozenSingleAttemptNeutralJob) {
    using namespace savor::db::execution::programdb::navigationcontext;

    std::string error;
    const auto source =
        SeedSourceSavestate(db_service_.get(), temp_root_, "schedule", &error);
    ASSERT_TRUE(source.has_value()) << error;
    const auto revision =
        SaveNavigationContextGraph(
            db_service_->AuthoringDb(),
            "schedule",
            &error);
    ASSERT_TRUE(revision.has_value()) << error;

    const auto working_root = temp_root_ / "navigation-runtime";
    ProgramKindRegistry registry;
    RegisterNavigationContextProbePhaseDescriptor(
        &registry,
        db_service_->ExecutionDb(),
        db_service_->StateDb(),
        NavigationContextPhaseRegistrationConfig{
            .working_dir_root = working_root,
        });
    const auto scheduled = CreateAndScheduleProbe(
        db_service_.get(),
        db_,
        &registry,
        *revision,
        source->savestate_id,
        37,
        &error);
    ASSERT_TRUE(scheduled.has_value()) << error;

    const auto job =
        db_service_->ExecutionDb()->GetJob(scheduled->job_id);
    ASSERT_TRUE(job.has_value());
    EXPECT_EQ(
        job->program_kind,
        static_cast<std::int32_t>(savor::PK_NavigationContextRunner));
    EXPECT_EQ(job->program_version, 1);
    EXPECT_EQ(job->program_ref_kind, "state_savestate");
    EXPECT_EQ(job->program_ref_id, source->savestate_id);
    EXPECT_EQ(job->savestate_id, source->savestate_id);
    EXPECT_EQ(job->priority, 37);
    EXPECT_EQ(job->max_attempts, 1);
    EXPECT_EQ(job->state, "QUEUED");

    const auto frozen = IniDoc::parse(job->input_ini);
    constexpr auto section = "NavigationContext.Job";
    EXPECT_EQ(frozen.get_u32(section, "version", 0), 1u);
    EXPECT_EQ(
        frozen.get_i64(section, "workflow_instance_id", 0),
        scheduled->workflow_instance_id);
    EXPECT_EQ(
        frozen.get_i64(section, "workflow_step_id", 0),
        scheduled->workflow_step_id);
    EXPECT_EQ(
        frozen.get_i64(section, "workflow_unit_activation_id", 0),
        scheduled->workflow_unit_activation_id);
    EXPECT_EQ(
        frozen.get_i64(section, "workflow_graph_revision_id", 0),
        scheduled->workflow_graph_revision_id);
    EXPECT_EQ(
        frozen.get_i64(section, "source_savestate_id", 0),
        source->savestate_id);
    EXPECT_EQ(
        frozen.get_i64(section, "source_artifact_id", 0),
        source->artifact_id);
    EXPECT_EQ(
        frozen.get_i64(section, "source_artifact_size_bytes", 0),
        source->size_bytes);
    EXPECT_EQ(
        frozen.get(section, "source_artifact_sha256", ""),
        source->sha256);
    EXPECT_FALSE(frozen.has(section, "run_timeout_ms"));

    const auto graph =
        db_service_->ExecutionDb()->WorkflowQueryService()->
            GetWorkflowGraph(scheduled->workflow_instance_id);
    ASSERT_TRUE(graph.has_value());
    ASSERT_EQ(graph->steps.size(), 1u);
    EXPECT_EQ(
        graph->steps.front().job_set_id,
        std::optional<std::int64_t>(scheduled->job_set_id));
    EXPECT_EQ(
        graph->steps.front().state,
        savor::db::execution::workflow::WorkflowStepState::Materialized);
    EXPECT_TRUE(graph->edges.empty());
    EXPECT_TRUE(graph->unit_activation_edges.empty());

    const auto* descriptor =
        registry.FindForStepKind("navigation.context_probe");
    ASSERT_NE(descriptor, nullptr);
    ASSERT_NE(descriptor->runtime_init, nullptr);
    const auto init =
        descriptor->runtime_init->BuildRuntimeInit(scheduled->job_id);
    EXPECT_EQ(init.savestate_ref_kind, "state_savestate");
    EXPECT_EQ(init.savestate_ref_id, source->savestate_id);

    const auto materialized =
        descriptor->runtime_init->MaterializePsJob(
            scheduled->job_id,
            init);
    ASSERT_TRUE(materialized.has_value());
    savor::PSContext payload_context;
    ASSERT_TRUE(
        phase::navigation::ctx::decode_payload(
            materialized->payload,
            payload_context));
    savor::GCInputFrame neutral;
    ASSERT_TRUE(payload_context.get(
        savor::context::key::navigation::NEUTRAL_INPUT,
        neutral));
    EXPECT_EQ(neutral, savor::GCInputFrame{});

    const auto tamper_sql =
        "UPDATE exec_workflow_unit_activation"
        " SET unit_kind='forged.navigation'"
        " WHERE workflow_unit_activation_id="
        + std::to_string(scheduled->workflow_unit_activation_id)
        + ";";
    ASSERT_EQ(
        SQLITE_OK,
        sqlite3_exec(
            db_,
            tamper_sql.c_str(),
            nullptr,
            nullptr,
            nullptr));
    const auto rejected_init =
        descriptor->runtime_init->BuildRuntimeInit(scheduled->job_id);
    EXPECT_EQ(rejected_init.savestate_ref_id, 0);
    EXPECT_FALSE(
        descriptor->runtime_init->MaterializePsJob(
            scheduled->job_id,
            init).has_value());
}

TEST_F(
    SqliteDbFixture,
    NavigationContextSuccessPersistsContextSavestateAndAllDedupeDerivations) {
    using namespace savor::db::execution::programdb;
    using namespace savor::db::execution::programdb::navigationcontext;

    std::string error;
    const auto source =
        SeedSourceSavestate(db_service_.get(), temp_root_, "success", &error);
    ASSERT_TRUE(source.has_value()) << error;
    const auto revision =
        SaveNavigationContextGraph(
            db_service_->AuthoringDb(),
            "success",
            &error);
    ASSERT_TRUE(revision.has_value()) << error;

    const auto working_root = temp_root_ / "navigation-success";
    ProgramKindRegistry registry;
    RegisterNavigationContextProbePhaseDescriptor(
        &registry,
        db_service_->ExecutionDb(),
        db_service_->StateDb(),
        NavigationContextPhaseRegistrationConfig{
            .working_dir_root = working_root,
        });
    const auto* descriptor =
        registry.FindForStepKind("navigation.context_probe");
    ASSERT_NE(descriptor, nullptr);
    ASSERT_NE(descriptor->result_mapper, nullptr);
    ASSERT_NE(descriptor->workflow_transition, nullptr);

    std::string context_blob;
    ASSERT_TRUE(
        soa::navigation::ctx::codec::encode(
            MakeContext(),
            context_blob));

    std::vector<std::int64_t> output_savestate_ids;
    std::vector<std::int64_t> derivation_ids;
    std::int64_t context_artifact_id = 0;
    for (int run = 0; run < 2; ++run) {
        const auto scheduled = CreateAndScheduleProbe(
            db_service_.get(),
            db_,
            &registry,
            *revision,
            source->savestate_id,
            10 + run,
            &error);
        ASSERT_TRUE(scheduled.has_value()) << error;
        const auto output_path =
            DecodeOutputPath(*descriptor, scheduled->job_id);
        ASSERT_FALSE(output_path.empty());
        std::error_code create_error;
        std::filesystem::create_directories(
            output_path.parent_path(),
            create_error);
        ASSERT_FALSE(create_error) << create_error.message();
        {
            std::ofstream output(
                output_path,
                std::ios::binary | std::ios::trunc);
            output << "navigation-output-savestate-" << run;
            ASSERT_TRUE(output.good());
        }

        constexpr std::uint32_t entry_pc = 0x801012b4u;
        const auto result = MakeSuccessfulResult(
            output_path,
            entry_pc + static_cast<std::uint32_t>(run),
            context_blob);
        const auto result_ini =
            descriptor->result_mapper->BuildResultIniFromPrResult(
                scheduled->job_id,
                result);
        const auto parsed = IniDoc::parse(result_ini);
        EXPECT_EQ(
            parsed.get_u32(
                "NavigationContext.Results",
                "entry_pc",
                0),
            entry_pc + static_cast<std::uint32_t>(run));
        const auto mapped =
            descriptor->result_mapper->MapPrimaryResult(
                scheduled->job_id,
                result_ini);
        EXPECT_EQ(mapped.result_kind, std::string(ResultKind));
        EXPECT_EQ(mapped.output_key, std::string(OutputKey));
        EXPECT_EQ(
            mapped.output_data_kind,
            std::string(OutputDataKind));
        EXPECT_EQ(
            mapped.output_ref_kind,
            std::string(OutputRefKind));
        ASSERT_GT(mapped.output_ref_id, 0);
        if (run == 0) {
            context_artifact_id = mapped.output_ref_id;
        } else {
            EXPECT_EQ(mapped.output_ref_id, context_artifact_id)
                << "identical NCTX bytes should reuse their artifact";
        }
        const auto job =
            db_service_->ExecutionDb()->GetJob(scheduled->job_id);
        ASSERT_TRUE(job.has_value());
        EXPECT_EQ(job->state, "SUCCEEDED");
    }

    ASSERT_GT(context_artifact_id, 0);
    EXPECT_EQ(
        QueryText(
            db_,
            "SELECT artifact_kind FROM state_artifact WHERE artifact_id="
                + std::to_string(context_artifact_id) + ";"),
        "OTHER");
    EXPECT_EQ(
        QueryText(
            db_,
            "SELECT file_ext FROM state_artifact WHERE artifact_id="
                + std::to_string(context_artifact_id) + ";"),
        ".nctx");

    const auto materialized_context_path =
        temp_root_ / "materialized-navigation-context.nctx";
    const auto materialized_context =
        db_service_->StateDb()->MaterializeArtifactToPath(
            context_artifact_id,
            materialized_context_path.string(),
            &error);
    ASSERT_TRUE(materialized_context.has_value()) << error;
    std::ifstream materialized_input(
        materialized_context_path,
        std::ios::binary);
    const std::string materialized_bytes{
        std::istreambuf_iterator<char>(materialized_input),
        std::istreambuf_iterator<char>()};
    ASSERT_TRUE(materialized_input.good() || materialized_input.eof());
    soa::navigation::ctx::NavigationContext materialized_value{};
    ASSERT_TRUE(
        soa::navigation::ctx::codec::decode(
            materialized_bytes,
            materialized_value));
    EXPECT_TRUE(materialized_value.has_ground);
    EXPECT_EQ(materialized_value.ground_tbl_id, 0x3456u);

    const auto derivations =
        db_service_->StateDb()->
            ListSavestateDerivationsBySourceContext(
                DerivationSourceContextKind,
                context_artifact_id);
    ASSERT_EQ(derivations.size(), 2u)
        << "one deduplicated context artifact may back multiple captures";
    for (const auto& derivation : derivations) {
        EXPECT_EQ(derivation.from_savestate_id, source->savestate_id);
        EXPECT_EQ(
            derivation.method_kind,
            std::string(DerivationMethod));
        EXPECT_EQ(
            derivation.source_context_kind,
            std::string(DerivationSourceContextKind));
        EXPECT_EQ(derivation.source_context_id, context_artifact_id);
        const auto output =
            db_service_->StateDb()->GetSavestate(
                derivation.to_savestate_id);
        ASSERT_TRUE(output.has_value());
        EXPECT_TRUE(output->is_complete);
        EXPECT_EQ(output->savestate_type, "NAVIGATION_CONTEXT");
        EXPECT_EQ(
            output->note,
            "Navigation context captured at initial player input");
        output_savestate_ids.push_back(output->savestate_id);
        derivation_ids.push_back(derivation.derivation_id);
    }
    std::sort(
        output_savestate_ids.begin(),
        output_savestate_ids.end());
    output_savestate_ids.erase(
        std::unique(
            output_savestate_ids.begin(),
            output_savestate_ids.end()),
        output_savestate_ids.end());
    EXPECT_EQ(output_savestate_ids.size(), 2u);
    std::sort(derivation_ids.begin(), derivation_ids.end());
    EXPECT_LT(derivation_ids.front(), derivation_ids.back());

    const auto transition =
        descriptor->workflow_transition->EvaluateTransition({});
    EXPECT_TRUE(transition.should_advance);
    EXPECT_FALSE(transition.terminal_failure);
    EXPECT_FALSE(transition.next_step_key.has_value());
    EXPECT_TRUE(transition.spawn_steps.empty());
}

TEST_F(
    SqliteDbFixture,
    NavigationContextFrozenSourceAndInvalidResultsFailClosed) {
    using namespace savor::db::execution::programdb;
    using namespace savor::db::execution::programdb::navigationcontext;

    std::string error;
    const auto source =
        SeedSourceSavestate(db_service_.get(), temp_root_, "fail", &error);
    ASSERT_TRUE(source.has_value()) << error;
    const auto revision =
        SaveNavigationContextGraph(
            db_service_->AuthoringDb(),
            "fail",
            &error);
    ASSERT_TRUE(revision.has_value()) << error;

    const auto working_root = temp_root_ / "navigation-fail";
    ProgramKindRegistry registry;
    RegisterNavigationContextProbePhaseDescriptor(
        &registry,
        db_service_->ExecutionDb(),
        db_service_->StateDb(),
        NavigationContextPhaseRegistrationConfig{
            .working_dir_root = working_root,
        });
    const auto* descriptor =
        registry.FindForStepKind("navigation.context_probe");
    ASSERT_NE(descriptor, nullptr);

    const auto tampered = CreateAndScheduleProbe(
        db_service_.get(),
        db_,
        &registry,
        *revision,
        source->savestate_id,
        1,
        &error);
    ASSERT_TRUE(tampered.has_value()) << error;
    {
        std::ofstream file(
            source->path,
            std::ios::binary | std::ios::trunc);
        file << std::string(
            static_cast<std::size_t>(source->size_bytes),
            'x');
        ASSERT_TRUE(file.good());
    }
    const auto rejected_init =
        descriptor->runtime_init->BuildRuntimeInit(tampered->job_id);
    EXPECT_EQ(rejected_init.savestate_ref_id, 0);
    savor::db::execution::programdb::RuntimeInitRequest forged{};
    forged.savestate_ref_kind = "state_savestate";
    forged.savestate_ref_id = source->savestate_id;
    forged.bootstrap_profile = "navigation.context_probe";
    EXPECT_FALSE(
        descriptor->runtime_init->MaterializePsJob(
            tampered->job_id,
            forged).has_value());

    const auto derivations_before =
        QueryInt64(
            db_,
            "SELECT COUNT(1) FROM state_savestate_derivation"
            " WHERE method_kind='navigation_context_capture';");
    savor::PRResult incomplete{};
    incomplete.ps.ok = false;
    incomplete.ps.w_err = savor::WERR_VMInit;
    const auto tampered_mapped =
        descriptor->result_mapper->MapPrimaryResult(
            tampered->job_id,
            descriptor->result_mapper->BuildResultIniFromPrResult(
                tampered->job_id,
                incomplete));
    EXPECT_EQ(
        tampered_mapped.result_kind,
        "state.navigation_context.failed");
    EXPECT_EQ(tampered_mapped.output_ref_id, 0);
    const auto tampered_job =
        db_service_->ExecutionDb()->GetJob(tampered->job_id);
    ASSERT_TRUE(tampered_job.has_value());
    EXPECT_EQ(tampered_job->state, "FAILED");
    EXPECT_EQ(
        QueryInt64(
            db_,
            "SELECT COUNT(1) FROM state_savestate_derivation"
            " WHERE method_kind='navigation_context_capture';"),
        derivations_before);

    const auto clean_source =
        SeedSourceSavestate(
            db_service_.get(),
            temp_root_,
            "invalid-result",
            &error);
    ASSERT_TRUE(clean_source.has_value()) << error;
    const auto invalid = CreateAndScheduleProbe(
        db_service_.get(),
        db_,
        &registry,
        *revision,
        clean_source->savestate_id,
        2,
        &error);
    ASSERT_TRUE(invalid.has_value()) << error;
    const auto artifact_count_before =
        QueryInt64(db_, "SELECT COUNT(1) FROM state_artifact;");
    const auto savestate_count_before =
        QueryInt64(db_, "SELECT COUNT(1) FROM state_savestate;");
    const auto invalid_output =
        working_root / ("job-" + std::to_string(invalid->job_id))
            / "output" / "navigation_context.sav";
    std::error_code create_error;
    std::filesystem::create_directories(
        invalid_output.parent_path(),
        create_error);
    ASSERT_FALSE(create_error) << create_error.message();
    {
        std::ofstream output(
            invalid_output,
            std::ios::binary | std::ios::trunc);
        output << "must-not-persist";
        ASSERT_TRUE(output.good());
    }
    std::string valid_blob;
    ASSERT_TRUE(
        soa::navigation::ctx::codec::encode(
            MakeContext(),
            valid_blob));
    auto wrong_hit = MakeSuccessfulResult(
        invalid_output,
        0x800e3694u,
        valid_blob);
    wrong_hit.ps.ctx[
        savor::context::key::core::RUN_HIT_PC] =
            std::uint32_t{0x800e3694u};
    const auto invalid_mapped =
        descriptor->result_mapper->MapPrimaryResult(
            invalid->job_id,
            descriptor->result_mapper->BuildResultIniFromPrResult(
                invalid->job_id,
                wrong_hit));
    EXPECT_EQ(
        invalid_mapped.result_kind,
        "state.navigation_context.failed");
    EXPECT_EQ(invalid_mapped.output_ref_id, 0);
    const auto invalid_job =
        db_service_->ExecutionDb()->GetJob(invalid->job_id);
    ASSERT_TRUE(invalid_job.has_value());
    EXPECT_EQ(invalid_job->state, "FAILED");
    EXPECT_EQ(
        QueryInt64(db_, "SELECT COUNT(1) FROM state_artifact;"),
        artifact_count_before);
    EXPECT_EQ(
        QueryInt64(db_, "SELECT COUNT(1) FROM state_savestate;"),
        savestate_count_before);
    EXPECT_EQ(
        QueryInt64(
            db_,
            "SELECT COUNT(1) FROM state_savestate_derivation"
            " WHERE method_kind='navigation_context_capture';"),
        derivations_before);
}

} // namespace

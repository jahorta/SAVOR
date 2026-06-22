#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <sqlite3.h>

#include "Common/Migrations/MigrationRunner.h"
#include "Common/DbService.h"
#include "Common/Events/EventCatalog.h"
#include "Common/Events/EventPayloadDispatch.h"
#include "Common/Events/EventPayloadValidation.h"
#include "Common/Events/EventTypeFormat.h"
#include "Common/Events/OutboxRelay.h"
#include "SavorDb.h"
#include "Analysis/SqliteAnalysisDb.h"
#include "Authoring/SqliteAuthoringDb.h"
#include "Archive/SqliteArchiveDb.h"
#include "Archive/ArchivePackageService.h"
#include "Archive/RehydrateExecutor.h"
#include "State/SqliteStateDb.h"
#include "UIRead/SqliteUiReadDb.h"
#include "Execution/ArchiveWorkflowCommands.h"
#include "Execution/Workflow/SqliteExecutionDb.h"
#include "Execution/Jobs/JobEventOrchestration.h"
#include "Execution/Workflow/WorkflowIntegrityChecks.h"
#include "Execution/Workflow/WorkflowProjector.h"
#include "Execution/Workflow/WorkflowRecoveryService.h"
#include "Execution/Workflow/AdapterChainOrchestrator.h"
#include "Execution/Workflow/WorkflowCoordinatorService.h"
#include "Execution/Workflow/WorkflowGraphRoutingService.h"
#include "Execution/Workflow/WorkflowTerminalAdvancementService.h"
#include "Execution/Workflow/WorkflowTerminalOutboxSubscriber.h"
#include "Execution/ProgramDB/BattleSingleTurn/BattleSingleTurnAdapters.h"
#include "Execution/ProgramDB/BattleContext/BattleContextProbePhaseRegistration.h"
#include "Execution/ProgramDB/SeedProbe/SeedProbePhaseRegistration.h"
#include "Execution/ProgramDB/SeedProbe/SeedProbeNeutralAdapters.h"
#include "Execution/ProgramDB/SeedProbe/SeedProbeGridAdapters.h"
#include "Execution/ProgramDB/SeedProbe/SeedProbeUniqueAdapters.h"
#include "Execution/ProgramDB/TasMovie/TasMoviePhaseRegistration.h"
#include "UIRead/Projectors/ProjectorContract.h"
#include "Execution/WorkflowCoordinatorBridge.h"
#include "Execution/DBWorkflowWorkerCoordinator.h"
#include "Execution/WorkflowSchedulerAdapter.h"
#include "Execution/StepInputAggregationService.h"
#include "Execution/JobMaterializationService.h"
#include "Utils/Hash.h"
#include "Core/Memory/Soa/Battle/BattleContextCodec.h"
#include "Runner/Breakpoints/BpRegistry.h"

#include "common/RecordingExecutionDb.h"
#include "common/AlwaysAdvanceTransitionHandler.h"
#include "common/SqliteDbFixture.h"
#include "common/savordb_helpers.h"

namespace savordb {
namespace workflow = savor::db::execution::workflow;

workflow::WorkflowCreateUnitActivationSpec TestUnitActivation(
    std::string activation_key,
    std::string unit_kind,
    std::string display_name,
    std::vector<std::string> dependencies = {},
    int priority = 1,
    int max_attempts = 1) {
    workflow::WorkflowCreateUnitActivationSpec activation{};
    activation.activation_key = activation_key;
    activation.graph_node_key = activation_key;
    activation.unit_kind = unit_kind;
    activation.display_name = display_name;
    activation.activation_params_json = "{}";
    activation.dependencies = std::move(dependencies);
    activation.steps.push_back({
        .step_key = activation_key,
        .step_kind = unit_kind,
        .priority = priority,
        .max_attempts = max_attempts,
    });
    return activation;
}

const savor::db::uiread::projectors::UiReadProjectionStreamTelemetrySnapshot* FindProjectionStream(
    const savor::db::core::DBServicePerformanceSnapshot& snapshot,
    const std::string& stream_id) {
    const auto& streams = snapshot.ui_read_projection.streams;
    const auto it = std::find_if(
        streams.begin(),
        streams.end(),
        [&stream_id](const auto& stream) {
            return stream.stream_id == stream_id;
        });
    if (it == streams.end()) {
        return nullptr;
    }
    return &(*it);
}

std::int64_t ReadInt64(sqlite3* db, const char* sql) {
    sqlite3_stmt* st = nullptr;
    EXPECT_EQ(SQLITE_OK, sqlite3_prepare_v2(db, sql, -1, &st, nullptr));
    if (st == nullptr) {
        return 0;
    }
    const auto rc = sqlite3_step(st);
    EXPECT_EQ(SQLITE_ROW, rc);
    const auto value = rc == SQLITE_ROW ? sqlite3_column_int64(st, 0) : 0;
    sqlite3_finalize(st);
    return value;
}

std::string ReadText(sqlite3* db, const char* sql) {
    sqlite3_stmt* st = nullptr;
    EXPECT_EQ(SQLITE_OK, sqlite3_prepare_v2(db, sql, -1, &st, nullptr));
    if (st == nullptr) {
        return {};
    }
    std::string value;
    if (sqlite3_step(st) == SQLITE_ROW && sqlite3_column_text(st, 0) != nullptr) {
        value = reinterpret_cast<const char*>(sqlite3_column_text(st, 0));
    }
    sqlite3_finalize(st);
    return value;
}

bool QueryPlanContains(sqlite3* db, const char* sql, const std::string& expected) {
    sqlite3_stmt* st = nullptr;
    EXPECT_EQ(SQLITE_OK, sqlite3_prepare_v2(db, sql, -1, &st, nullptr));
    if (st == nullptr) {
        return false;
    }
    bool found = false;
    while (sqlite3_step(st) == SQLITE_ROW) {
        const auto* detail = sqlite3_column_text(st, 3);
        if (detail != nullptr && std::string(reinterpret_cast<const char*>(detail)).find(expected) != std::string::npos) {
            found = true;
            break;
        }
    }
    sqlite3_finalize(st);
    return found;
}

void RunUiReadProjectionUntilCaughtUp(
    savor::db::core::DBService& service,
    const std::string& stream_id) {
    for (int attempt = 0; attempt < 10; ++attempt) {
        std::string err;
        ASSERT_TRUE(service.RunUiReadProjectionOnce(&err)) << err;
        const auto snapshot = service.SnapshotPerformance();
        const auto* stream = FindProjectionStream(snapshot, stream_id);
        ASSERT_NE(stream, nullptr);
        if (stream->lag_count == 0 && stream->dirty_count == 0 && stream->last_outbox_id == stream->source_high_water_outbox_id) {
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{ 10 });
    }
    ADD_FAILURE() << "projection stream did not catch up: " << stream_id;
}

TEST_F(SqliteDbFixture, EmbeddedMigrationsApplyOncePerContextAndTrackVersion) {
    using namespace savor::db::migrations;

    const MigrationSourceOptions embedded_options{
        .source_kind = MigrationSourceKind::Embedded,
    };

    for (const auto context : ListAllMigrationContexts()) {
        std::string err;
        EXPECT_TRUE(ApplyContextMigrations(db_, context, embedded_options, &err)) << err;

        const auto version = GetCurrentContextSchemaVersion(db_, context, &err);
        ASSERT_TRUE(version.has_value()) << err;
        EXPECT_GT(*version, 0) << "Expected non-zero version for context " << ToString(context);

        const auto entries = LoadContextMigrations(context, embedded_options);
        ASSERT_FALSE(entries.empty());

        bool applied = false;
        EXPECT_TRUE(HasMigrationBeenApplied(db_, context, entries.front().name, &applied, &err)) << err;
        EXPECT_TRUE(applied) << "Expected first migration to be marked applied for context " << ToString(context);

        // Second apply should be a no-op and still succeed.
        EXPECT_TRUE(ApplyContextMigrations(db_, context, embedded_options, &err)) << err;
    }
}

TEST_F(SqliteDbFixture, DBOwnedEventIdsAllowRepeatedStateWritesAndTasVariantEnsure) {
    auto* state_db = db_service_->StateDb();
    ASSERT_NE(state_db, nullptr);

    using savor::db::types::UtcTimePoint;
    const auto t1 = UtcTimePoint(std::chrono::milliseconds(1000));
    const auto t2 = UtcTimePoint(std::chrono::milliseconds(2000));

    std::int64_t base_artifact_id = 0;
    std::string error;
    ASSERT_TRUE(state_db->StoreArtifact(
        {
            .sha256 = "state-event-id-base",
            .size_bytes = 12,
            .compression_kind = 0,
            .filename = "base.dtm",
            .file_ext = ".dtm",
            .artifact_kind = "DTM",
            .created_at_utc = t1,
            .correlation_id = "repeat-state",
            .causation_id = "test",
        },
        &base_artifact_id,
        &error)) << error;

    std::int64_t repeated_artifact_id = 0;
    ASSERT_TRUE(state_db->StoreArtifact(
        {
            .sha256 = "state-event-id-base",
            .size_bytes = 13,
            .compression_kind = 0,
            .filename = "base-updated.dtm",
            .file_ext = ".dtm",
            .artifact_kind = "DTM",
            .created_at_utc = t2,
            .correlation_id = "repeat-state",
            .causation_id = "test",
        },
        &repeated_artifact_id,
        &error)) << error;
    EXPECT_EQ(base_artifact_id, repeated_artifact_id);
    EXPECT_EQ(2, ReadInt64(db_, "SELECT COUNT(1) FROM state_outbox_message WHERE event_type='State.ArtifactStored.v1';"));
    EXPECT_EQ(2, ReadInt64(db_, "SELECT COUNT(DISTINCT event_id) FROM state_outbox_message WHERE event_type='State.ArtifactStored.v1';"));

    std::int64_t variant_id = 0;
    ASSERT_TRUE(state_db->CreateTasVariant(
        {
            .name = "repeatable-tas-variant",
            .base_dtm_artifact_id = base_artifact_id,
            .mutation_mode = "RTC_OVERRIDE",
            .rtc_value = 7,
            .created_at_utc = t1,
            .correlation_id = "repeat-state",
            .causation_id = "test",
        },
        &variant_id,
        &error)) << error;

    std::int64_t duplicate_variant_id = 0;
    ASSERT_TRUE(state_db->CreateTasVariant(
        {
            .name = "repeatable-tas-variant",
            .base_dtm_artifact_id = base_artifact_id,
            .mutation_mode = "RTC_OVERRIDE",
            .rtc_value = 7,
            .created_at_utc = t2,
            .correlation_id = "repeat-state",
            .causation_id = "test",
        },
        &duplicate_variant_id,
        &error)) << error;
    EXPECT_EQ(variant_id, duplicate_variant_id);
    EXPECT_EQ(1, ReadInt64(db_, "SELECT COUNT(1) FROM state_tas_movie_variant WHERE name='repeatable-tas-variant';"));
    EXPECT_EQ(1, ReadInt64(db_, "SELECT COUNT(1) FROM state_outbox_message WHERE event_type='State.TasVariantCreated.v1';"));

    std::int64_t other_artifact_id = 0;
    ASSERT_TRUE(state_db->StoreArtifact(
        {
            .sha256 = "state-event-id-other",
            .size_bytes = 14,
            .compression_kind = 0,
            .filename = "other.dtm",
            .file_ext = ".dtm",
            .artifact_kind = "DTM",
            .created_at_utc = t2,
            .correlation_id = "repeat-state",
            .causation_id = "test",
        },
        &other_artifact_id,
        &error)) << error;

    EXPECT_FALSE(state_db->CreateTasVariant(
        {
            .name = "repeatable-tas-variant",
            .base_dtm_artifact_id = other_artifact_id,
            .mutation_mode = "RTC_OVERRIDE",
            .rtc_value = 7,
            .created_at_utc = t2,
            .correlation_id = "repeat-state",
            .causation_id = "test",
        },
        nullptr,
        &error));
    EXPECT_NE(std::string::npos, error.find("different defining fields"));
}

TEST_F(SqliteDbFixture, TasMovieRepeatedSchedulesUseDbJobSetScopedFingerprints) {
    using namespace savor::db;
    using namespace savor::db::execution::programdb;
    using namespace savor::db::execution::programdb::tasmovie;

    auto* state_db = db_service_->StateDb();
    auto* analysis_db = db_service_->AnalysisDb();
    auto* execution_db = db_service_->ExecutionDb();
    ASSERT_NE(state_db, nullptr);
    ASSERT_NE(analysis_db, nullptr);
    ASSERT_NE(execution_db, nullptr);

    std::string error;
    std::int64_t base_artifact_id = 0;
    ASSERT_TRUE(state_db->StoreArtifact(
        {
            .sha256 = "tasmovie-repeated-job-fingerprint-base",
            .size_bytes = 12,
            .compression_kind = 0,
            .filename = "base.dtm",
            .file_ext = ".dtm",
            .artifact_kind = "DTM",
            .created_at_utc = types::UtcNow(),
            .correlation_id = "test.tasmovie.repeat",
            .causation_id = "test",
        },
        &base_artifact_id,
        &error))
        << error;

    ProgramKindRegistry registry;
    TasMoviePhaseRegistrationConfig config{};
    config.blueprint.base_dtm_artifact_id = base_artifact_id;
    config.blueprint.rtc_low = 0;
    config.blueprint.rtc_high = 0;
    config.working_dir_root = temp_root_ / "tasmovie-repeat";
    RegisterTasMoviePhaseDescriptor(&registry, execution_db, state_db, analysis_db, std::move(config));

    const auto* descriptor = registry.FindForStepKind("tas_movie");
    ASSERT_NE(descriptor, nullptr);
    ASSERT_NE(descriptor->job_persistence, nullptr);

    const auto first = descriptor->job_persistence->EncodeForQueueing(
        WorkflowStepScheduleContext{ .domain_ref_id = base_artifact_id, .step_priority = 42 });
    const auto second = descriptor->job_persistence->EncodeForQueueing(
        WorkflowStepScheduleContext{ .domain_ref_id = base_artifact_id, .step_priority = 42 });
    ASSERT_GT(first.root_job_set_id, 0);
    ASSERT_GT(second.root_job_set_id, 0);
    ASSERT_NE(first.root_job_set_id, second.root_job_set_id);

    EXPECT_EQ(1, ReadInt64(db_, "SELECT COUNT(1) FROM state_tas_movie_variant WHERE name='tasmovie-base-1-rtc-0';"));
    EXPECT_EQ(2, ReadInt64(db_, "SELECT COUNT(1) FROM exec_job WHERE program_kind=2;"));
    EXPECT_EQ(2, ReadInt64(db_, "SELECT COUNT(DISTINCT fingerprint) FROM exec_job WHERE program_kind=2;"));
    EXPECT_EQ(2, ReadInt64(db_, "SELECT COUNT(1) FROM exec_job WHERE program_kind=2 AND priority=42;"));

    sqlite3_stmt* st = nullptr;
    ASSERT_EQ(SQLITE_OK, sqlite3_prepare_v2(
        db_,
        "SELECT COUNT(1) FROM exec_job WHERE fingerprint LIKE ?1 OR fingerprint LIKE ?2;",
        -1,
        &st,
        nullptr));
    const auto first_like = "%;job_set_id=" + std::to_string(first.root_job_set_id) + ";%";
    const auto second_like = "%;job_set_id=" + std::to_string(second.root_job_set_id) + ";%";
    sqlite3_bind_text(st, 1, first_like.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, second_like.c_str(), -1, SQLITE_TRANSIENT);
    ASSERT_EQ(SQLITE_ROW, sqlite3_step(st));
    EXPECT_EQ(2, sqlite3_column_int64(st, 0));
    sqlite3_finalize(st);
}

TEST_F(SqliteDbFixture, DBOwnedEventIdsAllowRepeatedAuthoringAndAnalysisWrites) {
    auto* authoring_db = db_service_->AuthoringDb();
    auto* analysis_db = db_service_->AnalysisDb();
    ASSERT_NE(authoring_db, nullptr);
    ASSERT_NE(analysis_db, nullptr);

    using savor::db::types::UtcTimePoint;
    const auto t1 = UtcTimePoint(std::chrono::milliseconds(3000));
    const auto t2 = UtcTimePoint(std::chrono::milliseconds(4000));

    std::string error;
    std::int64_t spec_a = 0;
    ASSERT_TRUE(authoring_db->SaveSeedProbeSpec(
        {
            .name = "repeat-authoring-a",
            .priority = 1,
            .run_ms = 1000,
            .vi_stall_ms = 16,
            .min_value = 0,
            .max_value = 255,
            .created_at_utc = t1,
            .correlation_id = "repeat-authoring-analysis",
            .causation_id = "test",
        },
        &spec_a,
        &error)) << error;

    std::int64_t spec_b = 0;
    ASSERT_TRUE(authoring_db->SaveSeedProbeSpec(
        {
            .name = "repeat-authoring-b",
            .priority = 1,
            .run_ms = 1000,
            .vi_stall_ms = 16,
            .min_value = 0,
            .max_value = 255,
            .created_at_utc = t2,
            .correlation_id = "repeat-authoring-analysis",
            .causation_id = "test",
        },
        &spec_b,
        &error)) << error;
    EXPECT_NE(spec_a, spec_b);
    EXPECT_EQ(2, ReadInt64(db_, "SELECT COUNT(DISTINCT event_id) FROM au_outbox_message WHERE event_type='Authoring.SeedProbeSpecSaved.v1';"));

    std::int64_t spec_a_repeat = 0;
    ASSERT_TRUE(authoring_db->SaveSeedProbeSpec(
        {
            .name = "repeat-authoring-a",
            .priority = 1,
            .run_ms = 1000,
            .vi_stall_ms = 16,
            .min_value = 0,
            .max_value = 255,
            .created_at_utc = t2,
            .correlation_id = "repeat-authoring-analysis",
            .causation_id = "test",
        },
        &spec_a_repeat,
        &error)) << error;
    EXPECT_EQ(spec_a, spec_a_repeat);
    EXPECT_EQ(2, ReadInt64(db_, "SELECT COUNT(*) FROM au_outbox_message WHERE event_type='Authoring.SeedProbeSpecSaved.v1';"));

    EXPECT_FALSE(authoring_db->SaveSeedProbeSpec(
        {
            .name = "repeat-authoring-a",
            .priority = 1,
            .run_ms = 1000,
            .vi_stall_ms = 16,
            .min_value = 0,
            .max_value = 254,
            .created_at_utc = t2,
            .correlation_id = "repeat-authoring-analysis",
            .causation_id = "test",
        },
        nullptr,
        &error));
    EXPECT_NE(std::string::npos, error.find("different defining fields"));

    savor::db::SaveWorkflowGraphResult graph_a{};
    ASSERT_TRUE(authoring_db->SaveWorkflowGraph(
        {
            .name = "repeat-authoring-graph",
            .description = "repeat graph",
            .graph_version = 1,
            .graph_hash = "repeat-authoring-graph-hash",
            .nodes = {
                {
                    .node_key = "probe_1",
                    .unit_kind = "battle_seed_probe",
                    .display_name = "Battle Seed Probe",
                    .authored_ref_kind = std::string("seed_probe_spec"),
                    .authored_ref_id = spec_a,
                    .inputs = {
                        { .input_key = "entry_savestate", .data_kind = "state.savestate_id", .display_name = "Entry savestate" },
                    },
                },
            },
            .created_at_utc = t1,
            .correlation_id = "repeat-authoring-analysis",
            .causation_id = "test",
        },
        &graph_a,
        &error)) << error;

    savor::db::SaveWorkflowGraphResult graph_a_repeat{};
    ASSERT_TRUE(authoring_db->SaveWorkflowGraph(
        {
            .name = "repeat-authoring-graph",
            .description = "repeat graph",
            .graph_version = 1,
            .graph_hash = "repeat-authoring-graph-hash",
            .nodes = {
                {
                    .node_key = "probe_1",
                    .unit_kind = "battle_seed_probe",
                    .display_name = "Battle Seed Probe",
                    .authored_ref_kind = std::string("seed_probe_spec"),
                    .authored_ref_id = spec_a,
                    .inputs = {
                        { .input_key = "entry_savestate", .data_kind = "state.savestate_id", .display_name = "Entry savestate" },
                    },
                },
            },
            .created_at_utc = t2,
            .correlation_id = "repeat-authoring-analysis",
            .causation_id = "test",
        },
        &graph_a_repeat,
        &error)) << error;
    EXPECT_EQ(graph_a.workflow_graph_id, graph_a_repeat.workflow_graph_id);
    EXPECT_EQ(graph_a.workflow_graph_revision_id, graph_a_repeat.workflow_graph_revision_id);
    EXPECT_EQ(1, ReadInt64(db_, "SELECT COUNT(*) FROM au_outbox_message WHERE event_type='Authoring.WorkflowGraphSaved.v1';"));

    std::int64_t set_a = 0;
    ASSERT_TRUE(analysis_db->CreateSeedProbeSet(
        {
            .name = "repeat-analysis-a",
            .probe_flavor = "BATTLE_PRE",
            .breakpoint_policy_name = "default",
            .segment_source_kind = "test",
            .created_at_utc = t1,
            .correlation_id = "repeat-authoring-analysis",
            .causation_id = "test",
        },
        &set_a,
        &error)) << error;

    std::int64_t set_b = 0;
    ASSERT_TRUE(analysis_db->CreateSeedProbeSet(
        {
            .name = "repeat-analysis-b",
            .probe_flavor = "BATTLE_PRE",
            .breakpoint_policy_name = "default",
            .segment_source_kind = "test",
            .created_at_utc = t2,
            .correlation_id = "repeat-authoring-analysis",
            .causation_id = "test",
        },
        &set_b,
        &error)) << error;
    EXPECT_NE(set_a, set_b);
    EXPECT_EQ(2, ReadInt64(db_, "SELECT COUNT(DISTINCT event_id) FROM sp_outbox_message WHERE event_type='AnalysisSeedProbe.SetCreated.v1';"));
}

TEST_F(SqliteDbFixture, AllScenarioStyleRepeatedSeedWritesUseDbOwnedOutboxEventIds) {
    auto* state_db = db_service_->StateDb();
    auto* authoring_db = db_service_->AuthoringDb();
    ASSERT_NE(state_db, nullptr);
    ASSERT_NE(authoring_db, nullptr);

    using savor::db::types::UtcTimePoint;
    const auto t1 = UtcTimePoint(std::chrono::milliseconds(5000));
    const auto t2 = UtcTimePoint(std::chrono::milliseconds(6000));

    std::string error;
    for (int i = 0; i < 2; ++i) {
        const auto now = i == 0 ? t1 : t2;
        std::int64_t artifact_id = 0;
        ASSERT_TRUE(state_db->StoreArtifact(
            {
                .sha256 = "all-scenario-style-sav",
                .size_bytes = 99 + i,
                .compression_kind = 0,
                .filename = "beginning_in_first_battle.sav",
                .file_ext = ".sav",
                .artifact_kind = "SAV",
                .created_at_utc = now,
                .correlation_id = "all-scenario-style",
                .causation_id = "test",
            },
            &artifact_id,
            &error)) << error;

        std::int64_t savestate_id = 0;
        ASSERT_TRUE(state_db->CreateSavestate(
            {
                .artifact_id = artifact_id,
                .savestate_type = "dolphin",
                .note = "repeat seed",
                .is_complete = true,
                .created_at_utc = now,
                .correlation_id = "all-scenario-style",
                .causation_id = "test",
            },
            &savestate_id,
            &error)) << error;

        std::int64_t spec_id = 0;
        ASSERT_TRUE(authoring_db->SaveSeedProbeSpec(
            {
                .name = std::string("all-scenario-style-seedprobe-") + std::to_string(i),
                .priority = 1,
                .run_ms = 1000,
                .vi_stall_ms = 16,
                .min_value = 0,
                .max_value = 255,
                .created_at_utc = now,
                .correlation_id = "all-scenario-style",
                .causation_id = "test",
            },
            &spec_id,
            &error)) << error;
    }

    EXPECT_EQ(2, ReadInt64(db_, "SELECT COUNT(DISTINCT event_id) FROM state_outbox_message WHERE event_type='State.ArtifactStored.v1';"));
    EXPECT_EQ(2, ReadInt64(db_, "SELECT COUNT(DISTINCT event_id) FROM state_outbox_message WHERE event_type='State.SavestateCreated.v1';"));
    EXPECT_EQ(2, ReadInt64(db_, "SELECT COUNT(DISTINCT event_id) FROM au_outbox_message WHERE event_type='Authoring.SeedProbeSpecSaved.v1';"));
}

TEST_F(SqliteDbFixture, Stage3bWorkflowMigrationsCreateExecutionAndUiReadTables) {
    using namespace savor::db::migrations;

    const MigrationSourceOptions embedded_options{
        .source_kind = MigrationSourceKind::Embedded,
    };

    std::string err;
    ASSERT_TRUE(ApplyContextMigrations(db_, MigrationContext::Execution, embedded_options, &err)) << err;
    ASSERT_TRUE(ApplyContextMigrations(db_, MigrationContext::UIRead, embedded_options, &err)) << err;
    ASSERT_TRUE(ApplyContextMigrations(db_, MigrationContext::Archive, embedded_options, &err)) << err;

    EXPECT_TRUE(TableExists(db_, "exec_workflow_instance"));
    EXPECT_TRUE(TableExists(db_, "exec_workflow_step"));
    EXPECT_TRUE(TableExists(db_, "exec_workflow_edge"));
    EXPECT_TRUE(TableExists(db_, "exec_workflow_event"));
    EXPECT_TRUE(TableExists(db_, "exec_workflow_step_output"));
    EXPECT_TRUE(TableExists(db_, "exec_job_output"));

    EXPECT_TRUE(TableExists(db_, "ui_workflow_instance"));
    EXPECT_TRUE(TableExists(db_, "ui_workflow_step"));
    EXPECT_TRUE(TableExists(db_, "ui_workflow_edge"));
    EXPECT_TRUE(TableExists(db_, "ui_workflow_alert"));

    EXPECT_TRUE(TableExists(db_, "ar_archive_item_kind_catalog"));

    EXPECT_TRUE(ColumnExists(db_, "exec_workflow_step", "guard_kind"));
    EXPECT_TRUE(ColumnExists(db_, "exec_workflow_step", "guard_value"));
    EXPECT_TRUE(ColumnExists(db_, "exec_workflow_step", "job_set_id"));
    EXPECT_TRUE(ColumnExists(db_, "exec_workflow_step", "graph_node_key"));
    EXPECT_TRUE(ColumnExists(db_, "exec_workflow_instance", "failure_text"));
    EXPECT_TRUE(ColumnExists(db_, "exec_workflow_instance", "workflow_graph_revision_id"));
    EXPECT_TRUE(TableExists(db_, "exec_workflow_instance_input_binding"));
    EXPECT_TRUE(TableExists(db_, "exec_workflow_instance_argument"));
    EXPECT_TRUE(ColumnExists(db_, "ui_workflow_instance", "display_state"));
    EXPECT_TRUE(ColumnExists(db_, "ui_workflow_step", "job_failed_count"));
    EXPECT_TRUE(ColumnExists(db_, "ui_workflow_alert", "is_active"));

    EXPECT_TRUE(IndexExists(db_, "ix_exec_workflow_instance_state_created"));
    EXPECT_TRUE(IndexExists(db_, "ix_exec_workflow_step_instance_state_priority_ready"));
    EXPECT_TRUE(IndexExists(db_, "ix_exec_workflow_step_job_set"));
    EXPECT_TRUE(IndexExists(db_, "ix_exec_workflow_edge_instance_to"));
    EXPECT_TRUE(IndexExists(db_, "ix_exec_outbox_payload_ref"));
    EXPECT_TRUE(IndexExists(db_, "ix_exec_outbox_replay_cursor"));
    EXPECT_TRUE(IndexExists(db_, "ix_exec_job_event_job_event_id"));
    EXPECT_TRUE(IndexExists(db_, "ix_exec_workflow_step_state_instance"));
    EXPECT_TRUE(IndexExists(db_, "ix_exec_workflow_instance_graph_revision"));
    EXPECT_TRUE(IndexExists(db_, "ix_exec_workflow_instance_input_binding_instance"));
    EXPECT_TRUE(IndexExists(db_, "ix_exec_workflow_instance_argument_instance"));
    EXPECT_TRUE(IndexExists(db_, "ix_ui_workflow_instance_state_created"));
    EXPECT_TRUE(IndexExists(db_, "ix_ui_workflow_step_instance_state"));
    EXPECT_TRUE(IndexExists(db_, "ix_ui_workflow_alert_active"));
    EXPECT_TRUE(IndexExists(db_, "ix_ui_projection_dirty_entity_priority"));
    EXPECT_TRUE(IndexExists(db_, "ix_ui_job_summary_job_set_job"));
    EXPECT_TRUE(IndexExists(db_, "ix_ui_workflow_step_job_set_instance"));
    EXPECT_TRUE(IndexExists(db_, "ix_ui_workflow_instance_display_state_created"));

    const auto step_table_sql = TableCreateSql(db_, "exec_workflow_step");
    EXPECT_NE(step_table_sql.find("CHECK(state IN ('WAITING','READY','MATERIALIZED','RUNNING','COMPLETED','FAILED','SKIPPED'))"), std::string::npos);
    EXPECT_NE(step_table_sql.find("CONSTRAINT uq_exec_workflow_step_instance_step_key UNIQUE (workflow_instance_id, step_key)"), std::string::npos);
    EXPECT_NE(step_table_sql.find("CONSTRAINT uq_exec_workflow_step_job_set_id UNIQUE (job_set_id)"), std::string::npos);

    const auto instance_table_sql = TableCreateSql(db_, "exec_workflow_instance");
    EXPECT_NE(instance_table_sql.find("CHECK(state IN ('PENDING','RUNNING','COMPLETED','FAILED','CANCELED'))"), std::string::npos);
    EXPECT_NE(instance_table_sql.find("CHECK(root_scope_kind IN ('job_set','run','manual'))"), std::string::npos);
}

TEST_F(SqliteDbFixture, UiReadTerminalDisplayStateRepairMigrationCorrectsStaleTerminalRows) {
    using namespace savor::db::migrations;

    const MigrationSourceOptions embedded_options{
        .source_kind = MigrationSourceKind::Embedded,
    };

    std::string err;
    ASSERT_TRUE(ApplyContextMigrations(db_, MigrationContext::UIRead, embedded_options, &err)) << err;
    ASSERT_TRUE(ExecSql(db_, R"SQL(
INSERT INTO ui_workflow_instance(workflow_instance_id,workflow_kind,state,display_state,root_scope_kind,root_scope_id,created_by,blocked_step_count,failed_step_count,created_at_utc,started_at_utc,completed_at_utc,failure_code,failure_text)
VALUES
(6201,'repair-test','COMPLETED','WAITING','manual',NULL,'test',0,0,1000,1100,1200,NULL,NULL),
(6202,'repair-test','FAILED','QUEUED','manual',NULL,'test',0,1,1000,1100,1200,'failed','failed'),
(6203,'repair-test','RUNNING','WAITING','manual',NULL,'test',0,0,1000,1100,NULL,NULL,NULL);
DELETE FROM migration_history
WHERE context='UIRead' AND migration_name='202606181400_uiread_terminal_display_state_repair.sql';
)SQL"));

    ASSERT_TRUE(ApplyContextMigrations(db_, MigrationContext::UIRead, embedded_options, &err)) << err;
    EXPECT_EQ(ReadText(db_, "SELECT display_state FROM ui_workflow_instance WHERE workflow_instance_id=6201;"), "COMPLETED");
    EXPECT_EQ(ReadText(db_, "SELECT display_state FROM ui_workflow_instance WHERE workflow_instance_id=6202;"), "FAILED");
    EXPECT_EQ(ReadText(db_, "SELECT display_state FROM ui_workflow_instance WHERE workflow_instance_id=6203;"), "WAITING");
}

TEST_F(SqliteDbFixture, Stage5WorkflowAppendDynamicStepsCreatesReadyIdempotentChildren) {
    using namespace savor::db::execution::workflow;

    SqliteExecutionDb execution_db(db_);

    std::int64_t workflow_instance_id = 0;
    std::string error;
    WorkflowCreateInstanceCommand create{};
    create.workflow_kind = "workflow_graph";
    create.root_scope_kind = "run";
    create.root_scope_id = 9002;
    create.workflow_graph_revision_id = 1;
    create.created_by = "sqlite-fixture";
    create.created_at_utc = savor::db::types::UtcNow().time_since_epoch().count();
    create.unit_activations.push_back({
        .activation_key = "SeedProbe",
        .graph_node_key = "SeedProbe",
        .unit_kind = "test_seedprobe_chain",
        .display_name = "Test Seed Probe Chain",
        .activation_params_json = "{}",
        .steps = {
            { .step_key = "Neutral", .step_kind = "seedprobe.neutral", .priority = 1, .max_attempts = 2, .input_ref_kind = std::string("sp_probe_run"), .input_ref_id = 9002 },
            { .step_key = "Grid", .step_kind = "seedprobe.grid", .dependencies = { "Neutral" }, .priority = 1, .max_attempts = 2 },
            { .step_key = "Unique", .step_kind = "seedprobe.unique", .dependencies = { "Grid" }, .priority = 1, .max_attempts = 2 },
            { .step_key = "Done", .step_kind = "seedprobe.done", .dependencies = { "Unique" }, .priority = 1, .max_attempts = 1 },
        },
    });
    ASSERT_TRUE(execution_db.WorkflowCommandService()->CreateWorkflowInstance(create, &workflow_instance_id, &error)) << error;

    const auto graph = execution_db.WorkflowQueryService()->GetWorkflowGraph(workflow_instance_id);
    ASSERT_TRUE(graph.has_value());
    const auto parent_it = std::find_if(
        graph->steps.begin(),
        graph->steps.end(),
        [](const auto& step) { return step.step_key == "Neutral"; });
    ASSERT_NE(parent_it, graph->steps.end());

    ASSERT_TRUE(execution_db.WorkflowCommandService()->AppendDynamicSteps(
        {
            .workflow_instance_id = workflow_instance_id,
            .parent_workflow_step_id = parent_it->workflow_step_id,
            .steps = {
                {
                    .step_key = "BattleTurn/t001/w010",
                    .step_kind = "battle.single_turn",
                    .input_ref_kind = std::string("analysis_battle.wave_id"),
                    .input_ref_id = 10,
                    .priority = 5,
                    .max_attempts = 2,
                },
                {
                    .step_key = "BattleTurn/t001/w011",
                    .step_kind = "battle.single_turn",
                    .input_ref_kind = std::string("analysis_battle.wave_id"),
                    .input_ref_id = 11,
                    .priority = 5,
                    .max_attempts = 2,
                },
            },
            .requested_by = "test",
        },
        &error)) << error;

    const auto with_dynamic = execution_db.WorkflowQueryService()->GetWorkflowGraph(workflow_instance_id);
    ASSERT_TRUE(with_dynamic.has_value());
    EXPECT_EQ(with_dynamic->steps.size(), graph->steps.size() + 2);
    EXPECT_EQ(with_dynamic->edges.size(), graph->edges.size() + 2);
    EXPECT_EQ(std::count_if(
        with_dynamic->steps.begin(),
        with_dynamic->steps.end(),
        [](const auto& step) { return step.step_kind == "battle.single_turn" && step.state == WorkflowStepState::Ready; }), 2);

    ASSERT_TRUE(execution_db.WorkflowCommandService()->AppendDynamicSteps(
        {
            .workflow_instance_id = workflow_instance_id,
            .parent_workflow_step_id = parent_it->workflow_step_id,
            .steps = {
                {
                    .step_key = "BattleTurn/t001/w010",
                    .step_kind = "battle.single_turn",
                    .input_ref_kind = std::string("analysis_battle.wave_id"),
                    .input_ref_id = 10,
                    .priority = 5,
                    .max_attempts = 2,
                },
                {
                    .step_key = "BattleTurn/t001/w011",
                    .step_kind = "battle.single_turn",
                    .input_ref_kind = std::string("analysis_battle.wave_id"),
                    .input_ref_id = 11,
                    .priority = 5,
                    .max_attempts = 2,
                },
            },
            .requested_by = "test-retry",
        },
        &error)) << error;

    const auto after_retry = execution_db.WorkflowQueryService()->GetWorkflowGraph(workflow_instance_id);
    ASSERT_TRUE(after_retry.has_value());
    EXPECT_EQ(after_retry->steps.size(), with_dynamic->steps.size());
    EXPECT_EQ(after_retry->edges.size(), with_dynamic->edges.size());
}

TEST_F(SqliteDbFixture, BattleContextProbeEnqueueUsesWorkflowStepPriority) {
    using namespace savor::db;
    using namespace savor::db::execution::programdb;
    using namespace savor::db::execution::programdb::battlecontext;

    auto* analysis_db = db_service_->AnalysisDb();
    auto* execution_db = db_service_->ExecutionDb();
    ASSERT_NE(analysis_db, nullptr);
    ASSERT_NE(execution_db, nullptr);

    const auto now = types::UtcTimePoint(std::chrono::milliseconds(1781000000100));
    std::string err;

    std::int64_t battle_set_id = 0;
    ASSERT_TRUE(analysis_db->CreateBattleSet(
        {
            .name = "context-probe-priority-set",
            .entry_savestate_id = 101,
            .battle_run_spec_id = 202,
            .explorer_settings_id = 303,
            .status = BattleSetStatus::Active,
            .created_at_utc = now,
            .correlation_id = "context-probe-priority",
            .causation_id = "test",
        },
        &battle_set_id,
        &err)) << err;

    std::int64_t seed_candidate_id = 0;
    ASSERT_TRUE(analysis_db->AddBattleSeedCandidate(
        {
            .battle_set_id = battle_set_id,
            .seed_value = 12345,
            .source_kind = BattleSeedCandidateSourceKind::Synthetic,
            .candidate_status = BattleSeedCandidateStatus::Ready,
            .created_at_utc = now,
            .correlation_id = "context-probe-priority",
            .causation_id = "test",
        },
        &seed_candidate_id,
        &err)) << err;

    std::int64_t wave_id = 0;
    ASSERT_TRUE(analysis_db->CreateBattleTurnWave(
        {
            .battle_set_id = battle_set_id,
            .turn_index = 1,
            .seed_candidate_id = seed_candidate_id,
            .status = BattleTurnWaveStatus::Ready,
            .created_at_utc = now,
            .correlation_id = "context-probe-priority",
            .causation_id = "test",
        },
        &wave_id,
        &err)) << err;

    const auto descriptor = BuildBattleContextProbeDescriptor(execution_db, analysis_db, {});
    ASSERT_NE(descriptor.job_persistence, nullptr);
    const auto scheduled = descriptor.job_persistence->EncodeForQueueing(
        WorkflowStepScheduleContext{ .domain_ref_id = wave_id, .step_priority = 42 });
    ASSERT_GT(scheduled.root_job_set_id, 0);
    EXPECT_EQ(
        ReadInt64(db_, ("SELECT priority FROM exec_job WHERE job_set_id=" + std::to_string(scheduled.root_job_set_id) + ";").c_str()),
        42);
}

TEST_F(SqliteDbFixture, BattleSingleTurnEnqueueUsesWorkflowStepPriority) {
    using namespace savor::db;
    using namespace savor::db::execution::programdb;
    using namespace savor::db::execution::programdb::battle;

    auto* analysis_db = db_service_->AnalysisDb();
    auto* authoring_db = db_service_->AuthoringDb();
    auto* execution_db = db_service_->ExecutionDb();
    ASSERT_NE(analysis_db, nullptr);
    ASSERT_NE(authoring_db, nullptr);
    ASSERT_NE(execution_db, nullptr);

    const auto now = types::UtcTimePoint(std::chrono::milliseconds(1781000000200));
    std::string err;

    std::int64_t battle_run_spec_id = 0;
    ASSERT_TRUE(authoring_db->SaveBattleRunSpec(
        {
            .name = "single-turn-priority-run",
            .priority = 7,
            .run_ms = 1000,
            .vi_stall_ms = 0,
            .use_single_turn_runner = true,
            .created_at_utc = now,
            .correlation_id = "single-turn-priority",
            .causation_id = "test",
        },
        &battle_run_spec_id,
        &err)) << err;

    std::int64_t plan_id = 0;
    ASSERT_TRUE(authoring_db->SavePlan(
        {
            .name = "single-turn-priority-plan",
            .fingerprint = "single-turn-priority-plan",
            .num_turns = 1,
            .created_at_utc = now,
            .correlation_id = "single-turn-priority",
            .causation_id = "test",
        },
        &plan_id,
        &err)) << err;

    std::int64_t action_preset_id = 0;
    ASSERT_TRUE(authoring_db->SaveBattlePlanActionPreset(
        {
            .name = "attack-enemy",
            .macro = BattlePlanActionMacro::Attack,
            .target_kind = BattlePlanTargetKind::SingleEnemy,
            .target_single_slot = 4,
            .created_at_utc = now,
            .correlation_id = "single-turn-priority",
            .causation_id = "test",
        },
        &action_preset_id,
        &err)) << err;

    std::int64_t plan_turn_id = 0;
    ASSERT_TRUE(authoring_db->SaveBattlePlanTurn(
        {
            .plan_id = plan_id,
            .turn_index = 1,
            .actions = {
                {
                    .actor_slot = 0,
                    .action_preset_id = action_preset_id,
                    .ordinal = 0,
                },
            },
            .replace_existing_actions = true,
            .created_at_utc = now,
            .correlation_id = "single-turn-priority",
            .causation_id = "test",
        },
        &plan_turn_id,
        &err)) << err;

    std::int64_t explorer_settings_id = 0;
    ASSERT_TRUE(authoring_db->SaveExplorerSettings(
        {
            .name = "single-turn-priority-settings",
            .default_plan_id = plan_id,
            .created_at_utc = now,
            .correlation_id = "single-turn-priority",
            .causation_id = "test",
        },
        &explorer_settings_id,
        &err)) << err;

    std::int64_t battle_set_id = 0;
    ASSERT_TRUE(analysis_db->CreateBattleSet(
        {
            .name = "single-turn-priority-set",
            .entry_savestate_id = 101,
            .battle_run_spec_id = battle_run_spec_id,
            .explorer_settings_id = explorer_settings_id,
            .status = BattleSetStatus::Active,
            .created_at_utc = now,
            .correlation_id = "single-turn-priority",
            .causation_id = "test",
        },
        &battle_set_id,
        &err)) << err;

    std::int64_t seed_candidate_id = 0;
    ASSERT_TRUE(analysis_db->AddBattleSeedCandidate(
        {
            .battle_set_id = battle_set_id,
            .seed_value = 12345,
            .source_kind = BattleSeedCandidateSourceKind::Synthetic,
            .candidate_status = BattleSeedCandidateStatus::Ready,
            .created_at_utc = now,
            .correlation_id = "single-turn-priority",
            .causation_id = "test",
        },
        &seed_candidate_id,
        &err)) << err;

    std::int64_t wave_id = 0;
    ASSERT_TRUE(analysis_db->CreateBattleTurnWave(
        {
            .battle_set_id = battle_set_id,
            .turn_index = 1,
            .seed_candidate_id = seed_candidate_id,
            .status = BattleTurnWaveStatus::Ready,
            .created_at_utc = now,
            .correlation_id = "single-turn-priority",
            .causation_id = "test",
        },
        &wave_id,
        &err)) << err;

    std::int64_t context_probe_id = 0;
    ASSERT_TRUE(analysis_db->CreateBattleContextProbe(
        {
            .wave_id = wave_id,
            .source_savestate_id = 101,
            .probe_status = BattleContextProbeStatus::Queued,
            .created_at_utc = now,
            .correlation_id = "single-turn-priority",
            .causation_id = "test",
        },
        &context_probe_id,
        &err)) << err;
    ASSERT_TRUE(analysis_db->SetBattleContextProbeExecJobId(context_probe_id, 9201, &err)) << err;

    soa::battle::ctx::BattleContext battle_context{};
    for (std::uint32_t slot = 0; slot < 4; ++slot) {
        battle_context.slots_[slot].is_player = 1;
        battle_context.slots_[slot].present = 1;
        battle_context.slots_[slot].is_alive = 1;
    }
    battle_context.slots_[4].is_player = 0;
    battle_context.slots_[4].present = 1;
    battle_context.slots_[4].is_alive = 1;
    std::string context_blob;
    ASSERT_TRUE(soa::battle::ctx::codec::encode(battle_context, context_blob));
    ASSERT_TRUE(analysis_db->CompleteBattleContextProbe(
        {
            .exec_job_id = 9201,
            .probe_status = BattleContextProbeStatus::Succeeded,
            .context_blob = context_blob,
            .context_version = soa::battle::ctx::codec::ver,
            .recorded_at_utc = now,
        },
        &err)) << err;

    const auto descriptor = BuildBattleSingleTurnDescriptor(
        execution_db,
        nullptr,
        analysis_db,
        BattleSingleTurnPhaseRegistrationConfig{
            .authoring_db = authoring_db,
            .working_dir_root = temp_root_,
        });
    ASSERT_NE(descriptor.job_persistence, nullptr);
    const auto scheduled = descriptor.job_persistence->EncodeForQueueing(
        WorkflowStepScheduleContext{ .domain_ref_id = wave_id, .step_priority = 42 });
    ASSERT_GT(scheduled.root_job_set_id, 0);
    EXPECT_EQ(
        ReadInt64(db_, ("SELECT priority FROM exec_job WHERE job_set_id=" + std::to_string(scheduled.root_job_set_id) + ";").c_str()),
        42);
}

TEST_F(SqliteDbFixture, BattleSingleTurnTransitionSpawnsNextTurnDirectlyFromReturnedContext) {
    using namespace savor::db;
    using namespace savor::db::execution::programdb;
    using namespace savor::db::execution::programdb::battle;

    auto* analysis_db = db_service_->AnalysisDb();
    auto* authoring_db = db_service_->AuthoringDb();
    ASSERT_NE(analysis_db, nullptr);
    ASSERT_NE(authoring_db, nullptr);

    const auto now = types::UtcTimePoint(std::chrono::milliseconds(1781000000000));
    std::string err;

    std::int64_t battle_run_spec_id = 0;
    ASSERT_TRUE(authoring_db->SaveBattleRunSpec(
        {
            .name = "direct-context-chain",
            .priority = 7,
            .run_ms = 10000,
            .vi_stall_ms = 1000,
            .progress_enable = true,
            .use_single_turn_runner = true,
            .auto_wave_trigger_enable = true,
            .created_at_utc = now,
            .correlation_id = "direct-context",
            .causation_id = "test",
        },
        &battle_run_spec_id,
        &err)) << err;

    std::int64_t plan_id = 0;
    ASSERT_TRUE(authoring_db->SavePlan(
        {
            .name = "two-turn-plan",
            .fingerprint = "two-turn-plan-fp",
            .num_turns = 2,
            .created_at_utc = now,
            .correlation_id = "direct-context",
            .causation_id = "test",
        },
        &plan_id,
        &err)) << err;

    std::int64_t action_preset_id = 0;
    ASSERT_TRUE(authoring_db->SaveBattlePlanActionPreset(
        {
            .name = "attack-first-target",
            .macro = BattlePlanActionMacro::Attack,
            .target_kind = BattlePlanTargetKind::SingleEnemy,
            .target_single_slot = 0,
            .created_at_utc = now,
            .correlation_id = "direct-context",
            .causation_id = "test",
        },
        &action_preset_id,
        &err)) << err;

    for (int turn_index = 1; turn_index <= 2; ++turn_index) {
        std::int64_t plan_turn_id = 0;
        ASSERT_TRUE(authoring_db->SaveBattlePlanTurn(
            {
                .plan_id = plan_id,
                .turn_index = turn_index,
                .actions = {
                    {
                        .actor_slot = 0,
                        .action_preset_id = action_preset_id,
                        .ordinal = 0,
                    },
                },
                .replace_existing_actions = true,
                .created_at_utc = now,
                .correlation_id = "direct-context",
                .causation_id = "test",
            },
            &plan_turn_id,
            &err)) << err;
    }

    std::int64_t explorer_settings_id = 0;
    ASSERT_TRUE(authoring_db->SaveExplorerSettings(
        {
            .name = "direct-context-settings",
            .description = "Use returned battle context for follow-up turns",
            .default_plan_id = plan_id,
            .created_at_utc = now,
            .correlation_id = "direct-context",
            .causation_id = "test",
        },
        &explorer_settings_id,
        &err)) << err;

    std::int64_t battle_set_id = 0;
    ASSERT_TRUE(analysis_db->CreateBattleSet(
        {
            .name = "direct-context-set",
            .entry_savestate_id = 101,
            .battle_run_spec_id = battle_run_spec_id,
            .explorer_settings_id = explorer_settings_id,
            .status = BattleSetStatus::Active,
            .created_at_utc = now,
            .correlation_id = "direct-context",
            .causation_id = "test",
        },
        &battle_set_id,
        &err)) << err;

    std::int64_t seed_candidate_id = 0;
    ASSERT_TRUE(analysis_db->AddBattleSeedCandidate(
        {
            .battle_set_id = battle_set_id,
            .seed_value = 12345,
            .source_kind = BattleSeedCandidateSourceKind::Synthetic,
            .candidate_status = BattleSeedCandidateStatus::Ready,
            .created_at_utc = now,
            .correlation_id = "direct-context",
            .causation_id = "test",
        },
        &seed_candidate_id,
        &err)) << err;

    std::int64_t wave_id = 0;
    ASSERT_TRUE(analysis_db->CreateBattleTurnWave(
        {
            .battle_set_id = battle_set_id,
            .turn_index = 1,
            .seed_candidate_id = seed_candidate_id,
            .status = BattleTurnWaveStatus::Ready,
            .created_at_utc = now,
            .correlation_id = "direct-context",
            .causation_id = "test",
        },
        &wave_id,
        &err)) << err;

    std::int64_t turn_job_id = 0;
    ASSERT_TRUE(analysis_db->RecordBattleTurnJob(
        {
            .wave_id = wave_id,
            .exec_job_id = 7001,
            .plan_id = plan_id,
            .fake_attacks_this_turn = 0,
            .fake_attacks_used_before = 0,
            .job_state = BattleTurnJobState::Succeeded,
            .started_at_utc = now,
            .ended_at_utc = now,
            .has_results = true,
            .rng_seed = 424242,
            .battle_outcome = savor::battle::Outcome::ReachedNextTurn,
            .pred_passed = 1,
            .pred_total = 1,
            .pred_abort_run = 0,
            .output_savestate_id = 202,
            .result_context_blob_base64 = std::string("AQID"),
            .result_context_version = 1,
            .recorded_at_utc = now,
            .correlation_id = "direct-context",
            .causation_id = "test",
        },
        &turn_job_id,
        &err)) << err;

    const auto descriptor = BuildBattleSingleTurnDescriptor(
        nullptr,
        nullptr,
        analysis_db,
        BattleSingleTurnPhaseRegistrationConfig{
            .authoring_db = authoring_db,
            .working_dir_root = temp_root_,
        });
    ASSERT_NE(descriptor.workflow_transition, nullptr);

    const auto decision = descriptor.workflow_transition->EvaluateTransition(
        WorkflowTransitionContext{
            .workflow_instance_id = 9001,
            .workflow_step_id = 9002,
            .job_set_id = 9003,
            .workflow_kind = "workflow_graph",
            .step_key = "BattleTurn/t1/w" + std::to_string(wave_id),
            .input_ref_kind = std::string("analysis_battle.turn_wave"),
            .input_ref_id = wave_id,
        });

    ASSERT_TRUE(decision.should_advance);
    EXPECT_FALSE(decision.blocked_reason.has_value());
    ASSERT_EQ(decision.spawn_steps.size(), 1);
    EXPECT_EQ(decision.spawn_steps[0].step_kind, "battle.single_turn");
    EXPECT_EQ(decision.spawn_steps[0].input_ref_kind.value_or(""), "analysis_battle.turn_wave");
    EXPECT_EQ(decision.spawn_steps[0].priority, 0);
    EXPECT_NE(decision.spawn_steps[0].step_key.find("BattleTurn/t2/w"), std::string::npos);
    EXPECT_EQ(decision.spawn_steps[0].step_key.find("BattleContext/"), std::string::npos);

    const auto waves = analysis_db->ListBattleTurnWaves(battle_set_id);
    ASSERT_EQ(waves.size(), 2);
    const auto child_it = std::find_if(waves.begin(), waves.end(), [&](const auto& wave) {
        return wave.wave_id == decision.spawn_steps[0].input_ref_id.value_or(0);
    });
    ASSERT_NE(child_it, waves.end());
    EXPECT_EQ(child_it->turn_index, 2);
    EXPECT_EQ(child_it->status, BattleTurnWaveStatus::Ready);
    EXPECT_FALSE(child_it->context_probe_id.has_value());
    EXPECT_EQ(child_it->parent_wave_id.value_or(0), wave_id);
    EXPECT_EQ(child_it->parent_turn_job_id.value_or(0), turn_job_id);
}

TEST_F(SqliteDbFixture, Stage3cReadinessGuardRequiresStage3bWorkflowSchemaVersion) {
    using namespace savor::db::migrations;

    const MigrationSourceOptions embedded_options{
        .source_kind = MigrationSourceKind::Embedded,
    };

    sqlite3* readiness_db = nullptr;
    ASSERT_EQ(SQLITE_OK, sqlite3_open(":memory:", &readiness_db));

    std::string reason;
    EXPECT_FALSE(savor::db::Stage3cWorkflowSliceReady(readiness_db, &reason));
    EXPECT_NE(reason.find("Execution schema version"), std::string::npos);

    std::string err;
    ASSERT_TRUE(ApplyContextMigrations(readiness_db, MigrationContext::Execution, embedded_options, &err)) << err;
    ASSERT_TRUE(ApplyContextMigrations(readiness_db, MigrationContext::UIRead, embedded_options, &err)) << err;

    EXPECT_TRUE(savor::db::Stage3cWorkflowSliceReady(readiness_db, &reason)) << reason;
    EXPECT_EQ(reason, "OK");

    sqlite3_close(readiness_db);
}

TEST_F(SqliteDbFixture, Stage3cExecutionDbServicesResolveAndRoundTripQueryCommand) {
    using namespace savor::db::migrations;
    using namespace savor::db::execution::workflow;

    const MigrationSourceOptions embedded_options{ .source_kind = MigrationSourceKind::Embedded };

    std::string err;
    ASSERT_TRUE(ApplyContextMigrations(db_, MigrationContext::Execution, embedded_options, &err)) << err;
    ASSERT_TRUE(ApplyContextMigrations(db_, MigrationContext::UIRead, embedded_options, &err)) << err;

    ASSERT_TRUE(ExecSql(db_, R"SQL(
INSERT INTO exec_workflow_instance(workflow_instance_id, workflow_kind, state, root_scope_kind, created_by, created_at_utc)
VALUES(1001, 'SEED_PROBE_CHAIN', 'RUNNING', 'manual', 'test', unixepoch());
INSERT INTO exec_job_set(job_set_id, program_kind, purpose, created_at_utc)
VALUES(9501, 1, 'workflow', unixepoch());
INSERT INTO exec_workflow_step(workflow_step_id, workflow_instance_id, step_key, step_kind, state, priority, attempts, max_attempts, created_at_utc)
VALUES
  (2001,1001,'Neutral','seedprobe.neutral','FAILED',10,0,2,unixepoch()),
  (2002,1001,'Grid','seedprobe.grid','WAITING',8,0,2,unixepoch()),
  (2003,1001,'Unique','seedprobe.unique','WAITING',7,0,2,unixepoch()),
  (2004,1001,'Done','seedprobe.done','WAITING',0,0,1,unixepoch());
INSERT INTO exec_workflow_edge(workflow_edge_id, workflow_instance_id, from_step_id, to_step_id, created_at_utc)
VALUES (3001,1001,2001,2002,unixepoch()),(3002,1001,2002,2003,unixepoch()),(3003,1001,2003,2004,unixepoch());
)SQL"));

    savor::db::execution::workflow::SqliteExecutionDb execution_db(db_);
    ASSERT_NE(execution_db.WorkflowQueryService(), nullptr);
    ASSERT_NE(execution_db.WorkflowCommandService(), nullptr);

    const auto graph = execution_db.WorkflowQueryService()->GetWorkflowGraph(1001);
    ASSERT_TRUE(graph.has_value());
    EXPECT_EQ(graph->steps.size(), 4);

    std::string command_error;
    EXPECT_TRUE(execution_db.WorkflowCommandService()->RetryFailedStep({ .workflow_step_id = 2001, .requested_by = "test" }, &command_error)) << command_error;
    EXPECT_TRUE(execution_db.WorkflowCommandService()->MarkStepMaterialized(
        { .workflow_step_id = 2001, .job_set_id = 9501, .requested_by = "test" },
        &command_error))
        << command_error;
    EXPECT_TRUE(execution_db.WorkflowCommandService()->MarkStepTerminal(
        { .workflow_step_id = 2001, .terminal_state = "COMPLETED", .requested_by = "test" },
        &command_error))
        << command_error;
    // Idempotent duplicate terminal callback.
    EXPECT_TRUE(execution_db.WorkflowCommandService()->MarkStepTerminal(
        { .workflow_step_id = 2001, .terminal_state = "COMPLETED", .requested_by = "test" },
        &command_error))
        << command_error;

    const auto map = execution_db.WorkflowQueryService()->GetStepToJobSetMap(1001);
    ASSERT_EQ(map.size(), 1u);
    EXPECT_EQ(map[0].first, 2001);
    EXPECT_EQ(map[0].second, 9501);

    sqlite3_stmt* st = nullptr;
    ASSERT_EQ(SQLITE_OK, sqlite3_prepare_v2(db_, "SELECT state, attempts FROM exec_workflow_step WHERE workflow_step_id=2001;", -1, &st, nullptr));
    ASSERT_EQ(SQLITE_ROW, sqlite3_step(st));
    EXPECT_EQ(std::string(reinterpret_cast<const char*>(sqlite3_column_text(st, 0))), "COMPLETED");
    EXPECT_EQ(sqlite3_column_int(st, 1), 1);
    sqlite3_finalize(st);

    ASSERT_EQ(SQLITE_OK, sqlite3_prepare_v2(
        db_,
        "SELECT COUNT(1) FROM exec_workflow_event WHERE workflow_instance_id=1001 AND event_kind='Execution.WorkflowStepMaterialized.v1';",
        -1,
        &st,
        nullptr));
    ASSERT_EQ(SQLITE_ROW, sqlite3_step(st));
    EXPECT_EQ(sqlite3_column_int(st, 0), 1);
    sqlite3_finalize(st);

    ASSERT_EQ(SQLITE_OK, sqlite3_prepare_v2(
        db_,
        "SELECT COUNT(1) FROM exec_workflow_event WHERE workflow_instance_id=1001 AND event_kind='Execution.WorkflowStepCompleted.v1';",
        -1,
        &st,
        nullptr));
    ASSERT_EQ(SQLITE_ROW, sqlite3_step(st));
    EXPECT_EQ(sqlite3_column_int(st, 0), 1);
    sqlite3_finalize(st);
}

TEST_F(SqliteDbFixture, Stage3cWorkflowQueryListsReadyStepsWithoutGraphFanout) {
    using namespace savor::db::migrations;
    using namespace savor::db::execution::workflow;

    const MigrationSourceOptions embedded_options{ .source_kind = MigrationSourceKind::Embedded };
    std::string err;
    ASSERT_TRUE(ApplyContextMigrations(db_, MigrationContext::Execution, embedded_options, &err)) << err;

    ASSERT_TRUE(ExecSql(db_, R"SQL(
INSERT INTO exec_workflow_instance(workflow_instance_id, workflow_kind, state, root_scope_kind, created_at_utc)
VALUES
  (1101, 'SEED_PROBE_CHAIN', 'RUNNING', 'manual', unixepoch()),
  (1102, 'SEED_PROBE_CHAIN', 'PENDING', 'manual', unixepoch());
INSERT INTO exec_workflow_step(workflow_step_id, workflow_instance_id, step_key, step_kind, state, priority, ready_at_utc, attempts, max_attempts, created_at_utc)
VALUES
  (2101, 1101, 'Grid', 'seedprobe.grid', 'READY', 8, unixepoch()-5, 0, 2, unixepoch()),
  (2102, 1101, 'Unique', 'seedprobe.unique', 'READY', 10, unixepoch()-10, 0, 2, unixepoch()),
  (2103, 1101, 'Done', 'seedprobe.done', 'WAITING', 0, NULL, 0, 1, unixepoch()),
  (2104, 1102, 'Grid', 'seedprobe.grid', 'READY', 9, unixepoch()-20, 0, 2, unixepoch());
)SQL"));

    savor::db::execution::workflow::SqliteExecutionDb execution_db(db_);
    const auto ready_steps = execution_db.WorkflowQueryService()->ListReadySteps(10);
    ASSERT_EQ(ready_steps.size(), 2u);
    EXPECT_EQ(ready_steps[0].workflow_step_id, 2102);
    EXPECT_EQ(ready_steps[1].workflow_step_id, 2101);
}

TEST_F(SqliteDbFixture, Stage3cMarkStepReadyAppliesPriorityDeltaOnce) {
    using namespace savor::db::migrations;
    using namespace savor::db::execution::workflow;

    const MigrationSourceOptions embedded_options{ .source_kind = MigrationSourceKind::Embedded };
    std::string err;
    ASSERT_TRUE(ApplyContextMigrations(db_, MigrationContext::Execution, embedded_options, &err)) << err;

    ASSERT_TRUE(ExecSql(db_, R"SQL(
INSERT INTO exec_workflow_instance(workflow_instance_id, workflow_kind, state, root_scope_kind, created_at_utc)
VALUES(1201, 'priority-delta', 'RUNNING', 'manual', unixepoch());
INSERT INTO exec_workflow_step(workflow_step_id, workflow_instance_id, step_key, step_kind, state, priority, attempts, max_attempts, created_at_utc)
VALUES(2201, 1201, 'next', 'seedprobe.next', 'WAITING', 3, 0, 1, unixepoch());
)SQL"));

    savor::db::execution::workflow::SqliteExecutionDb execution_db(db_);
    ASSERT_TRUE(execution_db.WorkflowCommandService()->MarkStepReady(
        {
            .workflow_instance_id = 1201,
            .step_key = "next",
            .requested_by = "test",
            .priority_delta = 10,
        },
        &err)) << err;
    EXPECT_EQ(ReadInt64(db_, "SELECT priority FROM exec_workflow_step WHERE workflow_step_id=2201;"), 13);

    ASSERT_TRUE(execution_db.WorkflowCommandService()->MarkStepReady(
        {
            .workflow_instance_id = 1201,
            .step_key = "next",
            .requested_by = "test-repeat",
            .priority_delta = 10,
        },
        &err)) << err;
    EXPECT_EQ(ReadInt64(db_, "SELECT priority FROM exec_workflow_step WHERE workflow_step_id=2201;"), 13);
}

TEST_F(SqliteDbFixture, Stage3cWorkflowProjectorProjectsUiReadRows) {
    using namespace savor::db::migrations;
    using namespace savor::db::execution::workflow;

    const MigrationSourceOptions embedded_options{ .source_kind = MigrationSourceKind::Embedded };
    std::string err;
    ASSERT_TRUE(ApplyContextMigrations(db_, MigrationContext::Execution, embedded_options, &err)) << err;
    ASSERT_TRUE(ApplyContextMigrations(db_, MigrationContext::UIRead, embedded_options, &err)) << err;

    ASSERT_TRUE(ExecSql(db_, R"SQL(
INSERT INTO exec_workflow_instance(workflow_instance_id, workflow_kind, state, root_scope_kind, created_by, created_at_utc)
VALUES(4001, 'SEED_PROBE_CHAIN', 'RUNNING', 'manual', 'test', unixepoch());
INSERT INTO exec_job_set(job_set_id, program_kind, purpose, created_at_utc)
VALUES(9001, 1, 'workflow', unixepoch());
INSERT INTO exec_job(job_id, job_set_id, program_kind, program_version, program_ref_kind, program_ref_id, fingerprint, priority, state, attempts, max_attempts, queued_at_utc)
VALUES(9101, 9001, 1, 1, 'seedprobe', 1, 'wf-step-4001', 10, 'COMPLETED', 1, 2, unixepoch());
INSERT INTO exec_workflow_step(workflow_step_id, workflow_instance_id, step_key, step_kind, state, job_set_id, priority, attempts, max_attempts, created_at_utc, completed_at_utc)
VALUES(5001, 4001, 'Neutral', 'seedprobe.neutral', 'COMPLETED', 9001, 10, 1, 2, unixepoch(), unixepoch());
INSERT INTO exec_workflow_edge(workflow_edge_id, workflow_instance_id, from_step_id, to_step_id, created_at_utc)
VALUES(6001, 4001, 5001, 5001, unixepoch());
)SQL"));

    WorkflowProjector projector(db_);
    ASSERT_TRUE(projector.ProjectInstance(4001, &err)) << err;

    sqlite3_stmt* st = nullptr;
    ASSERT_EQ(SQLITE_OK, sqlite3_prepare_v2(db_, "SELECT state, blocked_step_count, failed_step_count FROM ui_workflow_instance WHERE workflow_instance_id=4001;", -1, &st, nullptr));
    ASSERT_EQ(SQLITE_ROW, sqlite3_step(st));
    EXPECT_EQ(std::string(reinterpret_cast<const char*>(sqlite3_column_text(st, 0))), "RUNNING");
    EXPECT_EQ(sqlite3_column_int(st, 1), 0);
    EXPECT_EQ(sqlite3_column_int(st, 2), 0);
    sqlite3_finalize(st);
}

TEST_F(SqliteDbFixture, UiReadWorkflowListFiltersProjectedBattleVictoryRows) {
    using namespace savor::db;
    using namespace savor::db::migrations;

    const MigrationSourceOptions embedded_options{ .source_kind = MigrationSourceKind::Embedded };
    std::string err;
    ASSERT_TRUE(ApplyContextMigrations(db_, MigrationContext::UIRead, embedded_options, &err)) << err;

    ASSERT_TRUE(ExecSql(db_, R"SQL(
INSERT INTO ui_workflow_instance(
    workflow_instance_id, workflow_kind, state, root_scope_kind, created_by,
    blocked_step_count, failed_step_count, created_at_utc,
    battle_advancement_rank, battle_desired_outcome_count, battle_final_victory_count, battle_selected_count)
VALUES
  (7001, 'workflow_graph', 'COMPLETED', 'manual', 'test', 0, 0, 2000, 2, 2, 1, 1),
  (7002, 'workflow_graph', 'COMPLETED', 'manual', 'test', 0, 0, 1000, 1, 1, 0, 0);
)SQL"));

    SqliteUiReadDb ui_read_db(db_);
    UiWorkflowInstanceListQuery query{};
    query.limit = 10;

    const auto all = ui_read_db.ListWorkflowInstances(query);
    ASSERT_EQ(all.items.size(), 2u);
    EXPECT_EQ(all.items.front().workflow_instance_id, 7001);
    EXPECT_EQ(all.items.front().battle_final_victory_count, 1);

    query.battle_final_victory_only = true;
    const auto victory_only = ui_read_db.ListWorkflowInstances(query);
    ASSERT_EQ(victory_only.items.size(), 1u);
    EXPECT_EQ(victory_only.items.front().workflow_instance_id, 7001);
    EXPECT_EQ(victory_only.items.front().battle_final_victory_count, 1);
}

TEST_F(SqliteDbFixture, Stage3cWorkflowProjectorProjectsAndClearsUiAlerts) {
    using namespace savor::db::migrations;
    using namespace savor::db::execution::workflow;

    const MigrationSourceOptions embedded_options{ .source_kind = MigrationSourceKind::Embedded };
    std::string err;
    ASSERT_TRUE(ApplyContextMigrations(db_, MigrationContext::Execution, embedded_options, &err)) << err;
    ASSERT_TRUE(ApplyContextMigrations(db_, MigrationContext::UIRead, embedded_options, &err)) << err;

    ASSERT_TRUE(ExecSql(db_, R"SQL(
INSERT INTO exec_workflow_instance(workflow_instance_id, workflow_kind, state, root_scope_kind, created_by, created_at_utc)
VALUES(4100, 'SEED_PROBE_CHAIN', 'RUNNING', 'manual', 'test', unixepoch());
INSERT INTO exec_workflow_step(workflow_step_id, workflow_instance_id, step_key, step_kind, state, blocked_reason, priority, attempts, max_attempts, created_at_utc, failed_at_utc)
VALUES
  (5101, 4100, 'Grid', 'seedprobe.grid', 'WAITING', 'waiting_on_dependency', 8, 0, 2, unixepoch(), NULL),
  (5102, 4100, 'Unique', 'seedprobe.unique', 'FAILED', NULL, 7, 1, 2, unixepoch(), unixepoch());
)SQL"));

    WorkflowProjector projector(db_);
    ASSERT_TRUE(projector.ProjectInstance(4100, &err)) << err;

    sqlite3_stmt* st = nullptr;
    ASSERT_EQ(SQLITE_OK, sqlite3_prepare_v2(
        db_,
        "SELECT COUNT(1) FROM ui_workflow_alert WHERE workflow_instance_id=4100 AND is_active=1;",
        -1,
        &st,
        nullptr));
    ASSERT_EQ(SQLITE_ROW, sqlite3_step(st));
    EXPECT_EQ(sqlite3_column_int(st, 0), 2);
    sqlite3_finalize(st);

    ASSERT_TRUE(ExecSql(db_, R"SQL(
UPDATE exec_workflow_step SET blocked_reason=NULL, state='COMPLETED', completed_at_utc=unixepoch()
WHERE workflow_step_id=5101;
UPDATE exec_workflow_step SET state='COMPLETED', completed_at_utc=unixepoch(), failed_at_utc=NULL
WHERE workflow_step_id=5102;
)SQL"));

    ASSERT_TRUE(projector.ProjectInstance(4100, &err)) << err;

    ASSERT_EQ(SQLITE_OK, sqlite3_prepare_v2(
        db_,
        "SELECT COUNT(1) FROM ui_workflow_alert WHERE workflow_instance_id=4100 AND is_active=1;",
        -1,
        &st,
        nullptr));
    ASSERT_EQ(SQLITE_ROW, sqlite3_step(st));
    EXPECT_EQ(sqlite3_column_int(st, 0), 0);
    sqlite3_finalize(st);

    ASSERT_EQ(SQLITE_OK, sqlite3_prepare_v2(
        db_,
        "SELECT COUNT(1) FROM ui_workflow_alert WHERE workflow_instance_id=4100 AND is_active=0 AND cleared_at_utc IS NOT NULL;",
        -1,
        &st,
        nullptr));
    ASSERT_EQ(SQLITE_ROW, sqlite3_step(st));
    EXPECT_EQ(sqlite3_column_int(st, 0), 2);
    sqlite3_finalize(st);
}

TEST_F(SqliteDbFixture, Stage3cRecoveryServiceReconcilesInFlightRunningSteps) {
    using namespace savor::db::migrations;
    using namespace savor::db::execution::workflow;

    const MigrationSourceOptions embedded_options{ .source_kind = MigrationSourceKind::Embedded };
    std::string err;
    ASSERT_TRUE(ApplyContextMigrations(db_, MigrationContext::Execution, embedded_options, &err)) << err;

    ASSERT_TRUE(ExecSql(db_, R"SQL(
INSERT INTO exec_workflow_instance(workflow_instance_id, workflow_kind, state, root_scope_kind, created_at_utc)
VALUES(7001, 'SEED_PROBE_CHAIN', 'RUNNING', 'manual', unixepoch());
INSERT INTO exec_job_set(job_set_id, program_kind, purpose, created_at_utc)
VALUES(8001, 1, 'workflow', unixepoch());
INSERT INTO exec_job(job_id, job_set_id, program_kind, program_version, program_ref_kind, program_ref_id, fingerprint, priority, state, attempts, max_attempts, queued_at_utc, ended_at_utc)
VALUES(8101, 8001, 1, 1, 'seedprobe', 1, 'recovery-step', 10, 'FAILED', 1, 2, unixepoch(), unixepoch());
INSERT INTO exec_workflow_step(workflow_step_id, workflow_instance_id, step_key, step_kind, state, job_set_id, priority, attempts, max_attempts, created_at_utc)
VALUES(7101, 7001, 'Grid', 'seedprobe.grid', 'RUNNING', 8001, 10, 1, 2, unixepoch());
)SQL"));

    WorkflowRecoveryService recovery(db_);
    WorkflowRecoveryResult result{};
    ASSERT_TRUE(recovery.ReconcileInFlightInstances(&result, &err)) << err;
    EXPECT_EQ(result.failed_steps, 1);

    sqlite3_stmt* st = nullptr;
    ASSERT_EQ(SQLITE_OK, sqlite3_prepare_v2(db_, "SELECT state FROM exec_workflow_step WHERE workflow_step_id=7101;", -1, &st, nullptr));
    ASSERT_EQ(SQLITE_ROW, sqlite3_step(st));
    EXPECT_EQ(std::string(reinterpret_cast<const char*>(sqlite3_column_text(st, 0))), "FAILED");
    sqlite3_finalize(st);

    ASSERT_EQ(SQLITE_OK, sqlite3_prepare_v2(
        db_,
        "SELECT COUNT(1) FROM exec_outbox_message WHERE payload_ref_kind='workflow_event' AND event_type='Execution.WorkflowStepFailed.v1';",
        -1,
        &st,
        nullptr));
    ASSERT_EQ(SQLITE_ROW, sqlite3_step(st));
    EXPECT_EQ(sqlite3_column_int(st, 0), 1);
    sqlite3_finalize(st);
}

TEST_F(SqliteDbFixture, Stage3cRecoveryServiceUsesJobSetAggregateTerminalState) {
    using namespace savor::db::migrations;
    using namespace savor::db::execution::workflow;

    const MigrationSourceOptions embedded_options{ .source_kind = MigrationSourceKind::Embedded };
    std::string err;
    ASSERT_TRUE(ApplyContextMigrations(db_, MigrationContext::Execution, embedded_options, &err)) << err;

    ASSERT_TRUE(ExecSql(db_, R"SQL(
INSERT INTO exec_workflow_instance(workflow_instance_id, workflow_kind, state, root_scope_kind, created_at_utc)
VALUES
  (7011, 'SEED_PROBE_CHAIN', 'RUNNING', 'manual', unixepoch()),
  (7012, 'SEED_PROBE_CHAIN', 'RUNNING', 'manual', unixepoch()),
  (7013, 'SEED_PROBE_CHAIN', 'RUNNING', 'manual', unixepoch());
INSERT INTO exec_job_set(job_set_id, program_kind, purpose, created_at_utc)
VALUES
  (8011, 1, 'workflow', unixepoch()),
  (8012, 1, 'workflow', unixepoch()),
  (8013, 1, 'workflow', unixepoch());
INSERT INTO exec_workflow_step(workflow_step_id, workflow_instance_id, step_key, step_kind, state, job_set_id, priority, attempts, max_attempts, created_at_utc)
VALUES
  (7111, 7011, 'Grid', 'seedprobe.grid', 'RUNNING', 8011, 10, 1, 2, unixepoch()),
  (7112, 7012, 'Grid', 'seedprobe.grid', 'RUNNING', 8012, 10, 1, 2, unixepoch()),
  (7113, 7013, 'Grid', 'seedprobe.grid', 'RUNNING', 8013, 10, 1, 2, unixepoch());
INSERT INTO exec_job(job_id, job_set_id, program_kind, program_version, program_ref_kind, program_ref_id, fingerprint, priority, state, attempts, max_attempts, queued_at_utc, ended_at_utc)
VALUES
  (8111, 8011, 1, 1, 'seedprobe', 1, 'agg-success-1', 10, 'SUCCEEDED', 1, 2, unixepoch(), unixepoch()),
  (8112, 8011, 1, 1, 'seedprobe', 1, 'agg-success-2', 10, 'SUCCEEDED_WINNER', 1, 2, unixepoch(), unixepoch()),
  (8121, 8012, 1, 1, 'seedprobe', 1, 'agg-fail-1', 10, 'SUCCEEDED', 1, 2, unixepoch(), unixepoch()),
  (8122, 8012, 1, 1, 'seedprobe', 1, 'agg-fail-2', 10, 'FAILED', 1, 2, unixepoch(), unixepoch()),
  (8131, 8013, 1, 1, 'seedprobe', 1, 'agg-running', 10, 'RUNNING', 1, 2, unixepoch(), NULL);
)SQL"));

    WorkflowRecoveryService recovery(db_);
    WorkflowRecoveryResult result{};
    ASSERT_TRUE(recovery.ReconcileInFlightInstances(&result, &err)) << err;
    EXPECT_EQ(result.completed_steps, 1);
    EXPECT_EQ(result.failed_steps, 1);

    sqlite3_stmt* st = nullptr;
    ASSERT_EQ(SQLITE_OK, sqlite3_prepare_v2(
        db_,
        "SELECT workflow_step_id, state FROM exec_workflow_step WHERE workflow_step_id IN (7111,7112,7113) ORDER BY workflow_step_id;",
        -1,
        &st,
        nullptr));

    ASSERT_EQ(SQLITE_ROW, sqlite3_step(st));
    EXPECT_EQ(sqlite3_column_int64(st, 0), 7111);
    EXPECT_EQ(std::string(reinterpret_cast<const char*>(sqlite3_column_text(st, 1))), "COMPLETED");
    ASSERT_EQ(SQLITE_ROW, sqlite3_step(st));
    EXPECT_EQ(sqlite3_column_int64(st, 0), 7112);
    EXPECT_EQ(std::string(reinterpret_cast<const char*>(sqlite3_column_text(st, 1))), "FAILED");
    ASSERT_EQ(SQLITE_ROW, sqlite3_step(st));
    EXPECT_EQ(sqlite3_column_int64(st, 0), 7113);
    EXPECT_EQ(std::string(reinterpret_cast<const char*>(sqlite3_column_text(st, 1))), "RUNNING");
    sqlite3_finalize(st);
}

TEST_F(SqliteDbFixture, Stage3cWorkflowIntegrityChecksDetectViolations) {
    using namespace savor::db::migrations;
    using namespace savor::db::execution::workflow;

    const MigrationSourceOptions embedded_options{ .source_kind = MigrationSourceKind::Embedded };
    std::string err;
    ASSERT_TRUE(ApplyContextMigrations(db_, MigrationContext::Execution, embedded_options, &err)) << err;

    ASSERT_TRUE(ExecSql(db_, R"SQL(
INSERT INTO exec_workflow_instance(workflow_instance_id, workflow_kind, state, root_scope_kind, created_at_utc)
VALUES(7201, 'SEED_PROBE_CHAIN', 'COMPLETED', 'manual', unixepoch());
INSERT INTO exec_workflow_step(workflow_step_id, workflow_instance_id, step_key, step_kind, state, priority, attempts, max_attempts, created_at_utc)
VALUES
  (7301, 7201, 'Grid', 'seedprobe.grid', 'RUNNING', 5, 0, 2, unixepoch()),
  (7302, 7201, 'Unique', 'seedprobe.unique', 'COMPLETED', 5, 1, 2, unixepoch());
INSERT INTO exec_workflow_edge(workflow_edge_id, workflow_instance_id, from_step_id, to_step_id, created_at_utc)
VALUES(7401, 7201, 999999, 7302, unixepoch());
)SQL"));

    WorkflowIntegrityReport report{};
    ASSERT_TRUE(RunWorkflowIntegrityChecks(db_, &report, &err)) << err;
    EXPECT_GT(report.dangling_edge_count, 0);
    EXPECT_GT(report.missing_job_set_link_count, 0);
    EXPECT_GT(report.non_terminal_step_in_completed_instance_count, 0);
    EXPECT_GT(report.non_terminal_step_in_terminal_instance_count, 0);
    EXPECT_FALSE(report.IsClean());
}

TEST_F(SqliteDbFixture, Stage3cWorkflowIntegrityChecksPassForCleanRows) {
    using namespace savor::db::migrations;
    using namespace savor::db::execution::workflow;

    const MigrationSourceOptions embedded_options{ .source_kind = MigrationSourceKind::Embedded };
    std::string err;
    ASSERT_TRUE(ApplyContextMigrations(db_, MigrationContext::Execution, embedded_options, &err)) << err;

    ASSERT_TRUE(ExecSql(db_, R"SQL(
INSERT INTO exec_workflow_instance(workflow_instance_id, workflow_kind, state, root_scope_kind, created_at_utc)
VALUES(8201, 'SEED_PROBE_CHAIN', 'COMPLETED', 'manual', unixepoch());
INSERT INTO exec_job_set(job_set_id, program_kind, purpose, created_at_utc)
VALUES(8301, 1, 'workflow', unixepoch()),
      (8302, 1, 'workflow', unixepoch());
INSERT INTO exec_workflow_step(workflow_step_id, workflow_instance_id, step_key, step_kind, state, job_set_id, priority, attempts, max_attempts, created_at_utc, completed_at_utc)
VALUES
  (8401, 8201, 'Grid', 'seedprobe.grid', 'COMPLETED', 8301, 5, 1, 2, unixepoch(), unixepoch()),
  (8402, 8201, 'Unique', 'seedprobe.unique', 'SKIPPED', 8302, 5, 1, 2, unixepoch(), unixepoch());
INSERT INTO exec_workflow_edge(workflow_edge_id, workflow_instance_id, from_step_id, to_step_id, created_at_utc)
VALUES(8501, 8201, 8401, 8402, unixepoch());
)SQL"));

    WorkflowIntegrityReport report{};
    ASSERT_TRUE(RunWorkflowIntegrityChecks(db_, &report, &err)) << err;
    EXPECT_TRUE(report.IsClean());
    EXPECT_EQ(report.non_terminal_step_in_terminal_instance_count, 0);
    EXPECT_EQ(report.completed_step_missing_completion_ts_count, 0);
}

TEST_F(SqliteDbFixture, Stage3cTerminalSubscriberProcessesTerminalJobEventsAsynchronously) {
    using namespace savor::db::execution::workflow;

    const savor::db::migrations::MigrationSourceOptions embedded_options{ .source_kind = savor::db::migrations::MigrationSourceKind::Embedded };
    std::string err;
    ASSERT_TRUE(savor::db::migrations::ApplyContextMigrations(db_, savor::db::migrations::MigrationContext::Execution, embedded_options, &err)) << err;

    ASSERT_TRUE(ExecSql(db_, R"SQL(
INSERT INTO exec_workflow_instance(workflow_instance_id, workflow_kind, state, root_scope_kind, created_by, created_at_utc)
VALUES(1, 'SEED_PROBE_CHAIN', 'RUNNING', 'manual', 'test', unixepoch()*1000);
INSERT INTO exec_job_set(job_set_id, program_kind, purpose, created_at_utc, expected_total)
VALUES(10, 1, 'workflow', unixepoch()*1000, 1);
INSERT INTO exec_workflow_step(workflow_step_id, workflow_instance_id, step_key, step_kind, state, job_set_id, priority, attempts, max_attempts, created_at_utc)
VALUES(100, 1, 'Neutral', 'seedprobe.neutral', 'MATERIALIZED', 10, 0, 0, 1, unixepoch()*1000);
INSERT INTO exec_workflow_step(workflow_step_id, workflow_instance_id, step_key, step_kind, state, priority, attempts, max_attempts, created_at_utc)
VALUES(101, 1, 'next', 'seedprobe.next', 'WAITING', 0, 0, 1, unixepoch()*1000);
INSERT INTO exec_job(job_id, job_set_id, program_kind, program_version, program_ref_kind, program_ref_id, fingerprint, priority, state, attempts, max_attempts, queued_at_utc)
VALUES(1000, 10, 1, 1, 'seedprobe_spec', 44, 'fp-1', 0, 'SUCCEEDED', 0, 1, unixepoch()*1000);
)SQL"));

    savor::db::execution::jobs::SqliteJobEventCommandService job_events(db_);
    std::string job_error;
    ASSERT_TRUE(job_events.AppendLifecycleEvent(
        {
            .kind = savor::db::execution::jobs::JobLifecycleEventKind::JobCompleted,
            .job_id = 1000,
            .terminal_state = std::string("SUCCEEDED"),
        },
        &job_error)) << job_error;

    savor::db::execution::programdb::ProgramKindRegistry registry;
    savor::db::execution::programdb::ProgramKindDescriptor descriptor{};
    descriptor.program_kind = 1;
    descriptor.program_name = "seedprobe.neutral";
    descriptor.workflow_transition = std::make_shared<AlwaysAdvanceTransitionHandler>();
    ASSERT_TRUE(registry.RegisterForStepKind("seedprobe.neutral", descriptor));
    StepCompletionGateService gate;
    AdapterChainOrchestrator orchestrator(&registry, &gate);

    SqliteWorkflowOrchestrationCommandService command_service(db_);
    WorkflowTerminalOutboxSubscriber subscriber(db_, &orchestrator, &command_service);
    WorkflowTerminalOutboxSubscriberResult result{};
    std::string sub_error;
    ASSERT_TRUE(subscriber.ConsumeFromCursor(0, 32, &result, &sub_error)) << sub_error;
    EXPECT_GE(result.handled_count, 1);

    sqlite3_stmt* st = nullptr;
    ASSERT_EQ(SQLITE_OK, sqlite3_prepare_v2(db_, "SELECT state, blocked_reason FROM exec_workflow_step WHERE workflow_step_id=100;", -1, &st, nullptr));
    ASSERT_EQ(SQLITE_ROW, sqlite3_step(st));
    EXPECT_STREQ(reinterpret_cast<const char*>(sqlite3_column_text(st, 0)), "COMPLETED");
    EXPECT_EQ(sqlite3_column_type(st, 1), SQLITE_NULL);
    sqlite3_finalize(st);

    ASSERT_EQ(SQLITE_OK, sqlite3_prepare_v2(db_, "SELECT state, ready_at_utc FROM exec_workflow_step WHERE workflow_step_id=101;", -1, &st, nullptr));
    ASSERT_EQ(SQLITE_ROW, sqlite3_step(st));
    EXPECT_STREQ(reinterpret_cast<const char*>(sqlite3_column_text(st, 0)), "READY");
    EXPECT_NE(sqlite3_column_type(st, 1), SQLITE_NULL);
    sqlite3_finalize(st);
}

TEST_F(SqliteDbFixture, Stage3cTerminalAdvancementServiceMarksNextStepReadyFromTerminalJob) {
    using namespace savor::db::execution::workflow;

    const savor::db::migrations::MigrationSourceOptions embedded_options{ .source_kind = savor::db::migrations::MigrationSourceKind::Embedded };
    std::string err;
    ASSERT_TRUE(savor::db::migrations::ApplyContextMigrations(db_, savor::db::migrations::MigrationContext::Execution, embedded_options, &err)) << err;

    ASSERT_TRUE(ExecSql(db_, R"SQL(
INSERT INTO exec_workflow_instance(workflow_instance_id, workflow_kind, state, root_scope_kind, created_by, created_at_utc)
VALUES(2, 'SEED_PROBE_CHAIN', 'RUNNING', 'manual', 'test', unixepoch()*1000);
INSERT INTO exec_job_set(job_set_id, program_kind, purpose, created_at_utc, expected_total)
VALUES(20, 1, 'workflow', unixepoch()*1000, 1);
INSERT INTO exec_workflow_step(workflow_step_id, workflow_instance_id, step_key, step_kind, state, job_set_id, priority, attempts, max_attempts, created_at_utc)
VALUES(200, 2, 'Neutral', 'seedprobe.neutral', 'MATERIALIZED', 20, 0, 0, 1, unixepoch()*1000);
INSERT INTO exec_workflow_step(workflow_step_id, workflow_instance_id, step_key, step_kind, state, priority, attempts, max_attempts, created_at_utc)
VALUES(201, 2, 'next', 'seedprobe.next', 'WAITING', 0, 0, 1, unixepoch()*1000);
INSERT INTO exec_job(job_id, job_set_id, program_kind, program_version, program_ref_kind, program_ref_id, fingerprint, priority, state, attempts, max_attempts, queued_at_utc)
VALUES(2000, 20, 1, 1, 'seedprobe_spec', 44, 'fp-2', 0, 'SUCCEEDED', 0, 1, unixepoch()*1000);
)SQL"));

    savor::db::execution::programdb::ProgramKindRegistry registry;
    savor::db::execution::programdb::ProgramKindDescriptor descriptor{};
    descriptor.program_kind = 1;
    descriptor.program_name = "seedprobe.neutral";
    descriptor.workflow_transition = std::make_shared<AlwaysAdvanceTransitionHandler>();
    ASSERT_TRUE(registry.RegisterForStepKind("seedprobe.neutral", descriptor));
    StepCompletionGateService gate;
    AdapterChainOrchestrator orchestrator(&registry, &gate);
    SqliteWorkflowOrchestrationQueryService query_service(db_);
    SqliteWorkflowOrchestrationCommandService command_service(db_);
    WorkflowTerminalAdvancementService advancement(&orchestrator, &query_service, &command_service);

    const auto terminal_ready = query_service.ListTerminalReadyStepSnapshots(10);
    ASSERT_EQ(terminal_ready.size(), 1u);
    EXPECT_EQ(terminal_ready.front().workflow_step_id, 200);
    EXPECT_EQ(terminal_ready.front().expected_total, 1);
    EXPECT_EQ(terminal_ready.front().discovered_total, 1);
    EXPECT_EQ(terminal_ready.front().terminal_total, 1);

    WorkflowTerminalAdvancementResult result{};
    ASSERT_TRUE(advancement.AdvanceForTerminalJob(2000, &result, &err)) << err;
    EXPECT_TRUE(result.snapshot_found);
    EXPECT_TRUE(result.gate_can_transition);
    EXPECT_TRUE(result.step_marked_terminal);
    EXPECT_TRUE(result.transition_evaluated);
    EXPECT_TRUE(result.advanced_next_step);

    sqlite3_stmt* st = nullptr;
    ASSERT_EQ(SQLITE_OK, sqlite3_prepare_v2(
        db_,
        "SELECT source.state, next.state, next.priority "
        "FROM exec_workflow_step source "
        "JOIN exec_workflow_step next ON next.workflow_instance_id=source.workflow_instance_id AND next.step_key='next' "
        "WHERE source.workflow_step_id=200;",
        -1,
        &st,
        nullptr));
    ASSERT_EQ(SQLITE_ROW, sqlite3_step(st));
    EXPECT_STREQ(reinterpret_cast<const char*>(sqlite3_column_text(st, 0)), "COMPLETED");
    EXPECT_STREQ(reinterpret_cast<const char*>(sqlite3_column_text(st, 1)), "READY");
    EXPECT_EQ(sqlite3_column_int(st, 2), 10);
    sqlite3_finalize(st);
}

TEST_F(SqliteDbFixture, Stage3cTerminalAdvancementBoostsDynamicSuccessorSteps) {
    using namespace savor::db::execution::workflow;

    class SpawnStepTransitionHandler final : public savor::db::execution::programdb::IWorkflowTransitionHandler {
    public:
        savor::db::execution::programdb::WorkflowTransitionDecision EvaluateTransition(
            const savor::db::execution::programdb::WorkflowTransitionContext&) const override {
            savor::db::execution::programdb::WorkflowTransitionDecision decision{};
            decision.should_advance = true;
            decision.spawn_steps.push_back({
                .step_key = "spawned-next",
                .step_kind = "mock.spawned",
                .priority = 4,
                .max_attempts = 2,
            });
            return decision;
        }
    };

    savor::db::execution::programdb::ProgramKindRegistry registry;
    savor::db::execution::programdb::ProgramKindDescriptor descriptor{};
    descriptor.program_kind = 1;
    descriptor.program_name = "mock.spawn";
    descriptor.workflow_transition = std::make_shared<SpawnStepTransitionHandler>();
    ASSERT_TRUE(registry.RegisterForStepKind("mock.spawn", descriptor));
    StepCompletionGateService gate;
    AdapterChainOrchestrator orchestrator(&registry, &gate);
    RecordingWorkflowCommandService command_service;
    WorkflowTerminalAdvancementService advancement(&orchestrator, nullptr, &command_service);

    WorkflowStepTerminalSnapshot snapshot{};
    snapshot.workflow_instance_id = 77;
    snapshot.workflow_step_id = 88;
    snapshot.job_set_id = 99;
    snapshot.expected_total = 1;
    snapshot.discovered_total = 1;
    snapshot.terminal_total = 1;
    snapshot.failed_total = 0;
    snapshot.priority = 20;
    snapshot.workflow_kind = "mock";
    snapshot.step_key = "source";
    snapshot.step_kind = "mock.spawn";

    std::string err;
    WorkflowTerminalAdvancementResult result{};
    ASSERT_TRUE(advancement.AdvanceSnapshot(snapshot, &result, &err)) << err;
    EXPECT_TRUE(result.advanced_next_step);
    EXPECT_EQ(result.spawned_step_count, 1);
    ASSERT_EQ(command_service.dynamic_step_calls.size(), 1u);
    ASSERT_EQ(command_service.dynamic_step_calls[0].steps.size(), 1u);
    EXPECT_EQ(command_service.dynamic_step_calls[0].steps[0].step_key, "spawned-next");
    EXPECT_EQ(command_service.dynamic_step_calls[0].steps[0].priority, 34);
}

TEST_F(SqliteDbFixture, Stage3cTerminalAdvancementServiceUsesRecordedStepOutputFromSnapshot) {
    using namespace savor::db::execution::workflow;

    class OutputAdvanceTransitionHandler final : public savor::db::execution::programdb::IWorkflowTransitionHandler {
    public:
        savor::db::execution::programdb::WorkflowTransitionDecision EvaluateTransition(
            const savor::db::execution::programdb::WorkflowTransitionContext& context) const override {
            savor::db::execution::programdb::WorkflowTransitionDecision decision{};
            if (!context.output_ref_kind.has_value()
                || *context.output_ref_kind != "mock.result"
                || !context.output_ref_id.has_value()
                || *context.output_ref_id != 2222) {
                decision.blocked_reason = "mock_output_missing";
                return decision;
            }
            decision.should_advance = true;
            decision.next_step_key = "next";
            return decision;
        }
    };

    const savor::db::migrations::MigrationSourceOptions embedded_options{ .source_kind = savor::db::migrations::MigrationSourceKind::Embedded };
    std::string err;
    ASSERT_TRUE(savor::db::migrations::ApplyContextMigrations(db_, savor::db::migrations::MigrationContext::Execution, embedded_options, &err)) << err;

    ASSERT_TRUE(ExecSql(db_, R"SQL(
INSERT INTO exec_workflow_instance(workflow_instance_id, workflow_kind, state, root_scope_kind, created_by, created_at_utc)
VALUES(22, 'MOCK_CHAIN', 'RUNNING', 'manual', 'test', unixepoch()*1000);
INSERT INTO exec_job_set(job_set_id, program_kind, purpose, created_at_utc, expected_total)
VALUES(220, 1, 'workflow', unixepoch()*1000, 1);
INSERT INTO exec_workflow_step(workflow_step_id, workflow_instance_id, step_key, step_kind, state, job_set_id, priority, attempts, max_attempts, created_at_utc)
VALUES(2200, 22, 'source', 'mock.source', 'MATERIALIZED', 220, 0, 0, 1, unixepoch()*1000);
INSERT INTO exec_workflow_step(workflow_step_id, workflow_instance_id, step_key, step_kind, state, priority, attempts, max_attempts, created_at_utc)
VALUES(2201, 22, 'next', 'mock.next', 'WAITING', 0, 0, 1, unixepoch()*1000);
INSERT INTO exec_job(job_id, job_set_id, program_kind, program_version, program_ref_kind, program_ref_id, fingerprint, priority, state, attempts, max_attempts, queued_at_utc)
VALUES(22000, 220, 1, 1, 'mock_spec', 44, 'fp-22', 0, 'SUCCEEDED', 0, 1, unixepoch()*1000);
)SQL"));

    SqliteWorkflowOrchestrationQueryService query_service(db_);
    SqliteWorkflowOrchestrationCommandService command_service(db_);
    ASSERT_TRUE(command_service.RecordStepOutput(
        {
            .workflow_step_id = 2200,
            .output_key = "primary",
            .output_data_kind = "mock.result_id",
            .output_ref_kind = "mock.result",
            .output_ref_id = 2222,
            .requested_by = "test",
        },
        &err)) << err;

    savor::db::execution::programdb::ProgramKindRegistry registry;
    savor::db::execution::programdb::ProgramKindDescriptor descriptor{};
    descriptor.program_kind = 1;
    descriptor.program_name = "mock.source";
    descriptor.workflow_transition = std::make_shared<OutputAdvanceTransitionHandler>();
    ASSERT_TRUE(registry.RegisterForStepKind("mock.source", descriptor));
    StepCompletionGateService gate;
    AdapterChainOrchestrator orchestrator(&registry, &gate);
    WorkflowTerminalAdvancementService advancement(&orchestrator, &query_service, &command_service);

    WorkflowTerminalAdvancementResult result{};
    ASSERT_TRUE(advancement.AdvanceForTerminalJob(22000, &result, &err)) << err;
    EXPECT_TRUE(result.snapshot_found);
    EXPECT_TRUE(result.gate_can_transition);
    EXPECT_TRUE(result.step_marked_terminal);
    EXPECT_TRUE(result.advanced_next_step);

    sqlite3_stmt* st = nullptr;
    ASSERT_EQ(SQLITE_OK, sqlite3_prepare_v2(
        db_,
        "SELECT source.state, source.output_ref_kind, source.output_ref_id, next.state "
        "FROM exec_workflow_step source "
        "JOIN exec_workflow_step next ON next.workflow_instance_id=source.workflow_instance_id AND next.step_key='next' "
        "WHERE source.workflow_step_id=2200;",
        -1,
        &st,
        nullptr));
    ASSERT_EQ(SQLITE_ROW, sqlite3_step(st));
    EXPECT_STREQ(reinterpret_cast<const char*>(sqlite3_column_text(st, 0)), "COMPLETED");
    EXPECT_STREQ(reinterpret_cast<const char*>(sqlite3_column_text(st, 1)), "mock.result");
    EXPECT_EQ(sqlite3_column_int64(st, 2), 2222);
    EXPECT_STREQ(reinterpret_cast<const char*>(sqlite3_column_text(st, 3)), "READY");
    sqlite3_finalize(st);
}

TEST_F(SqliteDbFixture, Stage3cTerminalAdvancementServiceCompletesWorkflowWhenFinalStepHasNoNextStep) {
    using namespace savor::db::execution::workflow;

    class FinalStepTransitionHandler final : public savor::db::execution::programdb::IWorkflowTransitionHandler {
    public:
        savor::db::execution::programdb::WorkflowTransitionDecision EvaluateTransition(
            const savor::db::execution::programdb::WorkflowTransitionContext&) const override {
            savor::db::execution::programdb::WorkflowTransitionDecision decision{};
            decision.should_advance = true;
            return decision;
        }
    };

    const savor::db::migrations::MigrationSourceOptions embedded_options{ .source_kind = savor::db::migrations::MigrationSourceKind::Embedded };
    std::string err;
    ASSERT_TRUE(savor::db::migrations::ApplyContextMigrations(db_, savor::db::migrations::MigrationContext::Execution, embedded_options, &err)) << err;

    ASSERT_TRUE(ExecSql(db_, R"SQL(
INSERT INTO exec_workflow_instance(workflow_instance_id, workflow_kind, state, root_scope_kind, created_by, created_at_utc)
VALUES(3, 'SEED_PROBE_CHAIN', 'RUNNING', 'manual', 'test', unixepoch()*1000);
INSERT INTO exec_job_set(job_set_id, program_kind, purpose, created_at_utc, expected_total)
VALUES(30, 1, 'workflow', unixepoch()*1000, 1);
INSERT INTO exec_workflow_step(workflow_step_id, workflow_instance_id, step_key, step_kind, state, job_set_id, priority, attempts, max_attempts, created_at_utc)
VALUES(300, 3, 'Unique', 'seedprobe.unique', 'MATERIALIZED', 30, 0, 0, 1, unixepoch()*1000);
INSERT INTO exec_job(job_id, job_set_id, program_kind, program_version, program_ref_kind, program_ref_id, fingerprint, priority, state, attempts, max_attempts, queued_at_utc)
VALUES(3000, 30, 1, 1, 'seedprobe_spec', 44, 'fp-3', 0, 'SUCCEEDED', 0, 1, unixepoch()*1000);
)SQL"));

    savor::db::execution::programdb::ProgramKindRegistry registry;
    savor::db::execution::programdb::ProgramKindDescriptor descriptor{};
    descriptor.program_kind = 1;
    descriptor.program_name = "seedprobe.unique";
    descriptor.workflow_transition = std::make_shared<FinalStepTransitionHandler>();
    ASSERT_TRUE(registry.RegisterForStepKind("seedprobe.unique", descriptor));
    StepCompletionGateService gate;
    AdapterChainOrchestrator orchestrator(&registry, &gate);
    SqliteWorkflowOrchestrationQueryService query_service(db_);
    SqliteWorkflowOrchestrationCommandService command_service(db_);
    WorkflowTerminalAdvancementService advancement(&orchestrator, &query_service, &command_service);

    WorkflowTerminalAdvancementResult result{};
    ASSERT_TRUE(advancement.AdvanceForTerminalJob(3000, &result, &err)) << err;
    EXPECT_TRUE(result.snapshot_found);
    EXPECT_TRUE(result.gate_can_transition);
    EXPECT_TRUE(result.step_marked_terminal);
    EXPECT_TRUE(result.transition_evaluated);
    EXPECT_FALSE(result.advanced_next_step);
    EXPECT_TRUE(result.workflow_completed);

    sqlite3_stmt* st = nullptr;
    ASSERT_EQ(SQLITE_OK, sqlite3_prepare_v2(
        db_,
        "SELECT i.state, s.state "
        "FROM exec_workflow_instance i "
        "JOIN exec_workflow_step s ON s.workflow_instance_id=i.workflow_instance_id "
        "WHERE i.workflow_instance_id=3 AND s.workflow_step_id=300;",
        -1,
        &st,
        nullptr));
    ASSERT_EQ(SQLITE_ROW, sqlite3_step(st));
    EXPECT_STREQ(reinterpret_cast<const char*>(sqlite3_column_text(st, 0)), "COMPLETED");
    EXPECT_STREQ(reinterpret_cast<const char*>(sqlite3_column_text(st, 1)), "COMPLETED");
    sqlite3_finalize(st);
}

TEST_F(SqliteDbFixture, Stage3cTerminalAdvancementServiceFailsWorkflowWhenTerminalJobSetHasFailures) {
    using namespace savor::db::execution::workflow;

    const savor::db::migrations::MigrationSourceOptions embedded_options{ .source_kind = savor::db::migrations::MigrationSourceKind::Embedded };
    std::string err;
    ASSERT_TRUE(savor::db::migrations::ApplyContextMigrations(db_, savor::db::migrations::MigrationContext::Execution, embedded_options, &err)) << err;

    ASSERT_TRUE(ExecSql(db_, R"SQL(
INSERT INTO exec_workflow_instance(workflow_instance_id, workflow_kind, state, root_scope_kind, created_by, created_at_utc)
VALUES(4, 'SEED_PROBE_CHAIN', 'RUNNING', 'manual', 'test', unixepoch()*1000);
INSERT INTO exec_job_set(job_set_id, program_kind, purpose, created_at_utc, expected_total)
VALUES(40, 1, 'workflow', unixepoch()*1000, 1);
INSERT INTO exec_workflow_step(workflow_step_id, workflow_instance_id, step_key, step_kind, state, job_set_id, priority, attempts, max_attempts, created_at_utc)
VALUES(400, 4, 'Neutral', 'seedprobe.neutral', 'MATERIALIZED', 40, 0, 0, 1, unixepoch()*1000);
INSERT INTO exec_workflow_step(workflow_step_id, workflow_instance_id, step_key, step_kind, state, priority, attempts, max_attempts, created_at_utc)
VALUES(401, 4, 'next', 'seedprobe.next', 'WAITING', 0, 0, 1, unixepoch()*1000);
INSERT INTO exec_job(job_id, job_set_id, program_kind, program_version, program_ref_kind, program_ref_id, fingerprint, priority, state, attempts, max_attempts, queued_at_utc)
VALUES(4000, 40, 1, 1, 'seedprobe_spec', 44, 'fp-4', 0, 'FAILED', 0, 1, unixepoch()*1000);
)SQL"));

    savor::db::execution::programdb::ProgramKindRegistry registry;
    savor::db::execution::programdb::ProgramKindDescriptor descriptor{};
    descriptor.program_kind = 1;
    descriptor.program_name = "seedprobe.neutral";
    descriptor.workflow_transition = std::make_shared<AlwaysAdvanceTransitionHandler>();
    ASSERT_TRUE(registry.RegisterForStepKind("seedprobe.neutral", descriptor));
    StepCompletionGateService gate;
    AdapterChainOrchestrator orchestrator(&registry, &gate);
    SqliteWorkflowOrchestrationQueryService query_service(db_);
    SqliteWorkflowOrchestrationCommandService command_service(db_);
    WorkflowTerminalAdvancementService advancement(&orchestrator, &query_service, &command_service);

    WorkflowTerminalAdvancementResult result{};
    ASSERT_TRUE(advancement.AdvanceForTerminalJob(4000, &result, &err)) << err;
    EXPECT_TRUE(result.snapshot_found);
    EXPECT_TRUE(result.gate_can_transition);
    EXPECT_TRUE(result.step_marked_terminal);
    EXPECT_TRUE(result.transition_evaluated);
    EXPECT_FALSE(result.advanced_next_step);
    EXPECT_FALSE(result.workflow_completed);
    EXPECT_TRUE(result.workflow_failed);

    sqlite3_stmt* st = nullptr;
    ASSERT_EQ(SQLITE_OK, sqlite3_prepare_v2(
        db_,
        "SELECT i.state, source.state, source.blocked_reason, next.state "
        "FROM exec_workflow_instance i "
        "JOIN exec_workflow_step source ON source.workflow_instance_id=i.workflow_instance_id AND source.workflow_step_id=400 "
        "JOIN exec_workflow_step next ON next.workflow_instance_id=i.workflow_instance_id AND next.step_key='next' "
        "WHERE i.workflow_instance_id=4;",
        -1,
        &st,
        nullptr));
    ASSERT_EQ(SQLITE_ROW, sqlite3_step(st));
    EXPECT_STREQ(reinterpret_cast<const char*>(sqlite3_column_text(st, 0)), "FAILED");
    EXPECT_STREQ(reinterpret_cast<const char*>(sqlite3_column_text(st, 1)), "FAILED");
    EXPECT_EQ(sqlite3_column_type(st, 2), SQLITE_NULL);
    EXPECT_STREQ(reinterpret_cast<const char*>(sqlite3_column_text(st, 3)), "WAITING");
    sqlite3_finalize(st);
}

TEST_F(SqliteDbFixture, Stage3fWorkflowProjectorOutboxReplayUsesSubscriptionCursorAndIsIdempotent) {
    using namespace savor::db::migrations;
    using namespace savor::db::execution::workflow;

    const MigrationSourceOptions embedded_options{ .source_kind = MigrationSourceKind::Embedded };
    std::string err;
    ASSERT_TRUE(ApplyContextMigrations(db_, MigrationContext::Execution, embedded_options, &err)) << err;
    ASSERT_TRUE(ApplyContextMigrations(db_, MigrationContext::UIRead, embedded_options, &err)) << err;

    ASSERT_TRUE(ExecSql(db_, R"SQL(
INSERT INTO exec_workflow_instance(workflow_instance_id, workflow_kind, state, root_scope_kind, created_by, created_at_utc)
VALUES(9201, 'SEED_PROBE_CHAIN', 'RUNNING', 'manual', 'test', unixepoch());
INSERT INTO exec_job_set(job_set_id, program_kind, purpose, created_at_utc)
VALUES(9301, 1, 'workflow', unixepoch());
INSERT INTO exec_workflow_step(workflow_step_id, workflow_instance_id, step_key, step_kind, state, job_set_id, priority, attempts, max_attempts, created_at_utc)
VALUES(9401, 9201, 'Neutral', 'seedprobe.neutral', 'MATERIALIZED', 9301, 10, 1, 2, unixepoch());
)SQL"));

    savor::db::execution::workflow::SqliteExecutionDb execution_db(db_);
    std::string cmd_error;
    ASSERT_TRUE(execution_db.WorkflowCommandService()->MarkStepTerminal(
        { .workflow_step_id = 9401, .terminal_state = "COMPLETED", .requested_by = "projector-replay-test" },
        &cmd_error))
        << cmd_error;

    WorkflowProjector projector(db_);
    sqlite3_stmt* st = nullptr;
    ASSERT_EQ(SQLITE_OK, sqlite3_prepare_v2(
        db_,
        "SELECT COUNT(1) "
        "FROM ui_projection_subscription "
        "WHERE projector_name='WorkflowProjector' "
        "AND source_context='Execution' "
        "AND source_outbox_table='exec_outbox_message';",
        -1,
        &st,
        nullptr));
    ASSERT_EQ(SQLITE_ROW, sqlite3_step(st));
    EXPECT_EQ(sqlite3_column_int(st, 0), 0);
    sqlite3_finalize(st);

    ASSERT_TRUE(projector.ProjectFromOutbox("WorkflowProjector", 100, &err)) << err;

    std::int64_t cursor_after_first = 0;
    st = nullptr;
    ASSERT_EQ(SQLITE_OK, sqlite3_prepare_v2(
        db_,
        "SELECT last_outbox_id, COALESCE(last_event_id, ''), status, COALESCE(last_error, '') "
        "FROM ui_projection_subscription "
        "WHERE projector_name='WorkflowProjector' "
        "AND source_context='Execution' "
        "AND source_outbox_table='exec_outbox_message';",
        -1,
        &st,
        nullptr));
    ASSERT_EQ(SQLITE_ROW, sqlite3_step(st));
    cursor_after_first = sqlite3_column_int64(st, 0);
    EXPECT_GT(cursor_after_first, 0);
    ASSERT_NE(sqlite3_column_text(st, 1), nullptr);
    EXPECT_FALSE(std::string(reinterpret_cast<const char*>(sqlite3_column_text(st, 1))).empty());
    ASSERT_NE(sqlite3_column_text(st, 2), nullptr);
    EXPECT_EQ(std::string(reinterpret_cast<const char*>(sqlite3_column_text(st, 2))), "ACTIVE");
    ASSERT_NE(sqlite3_column_text(st, 3), nullptr);
    EXPECT_TRUE(std::string(reinterpret_cast<const char*>(sqlite3_column_text(st, 3))).empty());
    sqlite3_finalize(st);

    st = nullptr;
    ASSERT_EQ(SQLITE_OK, sqlite3_prepare_v2(
        db_,
        "SELECT COUNT(1) FROM ui_workflow_step WHERE workflow_instance_id=9201;",
        -1,
        &st,
        nullptr));
    ASSERT_EQ(SQLITE_ROW, sqlite3_step(st));
    EXPECT_EQ(sqlite3_column_int(st, 0), 1);
    sqlite3_finalize(st);

    ASSERT_TRUE(projector.ProjectFromOutbox("WorkflowProjector", 100, &err)) << err;
    st = nullptr;
    ASSERT_EQ(SQLITE_OK, sqlite3_prepare_v2(
        db_,
        "SELECT last_outbox_id "
        "FROM ui_projection_subscription "
        "WHERE projector_name='WorkflowProjector' "
        "AND source_context='Execution' "
        "AND source_outbox_table='exec_outbox_message';",
        -1,
        &st,
        nullptr));
    ASSERT_EQ(SQLITE_ROW, sqlite3_step(st));
    EXPECT_EQ(sqlite3_column_int64(st, 0), cursor_after_first);
    sqlite3_finalize(st);
}

TEST_F(SqliteDbFixture, Stage3cOutboxRelayRoundTripsOccurredAtUtcEpochMilliseconds) {
    using namespace savor::db::events;
    using namespace savor::db::migrations;

    const MigrationSourceOptions embedded_options{ .source_kind = MigrationSourceKind::Embedded };
    std::string err;
    ASSERT_TRUE(ApplyContextMigrations(db_, MigrationContext::Execution, embedded_options, &err)) << err;

    constexpr std::int64_t kOccurredAtUtcEpochMillis = 1735689600123; // 2025-01-01T00:00:00.123Z
    ASSERT_TRUE(ExecSql(db_, R"SQL(
INSERT INTO exec_outbox_message(
    outbox_id,event_id,event_type,event_version,context_name,aggregate_kind,aggregate_id,correlation_id,causation_id,occurred_at_utc,payload_ref_kind,payload_ref_id
)
VALUES(
    6001,'evt-workflow-ms-roundtrip','Execution.WorkflowStepCompleted.v1',1,'Execution','workflow_instance','42','42','42',1735689600123,'workflow_event',4201
);
)SQL"));

    OutboxRelay relay({
        .db = db_,
        .outbox_table = "exec_outbox_message",
        .context_name = "Execution",
        .aggregate_kind = "workflow_instance",
        .payload_ref_kind = "workflow_event",
        .max_attempts = 3,
    });

    std::int64_t observed_occurred_at_utc_epoch_millis = -1;
    std::vector<OutboxRelayDispatchBinding> bindings;
    bindings.push_back(OutboxRelayDispatchBinding{
        .key = { .event_type = "Execution.WorkflowStepCompleted.v1", .event_version = 1 },
        .handler = [&observed_occurred_at_utc_epoch_millis](const EventEnvelope& envelope, std::string*) {
            observed_occurred_at_utc_epoch_millis = envelope.occurred_at_utc.time_since_epoch().count();
            return true;
        },
    });

    OutboxRelayResult result{};
    ASSERT_TRUE(relay.RelayBatch(0, 100, bindings, &result, &err)) << err;
    EXPECT_EQ(result.published_count, 1);
    EXPECT_EQ(observed_occurred_at_utc_epoch_millis, kOccurredAtUtcEpochMillis);

    sqlite3_stmt* st = nullptr;
    ASSERT_EQ(SQLITE_OK, sqlite3_prepare_v2(
        db_,
        "SELECT occurred_at_utc, published_at_utc FROM exec_outbox_message WHERE outbox_id=6001;",
        -1,
        &st,
        nullptr));
    ASSERT_EQ(SQLITE_ROW, sqlite3_step(st));
    EXPECT_EQ(sqlite3_column_int64(st, 0), kOccurredAtUtcEpochMillis);
    EXPECT_EQ(sqlite3_column_type(st, 1), SQLITE_INTEGER);
    sqlite3_finalize(st);
}

TEST_F(SqliteDbFixture, Stage3fProjectionSubscriptionSkipsDuplicateEventIdsAndAdvancesCursor) {
    using namespace savor::db::events;
    using namespace savor::db::migrations;
    using namespace savor::db::uiread::projectors;

    const MigrationSourceOptions embedded_options{ .source_kind = MigrationSourceKind::Embedded };
    std::string err;
    ASSERT_TRUE(ApplyContextMigrations(db_, MigrationContext::Execution, embedded_options, &err)) << err;
    ASSERT_TRUE(ApplyContextMigrations(db_, MigrationContext::UIRead, embedded_options, &err)) << err;

    ASSERT_TRUE(ExecSql(db_, R"SQL(
INSERT INTO exec_outbox_message(
    outbox_id,event_id,event_type,event_version,context_name,aggregate_kind,aggregate_id,occurred_at_utc,payload_ref_kind,payload_ref_id
)
VALUES(
    7001,'evt-projector-contract-dedup','Execution.JobQueued.v1',1,'Execution','job','42',unixepoch()*1000,'job',42
);
INSERT INTO ui_projection_handled_event(projector_name,event_id,last_outbox_id,handled_at_utc)
VALUES('ProjectorSubscription.exec|Execution|exec_outbox_message','evt-projector-contract-dedup',6999,unixepoch()*1000);
)SQL"));

    int handler_calls = 0;
    std::vector<OutboxRelayDispatchBinding> bindings;
    bindings.push_back(OutboxRelayDispatchBinding{
        .key = { .event_type = "Execution.JobQueued.v1", .event_version = 1 },
        .handler = [&handler_calls](const EventEnvelope&, std::string*) {
            handler_calls += 1;
            return true;
        },
    });

    ASSERT_TRUE(RunProjectorRelay(
        db_,
        "ProjectorSubscription.exec",
        "Execution",
        "exec_outbox_message",
        {
            .db = db_,
            .outbox_table = "exec_outbox_message",
            .context_name = "Execution",
            .aggregate_kind = "",
            .payload_ref_kind = "",
            .max_attempts = 5,
        },
        bindings,
        100,
        &err))
        << err;

    EXPECT_EQ(handler_calls, 0);

    sqlite3_stmt* st = nullptr;
    ASSERT_EQ(SQLITE_OK, sqlite3_prepare_v2(
        db_,
        "SELECT COALESCE(last_event_id, ''), last_outbox_id, status, COALESCE(last_error, '') "
        "FROM ui_projection_subscription "
        "WHERE projector_name='ProjectorSubscription.exec' "
        "AND source_context='Execution' "
        "AND source_outbox_table='exec_outbox_message';",
        -1,
        &st,
        nullptr));
    ASSERT_EQ(SQLITE_ROW, sqlite3_step(st));
    ASSERT_NE(sqlite3_column_text(st, 0), nullptr);
    EXPECT_EQ(std::string(reinterpret_cast<const char*>(sqlite3_column_text(st, 0))), "evt-projector-contract-dedup");
    EXPECT_EQ(sqlite3_column_int64(st, 1), 7001);
    ASSERT_NE(sqlite3_column_text(st, 2), nullptr);
    EXPECT_EQ(std::string(reinterpret_cast<const char*>(sqlite3_column_text(st, 2))), "ACTIVE");
    ASSERT_NE(sqlite3_column_text(st, 3), nullptr);
    EXPECT_TRUE(std::string(reinterpret_cast<const char*>(sqlite3_column_text(st, 3))).empty());
    sqlite3_finalize(st);
}

TEST_F(SqliteDbFixture, Stage3cWorkflowProjectorOutboxRelayFailureIncrementsAttemptsAndDeadLetters) {
    using namespace savor::db::migrations;
    using namespace savor::db::execution::workflow;

    const MigrationSourceOptions embedded_options{ .source_kind = MigrationSourceKind::Embedded };
    std::string err;
    ASSERT_TRUE(ApplyContextMigrations(db_, MigrationContext::Execution, embedded_options, &err)) << err;
    ASSERT_TRUE(ApplyContextMigrations(db_, MigrationContext::UIRead, embedded_options, &err)) << err;

    ASSERT_TRUE(ExecSql(db_, R"SQL(
INSERT INTO exec_outbox_message(
    outbox_id,event_id,event_type,event_version,context_name,aggregate_kind,aggregate_id,occurred_at_utc,payload_ref_kind,payload_ref_id
)
VALUES(
    5001,'evt-workflow-relay-missing-handler','Execution.WorkflowStepBlocked.v1',1,'Execution','workflow_instance','999',unixepoch()*1000,'workflow_event',9001
);
)SQL"));

    WorkflowProjector projector(db_);
    ASSERT_FALSE(projector.ProjectFromOutbox("WorkflowProjector", 100, &err, 2));
    EXPECT_EQ(err, "relay batch contained 1 failing event(s)");

    sqlite3_stmt* st = nullptr;
    ASSERT_EQ(SQLITE_OK, sqlite3_prepare_v2(
        db_,
        "SELECT attempt_count, last_error, published_at_utc FROM exec_outbox_message WHERE outbox_id=5001;",
        -1,
        &st,
        nullptr));
    ASSERT_EQ(SQLITE_ROW, sqlite3_step(st));
    EXPECT_EQ(sqlite3_column_int(st, 0), 1);
    ASSERT_NE(sqlite3_column_text(st, 1), nullptr);
    const std::string first_error(reinterpret_cast<const char*>(sqlite3_column_text(st, 1)));
    EXPECT_EQ(first_error.find("unsupported Execution event_type"), 0u);
    EXPECT_EQ(sqlite3_column_type(st, 2), SQLITE_NULL);
    sqlite3_finalize(st);

    ASSERT_TRUE(projector.ProjectFromOutbox("WorkflowProjector", 100, &err, 2)) << err;
    ASSERT_EQ(SQLITE_OK, sqlite3_prepare_v2(
        db_,
        "SELECT attempt_count, last_error FROM exec_outbox_message WHERE outbox_id=5001;",
        -1,
        &st,
        nullptr));
    ASSERT_EQ(SQLITE_ROW, sqlite3_step(st));
    EXPECT_EQ(sqlite3_column_int(st, 0), 2);
    ASSERT_NE(sqlite3_column_text(st, 1), nullptr);
    const std::string dead_letter_error(reinterpret_cast<const char*>(sqlite3_column_text(st, 1)));
    EXPECT_EQ(dead_letter_error.find("dead-letter: unsupported Execution event_type"), 0u);
    sqlite3_finalize(st);

    ASSERT_TRUE(projector.ProjectFromOutbox("WorkflowProjector", 100, &err, 2)) << err;
    ASSERT_EQ(SQLITE_OK, sqlite3_prepare_v2(
        db_,
        "SELECT attempt_count FROM exec_outbox_message WHERE outbox_id=5001;",
        -1,
        &st,
        nullptr));
    ASSERT_EQ(SQLITE_ROW, sqlite3_step(st));
    EXPECT_EQ(sqlite3_column_int(st, 0), 2);
    sqlite3_finalize(st);
}

TEST_F(SqliteDbFixture, Stage3cEndToEndWorkflowSeedProbeWithRestartMidRun) {
    using namespace savor::db::migrations;
    using namespace savor::db::execution::workflow;
    using namespace savor::runner::parallel::savordb;

    const MigrationSourceOptions embedded_options{ .source_kind = MigrationSourceKind::Embedded };
    std::string err;
    ASSERT_TRUE(ApplyContextMigrations(db_, MigrationContext::Execution, embedded_options, &err)) << err;
    ASSERT_TRUE(ApplyContextMigrations(db_, MigrationContext::UIRead, embedded_options, &err)) << err;

    savor::db::execution::workflow::SqliteExecutionDb execution_db(db_);

    std::int64_t workflow_instance_id = 0;
    WorkflowCreateInstanceCommand create{};
    create.workflow_kind = "workflow_graph";
    create.root_scope_kind = "manual";
    create.workflow_graph_revision_id = 1;
    create.created_by = "stage3c-e2e";
    create.created_at_utc = savor::db::types::UtcNow().time_since_epoch().count();
    create.unit_activations.push_back({
        .activation_key = "SeedProbe",
        .graph_node_key = "SeedProbe",
        .unit_kind = "test_seedprobe_chain",
        .display_name = "Test Seed Probe Chain",
        .activation_params_json = "{}",
        .steps = {
            { .step_key = "Neutral", .step_kind = "seedprobe.neutral", .priority = 1, .max_attempts = 2, .input_ref_kind = std::string("sp_probe_run"), .input_ref_id = 1 },
            { .step_key = "Grid", .step_kind = "seedprobe.grid", .dependencies = { "Neutral" }, .priority = 1, .max_attempts = 2 },
            { .step_key = "Unique", .step_kind = "seedprobe.unique", .dependencies = { "Grid" }, .priority = 1, .max_attempts = 2 },
            { .step_key = "Done", .step_kind = "seedprobe.done", .dependencies = { "Unique" }, .priority = 1, .max_attempts = 1 },
        },
    });
    ASSERT_TRUE(execution_db.WorkflowCommandService()->CreateWorkflowInstance(create, &workflow_instance_id, &err)) << err;
    ASSERT_GT(workflow_instance_id, 0);

    const auto graph = execution_db.WorkflowQueryService()->GetWorkflowGraph(workflow_instance_id);
    ASSERT_TRUE(graph.has_value());
    std::unordered_map<std::string, std::int64_t> step_ids_by_key;
    for (const auto& step : graph->steps) {
        step_ids_by_key.emplace(step.step_key, step.workflow_step_id);
    }
    ASSERT_TRUE(step_ids_by_key.count("Neutral"));
    ASSERT_TRUE(step_ids_by_key.count("Grid"));
    ASSERT_TRUE(step_ids_by_key.count("Unique"));
    ASSERT_TRUE(step_ids_by_key.count("Done"));

    const auto neutral_step_id = step_ids_by_key.at("Neutral");
    const auto grid_step_id = step_ids_by_key.at("Grid");
    const auto unique_step_id = step_ids_by_key.at("Unique");
    const auto done_step_id = step_ids_by_key.at("Done");

    std::int64_t next_job_set_id = 12000;
    auto schedule = [&](const WorkflowReadyStep& step) {
        const auto job_set_id = ++next_job_set_id;
        const std::string sql =
            "INSERT INTO exec_job_set(job_set_id, program_kind, purpose, created_at_utc) VALUES("
            + std::to_string(job_set_id) + ", 1, 'stage3c-item15', unixepoch());";
        const bool ok = ExecSql(db_, sql.c_str());
        EXPECT_TRUE(ok);
        return ScheduledJobSet{
            .job_set_id = job_set_id,
            .workflow_step_id = step.workflow_step_id,
        };
    };

    DBWorkflowWorkerCoordinator coordinator(
        &execution_db,
        DBWorkflowWorkerCoordinatorConfig{
            .desired_workers = 0,
            .controller_sleep_ms = 1,
        },
        CoordinatorIntegrationConfig{

        },
        schedule);

    auto insert_completed_job = [&](std::int64_t job_id, std::int64_t job_set_id, const char* fingerprint) {
        const std::string sql =
            "INSERT INTO exec_job(job_id, job_set_id, program_kind, program_version, program_ref_kind, program_ref_id, fingerprint, priority, state, attempts, max_attempts, queued_at_utc, ended_at_utc) VALUES("
            + std::to_string(job_id)
            + ","
            + std::to_string(job_set_id)
            + ",1,1,'seedprobe',1,'"
            + fingerprint
            + "',10,'COMPLETED',1,2,unixepoch(),unixepoch());";
        ASSERT_TRUE(ExecSql(db_, sql.c_str()));
    };

    auto query_job_set_for_step = [&](std::int64_t workflow_step_id) -> std::int64_t {
        sqlite3_stmt* stmt = nullptr;
        const std::string sql = "SELECT COALESCE(job_set_id,0) FROM exec_workflow_step WHERE workflow_step_id=" + std::to_string(workflow_step_id) + ";";
        EXPECT_EQ(SQLITE_OK, sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr));
        EXPECT_EQ(SQLITE_ROW, sqlite3_step(stmt));
        const auto value = sqlite3_column_int64(stmt, 0);
        sqlite3_finalize(stmt);
        return value;
    };

    const auto neutral = coordinator.MaterializeWorkflowStep({
        .workflow_instance_id = workflow_instance_id,
        .workflow_step_id = neutral_step_id,
        .step_key = "Neutral",
        .step_kind = "seedprobe.neutral",
        .priority = 10,
        .input_ref_kind = std::string("sp_probe_run"),
        .input_ref_id = 1,
    });
    ASSERT_TRUE(neutral.has_value());
    const std::int64_t neutral_job_set_id = query_job_set_for_step(neutral_step_id);
    ASSERT_GT(neutral_job_set_id, 0);
    EXPECT_TRUE(coordinator.PublishTerminalJobSet({
        .workflow_instance_id = workflow_instance_id,
        .workflow_step_id = neutral_step_id,
        .job_set_id = neutral_job_set_id,
        .terminal_state = "COMPLETED",
    }));

    ASSERT_TRUE(ExecSql(db_, ("UPDATE exec_workflow_step SET state='READY', ready_at_utc=unixepoch() WHERE workflow_step_id=" + std::to_string(grid_step_id) + ";").c_str()));

    const auto grid = coordinator.MaterializeWorkflowStep({
        .workflow_instance_id = workflow_instance_id,
        .workflow_step_id = grid_step_id,
        .step_key = "Grid",
        .step_kind = "seedprobe.grid",
        .priority = 8,
    });
    ASSERT_TRUE(grid.has_value());

    // Simulate restart in the middle: process exits after materialization, before terminal callback.
    DBWorkflowWorkerCoordinator after_restart(
        &execution_db,
        DBWorkflowWorkerCoordinatorConfig{
            .desired_workers = 0,
        },
        CoordinatorIntegrationConfig{

        },
        schedule);

    insert_completed_job(13001, grid->job_set_id, "item15-grid");

    WorkflowRecoveryService recovery(db_);
    WorkflowRecoveryResult recovery_result{};
    ASSERT_TRUE(recovery.ReconcileInFlightInstances(&recovery_result, &err)) << err;
    EXPECT_EQ(recovery_result.completed_steps, 1);

    ASSERT_TRUE(ExecSql(db_, ("UPDATE exec_workflow_step SET state='READY', ready_at_utc=unixepoch() WHERE workflow_step_id=" + std::to_string(unique_step_id) + ";").c_str()));
    const auto unique = after_restart.MaterializeWorkflowStep({
        .workflow_instance_id = workflow_instance_id,
        .workflow_step_id = unique_step_id,
        .step_key = "Unique",
        .step_kind = "seedprobe.unique",
        .priority = 7,
    });
    ASSERT_TRUE(unique.has_value());
    EXPECT_TRUE(after_restart.PublishTerminalJobSet({
        .workflow_instance_id = workflow_instance_id,
        .workflow_step_id = unique_step_id,
        .job_set_id = unique->job_set_id,
        .terminal_state = "COMPLETED",
    }));
    EXPECT_FALSE(after_restart.PublishTerminalJobSet({
        .workflow_instance_id = workflow_instance_id,
        .workflow_step_id = unique_step_id,
        .job_set_id = unique->job_set_id,
        .terminal_state = "COMPLETED",
    }));

    ASSERT_TRUE(ExecSql(db_, ("UPDATE exec_workflow_step SET state='READY', ready_at_utc=unixepoch() WHERE workflow_step_id=" + std::to_string(done_step_id) + ";").c_str()));
    const auto done = after_restart.MaterializeWorkflowStep({
        .workflow_instance_id = workflow_instance_id,
        .workflow_step_id = done_step_id,
        .step_key = "Done",
        .step_kind = "seedprobe.done",
        .priority = 1,
    });
    ASSERT_TRUE(done.has_value());
    EXPECT_TRUE(after_restart.PublishTerminalJobSet({
        .workflow_instance_id = workflow_instance_id,
        .workflow_step_id = done_step_id,
        .job_set_id = done->job_set_id,
        .terminal_state = "COMPLETED",
    }));

    insert_completed_job(13000, neutral_job_set_id, "item15-neutral");
    insert_completed_job(13002, unique->job_set_id, "item15-unique");
    insert_completed_job(13003, done->job_set_id, "item15-done");

    WorkflowRecoveryResult final_recovery_result{};
    ASSERT_TRUE(recovery.ReconcileInFlightInstances(&final_recovery_result, &err)) << err;
    EXPECT_EQ(final_recovery_result.completed_steps, 3);

    ASSERT_TRUE(ExecSql(db_, ("UPDATE exec_workflow_instance SET state='COMPLETED', completed_at_utc=unixepoch() WHERE workflow_instance_id=" + std::to_string(workflow_instance_id) + ";").c_str()));

    sqlite3_stmt* st = nullptr;
    ASSERT_EQ(SQLITE_OK, sqlite3_prepare_v2(
        db_,
        ("SELECT COUNT(1) FROM exec_workflow_step WHERE workflow_instance_id=" + std::to_string(workflow_instance_id) + " AND state='COMPLETED';").c_str(),
        -1,
        &st,
        nullptr));
    ASSERT_EQ(SQLITE_ROW, sqlite3_step(st));
    EXPECT_EQ(sqlite3_column_int(st, 0), 4);
    sqlite3_finalize(st);

    ASSERT_EQ(SQLITE_OK, sqlite3_prepare_v2(
        db_,
        ("SELECT COUNT(1) FROM exec_workflow_event WHERE workflow_instance_id=" + std::to_string(workflow_instance_id) + " AND event_kind='Execution.WorkflowStepMaterialized.v1';").c_str(),
        -1,
        &st,
        nullptr));
    ASSERT_EQ(SQLITE_ROW, sqlite3_step(st));
    EXPECT_EQ(sqlite3_column_int(st, 0), 4);
    sqlite3_finalize(st);

    WorkflowProjector projector(db_);
    ASSERT_TRUE(projector.ProjectFromOutbox("WorkflowProjector", 1000, &err)) << err;
    ASSERT_EQ(SQLITE_OK, sqlite3_prepare_v2(
        db_,
        ("SELECT state FROM ui_workflow_instance WHERE workflow_instance_id=" + std::to_string(workflow_instance_id) + ";").c_str(),
        -1,
        &st,
        nullptr));
    ASSERT_EQ(SQLITE_ROW, sqlite3_step(st));
    EXPECT_EQ(std::string(reinterpret_cast<const char*>(sqlite3_column_text(st, 0))), "COMPLETED");
    sqlite3_finalize(st);
}

TEST_F(SqliteDbFixture, Stage3cNoWorkDoneStepCompletesWorkflowFromReadyState) {
    using namespace savor::db::execution::workflow;
    using namespace savor::db::execution::programdb;

    ASSERT_TRUE(ExecSql(db_, R"SQL(
INSERT INTO exec_workflow_instance(workflow_instance_id, workflow_kind, root_scope_kind, state, created_by, created_at_utc)
VALUES(15001, 'SEED_PROBE_CHAIN', 'manual', 'RUNNING', 'stage3c-done-test', unixepoch()*1000);
INSERT INTO exec_workflow_step(workflow_step_id, workflow_instance_id, step_key, step_kind, state, priority, attempts, max_attempts, completed_at_utc, created_at_utc, ready_at_utc)
VALUES
  (15002, 15001, 'Neutral', 'seedprobe.neutral', 'COMPLETED', 10, 1, 2, unixepoch()*1000, unixepoch()*1000, unixepoch()*1000),
  (15003, 15001, 'Grid', 'seedprobe.grid', 'COMPLETED', 8, 1, 2, unixepoch()*1000, unixepoch()*1000, unixepoch()*1000),
  (15004, 15001, 'Unique', 'seedprobe.unique', 'COMPLETED', 7, 1, 2, unixepoch()*1000, unixepoch()*1000, unixepoch()*1000),
  (15005, 15001, 'Done', 'seedprobe.done', 'READY', 1, 0, 1, NULL, unixepoch()*1000, unixepoch()*1000);
)SQL"));

    SqliteExecutionDb execution_db(db_);
    ProgramKindRegistry registry;
    WorkflowCoordinatorService coordinator(
        &execution_db,
        &registry,
        WorkflowCoordinatorConfig{
            .poll_interval = std::chrono::milliseconds(1),
        });

    auto read_text = [&](const std::string& sql) -> std::string {
        sqlite3_stmt* st = nullptr;
        EXPECT_EQ(SQLITE_OK, sqlite3_prepare_v2(db_, sql.c_str(), -1, &st, nullptr));
        std::string value;
        if (sqlite3_step(st) == SQLITE_ROW) {
            const auto* text = sqlite3_column_text(st, 0);
            value = text ? reinterpret_cast<const char*>(text) : "";
        }
        sqlite3_finalize(st);
        return value;
    };
    auto read_int = [&](const std::string& sql) -> int {
        sqlite3_stmt* st = nullptr;
        EXPECT_EQ(SQLITE_OK, sqlite3_prepare_v2(db_, sql.c_str(), -1, &st, nullptr));
        int value = 0;
        if (sqlite3_step(st) == SQLITE_ROW) {
            value = sqlite3_column_int(st, 0);
        }
        sqlite3_finalize(st);
        return value;
    };

    coordinator.Start();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    while (read_text("SELECT state FROM exec_workflow_instance WHERE workflow_instance_id=15001;") != "COMPLETED"
        && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    coordinator.Stop();

    EXPECT_EQ(read_text("SELECT state FROM exec_workflow_step WHERE workflow_step_id=15005;"), "COMPLETED");
    EXPECT_EQ(read_text("SELECT state FROM exec_workflow_instance WHERE workflow_instance_id=15001;"), "COMPLETED");
    EXPECT_GE(read_int("SELECT COUNT(1) FROM exec_workflow_event WHERE workflow_instance_id=15001 AND event_kind='Execution.WorkflowInstanceCompleted.v1';"), 1);
}

TEST_F(SqliteDbFixture, Stage3dExecutionJobCommandServiceEmitsEventsOneThroughNine) {
    using namespace savor::db::execution::jobs;
    using namespace savor::db::execution::workflow;
    using namespace savor::db::migrations;

    const MigrationSourceOptions embedded_options{ .source_kind = MigrationSourceKind::Embedded };
    std::string err;
    ASSERT_TRUE(ApplyContextMigrations(db_, MigrationContext::Execution, embedded_options, &err)) << err;

    ASSERT_TRUE(ExecSql(db_, R"SQL(
INSERT INTO exec_job_set(job_set_id, program_kind, purpose, created_at_utc)
VALUES(501, 1, 'stage3d', unixepoch()*1000);
INSERT INTO exec_job(job_id, job_set_id, program_kind, program_version, program_ref_kind, program_ref_id, fingerprint, priority, state, attempts, max_attempts, queued_at_utc)
VALUES(601, 501, 1, 1, 'seed_probe', 10, 'fp-stage3d-601', 5, 'QUEUED', 0, 3, unixepoch()*1000);
)SQL"));

    savor::db::execution::workflow::SqliteExecutionDb execution_db(db_);
    auto* job_commands = execution_db.JobCommandService();
    ASSERT_NE(job_commands, nullptr);

    ASSERT_TRUE(job_commands->AppendLifecycleEvent({ .kind = JobLifecycleEventKind::JobSetCreated, .job_set_id = 501 }, &err)) << err;
    ASSERT_TRUE(job_commands->AppendLifecycleEvent({ .kind = JobLifecycleEventKind::JobQueued, .job_id = 601 }, &err)) << err;
    ASSERT_TRUE(job_commands->AppendLifecycleEvent({ .kind = JobLifecycleEventKind::JobClaimed, .job_id = 601, .claimed_by_token = std::string("worker-1"), .lease_expires_at_utc = 2000000 }, &err)) << err;
    ASSERT_TRUE(job_commands->AppendLifecycleEvent({ .kind = JobLifecycleEventKind::JobStarted, .job_id = 601, .requested_by = std::string("worker-1") }, &err)) << err;
    ASSERT_TRUE(job_commands->AppendLifecycleEvent({ .kind = JobLifecycleEventKind::JobLeaseRenewed, .job_id = 601, .lease_expires_at_utc = 3000000 }, &err)) << err;
    ASSERT_TRUE(job_commands->AppendLifecycleEvent({ .kind = JobLifecycleEventKind::JobProgressed, .job_id = 601, .message = std::string("50%") }, &err)) << err;
    ASSERT_TRUE(job_commands->AppendLifecycleEvent({ .kind = JobLifecycleEventKind::JobCompleted, .job_id = 601, .terminal_state = std::string("SUCCEEDED") }, &err)) << err;
    ASSERT_TRUE(job_commands->AppendLifecycleEvent({ .kind = JobLifecycleEventKind::JobEventArchived, .job_id = 601, .message = std::string("archived") }, &err)) << err;
    ASSERT_TRUE(job_commands->AppendLifecycleEvent({ .kind = JobLifecycleEventKind::JobRestored, .job_id = 601, .message = std::string("restored") }, &err)) << err;

    sqlite3_stmt* st = nullptr;
    ASSERT_EQ(SQLITE_OK, sqlite3_prepare_v2(db_, "SELECT COUNT(1) FROM exec_outbox_message WHERE event_type LIKE 'Execution.Job%.v1';", -1, &st, nullptr));
    ASSERT_EQ(SQLITE_ROW, sqlite3_step(st));
    EXPECT_EQ(sqlite3_column_int(st, 0), 9);
    sqlite3_finalize(st);

    ASSERT_EQ(SQLITE_OK, sqlite3_prepare_v2(db_, "SELECT COUNT(1) FROM exec_outbox_message WHERE payload_ref_kind='job' AND payload_ref_id=601;", -1, &st, nullptr));
    ASSERT_EQ(SQLITE_ROW, sqlite3_step(st));
    EXPECT_EQ(sqlite3_column_int(st, 0), 8);
    sqlite3_finalize(st);

    ASSERT_EQ(SQLITE_OK, sqlite3_prepare_v2(db_, "SELECT COUNT(1) FROM exec_job_event WHERE job_id=601;", -1, &st, nullptr));
    ASSERT_EQ(SQLITE_ROW, sqlite3_step(st));
    EXPECT_EQ(sqlite3_column_int(st, 0), 8);
    sqlite3_finalize(st);

    ASSERT_EQ(SQLITE_OK, sqlite3_prepare_v2(db_, "SELECT message FROM exec_job_event WHERE job_id=601 AND event_kind='Execution.JobProgressed.v1';", -1, &st, nullptr));
    ASSERT_EQ(SQLITE_ROW, sqlite3_step(st));
    EXPECT_STREQ(reinterpret_cast<const char*>(sqlite3_column_text(st, 0)), "50%");
    sqlite3_finalize(st);

    ASSERT_EQ(SQLITE_OK, sqlite3_prepare_v2(
        db_,
        "SELECT state, claimed_by_token, lease_expires_at_utc FROM exec_job WHERE job_id=601;",
        -1,
        &st,
        nullptr));
    ASSERT_EQ(SQLITE_ROW, sqlite3_step(st));
    EXPECT_STREQ(reinterpret_cast<const char*>(sqlite3_column_text(st, 0)), "SUCCEEDED");
    EXPECT_EQ(sqlite3_column_type(st, 1), SQLITE_NULL);
    EXPECT_EQ(sqlite3_column_type(st, 2), SQLITE_NULL);
    sqlite3_finalize(st);
}

TEST_F(SqliteDbFixture, Stage3dExecutionJobProgressEventsAreRepeatableAndDoNotDemoteTerminalJobs) {
    using namespace savor::db::execution::jobs;
    using namespace savor::db::execution::workflow;
    using namespace savor::db::migrations;

    const MigrationSourceOptions embedded_options{ .source_kind = MigrationSourceKind::Embedded };
    std::string err;
    ASSERT_TRUE(ApplyContextMigrations(db_, MigrationContext::Execution, embedded_options, &err)) << err;

    ASSERT_TRUE(ExecSql(db_, R"SQL(
INSERT INTO exec_job_set(job_set_id, program_kind, purpose, created_at_utc)
VALUES(701, 1, 'progress-repeat', unixepoch()*1000);
INSERT INTO exec_job(job_id, job_set_id, program_kind, program_version, program_ref_kind, program_ref_id, fingerprint, priority, state, attempts, max_attempts, queued_at_utc)
VALUES(801, 701, 1, 1, 'seed_probe', 10, 'fp-stage3d-801', 5, 'QUEUED', 0, 3, unixepoch()*1000);
)SQL"));

    savor::db::execution::workflow::SqliteExecutionDb execution_db(db_);
    auto* job_commands = execution_db.JobCommandService();
    ASSERT_NE(job_commands, nullptr);

    ASSERT_TRUE(job_commands->AppendLifecycleEvent({ .kind = JobLifecycleEventKind::JobProgressed, .job_id = 801, .message = std::string("first progress") }, &err)) << err;
    ASSERT_TRUE(job_commands->AppendLifecycleEvent({ .kind = JobLifecycleEventKind::JobProgressed, .job_id = 801, .message = std::string("second progress") }, &err)) << err;
    ASSERT_TRUE(job_commands->AppendLifecycleEvent({ .kind = JobLifecycleEventKind::JobCompleted, .job_id = 801, .terminal_state = std::string("SUCCEEDED") }, &err)) << err;
    ASSERT_TRUE(job_commands->AppendLifecycleEvent({ .kind = JobLifecycleEventKind::JobProgressed, .job_id = 801, .message = std::string("late progress") }, &err)) << err;

    sqlite3_stmt* st = nullptr;
    ASSERT_EQ(SQLITE_OK, sqlite3_prepare_v2(db_, "SELECT COUNT(1) FROM exec_job_event WHERE job_id=801 AND event_kind='Execution.JobProgressed.v1';", -1, &st, nullptr));
    ASSERT_EQ(SQLITE_ROW, sqlite3_step(st));
    EXPECT_EQ(sqlite3_column_int(st, 0), 3);
    sqlite3_finalize(st);

    ASSERT_EQ(SQLITE_OK, sqlite3_prepare_v2(db_, "SELECT COUNT(1) FROM exec_outbox_message WHERE event_type='Execution.JobProgressed.v1' AND payload_ref_kind='job' AND payload_ref_id=801;", -1, &st, nullptr));
    ASSERT_EQ(SQLITE_ROW, sqlite3_step(st));
    EXPECT_EQ(sqlite3_column_int(st, 0), 3);
    sqlite3_finalize(st);

    ASSERT_EQ(SQLITE_OK, sqlite3_prepare_v2(db_, "SELECT state FROM exec_job WHERE job_id=801;", -1, &st, nullptr));
    ASSERT_EQ(SQLITE_ROW, sqlite3_step(st));
    EXPECT_STREQ(reinterpret_cast<const char*>(sqlite3_column_text(st, 0)), "SUCCEEDED");
    sqlite3_finalize(st);
}

TEST_F(SqliteDbFixture, Stage5ExecutionJobActionsUpdateExecutionAndEmitProjectorOutboxEvents) {
    using namespace savor::db::execution::jobs;

    std::string err;

    auto* execution_db = db_service_->ExecutionDb();
    ASSERT_NE(execution_db, nullptr);
    std::int64_t job_set_id = 0;
    ASSERT_TRUE(execution_db->CreateJobSet(
        {
            .program_kind = 1,
            .purpose = "job-actions",
            .created_by = std::string("stage5-test"),
            .created_at_utc = savor::db::types::UtcNow().time_since_epoch().count(),
        },
        &job_set_id,
        &err))
        << err;

    std::int64_t restart_job_id = 0;
    ASSERT_TRUE(execution_db->EnqueueJob(
        {
            .job_set_id = job_set_id,
            .program_kind = 1,
            .program_ref_kind = "seed_probe",
            .program_ref_id = 10,
            .fingerprint = "fp-job-action-restart",
            .priority = 5,
            .max_attempts = 3,
            .input_ini = "[Job]\nmode=old\n",
        },
        &restart_job_id,
        &err))
        << err;
    std::int64_t cancel_job_id = 0;
    ASSERT_TRUE(execution_db->EnqueueJob(
        {
            .job_set_id = job_set_id,
            .program_kind = 1,
            .program_ref_kind = "seed_probe",
            .program_ref_id = 11,
            .fingerprint = "fp-job-action-cancel",
            .priority = 5,
            .max_attempts = 3,
            .input_ini = "[Job]\nmode=cancel\n",
        },
        &cancel_job_id,
        &err))
        << err;
    std::int64_t requeue_job_id = 0;
    ASSERT_TRUE(execution_db->EnqueueJob(
        {
            .job_set_id = job_set_id,
            .program_kind = 1,
            .program_ref_kind = "seed_probe",
            .program_ref_id = 12,
            .fingerprint = "fp-job-action-requeue",
            .priority = 5,
            .max_attempts = 3,
            .input_ini = "[Job]\nmode=requeue\n",
        },
        &requeue_job_id,
        &err))
        << err;

    auto* job_commands = execution_db->JobCommandService();
    ASSERT_NE(job_commands, nullptr);
    ASSERT_TRUE(job_commands->AppendLifecycleEvent({ .kind = JobLifecycleEventKind::JobCompleted, .job_id = restart_job_id, .terminal_state = std::string("FAILED") }, &err)) << err;
    ASSERT_TRUE(job_commands->AppendLifecycleEvent({ .kind = JobLifecycleEventKind::JobCompleted, .job_id = requeue_job_id, .terminal_state = std::string("CANCELED") }, &err)) << err;

    ASSERT_TRUE(execution_db->RestartFailedJob(restart_job_id, std::string("[Job]\nmode=new\n"), &err)) << err;
    ASSERT_TRUE(execution_db->CancelQueuedOrClaimedJob(cancel_job_id, &err)) << err;
    ASSERT_TRUE(execution_db->RequeueJob(requeue_job_id, &err)) << err;

    const auto restarted = execution_db->GetJob(restart_job_id);
    const auto canceled = execution_db->GetJob(cancel_job_id);
    const auto requeued = execution_db->GetJob(requeue_job_id);
    ASSERT_TRUE(restarted.has_value());
    ASSERT_TRUE(canceled.has_value());
    ASSERT_TRUE(requeued.has_value());
    EXPECT_EQ(restarted->state, "QUEUED");
    EXPECT_EQ(restarted->attempts, 0);
    EXPECT_EQ(restarted->input_ini, "[Job]\nmode=new\n");
    EXPECT_EQ(canceled->state, "CANCELED");
    EXPECT_EQ(requeued->state, "QUEUED");

    const auto input_ini = execution_db->GetJobInputIni(restart_job_id, &err);
    ASSERT_TRUE(input_ini.has_value()) << err;
    EXPECT_EQ(*input_ini, "[Job]\nmode=new\n");

    const auto restart_events = execution_db->ListJobEvents(restart_job_id, 10);
    ASSERT_GE(restart_events.size(), 2u);
    EXPECT_EQ(restart_events.front().event_kind, "Execution.JobQueued.v1");
    EXPECT_EQ(restart_events.front().message, "RESTART");

    sqlite3_stmt* st = nullptr;
    ASSERT_EQ(SQLITE_OK, sqlite3_prepare_v2(
        db_,
        "SELECT COUNT(1) FROM exec_outbox_message "
        "WHERE payload_ref_kind='job' "
        "AND event_type IN ('Execution.JobQueued.v1','Execution.JobCompleted.v1');",
        -1,
        &st,
        nullptr));
    ASSERT_EQ(SQLITE_ROW, sqlite3_step(st));
    EXPECT_EQ(sqlite3_column_int(st, 0), 5);
    sqlite3_finalize(st);

    err.clear();
    EXPECT_FALSE(execution_db->CancelQueuedOrClaimedJob(cancel_job_id, &err));
    EXPECT_NE(err.find("job cannot be canceled from state CANCELED"), std::string::npos);
}

TEST_F(SqliteDbFixture, Stage3dClaimNextReadyExecutionJobCommitsAfterReturningClaimedRow) {
    using namespace savor::db::execution::workflow;
    using namespace savor::db::migrations;

    const MigrationSourceOptions embedded_options{ .source_kind = MigrationSourceKind::Embedded };
    std::string err;
    ASSERT_TRUE(ApplyContextMigrations(db_, MigrationContext::Execution, embedded_options, &err)) << err;

    ASSERT_TRUE(ExecSql(db_, R"SQL(
INSERT INTO exec_workflow_instance(workflow_instance_id, workflow_kind, state, root_scope_kind, created_by, created_at_utc)
VALUES(1699, 'SEED_PROBE_CHAIN', 'RUNNING', 'manual', 'test', unixepoch()*1000);
INSERT INTO exec_job_set(job_set_id, program_kind, purpose, created_at_utc)
VALUES(1701, 7, 'stage3d-claim', unixepoch()*1000);
INSERT INTO exec_workflow_step(workflow_step_id, workflow_instance_id, step_key, step_kind, state, job_set_id, priority, attempts, max_attempts, created_at_utc)
VALUES(1700, 1699, 'Neutral', 'seedprobe.neutral', 'MATERIALIZED', 1701, 8, 0, 2, unixepoch()*1000);
INSERT INTO exec_job(job_id, job_set_id, program_kind, program_version, program_ref_kind, program_ref_id, fingerprint, priority, state, attempts, max_attempts, queued_at_utc)
VALUES(1702, 1701, 7, 1, 'seed_probe', 33, 'fp-stage3d-claim', 5, 'QUEUED', 0, 3, unixepoch()*1000);
)SQL"));

    SqliteExecutionDb execution_db(db_);
    std::string claim_error;
    const auto claimed = execution_db.ClaimNextReadyExecutionJob("worker-claim-regression", 30000, &claim_error);
    EXPECT_TRUE(claim_error.empty()) << claim_error;
    ASSERT_TRUE(claimed.has_value());
    EXPECT_EQ(claimed->job_id, 1702);
    EXPECT_EQ(claimed->job_set_id, 1701);
    EXPECT_EQ(claimed->workflow_instance_id, 1699);
    EXPECT_EQ(claimed->workflow_step_id, 1700);
    EXPECT_EQ(claimed->workflow_step_key, "Neutral");
    EXPECT_EQ(claimed->workflow_step_kind, "seedprobe.neutral");
    EXPECT_EQ(claimed->workflow_step_priority, 8);

    sqlite3_stmt* st = nullptr;
    ASSERT_EQ(SQLITE_OK, sqlite3_prepare_v2(
        db_,
        "SELECT state, claimed_by_token, lease_expires_at_utc FROM exec_job WHERE job_id=1702;",
        -1,
        &st,
        nullptr));
    ASSERT_EQ(SQLITE_ROW, sqlite3_step(st));
    EXPECT_STREQ(reinterpret_cast<const char*>(sqlite3_column_text(st, 0)), "CLAIMED");
    EXPECT_STREQ(reinterpret_cast<const char*>(sqlite3_column_text(st, 1)), "worker-claim-regression");
    EXPECT_NE(sqlite3_column_type(st, 2), SQLITE_NULL);
    sqlite3_finalize(st);

    ASSERT_EQ(SQLITE_OK, sqlite3_prepare_v2(
        db_,
        "SELECT COUNT(1) FROM exec_outbox_message "
        "WHERE event_type='Execution.JobClaimed.v1' AND payload_ref_kind='job' AND payload_ref_id=1702;",
        -1,
        &st,
        nullptr));
    ASSERT_EQ(SQLITE_ROW, sqlite3_step(st));
    EXPECT_EQ(sqlite3_column_int(st, 0), 1);
    sqlite3_finalize(st);
}

TEST_F(SqliteDbFixture, Stage3dClaimNextReadyExecutionJobOrdersByStepDerivedJobPriority) {
    using namespace savor::db::execution::workflow;
    using namespace savor::db::migrations;

    const MigrationSourceOptions embedded_options{ .source_kind = MigrationSourceKind::Embedded };
    std::string err;
    ASSERT_TRUE(ApplyContextMigrations(db_, MigrationContext::Execution, embedded_options, &err)) << err;

    ASSERT_TRUE(ExecSql(db_, R"SQL(
INSERT INTO exec_workflow_instance(workflow_instance_id, workflow_kind, state, root_scope_kind, created_by, created_at_utc)
VALUES(1710, 'priority-order', 'RUNNING', 'manual', 'test', unixepoch()*1000);
INSERT INTO exec_job_set(job_set_id, program_kind, purpose, created_at_utc)
VALUES(1711, 7, 'low-priority-step', unixepoch()*1000),
      (1721, 7, 'high-priority-step', unixepoch()*1000);
INSERT INTO exec_workflow_step(workflow_step_id, workflow_instance_id, step_key, step_kind, state, job_set_id, priority, attempts, max_attempts, created_at_utc)
VALUES(1712, 1710, 'Low', 'test.low', 'MATERIALIZED', 1711, 10, 0, 1, unixepoch()*1000),
      (1722, 1710, 'High', 'test.high', 'MATERIALIZED', 1721, 42, 0, 1, unixepoch()*1000);
INSERT INTO exec_job(job_id, job_set_id, program_kind, program_version, program_ref_kind, program_ref_id, fingerprint, priority, state, attempts, max_attempts, queued_at_utc)
VALUES(1713, 1711, 7, 1, 'test', 1, 'priority-order-low', 10, 'QUEUED', 0, 1, 1000),
      (1723, 1721, 7, 1, 'test', 2, 'priority-order-high', 42, 'QUEUED', 0, 1, 1000);
)SQL"));

    SqliteExecutionDb execution_db(db_);
    std::string claim_error;
    const auto claimed = execution_db.ClaimNextReadyExecutionJob("worker-priority-order", 30000, &claim_error);
    EXPECT_TRUE(claim_error.empty()) << claim_error;
    ASSERT_TRUE(claimed.has_value());
    EXPECT_EQ(claimed->job_id, 1723);
    EXPECT_EQ(claimed->workflow_step_id, 1722);
    EXPECT_EQ(claimed->workflow_step_priority, 42);
}

TEST_F(SqliteDbFixture, Stage3dDefaultEnqueueJobIsImmediatelyQueued) {
    using namespace savor::db::execution::workflow;
    using namespace savor::db::migrations;

    const MigrationSourceOptions embedded_options{ .source_kind = MigrationSourceKind::Embedded };
    std::string err;
    ASSERT_TRUE(ApplyContextMigrations(db_, MigrationContext::Execution, embedded_options, &err)) << err;

    ASSERT_TRUE(ExecSql(db_, R"SQL(
INSERT INTO exec_workflow_instance(workflow_instance_id, workflow_kind, state, root_scope_kind, created_by, created_at_utc)
VALUES(1829, 'SEED_PROBE_CHAIN', 'RUNNING', 'manual', 'test', unixepoch()*1000);
INSERT INTO exec_job_set(job_set_id, program_kind, purpose, created_at_utc)
VALUES(1831, 7, 'stage3d-default-queued', unixepoch()*1000);
INSERT INTO exec_workflow_step(workflow_step_id, workflow_instance_id, step_key, step_kind, state, job_set_id, priority, attempts, max_attempts, created_at_utc)
VALUES(1830, 1829, 'Neutral', 'seedprobe.neutral', 'MATERIALIZED', 1831, 8, 0, 2, unixepoch()*1000);
)SQL"));

    SqliteExecutionDb execution_db(db_);
    std::int64_t job_id = 0;
    ASSERT_TRUE(execution_db.EnqueueJob(
        {
            .job_set_id = 1831,
            .program_kind = 7,
            .program_ref_kind = "seed_probe",
            .program_ref_id = 33,
            .fingerprint = "fp-stage3d-default-queued",
            .priority = 5,
            .max_attempts = 3,
        },
        &job_id,
        &err))
        << err;
    EXPECT_GT(job_id, 0);

    const auto record = execution_db.GetJob(job_id);
    ASSERT_TRUE(record.has_value());
    EXPECT_EQ(record->state, "QUEUED");

    std::string claim_error;
    const auto claimed = execution_db.ClaimNextReadyExecutionJob("worker-default-queued", 30000, &claim_error);
    EXPECT_TRUE(claim_error.empty()) << claim_error;
    ASSERT_TRUE(claimed.has_value());
    EXPECT_EQ(claimed->job_id, job_id);
    EXPECT_EQ(claimed->workflow_step_id, 1830);
}

TEST_F(SqliteDbFixture, Stage3dPendingMaterializationJobIsReleasedWhenStepMaterialized) {
    using namespace savor::db::execution::workflow;
    using namespace savor::db::migrations;

    const MigrationSourceOptions embedded_options{ .source_kind = MigrationSourceKind::Embedded };
    std::string err;
    ASSERT_TRUE(ApplyContextMigrations(db_, MigrationContext::Execution, embedded_options, &err)) << err;

    ASSERT_TRUE(ExecSql(db_, R"SQL(
INSERT INTO exec_workflow_instance(workflow_instance_id, workflow_kind, state, root_scope_kind, created_by, created_at_utc)
VALUES(1849, 'SEED_PROBE_CHAIN', 'RUNNING', 'manual', 'test', unixepoch()*1000);
INSERT INTO exec_job_set(job_set_id, program_kind, purpose, created_at_utc)
VALUES(1851, 7, 'stage3d-pending-root', unixepoch()*1000);
INSERT INTO exec_workflow_step(workflow_step_id, workflow_instance_id, step_key, step_kind, state, priority, attempts, max_attempts, created_at_utc)
VALUES(1850, 1849, 'Neutral', 'seedprobe.neutral', 'READY', 8, 0, 2, unixepoch()*1000);
)SQL"));

    SqliteExecutionDb execution_db(db_);
    std::int64_t job_id = 0;
    ASSERT_TRUE(execution_db.EnqueueJob(
        {
            .job_set_id = 1851,
            .program_kind = 7,
            .program_ref_kind = "seed_probe",
            .program_ref_id = 33,
            .fingerprint = "fp-stage3d-pending-root",
            .priority = 5,
            .max_attempts = 3,
            .pending_until_workflow_materialized = true,
        },
        &job_id,
        &err))
        << err;

    auto record = execution_db.GetJob(job_id);
    ASSERT_TRUE(record.has_value());
    EXPECT_EQ(record->state, "PENDING_MATERIALIZATION");

    std::string claim_error;
    auto claimed = execution_db.ClaimNextReadyExecutionJob("worker-pending-before", 30000, &claim_error);
    EXPECT_TRUE(claim_error.empty()) << claim_error;
    EXPECT_FALSE(claimed.has_value());

    ASSERT_TRUE(execution_db.WorkflowCommandService()->MarkStepMaterialized(
        {
            .workflow_step_id = 1850,
            .job_set_id = 1851,
            .requested_by = "SavorTests",
        },
        &err))
        << err;

    record = execution_db.GetJob(job_id);
    ASSERT_TRUE(record.has_value());
    EXPECT_EQ(record->state, "QUEUED");

    claim_error.clear();
    claimed = execution_db.ClaimNextReadyExecutionJob("worker-pending-after", 30000, &claim_error);
    EXPECT_TRUE(claim_error.empty()) << claim_error;
    ASSERT_TRUE(claimed.has_value());
    EXPECT_EQ(claimed->job_id, job_id);
    EXPECT_EQ(claimed->workflow_instance_id, 1849);
    EXPECT_EQ(claimed->workflow_step_id, 1850);
}

TEST_F(SqliteDbFixture, Stage3dMarkStepMaterializedReleasesDescendantPendingJobs) {
    using namespace savor::db::execution::workflow;
    using namespace savor::db::migrations;

    const MigrationSourceOptions embedded_options{ .source_kind = MigrationSourceKind::Embedded };
    std::string err;
    ASSERT_TRUE(ApplyContextMigrations(db_, MigrationContext::Execution, embedded_options, &err)) << err;

    ASSERT_TRUE(ExecSql(db_, R"SQL(
INSERT INTO exec_workflow_instance(workflow_instance_id, workflow_kind, state, root_scope_kind, created_by, created_at_utc)
VALUES(1869, 'SEED_PROBE_CHAIN', 'RUNNING', 'manual', 'test', unixepoch()*1000);
INSERT INTO exec_job_set(job_set_id, parent_job_set_id, program_kind, purpose, created_at_utc)
VALUES(1871, NULL, 7, 'stage3d-pending-root', unixepoch()*1000);
INSERT INTO exec_job_set(job_set_id, parent_job_set_id, program_kind, purpose, created_at_utc)
VALUES(1872, 1871, 7, 'stage3d-pending-child', unixepoch()*1000);
INSERT INTO exec_workflow_step(workflow_step_id, workflow_instance_id, step_key, step_kind, state, priority, attempts, max_attempts, created_at_utc)
VALUES(1870, 1869, 'Unique', 'seedprobe.unique', 'READY', 9, 0, 2, unixepoch()*1000);
)SQL"));

    SqliteExecutionDb execution_db(db_);
    std::int64_t child_job_id = 0;
    ASSERT_TRUE(execution_db.EnqueueJob(
        {
            .job_set_id = 1872,
            .program_kind = 7,
            .program_ref_kind = "seed_probe",
            .program_ref_id = 33,
            .fingerprint = "fp-stage3d-pending-child",
            .priority = 5,
            .max_attempts = 3,
            .pending_until_workflow_materialized = true,
        },
        &child_job_id,
        &err))
        << err;

    std::string claim_error;
    auto claimed = execution_db.ClaimNextReadyExecutionJob("worker-descendant-before", 30000, &claim_error);
    EXPECT_TRUE(claim_error.empty()) << claim_error;
    EXPECT_FALSE(claimed.has_value());

    ASSERT_TRUE(execution_db.WorkflowCommandService()->MarkStepMaterialized(
        {
            .workflow_step_id = 1870,
            .job_set_id = 1871,
            .requested_by = "SavorTests",
        },
        &err))
        << err;

    const auto record = execution_db.GetJob(child_job_id);
    ASSERT_TRUE(record.has_value());
    EXPECT_EQ(record->state, "QUEUED");

    claim_error.clear();
    claimed = execution_db.ClaimNextReadyExecutionJob("worker-descendant-after", 30000, &claim_error);
    EXPECT_TRUE(claim_error.empty()) << claim_error;
    ASSERT_TRUE(claimed.has_value());
    EXPECT_EQ(claimed->job_id, child_job_id);
    EXPECT_EQ(claimed->job_set_id, 1872);
    EXPECT_EQ(claimed->workflow_step_id, 1870);
}

TEST_F(SqliteDbFixture, Stage3dRunningExecutionJobLeasesRenewAndExpireBackToQueued) {
    using namespace savor::db::execution::jobs;
    using namespace savor::db::execution::workflow;
    using namespace savor::db::migrations;

    const MigrationSourceOptions embedded_options{ .source_kind = MigrationSourceKind::Embedded };
    std::string err;
    ASSERT_TRUE(ApplyContextMigrations(db_, MigrationContext::Execution, embedded_options, &err)) << err;

    ASSERT_TRUE(ExecSql(db_, R"SQL(
INSERT INTO exec_workflow_instance(workflow_instance_id, workflow_kind, state, root_scope_kind, created_by, created_at_utc)
VALUES(1759, 'SEED_PROBE_CHAIN', 'RUNNING', 'manual', 'test', unixepoch()*1000);
INSERT INTO exec_job_set(job_set_id, program_kind, purpose, created_at_utc)
VALUES(1761, 7, 'stage3d-running-lease', unixepoch()*1000);
INSERT INTO exec_workflow_step(workflow_step_id, workflow_instance_id, step_key, step_kind, state, job_set_id, priority, attempts, max_attempts, created_at_utc)
VALUES(1760, 1759, 'Neutral', 'seedprobe.neutral', 'MATERIALIZED', 1761, 8, 0, 2, unixepoch()*1000);
INSERT INTO exec_job(job_id, job_set_id, program_kind, program_version, program_ref_kind, program_ref_id, fingerprint, priority, state, attempts, max_attempts, queued_at_utc)
VALUES(1762, 1761, 7, 1, 'seed_probe', 33, 'fp-stage3d-running-lease', 5, 'QUEUED', 0, 3, unixepoch()*1000);
)SQL"));

    SqliteExecutionDb execution_db(db_);
    std::string claim_error;
    const auto claimed = execution_db.ClaimNextReadyExecutionJob("worker-running-lease", 30000, &claim_error);
    EXPECT_TRUE(claim_error.empty()) << claim_error;
    ASSERT_TRUE(claimed.has_value());
    EXPECT_EQ(claimed->job_id, 1762);

    sqlite3_stmt* st = nullptr;
    ASSERT_EQ(SQLITE_OK, sqlite3_prepare_v2(
        db_,
        "SELECT state, claimed_by_token, started_at_utc FROM exec_job WHERE job_id=1762;",
        -1,
        &st,
        nullptr));
    ASSERT_EQ(SQLITE_ROW, sqlite3_step(st));
    EXPECT_STREQ(reinterpret_cast<const char*>(sqlite3_column_text(st, 0)), "CLAIMED");
    EXPECT_STREQ(reinterpret_cast<const char*>(sqlite3_column_text(st, 1)), "worker-running-lease");
    EXPECT_EQ(sqlite3_column_type(st, 2), SQLITE_NULL);
    sqlite3_finalize(st);

    bool renewed = false;
    ASSERT_TRUE(execution_db.RenewExecutionJobLease(1762, "worker-running-lease", 30000, &renewed, &err)) << err;
    EXPECT_TRUE(renewed);

    auto* job_commands = execution_db.JobCommandService();
    ASSERT_NE(job_commands, nullptr);
    ASSERT_TRUE(job_commands->AppendLifecycleEvent(
        {
            .kind = JobLifecycleEventKind::JobStarted,
            .job_id = 1762,
            .requested_by = "test-dispatch",
        },
        &err))
        << err;

    ASSERT_EQ(SQLITE_OK, sqlite3_prepare_v2(
        db_,
        "SELECT state, claimed_by_token, started_at_utc FROM exec_job WHERE job_id=1762;",
        -1,
        &st,
        nullptr));
    ASSERT_EQ(SQLITE_ROW, sqlite3_step(st));
    EXPECT_STREQ(reinterpret_cast<const char*>(sqlite3_column_text(st, 0)), "RUNNING");
    EXPECT_STREQ(reinterpret_cast<const char*>(sqlite3_column_text(st, 1)), "worker-running-lease");
    EXPECT_NE(sqlite3_column_type(st, 2), SQLITE_NULL);
    sqlite3_finalize(st);

    renewed = false;
    ASSERT_TRUE(execution_db.RenewExecutionJobLease(1762, "worker-running-lease", 30000, &renewed, &err)) << err;
    EXPECT_TRUE(renewed);

    ASSERT_TRUE(ExecSql(db_, "UPDATE exec_job SET lease_expires_at_utc=1 WHERE job_id=1762;"));
    int rows_requeued = 0;
    ASSERT_TRUE(execution_db.RequeueExpiredExecutionLeases(&rows_requeued, &err)) << err;
    EXPECT_EQ(rows_requeued, 1);

    ASSERT_EQ(SQLITE_OK, sqlite3_prepare_v2(
        db_,
        "SELECT state, claimed_by_token, lease_expires_at_utc, started_at_utc FROM exec_job WHERE job_id=1762;",
        -1,
        &st,
        nullptr));
    ASSERT_EQ(SQLITE_ROW, sqlite3_step(st));
    EXPECT_STREQ(reinterpret_cast<const char*>(sqlite3_column_text(st, 0)), "QUEUED");
    EXPECT_EQ(sqlite3_column_type(st, 1), SQLITE_NULL);
    EXPECT_EQ(sqlite3_column_type(st, 2), SQLITE_NULL);
    EXPECT_EQ(sqlite3_column_type(st, 3), SQLITE_NULL);
    sqlite3_finalize(st);
}

TEST_F(SqliteDbFixture, Stage3dLiveExpiredClaimRecoveryOnlyRequeuesClaimedJobs) {
    using namespace savor::db::execution::workflow;
    using namespace savor::db::migrations;

    const MigrationSourceOptions embedded_options{ .source_kind = MigrationSourceKind::Embedded };
    std::string err;
    ASSERT_TRUE(ApplyContextMigrations(db_, MigrationContext::Execution, embedded_options, &err)) << err;

    ASSERT_TRUE(ExecSql(db_, R"SQL(
INSERT INTO exec_job_set(job_set_id, program_kind, purpose, created_at_utc)
VALUES(1781, 7, 'live-claim-recovery', unixepoch()*1000);
INSERT INTO exec_job(job_id, job_set_id, program_kind, program_version, program_ref_kind, program_ref_id, fingerprint, priority, state, attempts, max_attempts, claimed_by_token, lease_expires_at_utc, queued_at_utc, started_at_utc)
VALUES
  (1782, 1781, 7, 1, 'seed_probe', 33, 'fp-live-claimed', 5, 'CLAIMED', 0, 3, 'workflow_job_materializer', 1, 1000, NULL),
  (1783, 1781, 7, 1, 'seed_probe', 33, 'fp-live-running', 4, 'RUNNING', 0, 3, 'worker-1', 1, 1000, 2000),
  (1784, 1781, 7, 1, 'seed_probe', 33, 'fp-live-queued-token', 3, 'QUEUED', 0, 3, 'worker-1', 1, 1000, NULL);
)SQL"));

    SqliteExecutionDb execution_db(db_);
    int rows_requeued = 0;
    ASSERT_TRUE(execution_db.RequeueExpiredClaimedExecutionJobs(&rows_requeued, &err)) << err;
    EXPECT_EQ(rows_requeued, 1);

    sqlite3_stmt* st = nullptr;
    ASSERT_EQ(SQLITE_OK, sqlite3_prepare_v2(
        db_,
        "SELECT state, claimed_by_token, lease_expires_at_utc FROM exec_job WHERE job_id=1782;",
        -1,
        &st,
        nullptr));
    ASSERT_EQ(SQLITE_ROW, sqlite3_step(st));
    EXPECT_STREQ(reinterpret_cast<const char*>(sqlite3_column_text(st, 0)), "QUEUED");
    EXPECT_EQ(sqlite3_column_type(st, 1), SQLITE_NULL);
    EXPECT_EQ(sqlite3_column_type(st, 2), SQLITE_NULL);
    sqlite3_finalize(st);

    ASSERT_EQ(SQLITE_OK, sqlite3_prepare_v2(
        db_,
        "SELECT state, claimed_by_token, started_at_utc FROM exec_job WHERE job_id=1783;",
        -1,
        &st,
        nullptr));
    ASSERT_EQ(SQLITE_ROW, sqlite3_step(st));
    EXPECT_STREQ(reinterpret_cast<const char*>(sqlite3_column_text(st, 0)), "RUNNING");
    EXPECT_STREQ(reinterpret_cast<const char*>(sqlite3_column_text(st, 1)), "worker-1");
    EXPECT_NE(sqlite3_column_type(st, 2), SQLITE_NULL);
    sqlite3_finalize(st);

    ASSERT_EQ(SQLITE_OK, sqlite3_prepare_v2(
        db_,
        "SELECT state, claimed_by_token FROM exec_job WHERE job_id=1784;",
        -1,
        &st,
        nullptr));
    ASSERT_EQ(SQLITE_ROW, sqlite3_step(st));
    EXPECT_STREQ(reinterpret_cast<const char*>(sqlite3_column_text(st, 0)), "QUEUED");
    EXPECT_STREQ(reinterpret_cast<const char*>(sqlite3_column_text(st, 1)), "worker-1");
    sqlite3_finalize(st);

    ASSERT_EQ(SQLITE_OK, sqlite3_prepare_v2(
        db_,
        "SELECT COUNT(1) FROM exec_outbox_message WHERE event_type='Execution.JobQueued.v1' AND payload_ref_kind='job' AND payload_ref_id=1782;",
        -1,
        &st,
        nullptr));
    ASSERT_EQ(SQLITE_ROW, sqlite3_step(st));
    EXPECT_EQ(sqlite3_column_int(st, 0), 1);
    sqlite3_finalize(st);
}

TEST_F(SqliteDbFixture, Stage3dStartupRecoveryRequeuesInterruptedAndClaimedJobs) {
    using namespace savor::db::execution::workflow;
    using namespace savor::db::migrations;

    const MigrationSourceOptions embedded_options{ .source_kind = MigrationSourceKind::Embedded };
    std::string err;
    ASSERT_TRUE(ApplyContextMigrations(db_, MigrationContext::Execution, embedded_options, &err)) << err;

    ASSERT_TRUE(ExecSql(db_, R"SQL(
INSERT INTO exec_job_set(job_set_id, program_kind, purpose, created_at_utc)
VALUES(1771, 7, 'startup-recovery', unixepoch()*1000);
INSERT INTO exec_job(job_id, job_set_id, program_kind, program_version, program_ref_kind, program_ref_id, fingerprint, priority, state, attempts, max_attempts, claimed_by_token, lease_expires_at_utc, queued_at_utc, started_at_utc, ended_at_utc, error_code, error_text)
VALUES
  (1772, 1771, 7, 1, 'seed_probe', 33, 'fp-startup-running-token', 5, 'RUNNING', 2, 3, 'worker-old', 9999999999999, 1000, 2000, 3000, 'OLD', 'old error'),
  (1773, 1771, 7, 1, 'seed_probe', 33, 'fp-startup-running-no-token', 4, 'RUNNING', 1, 3, NULL, NULL, 1100, 2100, NULL, NULL, NULL),
  (1774, 1771, 7, 1, 'seed_probe', 33, 'fp-startup-claimed', 3, 'CLAIMED', 0, 3, 'worker-old', 9999999999999, 1200, NULL, NULL, NULL, NULL),
  (1775, 1771, 7, 1, 'seed_probe', 33, 'fp-startup-queued-token', 2, 'QUEUED', 0, 3, 'worker-old', 9999999999999, 1300, NULL, NULL, NULL, NULL),
  (1776, 1771, 7, 1, 'seed_probe', 33, 'fp-startup-queued-clean', 1, 'QUEUED', 0, 3, NULL, NULL, 1400, NULL, NULL, NULL, NULL),
  (1777, 1771, 7, 1, 'seed_probe', 33, 'fp-startup-terminal-token', 1, 'SUCCEEDED', 0, 3, 'worker-old', 9999999999999, 1500, 2100, 2300, NULL, NULL),
  (1778, 1771, 7, 1, 'seed_probe', 33, 'fp-startup-terminal-failed-token', 1, 'FAILED', 0, 3, 'worker-old', 9999999999999, 1600, 2200, 2400, 'FAIL', 'old fail');
)SQL"));

    SqliteExecutionDb execution_db(db_);
    int rows_requeued = 0;
    ASSERT_TRUE(execution_db.RequeueInterruptedExecutionJobs(&rows_requeued, &err)) << err;
    EXPECT_EQ(rows_requeued, 4);

    sqlite3_stmt* st = nullptr;
    ASSERT_EQ(SQLITE_OK, sqlite3_prepare_v2(
        db_,
        "SELECT COUNT(1) FROM exec_job "
        "WHERE job_id BETWEEN 1772 AND 1775 "
        "AND state='QUEUED' "
        "AND claimed_by_token IS NULL "
        "AND lease_expires_at_utc IS NULL "
        "AND started_at_utc IS NULL "
        "AND ended_at_utc IS NULL "
        "AND error_code IS NULL "
        "AND error_text IS NULL;",
        -1,
        &st,
        nullptr));
    ASSERT_EQ(SQLITE_ROW, sqlite3_step(st));
    EXPECT_EQ(sqlite3_column_int(st, 0), 4);
    sqlite3_finalize(st);

    ASSERT_EQ(SQLITE_OK, sqlite3_prepare_v2(
        db_,
        "SELECT COUNT(1) FROM exec_job "
        "WHERE job_id IN (1777,1778) "
        "AND state IN ('SUCCEEDED','FAILED') "
        "AND claimed_by_token IS NULL "
        "AND lease_expires_at_utc IS NULL;",
        -1,
        &st,
        nullptr));
    ASSERT_EQ(SQLITE_ROW, sqlite3_step(st));
    EXPECT_EQ(sqlite3_column_int(st, 0), 2);
    sqlite3_finalize(st);

    ASSERT_EQ(SQLITE_OK, sqlite3_prepare_v2(
        db_,
        "SELECT attempts, queued_at_utc, state, claimed_by_token FROM exec_job WHERE job_id=1776;",
        -1,
        &st,
        nullptr));
    ASSERT_EQ(SQLITE_ROW, sqlite3_step(st));
    EXPECT_EQ(sqlite3_column_int(st, 0), 0);
    EXPECT_EQ(sqlite3_column_int64(st, 1), 1400);
    EXPECT_STREQ(reinterpret_cast<const char*>(sqlite3_column_text(st, 2)), "QUEUED");
    EXPECT_EQ(sqlite3_column_type(st, 3), SQLITE_NULL);
    sqlite3_finalize(st);

    ASSERT_EQ(SQLITE_OK, sqlite3_prepare_v2(
        db_,
        "SELECT COUNT(1) FROM exec_job_event WHERE event_kind='Execution.JobQueued.v1' AND message='STARTUP_RECOVERY';",
        -1,
        &st,
        nullptr));
    ASSERT_EQ(SQLITE_ROW, sqlite3_step(st));
    EXPECT_EQ(sqlite3_column_int(st, 0), 4);
    sqlite3_finalize(st);

    ASSERT_EQ(SQLITE_OK, sqlite3_prepare_v2(
        db_,
        "SELECT COUNT(1) FROM exec_outbox_message WHERE event_type='Execution.JobQueued.v1' AND payload_ref_kind='job' AND payload_ref_id BETWEEN 1772 AND 1775;",
        -1,
        &st,
        nullptr));
    ASSERT_EQ(SQLITE_ROW, sqlite3_step(st));
    EXPECT_EQ(sqlite3_column_int(st, 0), 4);
    sqlite3_finalize(st);
}

TEST_F(SqliteDbFixture, Stage3dClaimNextReadyExecutionJobResolvesWorkflowStepThroughJobSetAncestry) {
    using namespace savor::db::execution::workflow;
    using namespace savor::db::migrations;

    const MigrationSourceOptions embedded_options{ .source_kind = MigrationSourceKind::Embedded };
    std::string err;
    ASSERT_TRUE(ApplyContextMigrations(db_, MigrationContext::Execution, embedded_options, &err)) << err;

    ASSERT_TRUE(ExecSql(db_, R"SQL(
INSERT INTO exec_workflow_instance(workflow_instance_id, workflow_kind, state, root_scope_kind, created_by, created_at_utc)
VALUES(1799, 'SEED_PROBE_CHAIN', 'RUNNING', 'manual', 'test', unixepoch()*1000);
INSERT INTO exec_job_set(job_set_id, parent_job_set_id, program_kind, purpose, created_at_utc)
VALUES(1801, NULL, 7, 'stage3d-root', unixepoch()*1000);
INSERT INTO exec_job_set(job_set_id, parent_job_set_id, program_kind, purpose, created_at_utc)
VALUES(1802, 1801, 7, 'stage3d-child', unixepoch()*1000);
INSERT INTO exec_job_set(job_set_id, parent_job_set_id, program_kind, purpose, created_at_utc)
VALUES(1803, 1802, 7, 'stage3d-grandchild', unixepoch()*1000);
INSERT INTO exec_workflow_step(workflow_step_id, workflow_instance_id, step_key, step_kind, state, job_set_id, priority, attempts, max_attempts, created_at_utc)
VALUES(1800, 1799, 'Unique', 'seedprobe.unique', 'MATERIALIZED', 1801, 9, 0, 2, unixepoch()*1000);
INSERT INTO exec_job(job_id, job_set_id, program_kind, program_version, program_ref_kind, program_ref_id, fingerprint, priority, state, attempts, max_attempts, queued_at_utc)
VALUES(1804, 1803, 7, 1, 'seed_probe', 33, 'fp-stage3d-claim-ancestry', 5, 'QUEUED', 0, 3, unixepoch()*1000);
)SQL"));

    SqliteExecutionDb execution_db(db_);
    std::string claim_error;
    const auto claimed = execution_db.ClaimNextReadyExecutionJob("worker-claim-ancestry", 30000, &claim_error);
    EXPECT_TRUE(claim_error.empty()) << claim_error;
    ASSERT_TRUE(claimed.has_value());
    EXPECT_EQ(claimed->job_id, 1804);
    EXPECT_EQ(claimed->job_set_id, 1803);
    EXPECT_EQ(claimed->workflow_instance_id, 1799);
    EXPECT_EQ(claimed->workflow_step_id, 1800);
    EXPECT_EQ(claimed->workflow_step_key, "Unique");
    EXPECT_EQ(claimed->workflow_step_kind, "seedprobe.unique");
    EXPECT_EQ(claimed->workflow_step_priority, 9);

    const auto snapshot = execution_db.WorkflowQueryService()->GetStepTerminalSnapshotForJob(1804);
    ASSERT_TRUE(snapshot.has_value());
    EXPECT_EQ(snapshot->workflow_instance_id, 1799);
    EXPECT_EQ(snapshot->workflow_step_id, 1800);
    EXPECT_EQ(snapshot->job_set_id, 1801);
    EXPECT_EQ(snapshot->step_key, "Unique");
    EXPECT_EQ(snapshot->step_kind, "seedprobe.unique");
}

TEST_F(SqliteDbFixture, Stage3dJobSetTreeProgressAndTerminalSnapshotIncludeChildJobSets) {
    using namespace savor::db::execution::workflow;
    using namespace savor::db::migrations;

    const MigrationSourceOptions embedded_options{ .source_kind = MigrationSourceKind::Embedded };
    std::string err;
    ASSERT_TRUE(ApplyContextMigrations(db_, MigrationContext::Execution, embedded_options, &err)) << err;

    ASSERT_TRUE(ExecSql(db_, R"SQL(
INSERT INTO exec_workflow_instance(workflow_instance_id, workflow_kind, state, root_scope_kind, created_by, created_at_utc)
VALUES(1899, 'SEED_PROBE_CHAIN', 'RUNNING', 'manual', 'test', unixepoch()*1000);
INSERT INTO exec_job_set(job_set_id, parent_job_set_id, program_kind, purpose, expected_total, created_at_utc)
VALUES(1901, NULL, 7, 'stage3d-root', 2, unixepoch()*1000);
INSERT INTO exec_job_set(job_set_id, parent_job_set_id, program_kind, purpose, expected_total, meta_note, created_at_utc)
VALUES(1902, 1901, 7, 'stage3d-child-a', 2, 'expected_delta=11', unixepoch()*1000);
INSERT INTO exec_job_set(job_set_id, parent_job_set_id, program_kind, purpose, expected_total, meta_note, created_at_utc)
VALUES(1903, 1901, 7, 'stage3d-child-b', 1, 'expected_delta=22', unixepoch()*1000);
INSERT INTO exec_workflow_step(workflow_step_id, workflow_instance_id, step_key, step_kind, state, job_set_id, priority, attempts, max_attempts, created_at_utc)
VALUES(1900, 1899, 'Unique', 'seedprobe.unique', 'MATERIALIZED', 1901, 9, 0, 2, unixepoch()*1000);
INSERT INTO exec_job(job_id, job_set_id, program_kind, program_version, program_ref_kind, program_ref_id, fingerprint, priority, state, attempts, max_attempts, queued_at_utc, ended_at_utc)
VALUES(1904, 1902, 7, 1, 'seed_probe', 33, 'fp-stage3d-tree-a1', 5, 'SUCCEEDED_WINNER', 0, 3, unixepoch()*1000, unixepoch()*1000);
INSERT INTO exec_job(job_id, job_set_id, program_kind, program_version, program_ref_kind, program_ref_id, fingerprint, priority, state, attempts, max_attempts, queued_at_utc, ended_at_utc)
VALUES(1905, 1902, 7, 1, 'seed_probe', 33, 'fp-stage3d-tree-a2', 5, 'SUPERSEDED', 0, 3, unixepoch()*1000, unixepoch()*1000);
INSERT INTO exec_job(job_id, job_set_id, program_kind, program_version, program_ref_kind, program_ref_id, fingerprint, priority, state, attempts, max_attempts, queued_at_utc)
VALUES(1906, 1903, 7, 1, 'seed_probe', 33, 'fp-stage3d-tree-b1', 5, 'QUEUED', 0, 3, unixepoch()*1000);
)SQL"));

    SqliteExecutionDb execution_db(db_);
    const auto progress = execution_db.GetJobSetProgress(1901);
    ASSERT_TRUE(progress.has_value());
    EXPECT_EQ(progress->job_set_id, 1901);
    EXPECT_EQ(progress->expected_total, 3);
    EXPECT_EQ(progress->total_jobs, 3);
    EXPECT_EQ(progress->completed_jobs, 2);
    EXPECT_EQ(progress->succeeded_jobs, 2);
    EXPECT_EQ(progress->failed_jobs, 0);

    const auto children = execution_db.GetChildJobSetProgress(1901);
    ASSERT_EQ(children.size(), 2u);
    EXPECT_EQ(children[0].job_set_id, 1902);
    ASSERT_TRUE(children[0].expected_delta.has_value());
    EXPECT_EQ(*children[0].expected_delta, 11);
    EXPECT_EQ(children[0].completed_jobs, 2);
    EXPECT_EQ(children[1].job_set_id, 1903);
    ASSERT_TRUE(children[1].expected_delta.has_value());
    EXPECT_EQ(*children[1].expected_delta, 22);
    EXPECT_EQ(children[1].completed_jobs, 0);

    const auto snapshot = execution_db.WorkflowQueryService()->GetStepTerminalSnapshotForJob(1904);
    ASSERT_TRUE(snapshot.has_value());
    EXPECT_EQ(snapshot->job_set_id, 1901);
    EXPECT_EQ(snapshot->expected_total, 3);
    EXPECT_EQ(snapshot->discovered_total, 3);
    EXPECT_EQ(snapshot->terminal_total, 2);
    EXPECT_EQ(snapshot->failed_total, 0);
}

TEST_F(SqliteDbFixture, Stage3dMarkQueuedJobsSupersededEmitsJobCompletedOutbox) {
    using namespace savor::db::execution::workflow;
    using namespace savor::db::migrations;

    const MigrationSourceOptions embedded_options{ .source_kind = MigrationSourceKind::Embedded };
    std::string err;
    ASSERT_TRUE(ApplyContextMigrations(db_, MigrationContext::Execution, embedded_options, &err)) << err;

    ASSERT_TRUE(ExecSql(db_, R"SQL(
INSERT INTO exec_job_set(job_set_id, parent_job_set_id, program_kind, purpose, expected_total, created_at_utc)
VALUES(1910, NULL, 7, 'supersede-event-root', 4, 1000);
INSERT INTO exec_job(job_id, job_set_id, program_kind, program_version, program_ref_kind, program_ref_id, fingerprint, priority, state, attempts, max_attempts, queued_at_utc)
VALUES
(1911, 1910, 7, 1, 'seed_probe', 33, 'fp-supersede-keep', 5, 'QUEUED', 0, 3, 1000),
(1912, 1910, 7, 1, 'seed_probe', 33, 'fp-supersede-a', 5, 'QUEUED', 0, 3, 1000),
(1913, 1910, 7, 1, 'seed_probe', 33, 'fp-supersede-b', 5, 'QUEUED', 0, 3, 1000),
(1914, 1910, 7, 1, 'seed_probe', 33, 'fp-supersede-terminal', 5, 'SUCCEEDED', 0, 3, 1000);
)SQL"));

    SqliteExecutionDb execution_db(db_);
    int rows_superseded = 0;
    ASSERT_TRUE(execution_db.MarkQueuedJobsSuperseded(1910, 1911, &err, &rows_superseded)) << err;
    EXPECT_EQ(rows_superseded, 2);
    EXPECT_EQ(ReadText(db_, "SELECT state FROM exec_job WHERE job_id=1911;"), "QUEUED");
    EXPECT_EQ(ReadText(db_, "SELECT state FROM exec_job WHERE job_id=1912;"), "SUPERSEDED");
    EXPECT_EQ(ReadText(db_, "SELECT state FROM exec_job WHERE job_id=1913;"), "SUPERSEDED");
    EXPECT_EQ(ReadText(db_, "SELECT state FROM exec_job WHERE job_id=1914;"), "SUCCEEDED");
    EXPECT_EQ(ReadInt64(db_, "SELECT COUNT(1) FROM exec_outbox_message WHERE event_type='Execution.JobCompleted.v1' AND payload_ref_kind='job' AND payload_ref_id IN (1912,1913);"), 2);
    EXPECT_EQ(ReadInt64(db_, "SELECT COUNT(1) FROM exec_job_event WHERE event_kind='Execution.JobCompleted.v1' AND message='SUPERSEDE' AND job_id IN (1912,1913);"), 2);
    EXPECT_EQ(ReadInt64(db_, "SELECT COUNT(1) FROM exec_outbox_message WHERE payload_ref_kind='job' AND payload_ref_id=1911;"), 0);

    ASSERT_TRUE(execution_db.MarkQueuedJobsSuperseded(1910, 1911, &err, &rows_superseded)) << err;
    EXPECT_EQ(rows_superseded, 0);
    EXPECT_EQ(ReadInt64(db_, "SELECT COUNT(1) FROM exec_outbox_message WHERE event_type='Execution.JobCompleted.v1' AND payload_ref_kind='job' AND payload_ref_id IN (1912,1913);"), 2);
}

TEST_F(SqliteDbFixture, Stage3dClaimNextReadyExecutionJobFailsAndRollsBackWhenJobSetAncestryHasNoWorkflowStep) {
    using namespace savor::db::execution::workflow;
    using namespace savor::db::migrations;

    const MigrationSourceOptions embedded_options{ .source_kind = MigrationSourceKind::Embedded };
    std::string err;
    ASSERT_TRUE(ApplyContextMigrations(db_, MigrationContext::Execution, embedded_options, &err)) << err;

    ASSERT_TRUE(ExecSql(db_, R"SQL(
INSERT INTO exec_job_set(job_set_id, parent_job_set_id, program_kind, purpose, created_at_utc)
VALUES(1811, NULL, 7, 'stage3d-orphan-root', unixepoch()*1000);
INSERT INTO exec_job_set(job_set_id, parent_job_set_id, program_kind, purpose, created_at_utc)
VALUES(1812, 1811, 7, 'stage3d-orphan-child', unixepoch()*1000);
INSERT INTO exec_job(job_id, job_set_id, program_kind, program_version, program_ref_kind, program_ref_id, fingerprint, priority, state, attempts, max_attempts, queued_at_utc)
VALUES(1813, 1812, 7, 1, 'seed_probe', 33, 'fp-stage3d-claim-orphan', 5, 'QUEUED', 0, 3, unixepoch()*1000);
)SQL"));

    SqliteExecutionDb execution_db(db_);
    std::string claim_error;
    const auto claimed = execution_db.ClaimNextReadyExecutionJob("worker-claim-orphan", 30000, &claim_error);
    EXPECT_FALSE(claimed.has_value());
    EXPECT_NE(claim_error.find("workflow step not found for job_set ancestry"), std::string::npos) << claim_error;

    sqlite3_stmt* st = nullptr;
    ASSERT_EQ(SQLITE_OK, sqlite3_prepare_v2(
        db_,
        "SELECT claimed_by_token, lease_expires_at_utc FROM exec_job WHERE job_id=1813;",
        -1,
        &st,
        nullptr));
    ASSERT_EQ(SQLITE_ROW, sqlite3_step(st));
    EXPECT_EQ(sqlite3_column_type(st, 0), SQLITE_NULL);
    EXPECT_EQ(sqlite3_column_type(st, 1), SQLITE_NULL);
    sqlite3_finalize(st);
}

TEST_F(SqliteDbFixture, Stage3dExecutionJobCommandServiceRejectsJobCompletedWithoutTerminalState) {
    using namespace savor::db::execution::jobs;
    using namespace savor::db::execution::workflow;
    using namespace savor::db::migrations;

    const MigrationSourceOptions embedded_options{ .source_kind = MigrationSourceKind::Embedded };
    std::string err;
    ASSERT_TRUE(ApplyContextMigrations(db_, MigrationContext::Execution, embedded_options, &err)) << err;

    ASSERT_TRUE(ExecSql(db_, R"SQL(
INSERT INTO exec_job_set(job_set_id, program_kind, purpose, created_at_utc)
VALUES(1501, 1, 'stage3d', unixepoch()*1000);
INSERT INTO exec_job(job_id, job_set_id, program_kind, program_version, program_ref_kind, program_ref_id, fingerprint, priority, state, attempts, max_attempts, queued_at_utc)
VALUES(1601, 1501, 1, 1, 'seed_probe', 10, 'fp-stage3d-1601', 5, 'QUEUED', 0, 3, unixepoch()*1000);
)SQL"));

    SqliteExecutionDb execution_db(db_);
    auto* job_commands = execution_db.JobCommandService();
    ASSERT_NE(job_commands, nullptr);

    EXPECT_FALSE(job_commands->AppendLifecycleEvent({ .kind = JobLifecycleEventKind::JobCompleted, .job_id = 1601 }, &err));
    EXPECT_NE(err.find("terminal_state must be set for JobCompleted"), std::string::npos);
}

TEST_F(SqliteDbFixture, Stage3dExecutionPayloadResolverReadsJobSetAndJobPayloads) {
    using namespace savor::db::execution::workflow;
    using namespace savor::db::migrations;

    const MigrationSourceOptions embedded_options{ .source_kind = MigrationSourceKind::Embedded };
    std::string err;
    ASSERT_TRUE(ApplyContextMigrations(db_, MigrationContext::Execution, embedded_options, &err)) << err;

    ASSERT_TRUE(ExecSql(db_, R"SQL(
INSERT INTO exec_job_set(job_set_id, program_kind, purpose, created_at_utc)
VALUES(701, 1, 'payload', unixepoch()*1000);
INSERT INTO exec_job(job_id, job_set_id, program_kind, program_version, program_ref_kind, program_ref_id, fingerprint, priority, state, attempts, max_attempts, queued_at_utc)
VALUES(801, 701, 1, 1, 'seed_probe', 11, 'fp-stage3d-801', 5, 'QUEUED', 0, 2, unixepoch()*1000);
)SQL"));

    savor::db::execution::workflow::SqliteExecutionDb execution_db(db_);

    const auto from_job_set = execution_db.ResolveExecutionWorkflowJobPayload(
        "Execution.JobSetCreated.v1", 1, "job_set", 701);
    ASSERT_TRUE(from_job_set.has_value());
    EXPECT_EQ(from_job_set->job_set_id, 701);
    EXPECT_EQ(from_job_set->job_id, 0);

    const auto from_job = execution_db.ResolveExecutionWorkflowJobPayload(
        "Execution.JobQueued.v1", 1, "job", 801);
    ASSERT_TRUE(from_job.has_value());
    EXPECT_EQ(from_job->job_set_id, 701);
    EXPECT_EQ(from_job->job_id, 801);

    savor::db::events::EventEnvelope invalid{};
    invalid.event_type = "Execution.JobQueued.v1";
    invalid.event_version = 1;
    invalid.context_name = "Execution";
    invalid.aggregate_kind = "job";
    invalid.payload_ref_kind = "workflow_event";
    invalid.payload_ref_id = 801;
    EXPECT_FALSE(execution_db.ResolveExecutionWorkflowJobPayload(invalid).has_value());
}

TEST_F(SqliteDbFixture, Stage0ReplayBackfillValidationResolvesWorkflowPayloadRefs) {
    using namespace savor::db::events;
    using namespace savor::db::migrations;

    const MigrationSourceOptions embedded_options{ .source_kind = MigrationSourceKind::Embedded };
    std::string err;
    ASSERT_TRUE(ApplyContextMigrations(db_, MigrationContext::Execution, embedded_options, &err)) << err;

    ASSERT_TRUE(ExecSql(db_, R"SQL(
INSERT INTO exec_workflow_instance(
    workflow_instance_id, workflow_kind, state, root_scope_kind, created_by, created_at_utc, started_at_utc
)
VALUES(9201, 'SEED_PROBE_CHAIN', 'RUNNING', 'manual', 'stage0-replay', unixepoch()*1000, unixepoch()*1000);
INSERT INTO exec_workflow_step(
    workflow_step_id, workflow_instance_id, step_key, step_kind, state, created_at_utc, ready_at_utc
)
VALUES(9202, 9201, 'seedprobe.neutral', 'seedprobe.neutral', 'READY', unixepoch()*1000, unixepoch()*1000);
INSERT INTO exec_workflow_event(workflow_event_id, workflow_instance_id, workflow_step_id, event_kind, event_ts_utc, message)
VALUES(9203, 9201, 9202, 'Execution.WorkflowStepReady.v1', unixepoch()*1000, 'ready');
INSERT INTO exec_outbox_message(
    outbox_id,event_id,event_type,event_version,context_name,aggregate_kind,aggregate_id,correlation_id,causation_id,occurred_at_utc,payload_ref_kind,payload_ref_id
)
VALUES
    (9205,'evt-stage0-replay-1','Execution.WorkflowStepReady.v1',1,'Execution','workflow_instance','9201','corr-9201','cause-9201',unixepoch()*1000,'workflow_event',9203);
)SQL")) << sqlite3_errmsg(db_);

    savor::db::execution::workflow::SqliteExecutionDb execution_db(db_);
    int unresolved_count = 0;

    OutboxRelay relay({
        .db = db_,
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
    OutboxRelayResult result{};
    ASSERT_TRUE(relay.RelayBatch(0, 10, bindings, &result, &err)) << err;
    EXPECT_EQ(result.failure_count, 0);
    EXPECT_EQ(unresolved_count, 0);
    EXPECT_EQ(result.published_count, 1);
}

TEST_F(SqliteDbFixture, Stage3dAnalysisBattleCommandsEmitEventsThirtyThroughThirtySix) {
    using namespace savor::db;
    using namespace savor::db::analysis;
    using namespace savor::db::migrations;

    const MigrationSourceOptions embedded_options{ .source_kind = MigrationSourceKind::Embedded };
    std::string err;
    ASSERT_TRUE(ApplyContextMigrations(db_, MigrationContext::AnalysisBattle, embedded_options, &err)) << err;

    SqliteAnalysisDb analysis_db(db_);

    const auto now = types::UtcTimePoint(std::chrono::milliseconds(1712304000000));
    std::int64_t battle_set_id = 0;
    ASSERT_TRUE(analysis_db.CreateBattleSet(
        {
            .name = "stage3d-battle-set",
            .entry_savestate_id = 101,
            .battle_run_spec_id = 202,
            .explorer_settings_id = 303,
            .status = savor::db::BattleSetStatus::Active,
            .created_at_utc = now,
            .correlation_id = "ab-corr-1",
            .causation_id = "ab-cause-1",
        },
        &battle_set_id,
        &err))
        << err;
    ASSERT_GT(battle_set_id, 0);

    std::int64_t seed_candidate_id = 0;
    ASSERT_TRUE(analysis_db.AddBattleSeedCandidate(
        {
            .battle_set_id = battle_set_id,
            .source_unique_seed_id = 444,
            .seed_value = 555,
            .source_kind = savor::db::BattleSeedCandidateSourceKind::SeedProbeUnique,
            .candidate_status = savor::db::BattleSeedCandidateStatus::Pending,
            .created_at_utc = now,
            .correlation_id = "ab-corr-1",
            .causation_id = "ab-cause-2",
        },
        &seed_candidate_id,
        &err))
        << err;

    std::int64_t wave_id = 0;
    ASSERT_TRUE(analysis_db.CreateBattleTurnWave(
        {
            .battle_set_id = battle_set_id,
            .turn_index = 1,
            .seed_candidate_id = seed_candidate_id,
            .status = savor::db::BattleTurnWaveStatus::Running,
            .created_at_utc = now,
            .correlation_id = "ab-corr-1",
            .causation_id = "ab-cause-3",
        },
        &wave_id,
        &err))
        << err;

    std::int64_t turn_job_id = 0;
    ASSERT_TRUE(analysis_db.RecordBattleTurnJob(
        {
            .wave_id = wave_id,
            .exec_job_id = 7001,
            .plan_id = 9001,
            .source_savestate_id = 1234,
            .seed_candidate_id = seed_candidate_id,
            .authored_plan_id = 9001,
            .authored_turn_index = 1,
            .resolved_turn_commands_blob = "01000000000004ffff",
            .resolved_turn_variant_key = "variant-a",
            .fake_attacks_this_turn = 2,
            .fake_attacks_used_before = 1,
            .job_state = savor::db::BattleTurnJobState::Completed,
            .has_results = true,
            .battle_outcome = savor::battle::Outcome::Defeat,
            .recorded_at_utc = now,
            .correlation_id = "ab-corr-1",
            .causation_id = "ab-cause-4",
        },
        &turn_job_id,
        &err))
        << err;

    std::int64_t battle_advancement_pool_id = 0;
    ASSERT_TRUE(analysis_db.CreateBattleAdvancementPool(
        {
            .battle_set_id = battle_set_id,
            .turn_index = 1,
            .pool_name = "pool-a",
            .criterion_kind = savor::db::BattleAdvancementCriterionKind::MaxVi,
            .created_at_utc = now,
            .correlation_id = "ab-corr-1",
            .causation_id = "ab-cause-5",
        },
        &battle_advancement_pool_id,
        &err))
        << err;

    std::int64_t battle_advancement_decision_id = 0;
    ASSERT_TRUE(analysis_db.RecordBattleAdvancementDecision(
        {
            .battle_advancement_pool_id = battle_advancement_pool_id,
            .turn_job_id = turn_job_id,
            .decision_kind = savor::db::BattleAdvancementDecisionKind::Selected,
            .decision_reason = std::string("best vi"),
            .created_at_utc = now,
            .correlation_id = "ab-corr-1",
            .causation_id = "ab-cause-6",
        },
        &battle_advancement_decision_id,
        &err))
        << err;

    std::int64_t manual_followup_id = 0;
    ASSERT_TRUE(analysis_db.UpsertBattleManualFollowup(
        {
            .turn_job_id = turn_job_id,
            .manual_followup_status = savor::db::BattleManualFollowupStatus::Recorded,
            .recorded_dtm_artifact_id = 777,
            .note = std::string("stage3d"),
            .updated_at_utc = now,
            .correlation_id = "ab-corr-1",
            .causation_id = "ab-cause-7",
        },
        &manual_followup_id,
        &err))
        << err;
    EXPECT_GT(manual_followup_id, 0);

    sqlite3_stmt* st = nullptr;
    ASSERT_EQ(SQLITE_OK, sqlite3_prepare_v2(db_, "SELECT COUNT(1) FROM ab_outbox_message;", -1, &st, nullptr));
    ASSERT_EQ(SQLITE_ROW, sqlite3_step(st));
    EXPECT_EQ(sqlite3_column_int(st, 0), 7);
    sqlite3_finalize(st);

    ASSERT_EQ(SQLITE_OK, sqlite3_prepare_v2(db_, "SELECT COUNT(1) FROM ab_outbox_message WHERE context_name='AnalysisBattle';", -1, &st, nullptr));
    ASSERT_EQ(SQLITE_ROW, sqlite3_step(st));
    EXPECT_EQ(sqlite3_column_int(st, 0), 7);
    sqlite3_finalize(st);

    const auto payload_30 = analysis_db.ResolveBattlePayload(1, "battle_set", battle_set_id);
    ASSERT_TRUE(payload_30.has_value());
    EXPECT_EQ(payload_30->battle_set_id, battle_set_id);

    const auto payload_31 = analysis_db.ResolveBattlePayload(1, "seed_candidate", seed_candidate_id);
    ASSERT_TRUE(payload_31.has_value());
    EXPECT_EQ(payload_31->battle_set_id, battle_set_id);

    const auto payload_32 = analysis_db.ResolveBattlePayload(1, "turn_wave", wave_id);
    ASSERT_TRUE(payload_32.has_value());
    EXPECT_EQ(payload_32->wave_id, wave_id);

    const auto payload_33 = analysis_db.ResolveBattlePayload(1, "turn_job", turn_job_id);
    ASSERT_TRUE(payload_33.has_value());
    EXPECT_EQ(payload_33->turn_job_id, turn_job_id);

    const auto payload_34 = analysis_db.ResolveBattlePayload(1, "battle_advancement_pool", battle_advancement_pool_id);
    ASSERT_TRUE(payload_34.has_value());
    EXPECT_EQ(payload_34->battle_set_id, battle_set_id);

    const auto payload_35 = analysis_db.ResolveBattlePayload(1, "battle_advancement_decision", battle_advancement_decision_id);
    ASSERT_TRUE(payload_35.has_value());
    EXPECT_EQ(payload_35->battle_set_id, battle_set_id);
    EXPECT_EQ(payload_35->turn_job_id, turn_job_id);

    const auto payload_36 = analysis_db.ResolveBattlePayload(1, "manual_followup", manual_followup_id);
    ASSERT_TRUE(payload_36.has_value());
    EXPECT_EQ(payload_36->battle_set_id, battle_set_id);
    EXPECT_EQ(payload_36->turn_job_id, turn_job_id);
}

TEST_F(SqliteDbFixture, Stage3dBattleAuthoringAndAnalysisQueriesRoundTrip) {
    using namespace savor::db;
    using namespace savor::db::analysis;
    using namespace savor::db::migrations;

    const MigrationSourceOptions embedded_options{ .source_kind = MigrationSourceKind::Embedded };
    std::string err;
    ASSERT_TRUE(ApplyContextMigrations(db_, MigrationContext::Authoring, embedded_options, &err)) << err;
    ASSERT_TRUE(ApplyContextMigrations(db_, MigrationContext::AnalysisBattle, embedded_options, &err)) << err;

    SqliteAuthoringDb authoring_db(db_);
    SqliteAnalysisDb analysis_db(db_);
    const auto now = types::UtcTimePoint(std::chrono::milliseconds(1712304000000));

    std::int64_t battle_run_spec_id = 0;
    ASSERT_TRUE(authoring_db.SaveBattleRunSpec(
        {
            .name = "single-turn-run",
            .priority = 7,
            .run_ms = 35000,
            .vi_stall_ms = 1200,
            .progress_enable = true,
            .use_single_turn_runner = true,
            .auto_wave_trigger_enable = true,
            .created_at_utc = now,
            .correlation_id = "au-corr",
            .causation_id = "au-cause-1",
        },
        &battle_run_spec_id,
        &err)) << err;

    std::int64_t plan_id = 0;
    ASSERT_TRUE(authoring_db.SavePlan(
        {
            .name = "single-turn-plan",
            .fingerprint = "plan-fp-1",
            .num_turns = 2,
            .created_at_utc = now,
            .correlation_id = "au-corr",
            .causation_id = "au-cause-2",
        },
        &plan_id,
        &err)) << err;

    std::int64_t attack_preset_id = 0;
    ASSERT_TRUE(authoring_db.SaveBattlePlanActionPreset(
        {
            .name = "attack-mask-preset",
            .macro = soa::battle::actions::BattleAction::Attack,
            .target_kind = savor::db::BattlePlanTargetKind::MultipleEnemies,
            .target_mask_bits = 1 << 4,
            .created_at_utc = now,
            .correlation_id = "au-corr",
            .causation_id = "au-cause-3a",
        },
        &attack_preset_id,
        &err)) << err;
    ASSERT_GT(attack_preset_id, 0);

    std::int64_t item_preset_id = 0;
    ASSERT_TRUE(authoring_db.SaveBattlePlanActionPreset(
        {
            .name = "use-item-same-as-preset",
            .macro = soa::battle::actions::BattleAction::UseItem,
            .target_kind = savor::db::BattlePlanTargetKind::SameAsOtherPC,
            .target_same_as_actor_slot = 0,
            .item_id = 99,
            .created_at_utc = now,
            .correlation_id = "au-corr",
            .causation_id = "au-cause-3b",
        },
        &item_preset_id,
        &err)) << err;
    ASSERT_GT(item_preset_id, 0);

    ASSERT_TRUE(authoring_db.RenameBattlePlanActionPreset(
        {
            .action_preset_id = item_preset_id,
            .name = "renamed-use-item-same-as-preset",
            .updated_at_utc = now,
            .correlation_id = "au-corr",
            .causation_id = "au-cause-3c",
        },
        &err)) << err;

    std::int64_t edited_attack_preset_id = 0;
    ASSERT_TRUE(authoring_db.SaveBattlePlanActionPreset(
        {
            .name = "attack-mask-preset",
            .macro = soa::battle::actions::BattleAction::Attack,
            .target_kind = savor::db::BattlePlanTargetKind::SingleEnemy,
            .target_single_slot = 4,
            .created_at_utc = now,
            .correlation_id = "au-corr",
            .causation_id = "au-cause-3d",
        },
        &edited_attack_preset_id,
        &err)) << err;
    ASSERT_GT(edited_attack_preset_id, 0);
    EXPECT_NE(edited_attack_preset_id, attack_preset_id);

    std::int64_t plan_turn_id = 0;
    ASSERT_TRUE(authoring_db.SaveBattlePlanTurn(
        {
            .plan_id = plan_id,
            .turn_index = 1,
            .actions = {
                {
                    .actor_slot = 0,
                    .action_preset_id = attack_preset_id,
                    .ordinal = 0,
                },
                {
                    .actor_slot = 1,
                    .action_preset_id = item_preset_id,
                    .ordinal = 1,
                },
            },
            .created_at_utc = now,
            .correlation_id = "au-corr",
            .causation_id = "au-cause-3",
        },
        &plan_turn_id,
        &err)) << err;
    ASSERT_GT(plan_turn_id, 0);

    std::int64_t lhs_address_program_id = 0;
    ASSERT_TRUE(authoring_db.EnsureAddressProgram(
        {
            .program_version = 1,
            .prog_bytes = { 0x00 },
            .derived_buffer_version = 1,
            .derived_buffer_schema_hash = std::string("derived-hash"),
            .soa_structs_hash = std::string("soa-hash"),
            .description = "fixture lhs address program",
        },
        &lhs_address_program_id,
        &err)) << err;
    ASSERT_GT(lhs_address_program_id, 0);

    std::int64_t duplicate_address_program_id = 0;
    ASSERT_TRUE(authoring_db.EnsureAddressProgram(
        {
            .program_version = 1,
            .prog_bytes = { 0x00 },
            .derived_buffer_version = 1,
            .derived_buffer_schema_hash = std::string("derived-hash"),
            .soa_structs_hash = std::string("soa-hash"),
            .description = "duplicate description is not identity",
        },
        &duplicate_address_program_id,
        &err)) << err;
    EXPECT_EQ(duplicate_address_program_id, lhs_address_program_id);

    const auto address_program = authoring_db.GetAddressProgram(lhs_address_program_id);
    ASSERT_TRUE(address_program.has_value());
    EXPECT_EQ(address_program->program_version, 1);
    EXPECT_EQ(address_program->prog_bytes, std::vector<std::uint8_t>({ 0x00 }));
    EXPECT_EQ(address_program->derived_buffer_version.value_or(0), 1);
    EXPECT_EQ(address_program->derived_buffer_schema_hash.value_or(""), "derived-hash");
    EXPECT_EQ(address_program->soa_structs_hash.value_or(""), "soa-hash");

    std::int64_t predicate_spec_id = 0;
    ASSERT_TRUE(authoring_db.SavePredicateSpec(
        {
            .name = "battle-hp-check",
            .breakpoint_id = bp::battle::EndTurn,
            .required_breakpoint_ids = { bp::battle::EndTurn },
            .lhs_value = 0x1000,
            .rhs_value = 0,
            .cmp_op = savor::db::PredicateComparisonOp::GT,
            .width = 2,
            .flag_mask = static_cast<std::int64_t>(
                static_cast<std::uint32_t>(savor::pred::PredFlag::Active)
                | static_cast<std::uint32_t>(savor::pred::PredFlag::LhsIsProg)),
            .lhs_address_program_id = lhs_address_program_id,
            .abort_on_fail = true,
            .created_at_utc = now,
            .correlation_id = "au-corr",
            .causation_id = "au-cause-4",
        },
        &predicate_spec_id,
        &err)) << err;

    std::int64_t predicate_set_id = 0;
    ASSERT_TRUE(authoring_db.SavePredicateSet(
        {
            .name = "battle-predicate-set",
            .predicate_spec_ids = { predicate_spec_id },
            .created_at_utc = now,
        },
        &predicate_set_id,
        &err)) << err;

    std::int64_t explorer_settings_id = 0;
    ASSERT_TRUE(authoring_db.SaveExplorerSettings(
        {
            .name = "battle-explorer",
            .description = "fixture settings",
            .default_plan_id = plan_id,
            .default_predicate_set_id = predicate_set_id,
            .created_at_utc = now,
            .correlation_id = "au-corr",
            .causation_id = "au-cause-5",
        },
        &explorer_settings_id,
        &err)) << err;

    const auto run_spec = authoring_db.GetBattleRunSpec(battle_run_spec_id);
    ASSERT_TRUE(run_spec.has_value());
    EXPECT_TRUE(run_spec->use_single_turn_runner);
    EXPECT_EQ(run_spec->run_ms, 35000);

    const auto plan = authoring_db.GetBattlePlan(plan_id);
    ASSERT_TRUE(plan.has_value());
    ASSERT_EQ(plan->turns.size(), 1);
    EXPECT_EQ(plan->turns[0].turn_index, 1);
    ASSERT_EQ(plan->turns[0].actions.size(), 2);
    EXPECT_EQ(plan->turns[0].actions[0].action_preset_id, attack_preset_id);
    EXPECT_EQ(plan->turns[0].actions[0].action_preset.macro, soa::battle::actions::BattleAction::Attack);
    EXPECT_EQ(plan->turns[0].actions[0].action_preset.target_kind, savor::db::BattlePlanTargetKind::MultipleEnemies);
    EXPECT_EQ(plan->turns[0].actions[0].action_preset.target_mask_bits.value_or(-1), 1 << 4);
    EXPECT_EQ(plan->turns[0].actions[1].action_preset_id, item_preset_id);
    EXPECT_EQ(plan->turns[0].actions[1].action_preset.name, "renamed-use-item-same-as-preset");
    EXPECT_EQ(plan->turns[0].actions[1].action_preset.macro, soa::battle::actions::BattleAction::UseItem);
    EXPECT_EQ(plan->turns[0].actions[1].action_preset.target_kind, savor::db::BattlePlanTargetKind::SameAsOtherPC);
    EXPECT_EQ(plan->turns[0].actions[1].action_preset.target_same_as_actor_slot.value_or(-1), 0);
    EXPECT_EQ(plan->turns[0].actions[1].action_preset.item_id.value_or(-1), 99);

    const auto predicate_set = authoring_db.GetPredicateSet(predicate_set_id);
    ASSERT_TRUE(predicate_set.has_value());
    EXPECT_EQ(predicate_set->name, "battle-predicate-set");
    ASSERT_EQ(predicate_set->predicates.size(), 1);
    EXPECT_EQ(predicate_set->predicates[0].name, "battle-hp-check");
    EXPECT_EQ(predicate_set->predicates[0].breakpoint_id, bp::battle::EndTurn);
    EXPECT_EQ(predicate_set->predicates[0].required_breakpoint_ids, std::vector<BPKey>({ bp::battle::EndTurn }));
    EXPECT_EQ(predicate_set->predicates[0].width, 2);
    EXPECT_EQ(predicate_set->predicates[0].lhs_address_program_id.value_or(0), lhs_address_program_id);
    EXPECT_FALSE(predicate_set->predicates[0].rhs_address_program_id.has_value());
    EXPECT_TRUE(predicate_set->predicates[0].abort_on_fail);

    const auto explorer_settings = authoring_db.GetExplorerSettings(explorer_settings_id);
    ASSERT_TRUE(explorer_settings.has_value());
    EXPECT_EQ(explorer_settings->default_plan_id.value_or(0), plan_id);
    EXPECT_EQ(explorer_settings->default_predicate_set_id.value_or(0), predicate_set_id);

    std::int64_t battle_set_id = 0;
    ASSERT_TRUE(analysis_db.CreateBattleSet(
        {
            .name = "battle-set-roundtrip",
            .entry_savestate_id = 501,
            .battle_run_spec_id = battle_run_spec_id,
            .explorer_settings_id = explorer_settings_id,
            .status = savor::db::BattleSetStatus::Active,
            .created_at_utc = now,
            .correlation_id = "ab-corr",
            .causation_id = "ab-cause-1",
        },
        &battle_set_id,
        &err)) << err;

    std::int64_t seed_candidate_id = 0;
    ASSERT_TRUE(analysis_db.AddBattleSeedCandidate(
        {
            .battle_set_id = battle_set_id,
            .source_unique_seed_id = 2001,
            .seed_value = 7777,
            .source_kind = savor::db::BattleSeedCandidateSourceKind::SeedProbeUnique,
            .candidate_status = savor::db::BattleSeedCandidateStatus::Pending,
            .created_at_utc = now,
            .correlation_id = "ab-corr",
            .causation_id = "ab-cause-2",
        },
        &seed_candidate_id,
        &err)) << err;

    std::int64_t wave_id = 0;
    ASSERT_TRUE(analysis_db.CreateBattleTurnWave(
        {
            .battle_set_id = battle_set_id,
            .turn_index = 1,
            .seed_candidate_id = seed_candidate_id,
            .status = savor::db::BattleTurnWaveStatus::Ready,
            .created_at_utc = now,
            .correlation_id = "ab-corr",
            .causation_id = "ab-cause-3",
        },
        &wave_id,
        &err)) << err;

    std::int64_t turn_job_id = 0;
    ASSERT_TRUE(analysis_db.RecordBattleTurnJob(
        {
            .wave_id = wave_id,
            .exec_job_id = 88001,
            .plan_id = plan_id,
            .fake_attacks_this_turn = 2,
            .fake_attacks_used_before = 1,
            .job_state = savor::db::BattleTurnJobState::Completed,
            .started_at_utc = now,
            .ended_at_utc = now,
            .has_results = true,
            .vi_start = 100,
            .vi_end = 125,
            .delta_vi = 25,
            .rng_seed = 7777,
            .battle_outcome = savor::battle::Outcome::Defeat,
            .pred_passed = 1,
            .pred_total = 1,
            .pred_abort_run = 0,
            .output_savestate_id = 9501,
            .recorded_at_utc = now,
            .correlation_id = "ab-corr",
            .causation_id = "ab-cause-4",
        },
        &turn_job_id,
        &err)) << err;

    std::int64_t battle_advancement_pool_id = 0;
    ASSERT_TRUE(analysis_db.CreateBattleAdvancementPool(
        {
            .battle_set_id = battle_set_id,
            .turn_index = 1,
            .pool_name = "turn-1-results",
            .criterion_kind = savor::db::BattleAdvancementCriterionKind::ViDelta,
            .created_at_utc = now,
            .correlation_id = "ab-corr",
            .causation_id = "ab-cause-5",
        },
        &battle_advancement_pool_id,
        &err)) << err;

    std::int64_t battle_advancement_decision_id = 0;
    ASSERT_TRUE(analysis_db.RecordBattleAdvancementDecision(
        {
            .battle_advancement_pool_id = battle_advancement_pool_id,
            .turn_job_id = turn_job_id,
            .decision_kind = savor::db::BattleAdvancementDecisionKind::Selected,
            .decision_reason = std::string("best delta vi"),
            .created_at_utc = now,
            .correlation_id = "ab-corr",
            .causation_id = "ab-cause-6",
        },
        &battle_advancement_decision_id,
        &err)) << err;

    const auto battle_set = analysis_db.GetBattleSet(battle_set_id);
    ASSERT_TRUE(battle_set.has_value());
    EXPECT_EQ(battle_set->battle_run_spec_id, battle_run_spec_id);
    EXPECT_EQ(battle_set->explorer_settings_id, explorer_settings_id);

    const auto candidates = analysis_db.ListBattleSeedCandidates(battle_set_id);
    ASSERT_EQ(candidates.size(), 1);
    EXPECT_EQ(candidates[0].source_unique_seed_id.value_or(0), 2001);
    EXPECT_EQ(candidates[0].seed_value, 7777);

    const auto wave = analysis_db.GetBattleTurnWave(wave_id);
    ASSERT_TRUE(wave.has_value());
    EXPECT_EQ(wave->seed_candidate_id, seed_candidate_id);
    EXPECT_EQ(wave->status, savor::db::BattleTurnWaveStatus::Ready);

    const auto waves = analysis_db.ListBattleTurnWaves(battle_set_id);
    ASSERT_EQ(waves.size(), 1);
    EXPECT_EQ(waves[0].wave_id, wave_id);

    const auto turn_job = analysis_db.GetBattleTurnJobForExecJob(88001);
    ASSERT_TRUE(turn_job.has_value());
    EXPECT_EQ(turn_job->turn_job_id, turn_job_id);
    EXPECT_EQ(turn_job->plan_id, plan_id);
    EXPECT_EQ(turn_job->delta_vi.value_or(0), 25);
    EXPECT_EQ(turn_job->output_savestate_id.value_or(0), 9501);

    const auto turn_job_by_id = analysis_db.GetBattleTurnJob(turn_job_id);
    ASSERT_TRUE(turn_job_by_id.has_value());
    EXPECT_EQ(turn_job_by_id->exec_job_id.value_or(0), 88001);
    EXPECT_EQ(turn_job_by_id->rng_seed.value_or(0), 7777);

    const auto turn_jobs = analysis_db.ListBattleTurnJobsForWave(wave_id);
    ASSERT_EQ(turn_jobs.size(), 1);
    EXPECT_EQ(turn_jobs[0].exec_job_id.value_or(0), 88001);

    const auto decisions = analysis_db.ListBattleAdvancementDecisionsForPool(battle_advancement_pool_id);
    ASSERT_EQ(decisions.size(), 1);
    EXPECT_EQ(decisions[0].battle_advancement_decision_id, battle_advancement_decision_id);
    EXPECT_EQ(decisions[0].decision_kind, savor::db::BattleAdvancementDecisionKind::Selected);
    EXPECT_EQ(decisions[0].decision_reason.value_or(""), "best delta vi");
}

TEST_F(SqliteDbFixture, Stage5AuthoringInputSetsAreIdempotentByOrderedFrameContent) {
    using namespace savor::db;

    auto* authoring_db = db_service_->AuthoringDb();
    ASSERT_NE(authoring_db, nullptr);
    const auto now = types::UtcTimePoint(std::chrono::milliseconds(1712304000901));
    std::string err;

    const std::vector<AuthoringInputSetFrameCommand> frames = {
        { .main_x = 128, .main_y = 128, .cstick_x = 128, .cstick_y = 128, .trigger_x = 0, .trigger_y = 0 },
        { .main_x = 160, .main_y = 128, .cstick_x = 128, .cstick_y = 128, .trigger_x = 0, .trigger_y = 0 },
    };

    std::int64_t first_id = 0;
    ASSERT_TRUE(authoring_db->EnsureAuthoringInputSet(
        { .name = "neutral-plus-right", .frames = frames, .created_at_utc = now },
        &first_id,
        &err)) << err;
    ASSERT_GT(first_id, 0);

    std::int64_t duplicate_id = 0;
    ASSERT_TRUE(authoring_db->EnsureAuthoringInputSet(
        { .name = "same-content-different-name", .frames = frames, .created_at_utc = now },
        &duplicate_id,
        &err)) << err;
    EXPECT_EQ(duplicate_id, first_id);

    auto reversed = frames;
    std::reverse(reversed.begin(), reversed.end());
    std::int64_t reversed_id = 0;
    ASSERT_TRUE(authoring_db->EnsureAuthoringInputSet(
        { .name = "same-frames-different-order", .frames = reversed, .created_at_utc = now },
        &reversed_id,
        &err)) << err;
    EXPECT_GT(reversed_id, 0);
    EXPECT_NE(reversed_id, first_id);

    std::int64_t empty_id = 0;
    EXPECT_FALSE(authoring_db->EnsureAuthoringInputSet(
        { .name = "empty", .frames = {}, .created_at_utc = now },
        &empty_id,
        &err));

    const auto stored = authoring_db->ListAuthoringInputSetFrames(first_id);
    ASSERT_EQ(stored.size(), frames.size());
    EXPECT_EQ(stored[0].main_x, 128);
    EXPECT_EQ(stored[1].main_x, 160);
}

TEST_F(SqliteDbFixture, Stage5SeedProbeRunCreatesOwnedAnalysisInputSetAndRecordsUniqueFrames) {
    using namespace savor::db;

    auto* authoring_db = db_service_->AuthoringDb();
    auto* analysis_db = db_service_->AnalysisDb();
    ASSERT_NE(authoring_db, nullptr);
    ASSERT_NE(analysis_db, nullptr);
    const auto now = types::UtcTimePoint(std::chrono::milliseconds(1712304000902));
    std::string err;

    std::int64_t seed_probe_spec_id = 0;
    ASSERT_TRUE(authoring_db->SaveSeedProbeSpec(
        {
            .name = "input-set-owner-spec",
            .priority = 1,
            .run_ms = 1000,
            .vi_stall_ms = 0,
            .min_value = 0,
            .max_value = 0,
            .combo_attempts_per_target = 1,
            .created_at_utc = now,
            .correlation_id = "au-seedprobe-input-set-owner",
        },
        &seed_probe_spec_id,
        &err)) << err;

    std::int64_t probe_set_id = 0;
    ASSERT_TRUE(analysis_db->CreateSeedProbeSet(
        {
            .name = "input-set-owner-probe-set",
            .probe_flavor = "BATTLE_PRE",
            .breakpoint_policy_name = "test",
            .segment_source_kind = "fixture",
            .created_at_utc = now,
            .correlation_id = "an-probe-set-input-set-owner",
        },
        &probe_set_id,
        &err)) << err;

    std::int64_t probe_run_id = 0;
    ASSERT_TRUE(analysis_db->RequestSeedProbeRun(
        {
            .probe_set_id = probe_set_id,
            .entry_savestate_id = 77001,
            .seed_probe_spec_id = seed_probe_spec_id,
            .codec_version = 1,
            .status = "requested",
            .requested_at_utc = now,
            .correlation_id = "an-probe-run-input-set-owner",
        },
        &probe_run_id,
        &err)) << err;

    const auto probe_run = analysis_db->GetSeedProbeRun(probe_run_id);
    ASSERT_TRUE(probe_run.has_value());
    ASSERT_GT(probe_run->unique_input_set_id, 0);
    EXPECT_TRUE(analysis_db->ListAnalysisInputSetFrames(probe_run->unique_input_set_id).empty());

    std::int64_t input_frame_id = 0;
    ASSERT_TRUE(analysis_db->EnsureSeedProbeInputFrame(0x8080, 0x8080, 0x0000, &input_frame_id, &err)) << err;

    ASSERT_TRUE(analysis_db->SetSeedProbeRunNeutralSeed(probe_run_id, 0, &err)) << err;
    const auto probe_result_id = analysis_db->LookupSeedProbeResultId(probe_run_id);
    ASSERT_TRUE(probe_result_id.has_value());

    RecordSeedProbeUniqueSeedCommand unique_cmd{
        .probe_result_id = *probe_result_id,
        .input_frame_id = input_frame_id,
        .seed_value = 0,
        .seed_delta = 0,
        .recorded_at_utc = now,
        .correlation_id = "an-probe-unique-input-set-owner",
    };
    bool inserted = false;
    std::int64_t unique_seed_id = 0;
    ASSERT_TRUE(analysis_db->EnsureSeedProbeUniqueSeedDelta(unique_cmd, &inserted, &unique_seed_id, &err)) << err;
    EXPECT_TRUE(inserted);
    EXPECT_GT(unique_seed_id, 0);
    ASSERT_TRUE(analysis_db->EnsureSeedProbeUniqueSeedDelta(unique_cmd, &inserted, &unique_seed_id, &err)) << err;
    EXPECT_FALSE(inserted);

    const auto frames = analysis_db->ListAnalysisInputSetFrames(probe_run->unique_input_set_id);
    ASSERT_EQ(frames.size(), 1);
    EXPECT_EQ(frames[0].ordinal, 0);
    EXPECT_EQ(frames[0].input_frame_id, input_frame_id);
}

TEST_F(SqliteDbFixture, Stage5BattleChainGraphAcceptsInputSetsAndRejectsProbeRunFrameSetRefs) {
    using namespace savor::db;
    using namespace savor::db::execution::programdb;
    using namespace savor::db::execution::programdb::battlecontext;
    using namespace savor::db::execution::workflow;
    using namespace savor::runner::parallel::savordb;

    auto* authoring_db = db_service_->AuthoringDb();
    auto* analysis_db = db_service_->AnalysisDb();
    auto* execution_db = db_service_->ExecutionDb();
    ASSERT_NE(authoring_db, nullptr);
    ASSERT_NE(analysis_db, nullptr);
    ASSERT_NE(execution_db, nullptr);

    const auto now = types::UtcTimePoint(std::chrono::milliseconds(1712304000903));
    std::string err;

    std::int64_t battle_run_spec_id = 0;
    ASSERT_TRUE(authoring_db->SaveBattleRunSpec(
        {
            .name = "input-set-battle-run",
            .priority = 1,
            .run_ms = 1000,
            .vi_stall_ms = 0,
            .use_single_turn_runner = true,
            .created_at_utc = now,
            .correlation_id = "au-battle-input-set",
        },
        &battle_run_spec_id,
        &err)) << err;

    std::int64_t explorer_settings_id = 0;
    ASSERT_TRUE(authoring_db->SaveExplorerSettings(
        {
            .name = "input-set-battle-settings",
            .created_at_utc = now,
            .correlation_id = "au-battle-input-set",
        },
        &explorer_settings_id,
        &err)) << err;

    std::int64_t battle_chain_spec_id = 0;
    ASSERT_TRUE(authoring_db->SaveBattleChainSpec(
        {
            .name = "input-set-battle-chain",
            .battle_run_spec_id = battle_run_spec_id,
            .explorer_settings_id = explorer_settings_id,
            .created_at_utc = now,
            .correlation_id = "au-battle-input-set",
        },
        &battle_chain_spec_id,
        &err)) << err;

    std::int64_t authored_input_set_id = 0;
    ASSERT_TRUE(authoring_db->EnsureAuthoringInputSet(
        {
            .name = "neutral-for-battle",
            .frames = {
                { .main_x = 128, .main_y = 128, .cstick_x = 128, .cstick_y = 128, .trigger_x = 0, .trigger_y = 0 },
            },
            .created_at_utc = now,
        },
        &authored_input_set_id,
        &err)) << err;

    SaveWorkflowGraphResult saved{};
    ASSERT_TRUE(authoring_db->SaveWorkflowGraph(
        {
            .name = "battle-input-set-contract",
            .description = "Battle Chain input set ref-kind contract",
            .graph_version = 1,
            .graph_hash = "graph-hash-battle-input-set-contract",
            .nodes = {
                {
                    .node_key = "battle_1",
                    .unit_kind = "battle_chain",
                    .display_name = "Battle Chain",
                    .authored_ref_kind = std::string("authoring.battle_chain_spec"),
                    .authored_ref_id = battle_chain_spec_id,
                    .inputs = {
                        { .input_key = "entry_savestate", .data_kind = "state.savestate_id", .display_name = "Entry savestate" },
                        { .input_key = "initial_input_frames", .data_kind = "analysis.input_frame_set_id", .display_name = "Initial input frames" },
                    },
                },
            },
            .created_at_utc = now,
            .correlation_id = "au-battle-input-set",
        },
        &saved,
        &err)) << err;

    auto create_instance = [&](std::string ref_kind, std::int64_t ref_id) {
        std::int64_t workflow_instance_id = 0;
        WorkflowCreateInstanceCommand command{};
        command.workflow_kind = "workflow_graph";
        command.root_scope_kind = "manual";
        command.workflow_graph_revision_id = saved.workflow_graph_revision_id;
        command.created_by = "sqlite-fixture";
        command.created_at_utc = now.time_since_epoch().count();
        command.unit_activations.push_back(TestUnitActivation("battle_1", "battle_chain", "Battle Chain", {}, 5, 1));
        command.input_bindings.push_back({
            .node_key = "battle_1",
            .input_key = "entry_savestate",
            .data_kind = "state.savestate_id",
            .ref_kind = "state.savestate",
            .ref_id = 88001,
            .source_kind = "external",
        });
        command.input_bindings.push_back({
            .node_key = "battle_1",
            .input_key = "initial_input_frames",
            .data_kind = "analysis.input_frame_set_id",
            .ref_kind = std::move(ref_kind),
            .ref_id = ref_id,
            .source_kind = "external",
        });
        EXPECT_TRUE(execution_db->WorkflowCommandService()->CreateWorkflowInstance(command, &workflow_instance_id, &err)) << err;
        return workflow_instance_id;
    };

    ProgramKindRegistry registry;
    BattleContextProbePhaseRegistrationConfig config{};
    config.authoring_db = authoring_db;
    RegisterBattleContextProbePhaseDescriptor(&registry, execution_db, analysis_db, config);

    DBWorkflowWorkerCoordinator coordinator(
        execution_db,
        DBWorkflowWorkerCoordinatorConfig{},
        CoordinatorIntegrationConfig{ .workflow_enabled = true },
        &registry);

    const auto valid_instance_id = create_instance("au.input_set", authored_input_set_id);
    const auto valid_graph = execution_db->WorkflowQueryService()->GetWorkflowGraph(valid_instance_id);
    ASSERT_TRUE(valid_graph.has_value());
    ASSERT_EQ(valid_graph->steps.size(), 1u);
    const auto valid_scheduled = coordinator.MaterializeWorkflowStep(
        WorkflowReadyStep{
            .workflow_instance_id = valid_instance_id,
            .workflow_step_id = valid_graph->steps.front().workflow_step_id,
            .step_key = "battle_1",
            .step_kind = "battle_chain",
            .priority = 42,
        });
    ASSERT_TRUE(valid_scheduled.has_value());
    EXPECT_GT(valid_scheduled->job_set_id, 0);
    EXPECT_EQ(
        ReadInt64(db_, ("SELECT priority FROM exec_job WHERE job_set_id=" + std::to_string(valid_scheduled->job_set_id) + ";").c_str()),
        42);

    const auto invalid_instance_id = create_instance("sp_probe_run", 99001);
    const auto invalid_graph = execution_db->WorkflowQueryService()->GetWorkflowGraph(invalid_instance_id);
    ASSERT_TRUE(invalid_graph.has_value());
    ASSERT_EQ(invalid_graph->steps.size(), 1u);
    const auto invalid_scheduled = coordinator.MaterializeWorkflowStep(
        WorkflowReadyStep{
            .workflow_instance_id = invalid_instance_id,
            .workflow_step_id = invalid_graph->steps.front().workflow_step_id,
            .step_key = "battle_1",
            .step_kind = "battle_chain",
            .priority = 5,
        });
    EXPECT_FALSE(invalid_scheduled.has_value());
}

TEST_F(SqliteDbFixture, Stage5AuthoringWorkflowGraphStoresBattleChainSpecWithoutExternalInputs) {
    using namespace savor::db;
    std::string err;

    auto* authoring_db = db_service_->AuthoringDb();
    ASSERT_NE(authoring_db, nullptr);
    const auto now = types::UtcTimePoint(std::chrono::milliseconds(1712304000123));

    SaveWorkflowGraphResult saved{};
    ASSERT_TRUE(authoring_db->SaveWorkflowGraph(
        {
            .name = "canonical-start-to-battle",
            .description = "TAS -> seed probe -> battle chain",
            .graph_version = 1,
            .graph_hash = "graph-hash-canonical-start-to-battle",
            .nodes = {
                {
                    .node_key = "tas_1",
                    .unit_kind = "tas_movie",
                    .display_name = "TAS Movie",
                    .inputs = {
                        { .input_key = "dtm_artifact", .data_kind = "state_artifact.dtm_artifact_id", .display_name = "DTM artifact" },
                    },
                    .possible_outputs = {
                        { .output_key = "savestate", .data_kind = "state.savestate_id", .display_name = "Output savestate" },
                    },
                },
                {
                    .node_key = "probe_1",
                    .unit_kind = "seed_probe_chain",
                    .display_name = "Seed Probe Chain",
                    .inputs = {
                        { .input_key = "entry_savestate", .data_kind = "state.savestate_id", .display_name = "Entry savestate" },
                    },
                    .possible_outputs = {
                        { .output_key = "unique_input_frames", .data_kind = "analysis.input_frame_set_id", .display_name = "Unique input frames" },
                    },
                },
                {
                    .node_key = "battle_1",
                    .unit_kind = "battle_chain",
                    .display_name = "Battle Chain",
                    .authored_ref_kind = std::string("authoring.battle_chain_spec"),
                    .authored_ref_id = 77,
                    .inputs = {
                        { .input_key = "entry_savestate", .data_kind = "state.savestate_id", .display_name = "Entry savestate" },
                        { .input_key = "initial_input_frames", .data_kind = "analysis.input_frame_set_id", .display_name = "Initial input frames" },
                    },
                    .possible_outputs = {
                        { .output_key = "terminal_savestate", .data_kind = "state.savestate_id", .display_name = "Terminal savestate" },
                    },
                },
            },
            .edges = {
                { .from_node_key = "tas_1", .output_key = "savestate", .to_node_key = "probe_1", .input_key = "entry_savestate" },
                { .from_node_key = "tas_1", .output_key = "savestate", .to_node_key = "battle_1", .input_key = "entry_savestate" },
                { .from_node_key = "probe_1", .output_key = "unique_input_frames", .to_node_key = "battle_1", .input_key = "initial_input_frames" },
            },
            .created_at_utc = now,
            .correlation_id = "au-workflow-graph",
        },
        &saved,
        &err)) << err;
    ASSERT_GT(saved.workflow_graph_id, 0);
    ASSERT_GT(saved.workflow_graph_revision_id, 0);

    const auto graph = authoring_db->GetWorkflowGraph(saved.workflow_graph_id);
    ASSERT_TRUE(graph.has_value());
    EXPECT_EQ(graph->name, "canonical-start-to-battle");
    EXPECT_EQ(graph->workflow_graph_revision_id, saved.workflow_graph_revision_id);
    ASSERT_EQ(graph->nodes.size(), 3u);
    ASSERT_EQ(graph->edges.size(), 3u);
    EXPECT_EQ(graph->nodes[0].inputs[0].data_kind, "state_artifact.dtm_artifact_id");
    EXPECT_EQ(graph->nodes[1].possible_outputs[0].data_kind, "analysis.input_frame_set_id");
    EXPECT_EQ(graph->nodes[2].authored_ref_kind.value_or(""), "authoring.battle_chain_spec");
    EXPECT_EQ(graph->nodes[2].authored_ref_id.value_or(0), 77);
    EXPECT_EQ(graph->edges[2].from_node_key, "probe_1");

    sqlite3_stmt* input_binding_column_count = nullptr;
    ASSERT_EQ(SQLITE_OK, sqlite3_prepare_v2(
        db_,
        "SELECT COUNT(1) FROM pragma_table_info('au_workflow_graph_revision_node_input') "
        "WHERE name IN ('value','ref_id','artifact_id','savestate_id','analysis_id','payload_ref_id');",
        -1,
        &input_binding_column_count,
        nullptr));
    ASSERT_EQ(SQLITE_ROW, sqlite3_step(input_binding_column_count));
    EXPECT_EQ(sqlite3_column_int64(input_binding_column_count, 0), 0);
    sqlite3_finalize(input_binding_column_count);

    sqlite3_stmt* outbox = nullptr;
    ASSERT_EQ(SQLITE_OK, sqlite3_prepare_v2(
        db_,
        "SELECT aggregate_kind,payload_ref_kind,payload_ref_id FROM au_outbox_message WHERE event_type='Authoring.WorkflowGraphSaved.v1';",
        -1,
        &outbox,
        nullptr));
    ASSERT_EQ(SQLITE_ROW, sqlite3_step(outbox));
    EXPECT_STREQ(reinterpret_cast<const char*>(sqlite3_column_text(outbox, 0)), "workflow_graph");
    EXPECT_STREQ(reinterpret_cast<const char*>(sqlite3_column_text(outbox, 1)), "authoring_event");
    EXPECT_EQ(sqlite3_column_int64(outbox, 2), saved.workflow_graph_revision_id);
    sqlite3_finalize(outbox);

    const auto payload = authoring_db->ResolveAuthoringPayload(
        "Authoring.WorkflowGraphSaved.v1",
        1,
        "authoring_event",
        saved.workflow_graph_revision_id);
    ASSERT_TRUE(payload.has_value());
    EXPECT_EQ(payload->workflow_graph_id, saved.workflow_graph_id);
    EXPECT_EQ(payload->workflow_graph_revision_id, saved.workflow_graph_revision_id);
    EXPECT_EQ(payload->battle_chain_spec_id, 0);

    SaveWorkflowGraphResult revised{};
    ASSERT_TRUE(authoring_db->SaveWorkflowGraph(
        {
            .workflow_graph_id = saved.workflow_graph_id,
            .parent_revision_id = saved.workflow_graph_revision_id,
            .name = "canonical-start-to-battle",
            .description = "TAS -> seed probe -> battle chain",
            .graph_version = 2,
            .graph_hash = "graph-hash-canonical-start-to-battle-v2",
            .nodes = {
                {
                    .node_key = "tas_1",
                    .unit_kind = "tas_movie",
                    .display_name = "TAS Movie",
                    .inputs = {
                        { .input_key = "dtm_artifact", .data_kind = "state_artifact.dtm_artifact_id", .display_name = "DTM artifact" },
                    },
                    .possible_outputs = {
                        { .output_key = "savestate", .data_kind = "state.savestate_id", .display_name = "Output savestate" },
                    },
                },
                {
                    .node_key = "probe_1",
                    .unit_kind = "seed_probe_chain",
                    .display_name = "Seed Probe Chain",
                    .inputs = {
                        { .input_key = "entry_savestate", .data_kind = "state.savestate_id", .display_name = "Entry savestate" },
                    },
                    .possible_outputs = {
                        { .output_key = "unique_input_frames", .data_kind = "analysis.input_frame_set_id", .display_name = "Unique input frames" },
                    },
                },
                {
                    .node_key = "battle_1",
                    .unit_kind = "battle_chain",
                    .display_name = "Battle Chain",
                    .authored_ref_kind = std::string("authoring.battle_chain_spec"),
                    .authored_ref_id = 88,
                    .inputs = {
                        { .input_key = "entry_savestate", .data_kind = "state.savestate_id", .display_name = "Entry savestate" },
                        { .input_key = "initial_input_frames", .data_kind = "analysis.input_frame_set_id", .display_name = "Initial input frames" },
                    },
                    .possible_outputs = {
                        { .output_key = "terminal_savestate", .data_kind = "state.savestate_id", .display_name = "Terminal savestate" },
                    },
                },
            },
            .edges = {
                { .from_node_key = "tas_1", .output_key = "savestate", .to_node_key = "probe_1", .input_key = "entry_savestate" },
                { .from_node_key = "tas_1", .output_key = "savestate", .to_node_key = "battle_1", .input_key = "entry_savestate" },
                { .from_node_key = "probe_1", .output_key = "unique_input_frames", .to_node_key = "battle_1", .input_key = "initial_input_frames" },
            },
            .created_at_utc = now,
            .correlation_id = "au-workflow-graph",
            .causation_id = "au-workflow-graph",
        },
        &revised,
        &err)) << err;
    EXPECT_EQ(revised.workflow_graph_id, saved.workflow_graph_id);
    EXPECT_NE(revised.workflow_graph_revision_id, saved.workflow_graph_revision_id);

    const auto active_graph = authoring_db->GetWorkflowGraph(saved.workflow_graph_id);
    ASSERT_TRUE(active_graph.has_value());
    EXPECT_EQ(active_graph->workflow_graph_revision_id, revised.workflow_graph_revision_id);
    EXPECT_EQ(active_graph->parent_revision_id.value_or(0), saved.workflow_graph_revision_id);
    EXPECT_EQ(active_graph->nodes[2].authored_ref_id.value_or(0), 88);

    const auto first_revision = authoring_db->GetWorkflowGraphRevision(saved.workflow_graph_revision_id);
    ASSERT_TRUE(first_revision.has_value());
    EXPECT_EQ(first_revision->workflow_graph_id, saved.workflow_graph_id);
    EXPECT_EQ(first_revision->workflow_graph_revision_id, saved.workflow_graph_revision_id);
    EXPECT_EQ(first_revision->nodes[2].authored_ref_id.value_or(0), 77);

    sqlite3_stmt* revision_counts = nullptr;
    ASSERT_EQ(SQLITE_OK, sqlite3_prepare_v2(
        db_,
        "SELECT "
        "(SELECT COUNT(1) FROM au_workflow_graph WHERE workflow_graph_id=?1),"
        "(SELECT COUNT(1) FROM au_workflow_graph_revision WHERE workflow_graph_id=?1);",
        -1,
        &revision_counts,
        nullptr));
    sqlite3_bind_int64(revision_counts, 1, saved.workflow_graph_id);
    ASSERT_EQ(SQLITE_ROW, sqlite3_step(revision_counts));
    EXPECT_EQ(sqlite3_column_int64(revision_counts, 0), 1);
    EXPECT_EQ(sqlite3_column_int64(revision_counts, 1), 2);
    sqlite3_finalize(revision_counts);
}

TEST_F(SqliteDbFixture, Stage5ExecutionWorkflowInstanceStoresAuthoredGraphRevisionAndExternalInputs) {
    using namespace savor::db;
    using namespace savor::db::execution::workflow;

    auto* authoring_db = db_service_->AuthoringDb();
    ASSERT_NE(authoring_db, nullptr);

    std::string err;
    const auto now = types::UtcTimePoint(std::chrono::milliseconds(1712304000456));
    SaveWorkflowGraphResult saved{};
    ASSERT_TRUE(authoring_db->SaveWorkflowGraph(
        {
            .name = "launchable-seedprobe-to-battle",
            .description = "Seed probe can feed a battle chain",
            .graph_version = 1,
            .graph_hash = "graph-hash-launchable-seedprobe-to-battle",
            .nodes = {
                {
                    .node_key = "probe_1",
                    .unit_kind = "seed_probe_chain",
                    .display_name = "Seed Probe Chain",
                    .inputs = {
                        { .input_key = "entry_savestate", .data_kind = "state.savestate_id", .display_name = "Entry savestate" },
                    },
                    .possible_outputs = {
                        { .output_key = "unique_input_frames", .data_kind = "analysis.input_frame_set_id", .display_name = "Unique input frames" },
                    },
                },
                {
                    .node_key = "battle_1",
                    .unit_kind = "battle_chain",
                    .display_name = "Battle Chain",
                    .authored_ref_kind = std::string("authoring.battle_chain_spec"),
                    .authored_ref_id = 901,
                    .inputs = {
                        { .input_key = "initial_input_frames", .data_kind = "analysis.input_frame_set_id", .display_name = "Initial input frames" },
                    },
                    .possible_outputs = {
                        { .output_key = "terminal_savestate", .data_kind = "state.savestate_id", .display_name = "Terminal savestate" },
                    },
                },
            },
            .edges = {
                { .from_node_key = "probe_1", .output_key = "unique_input_frames", .to_node_key = "battle_1", .input_key = "initial_input_frames" },
            },
            .created_at_utc = now,
            .correlation_id = "au-workflow-graph-launch",
        },
        &saved,
        &err)) << err;

    auto* execution_db = db_service_->ExecutionDb();
    ASSERT_NE(execution_db, nullptr);
    std::int64_t workflow_instance_id = 0;
    WorkflowCreateInstanceCommand command{};
    command.workflow_kind = "workflow_graph";
    command.root_scope_kind = "manual";
    command.workflow_graph_revision_id = saved.workflow_graph_revision_id;
    command.created_by = "sqlite-fixture";
    command.created_at_utc = now.time_since_epoch().count();
    command.unit_activations.push_back(TestUnitActivation("probe_1", "seed_probe_chain", "Seed Probe Chain", {}, 10, 1));
    command.unit_activations.push_back(TestUnitActivation("battle_1", "battle_chain", "Battle Chain", { "probe_1" }, 5, 1));
    command.input_bindings.push_back({
        .node_key = "probe_1",
        .input_key = "entry_savestate",
        .data_kind = "state.savestate_id",
        .ref_kind = "state.savestate",
        .ref_id = 44001,
        .source_kind = "external",
    });
    command.arguments.push_back({ .node_key = "probe_1", .argument_key = "rtc", .value_type = "integer", .integer_value = 4, .source_kind = "launcher" });
    command.arguments.push_back({ .node_key = "battle_1", .argument_key = "fake_attack_min", .value_type = "integer", .integer_value = 22, .source_kind = "launcher" });
    command.arguments.push_back({ .node_key = "battle_1", .argument_key = "fake_attack_max", .value_type = "integer", .integer_value = 25, .source_kind = "launcher" });
    ASSERT_TRUE(execution_db->WorkflowCommandService()->CreateWorkflowInstance(command, &workflow_instance_id, &err)) << err;
    ASSERT_GT(workflow_instance_id, 0);

    const auto graph = execution_db->WorkflowQueryService()->GetWorkflowGraph(workflow_instance_id);
    ASSERT_TRUE(graph.has_value());
    EXPECT_EQ(graph->instance.workflow_graph_revision_id.value_or(0), saved.workflow_graph_revision_id);
    ASSERT_EQ(graph->input_bindings.size(), 1u);
    EXPECT_EQ(graph->input_bindings[0].workflow_instance_id, workflow_instance_id);
    EXPECT_EQ(graph->input_bindings[0].workflow_graph_revision_id, saved.workflow_graph_revision_id);
    EXPECT_EQ(graph->input_bindings[0].node_key, "probe_1");
    EXPECT_EQ(graph->input_bindings[0].input_key, "entry_savestate");
    EXPECT_EQ(graph->input_bindings[0].data_kind, "state.savestate_id");
    EXPECT_EQ(graph->input_bindings[0].ref_kind, "state.savestate");
    EXPECT_EQ(graph->input_bindings[0].ref_id, 44001);
    EXPECT_EQ(graph->input_bindings[0].source_kind, "external");
    ASSERT_EQ(graph->arguments.size(), 3u);
    EXPECT_EQ(graph->arguments[0].workflow_instance_id, workflow_instance_id);
    EXPECT_EQ(graph->arguments[0].node_key, "probe_1");
    EXPECT_EQ(graph->arguments[0].argument_key, "rtc");
    EXPECT_EQ(graph->arguments[0].value_type, "integer");
    ASSERT_TRUE(graph->arguments[0].integer_value.has_value());
    EXPECT_EQ(*graph->arguments[0].integer_value, 4);
    EXPECT_FALSE(graph->arguments[0].text_value.has_value());
    EXPECT_EQ(graph->arguments[0].source_kind, "launcher");
    EXPECT_EQ(graph->arguments[1].workflow_instance_id, workflow_instance_id);
    EXPECT_EQ(graph->arguments[1].node_key, "battle_1");
    EXPECT_EQ(graph->arguments[1].argument_key, "fake_attack_min");
    EXPECT_EQ(graph->arguments[1].value_type, "integer");
    ASSERT_TRUE(graph->arguments[1].integer_value.has_value());
    EXPECT_EQ(*graph->arguments[1].integer_value, 22);
    EXPECT_EQ(graph->arguments[1].source_kind, "launcher");
    EXPECT_EQ(graph->arguments[2].workflow_instance_id, workflow_instance_id);
    EXPECT_EQ(graph->arguments[2].node_key, "battle_1");
    EXPECT_EQ(graph->arguments[2].argument_key, "fake_attack_max");
    EXPECT_EQ(graph->arguments[2].value_type, "integer");
    ASSERT_TRUE(graph->arguments[2].integer_value.has_value());
    EXPECT_EQ(*graph->arguments[2].integer_value, 25);
    EXPECT_EQ(graph->arguments[2].source_kind, "launcher");

    const auto instances = execution_db->WorkflowQueryService()->ListWorkflowInstances(
        WorkflowInstanceState::Running,
        now.time_since_epoch().count() - 1,
        now.time_since_epoch().count() + 1);
    const auto instance_it = std::find_if(
        instances.begin(),
        instances.end(),
        [workflow_instance_id](const auto& row) { return row.workflow_instance_id == workflow_instance_id; });
    ASSERT_NE(instance_it, instances.end());
    EXPECT_EQ(instance_it->workflow_graph_revision_id.value_or(0), saved.workflow_graph_revision_id);

    err.clear();
    std::int64_t rejected_instance_id = 0;
    WorkflowCreateInstanceCommand rejected{};
    rejected.workflow_kind = "workflow_graph";
    rejected.root_scope_kind = "manual";
    rejected.created_by = "sqlite-fixture";
    rejected.created_at_utc = now.time_since_epoch().count();
    rejected.unit_activations.push_back(TestUnitActivation("probe_1", "seed_probe_chain", "Seed Probe Chain"));
    rejected.input_bindings.push_back({
        .node_key = "probe_1",
        .input_key = "entry_savestate",
        .data_kind = "state.savestate_id",
        .ref_kind = "state.savestate",
        .ref_id = 44002,
        .source_kind = "external",
    });
    EXPECT_FALSE(execution_db->WorkflowCommandService()->CreateWorkflowInstance(rejected, &rejected_instance_id, &err));
    EXPECT_NE(err.find("workflow_graph_revision_id is required"), std::string::npos);

    err.clear();
    WorkflowCreateInstanceCommand legacy{};
    legacy.workflow_kind = "SEED_PROBE_CHAIN";
    legacy.root_scope_kind = "manual";
    legacy.workflow_graph_revision_id = saved.workflow_graph_revision_id;
    legacy.created_by = "sqlite-fixture";
    legacy.created_at_utc = now.time_since_epoch().count();
    legacy.unit_activations.push_back(TestUnitActivation("probe_1", "seed_probe_chain", "Seed Probe Chain"));
    EXPECT_FALSE(execution_db->WorkflowCommandService()->CreateWorkflowInstance(legacy, &rejected_instance_id, &err));
    EXPECT_NE(err.find("workflow_kind must be workflow_graph"), std::string::npos);
}

TEST_F(SqliteDbFixture, Stage5GraphRoutingRoutesJobOutputsAndWaitsForRequiredInputs) {
    using namespace savor::db;
    using namespace savor::db::execution::workflow;

    auto* authoring_db = db_service_->AuthoringDb();
    auto* execution_db = db_service_->ExecutionDb();
    ASSERT_NE(authoring_db, nullptr);
    ASSERT_NE(execution_db, nullptr);

    std::string err;
    const auto now = types::UtcTimePoint(std::chrono::milliseconds(1712304000789));
    SaveWorkflowGraphResult saved{};
    ASSERT_TRUE(authoring_db->SaveWorkflowGraph(
        {
            .name = "route-tas-probe-battle",
            .description = "TAS and seed probe outputs feed battle",
            .graph_version = 1,
            .graph_hash = "graph-hash-route-tas-probe-battle",
            .nodes = {
                {
                    .node_key = "tas_1",
                    .unit_kind = "tas_movie",
                    .display_name = "TAS Movie",
                    .possible_outputs = {
                        { .output_key = "savestate", .data_kind = "state.savestate_id", .display_name = "Output savestate" },
                    },
                },
                {
                    .node_key = "probe_1",
                    .unit_kind = "seed_probe_chain",
                    .display_name = "Seed Probe Chain",
                    .inputs = {
                        { .input_key = "entry_savestate", .data_kind = "state.savestate_id", .display_name = "Entry savestate" },
                    },
                    .possible_outputs = {
                        { .output_key = "unique_input_frames", .data_kind = "analysis.input_frame_set_id", .display_name = "Unique input frames" },
                    },
                },
                {
                    .node_key = "battle_1",
                    .unit_kind = "battle_chain",
                    .display_name = "Battle Chain",
                    .inputs = {
                        { .input_key = "entry_savestate", .data_kind = "state.savestate_id", .display_name = "Entry savestate" },
                        { .input_key = "initial_input_frames", .data_kind = "analysis.input_frame_set_id", .display_name = "Initial input frames" },
                    },
                    .possible_outputs = {
                        { .output_key = "terminal_savestate", .data_kind = "state.savestate_id", .display_name = "Terminal savestate" },
                    },
                },
            },
            .edges = {
                { .from_node_key = "tas_1", .output_key = "savestate", .to_node_key = "probe_1", .input_key = "entry_savestate" },
                { .from_node_key = "tas_1", .output_key = "savestate", .to_node_key = "battle_1", .input_key = "entry_savestate" },
                { .from_node_key = "probe_1", .output_key = "unique_input_frames", .to_node_key = "battle_1", .input_key = "initial_input_frames" },
            },
            .created_at_utc = now,
            .correlation_id = "au-workflow-graph-routing",
        },
        &saved,
        &err)) << err;

    std::int64_t workflow_instance_id = 0;
    WorkflowCreateInstanceCommand create{};
    create.workflow_kind = "workflow_graph";
    create.root_scope_kind = "manual";
    create.workflow_graph_revision_id = saved.workflow_graph_revision_id;
    create.created_by = "sqlite-fixture";
    create.created_at_utc = now.time_since_epoch().count();
    create.unit_activations.push_back(TestUnitActivation("tas_1", "tas_movie", "TAS Movie", {}, 10, 1));
    create.unit_activations.push_back(TestUnitActivation("probe_1", "seed_probe_chain", "Seed Probe Chain", { "tas_1" }, 5, 1));
    create.unit_activations.push_back(TestUnitActivation("battle_1", "battle_chain", "Battle Chain", { "tas_1", "probe_1" }, 1, 1));
    ASSERT_TRUE(execution_db->WorkflowCommandService()->CreateWorkflowInstance(create, &workflow_instance_id, &err)) << err;

    auto step_id = [&](const std::string& step_key) -> std::int64_t {
        const auto graph = execution_db->WorkflowQueryService()->GetWorkflowGraph(workflow_instance_id);
        EXPECT_TRUE(graph.has_value());
        if (!graph.has_value()) {
            return 0;
        }
        const auto it = std::find_if(
            graph->steps.begin(),
            graph->steps.end(),
            [&](const auto& step) { return step.step_key == step_key; });
        EXPECT_NE(it, graph->steps.end());
        return it != graph->steps.end() ? it->workflow_step_id : 0;
    };
    auto step_state = [&](const std::string& step_key) -> WorkflowStepState {
        const auto graph = execution_db->WorkflowQueryService()->GetWorkflowGraph(workflow_instance_id);
        EXPECT_TRUE(graph.has_value());
        if (!graph.has_value()) {
            return WorkflowStepState::Failed;
        }
        const auto it = std::find_if(
            graph->steps.begin(),
            graph->steps.end(),
            [&](const auto& step) { return step.step_key == step_key; });
        EXPECT_NE(it, graph->steps.end());
        return it != graph->steps.end() ? it->state : WorkflowStepState::Failed;
    };
    auto step_priority = [&](const std::string& step_key) -> int {
        const auto graph = execution_db->WorkflowQueryService()->GetWorkflowGraph(workflow_instance_id);
        EXPECT_TRUE(graph.has_value());
        if (!graph.has_value()) {
            return 0;
        }
        const auto it = std::find_if(
            graph->steps.begin(),
            graph->steps.end(),
            [&](const auto& step) { return step.step_key == step_key; });
        EXPECT_NE(it, graph->steps.end());
        return it != graph->steps.end() ? it->priority : 0;
    };

    const auto tas_step_id = step_id("tas_1");
    std::int64_t tas_job_set_id = 0;
    ASSERT_TRUE(execution_db->CreateJobSet(
        {
            .program_kind = 10,
            .purpose = "TAS Movie",
            .created_by = std::string("test"),
            .expected_total = 1,
        },
        &tas_job_set_id,
        &err)) << err;
    ASSERT_TRUE(execution_db->WorkflowCommandService()->MarkStepMaterialized(
        { .workflow_step_id = tas_step_id, .job_set_id = tas_job_set_id, .requested_by = "test" },
        &err)) << err;
    std::int64_t tas_job_id = 0;
    ASSERT_TRUE(execution_db->EnqueueJob(
        {
            .job_set_id = tas_job_set_id,
            .program_kind = 10,
            .program_version = 1,
            .program_ref_kind = "authoring.tas_spec",
            .program_ref_id = 111,
            .fingerprint = "tas-route-job",
            .priority = 0,
            .max_attempts = 1,
        },
        &tas_job_id,
        &err)) << err;
    ASSERT_TRUE(execution_db->JobCommandService()->AppendLifecycleEvent(
        { .kind = execution::jobs::JobLifecycleEventKind::JobCompleted, .job_id = tas_job_id, .terminal_state = std::string("SUCCEEDED") },
        &err)) << err;
    ASSERT_TRUE(execution_db->RecordJobOutput(
        {
            .job_id = tas_job_id,
            .output_key = "savestate",
            .data_kind = "state.savestate_id",
            .ref_kind = "state.savestate",
            .ref_id = 7001,
            .requested_by = "worker_result_drain",
        },
        &err)) << err;
    ASSERT_TRUE(execution_db->WorkflowCommandService()->MarkStepTerminal(
        { .workflow_step_id = tas_step_id, .terminal_state = "COMPLETED", .requested_by = "test" },
        &err)) << err;

    WorkflowGraphRoutingService router(
        execution_db,
        authoring_db,
        execution_db->WorkflowQueryService(),
        execution_db->WorkflowCommandService());
    WorkflowGraphRoutingResult route_result{};
    ASSERT_TRUE(router.RouteTerminalStep(
        {
            .workflow_instance_id = workflow_instance_id,
            .workflow_step_id = tas_step_id,
            .job_set_id = tas_job_set_id,
            .workflow_kind = "workflow_graph",
            .workflow_graph_revision_id = saved.workflow_graph_revision_id,
            .step_key = "tas_1",
            .graph_node_key = "tas_1",
            .step_kind = "tas_movie",
            .priority = step_priority("tas_1"),
            .expected_total = 1,
            .discovered_total = 1,
            .terminal_total = 1,
            .failed_total = 0,
        },
        &route_result,
        &err)) << err;
    EXPECT_TRUE(route_result.routed_input_binding);
    EXPECT_TRUE(route_result.advanced_ready_step);
    EXPECT_EQ(step_state("probe_1"), WorkflowStepState::Ready);
    EXPECT_EQ(step_priority("probe_1"), 20);
    EXPECT_EQ(step_state("battle_1"), WorkflowStepState::Waiting);

    auto graph = execution_db->WorkflowQueryService()->GetWorkflowGraph(workflow_instance_id);
    ASSERT_TRUE(graph.has_value());
    auto has_binding = [&](const std::string& node_key, const std::string& input_key, std::int64_t ref_id) {
        return std::any_of(
            graph->input_bindings.begin(),
            graph->input_bindings.end(),
            [&](const auto& binding) {
                return binding.node_key == node_key
                    && binding.input_key == input_key
                    && binding.ref_id == ref_id
                    && binding.source_kind == "upstream";
            });
    };
    EXPECT_TRUE(has_binding("probe_1", "entry_savestate", 7001));
    EXPECT_TRUE(has_binding("battle_1", "entry_savestate", 7001));
    EXPECT_FALSE(has_binding("battle_1", "initial_input_frames", 8001));

    const auto probe_step_id = step_id("probe_1");
    std::int64_t probe_job_set_id = 0;
    ASSERT_TRUE(execution_db->CreateJobSet(
        {
            .program_kind = 20,
            .purpose = "Seed Probe",
            .created_by = std::string("test"),
            .expected_total = 1,
        },
        &probe_job_set_id,
        &err)) << err;
    ASSERT_TRUE(execution_db->WorkflowCommandService()->MarkStepMaterialized(
        { .workflow_step_id = probe_step_id, .job_set_id = probe_job_set_id, .requested_by = "test" },
        &err)) << err;
    std::int64_t probe_job_id = 0;
    ASSERT_TRUE(execution_db->EnqueueJob(
        {
            .job_set_id = probe_job_set_id,
            .program_kind = 20,
            .program_version = 1,
            .program_ref_kind = "sp_probe_run",
            .program_ref_id = 222,
            .fingerprint = "probe-route-job",
            .priority = 0,
            .max_attempts = 1,
        },
        &probe_job_id,
        &err)) << err;
    ASSERT_TRUE(execution_db->JobCommandService()->AppendLifecycleEvent(
        { .kind = execution::jobs::JobLifecycleEventKind::JobCompleted, .job_id = probe_job_id, .terminal_state = std::string("SUCCEEDED") },
        &err)) << err;
    ASSERT_TRUE(execution_db->RecordJobOutput(
        {
            .job_id = probe_job_id,
            .output_key = "unique_input_frames",
            .data_kind = "analysis.input_frame_set_id",
            .ref_kind = "an.input_set",
            .ref_id = 8001,
            .requested_by = "worker_result_drain",
        },
        &err)) << err;
    ASSERT_TRUE(execution_db->WorkflowCommandService()->MarkStepTerminal(
        { .workflow_step_id = probe_step_id, .terminal_state = "COMPLETED", .requested_by = "test" },
        &err)) << err;

    route_result = {};
    ASSERT_TRUE(router.RouteTerminalStep(
        {
            .workflow_instance_id = workflow_instance_id,
            .workflow_step_id = probe_step_id,
            .job_set_id = probe_job_set_id,
            .workflow_kind = "workflow_graph",
            .workflow_graph_revision_id = saved.workflow_graph_revision_id,
            .step_key = "probe_1",
            .graph_node_key = "probe_1",
            .step_kind = "seed_probe_chain",
            .priority = step_priority("probe_1"),
            .expected_total = 1,
            .discovered_total = 1,
            .terminal_total = 1,
            .failed_total = 0,
        },
        &route_result,
        &err)) << err;
    EXPECT_TRUE(route_result.routed_input_binding);
    EXPECT_TRUE(route_result.advanced_ready_step);
    EXPECT_EQ(step_state("battle_1"), WorkflowStepState::Ready);
    EXPECT_EQ(step_priority("battle_1"), 30);

    graph = execution_db->WorkflowQueryService()->GetWorkflowGraph(workflow_instance_id);
    ASSERT_TRUE(graph.has_value());
    EXPECT_TRUE(has_binding("battle_1", "initial_input_frames", 8001));

    const auto outputs = execution_db->WorkflowQueryService()->ListStepOutputs(workflow_instance_id);
    EXPECT_EQ(std::count_if(outputs.begin(), outputs.end(), [](const auto& output) {
        return output.graph_node_key == "tas_1" && output.output_key == "savestate";
    }), 1);
    EXPECT_EQ(std::count_if(outputs.begin(), outputs.end(), [](const auto& output) {
        return output.graph_node_key == "probe_1" && output.output_key == "unique_input_frames";
    }), 1);
}

TEST_F(SqliteDbFixture, Stage5GraphRoutingRespectsExternalOverrideInputBinding) {
    using namespace savor::db;
    using namespace savor::db::execution::workflow;

    auto* authoring_db = db_service_->AuthoringDb();
    auto* execution_db = db_service_->ExecutionDb();
    ASSERT_NE(authoring_db, nullptr);
    ASSERT_NE(execution_db, nullptr);

    std::string err;
    const auto now = types::UtcTimePoint(std::chrono::milliseconds(1712304000790));
    SaveWorkflowGraphResult saved{};
    ASSERT_TRUE(authoring_db->SaveWorkflowGraph(
        {
            .name = "route-external-override",
            .description = "Override an authored edge input at launch",
            .graph_version = 1,
            .graph_hash = "graph-hash-route-external-override",
            .nodes = {
                {
                    .node_key = "tas_1",
                    .unit_kind = "tas_movie",
                    .display_name = "TAS Movie",
                    .possible_outputs = {
                        { .output_key = "savestate", .data_kind = "state.savestate_id", .display_name = "Output savestate" },
                    },
                },
                {
                    .node_key = "probe_1",
                    .unit_kind = "seed_probe_chain",
                    .display_name = "Seed Probe Chain",
                    .inputs = {
                        { .input_key = "entry_savestate", .data_kind = "state.savestate_id", .display_name = "Entry savestate" },
                    },
                },
            },
            .edges = {
                { .from_node_key = "tas_1", .output_key = "savestate", .to_node_key = "probe_1", .input_key = "entry_savestate" },
            },
            .created_at_utc = now,
            .correlation_id = "au-workflow-graph-routing-override",
        },
        &saved,
        &err)) << err;

    std::int64_t workflow_instance_id = 0;
    WorkflowCreateInstanceCommand create{};
    create.workflow_kind = "workflow_graph";
    create.root_scope_kind = "manual";
    create.workflow_graph_revision_id = saved.workflow_graph_revision_id;
    create.created_by = "sqlite-fixture";
    create.created_at_utc = now.time_since_epoch().count();
    create.unit_activations.push_back(TestUnitActivation("tas_1", "tas_movie", "TAS Movie", {}, 10, 1));
    create.unit_activations.push_back(TestUnitActivation("probe_1", "seed_probe_chain", "Seed Probe Chain", { "tas_1" }, 5, 1));
    create.input_bindings.push_back({
        .node_key = "probe_1",
        .input_key = "entry_savestate",
        .data_kind = "state.savestate_id",
        .ref_kind = "state.savestate",
        .ref_id = 9901,
        .source_kind = "external_override",
    });
    ASSERT_TRUE(execution_db->WorkflowCommandService()->CreateWorkflowInstance(create, &workflow_instance_id, &err)) << err;

    const auto graph_before = execution_db->WorkflowQueryService()->GetWorkflowGraph(workflow_instance_id);
    ASSERT_TRUE(graph_before.has_value());
    const auto tas_step_it = std::find_if(
        graph_before->steps.begin(),
        graph_before->steps.end(),
        [](const auto& step) { return step.step_key == "tas_1"; });
    ASSERT_NE(tas_step_it, graph_before->steps.end());

    std::int64_t tas_job_set_id = 0;
    ASSERT_TRUE(execution_db->CreateJobSet(
        {
            .program_kind = 10,
            .purpose = "TAS Movie",
            .created_by = std::string("test"),
            .expected_total = 1,
        },
        &tas_job_set_id,
        &err)) << err;
    ASSERT_TRUE(execution_db->WorkflowCommandService()->MarkStepMaterialized(
        { .workflow_step_id = tas_step_it->workflow_step_id, .job_set_id = tas_job_set_id, .requested_by = "test" },
        &err)) << err;
    ASSERT_TRUE(execution_db->WorkflowCommandService()->MarkStepTerminal(
        { .workflow_step_id = tas_step_it->workflow_step_id, .terminal_state = "COMPLETED", .requested_by = "test" },
        &err)) << err;

    WorkflowGraphRoutingService router(
        execution_db,
        authoring_db,
        execution_db->WorkflowQueryService(),
        execution_db->WorkflowCommandService());
    WorkflowGraphRoutingResult route_result{};
    ASSERT_TRUE(router.RouteTerminalStep(
        {
            .workflow_instance_id = workflow_instance_id,
            .workflow_step_id = tas_step_it->workflow_step_id,
            .job_set_id = tas_job_set_id,
            .workflow_kind = "workflow_graph",
            .workflow_graph_revision_id = saved.workflow_graph_revision_id,
            .step_key = "tas_1",
            .graph_node_key = "tas_1",
            .step_kind = "tas_movie",
            .expected_total = 1,
            .discovered_total = 1,
            .terminal_total = 1,
            .failed_total = 0,
        },
        &route_result,
        &err)) << err;
    EXPECT_FALSE(route_result.routed_input_binding);
    EXPECT_TRUE(route_result.advanced_ready_step);

    const auto graph_after = execution_db->WorkflowQueryService()->GetWorkflowGraph(workflow_instance_id);
    ASSERT_TRUE(graph_after.has_value());
    const auto probe_step_it = std::find_if(
        graph_after->steps.begin(),
        graph_after->steps.end(),
        [](const auto& step) { return step.step_key == "probe_1"; });
    ASSERT_NE(probe_step_it, graph_after->steps.end());
    EXPECT_EQ(probe_step_it->state, WorkflowStepState::Ready);

    const auto override_count = std::count_if(
        graph_after->input_bindings.begin(),
        graph_after->input_bindings.end(),
        [](const auto& binding) {
            return binding.node_key == "probe_1"
                && binding.input_key == "entry_savestate"
                && binding.ref_id == 9901
                && binding.source_kind == "external_override";
        });
    EXPECT_EQ(override_count, 1);
}

TEST_F(SqliteDbFixture, Stage5CoordinatorMaterializesSeedProbeGraphNodeFromInstanceInputBinding) {
    using namespace savor::db;
    using namespace savor::db::execution::programdb;
    using namespace savor::db::execution::programdb::seedprobe;
    using namespace savor::db::execution::workflow;
    using namespace savor::runner::parallel::savordb;

    auto* authoring_db = db_service_->AuthoringDb();
    auto* analysis_db = db_service_->AnalysisDb();
    ASSERT_NE(authoring_db, nullptr);
    ASSERT_NE(analysis_db, nullptr);

    std::string err;
    std::int64_t seed_probe_spec_id = 0;
    ASSERT_TRUE(authoring_db->SaveSeedProbeSpec(
        {
            .name = "graph seed probe spec",
            .priority = 1,
            .run_ms = 12000,
            .vi_stall_ms = 500,
            .min_value = 80,
            .max_value = 180,
            .cap_trigger_top = true,
            .ignore_trigger_minmax = true,
            .combo_attempts_per_target = 1,
            .combo_sampler_tries = 1,
            .auto_schedule_battle_run = false,
            .created_at_utc = types::UtcNow(),
            .correlation_id = "test.graph-materialize",
            .causation_id = "test",
        },
        &seed_probe_spec_id,
        &err))
        << err;

    SaveWorkflowGraphResult saved{};
    ASSERT_TRUE(authoring_db->SaveWorkflowGraph(
        {
            .name = "graph-materialize-seedprobe",
            .description = "Seed probe chain launch graph",
            .graph_version = 1,
            .graph_hash = "graph-materialize-seedprobe-hash",
            .nodes = {
                {
                    .node_key = "probe_1",
                    .unit_kind = "seed_probe_chain",
                    .display_name = "Seed Probe Chain",
                    .authored_ref_kind = std::string("seed_probe_spec"),
                    .authored_ref_id = seed_probe_spec_id,
                    .inputs = {
                        { .input_key = "entry_savestate", .data_kind = "state.savestate_id", .display_name = "Entry savestate" },
                    },
                    .possible_outputs = {
                        { .output_key = "unique_input_frames", .data_kind = "analysis.input_frame_set_id", .display_name = "Unique input frames" },
                    },
                },
            },
            .created_at_utc = types::UtcNow(),
            .correlation_id = "test.graph-materialize",
        },
        &saved,
        &err))
        << err;

    auto* execution_db = db_service_->ExecutionDb();
    ASSERT_NE(execution_db, nullptr);
    std::int64_t workflow_instance_id = 0;
    WorkflowCreateInstanceCommand command{};
    command.workflow_kind = "workflow_graph";
    command.root_scope_kind = "manual";
    command.workflow_graph_revision_id = saved.workflow_graph_revision_id;
    command.created_by = "sqlite-fixture";
    command.created_at_utc = types::UtcNow().time_since_epoch().count();
    command.unit_activations.push_back(TestUnitActivation("probe_1", "seed_probe_chain", "Seed Probe Chain", {}, 10, 1));
    command.input_bindings.push_back({
        .node_key = "probe_1",
        .input_key = "entry_savestate",
        .data_kind = "state.savestate_id",
        .ref_kind = "state.savestate",
        .ref_id = 44001,
        .source_kind = "external",
    });
    command.arguments.push_back({
        .node_key = "probe_1",
        .argument_key = "samples_per_axis",
        .value_type = "integer",
        .integer_value = 1,
        .source_kind = "test",
    });
    ASSERT_TRUE(execution_db->WorkflowCommandService()->CreateWorkflowInstance(command, &workflow_instance_id, &err)) << err;

    const auto graph = execution_db->WorkflowQueryService()->GetWorkflowGraph(workflow_instance_id);
    ASSERT_TRUE(graph.has_value());
    ASSERT_EQ(graph->steps.size(), 1u);

    ProgramKindRegistry registry;
    SeedProbePhaseRegistrationConfig config{};
    config.authoring_db = authoring_db;
    RegisterSeedProbePhaseDescriptors(&registry, execution_db, analysis_db, config);

    DBWorkflowWorkerCoordinator coordinator(
        execution_db,
        DBWorkflowWorkerCoordinatorConfig{},
        CoordinatorIntegrationConfig{ .workflow_enabled = true },
        &registry);

    const auto scheduled = coordinator.MaterializeWorkflowStep(
        WorkflowReadyStep{
            .workflow_instance_id = workflow_instance_id,
            .workflow_step_id = graph->steps.front().workflow_step_id,
            .step_key = "probe_1",
            .step_kind = "seed_probe_chain",
            .priority = 10,
        });
    ASSERT_TRUE(scheduled.has_value());
    EXPECT_GT(scheduled->job_set_id, 0);

    sqlite3_stmt* st = nullptr;
    ASSERT_EQ(SQLITE_OK, sqlite3_prepare_v2(
        db_,
        "SELECT probe_run_id, probe_set_id, entry_savestate_id, seed_probe_spec_id, launch_samples_per_axis "
        "FROM sp_probe_run WHERE entry_savestate_id=?1 AND seed_probe_spec_id=?2 LIMIT 1;",
        -1,
        &st,
        nullptr));
    sqlite3_bind_int64(st, 1, 44001);
    sqlite3_bind_int64(st, 2, seed_probe_spec_id);
    ASSERT_EQ(SQLITE_ROW, sqlite3_step(st));
    const auto probe_run_id = sqlite3_column_int64(st, 0);
    EXPECT_GT(sqlite3_column_int64(st, 1), 0);
    EXPECT_EQ(sqlite3_column_int64(st, 2), 44001);
    EXPECT_EQ(sqlite3_column_int64(st, 3), seed_probe_spec_id);
    EXPECT_EQ(sqlite3_column_int(st, 4), 1);
    sqlite3_finalize(st);

    ASSERT_EQ(SQLITE_OK, sqlite3_prepare_v2(
        db_,
        "SELECT program_ref_kind, program_ref_id FROM exec_job WHERE job_set_id=?1 ORDER BY job_id LIMIT 1;",
        -1,
        &st,
        nullptr));
    sqlite3_bind_int64(st, 1, scheduled->job_set_id);
    ASSERT_EQ(SQLITE_ROW, sqlite3_step(st));
    ASSERT_NE(sqlite3_column_text(st, 0), nullptr);
    EXPECT_EQ(std::string(reinterpret_cast<const char*>(sqlite3_column_text(st, 0))), "seed_probe_run");
    EXPECT_EQ(sqlite3_column_int64(st, 1), probe_run_id);
    sqlite3_finalize(st);

    JobMaterializationService materializer(execution_db, &registry);
    const auto claimed = materializer.ClaimJobs(1, std::chrono::steady_clock::now());
    ASSERT_EQ(claimed, 1u);
    ASSERT_TRUE(materializer.MaterializeClaimedJobPayload(std::chrono::steady_clock::now()));
    const auto materialized = materializer.ListByState(ClaimedJobLifecycleState::Materialized);
    ASSERT_EQ(materialized.size(), 1u);
    EXPECT_EQ(materialized.front().step.step_kind, "seed_probe_chain");
    EXPECT_EQ(materialized.front().runtime_init.bootstrap_profile, "seedprobe.neutral.required_savestate");
    EXPECT_EQ(materialized.front().runtime_init.savestate_ref_id, 44001);
    EXPECT_TRUE(materialized.front().payload.has_value());
}

TEST_F(SqliteDbFixture, Stage5CoordinatorMaterializesTasMovieGraphNodesWithTasSpecScopedDedupe) {
    using namespace savor::db;
    using namespace savor::db::execution::programdb;
    using namespace savor::db::execution::programdb::tasmovie;
    using namespace savor::db::execution::workflow;
    using namespace savor::runner::parallel::savordb;

    auto* authoring_db = db_service_->AuthoringDb();
    auto* state_db = db_service_->StateDb();
    auto* analysis_db = db_service_->AnalysisDb();
    auto* execution_db = db_service_->ExecutionDb();
    ASSERT_NE(authoring_db, nullptr);
    ASSERT_NE(state_db, nullptr);
    ASSERT_NE(analysis_db, nullptr);
    ASSERT_NE(execution_db, nullptr);

    const auto source_path = temp_root_ / "graph-dedupe-base.dtm";
    {
        std::ofstream out(source_path, std::ios::binary);
        out << "dtm-bytes";
    }

    std::string err;
    std::int64_t base_artifact_id = 0;
    ASSERT_TRUE(state_db->StoreArtifact(
        {
            .sha256 = "stage5-tasmovie-graph-dedupe-base-dtm",
            .size_bytes = static_cast<std::int64_t>(std::filesystem::file_size(source_path)),
            .compression_kind = 0,
            .filename = source_path.string(),
            .file_ext = ".dtm",
            .artifact_kind = "DTM",
            .created_at_utc = types::UtcNow(),
            .correlation_id = "test.tasmovie.graph-dedupe",
            .causation_id = "test",
        },
        &base_artifact_id,
        &err))
        << err;

    auto save_tas_spec = [&](std::string_view suffix, std::int64_t run_ms) {
        const auto base_name = std::string("graph tas spec ") + std::string(suffix);
        std::int64_t tas_spec_id = 0;
        EXPECT_TRUE(authoring_db->SaveTasSpec(
            {
                .base_name = base_name,
                .priority = 1,
                .run_ms = run_ms,
                .vi_stall_ms = 2500,
                .progress_enable = true,
                .base_dtm_artifact_id = base_artifact_id,
                .created_at_utc = types::UtcNow(),
                .correlation_id = "test.tasmovie.graph-dedupe",
                .causation_id = "test",
            },
            &tas_spec_id,
            nullptr,
            &err))
            << err;
        if (tas_spec_id > 0) {
            return tas_spec_id;
        }
        for (const auto& spec : authoring_db->ListTasSpecs(10)) {
            if (spec.base_name == base_name && spec.run_ms == run_ms) {
                return spec.tas_spec_id;
            }
        }
        return std::int64_t{0};
    };

    const auto first_tas_spec_id = save_tas_spec("first", 60000);
    const auto second_tas_spec_id = save_tas_spec("second", 90000);
    ASSERT_GT(first_tas_spec_id, 0);
    ASSERT_GT(second_tas_spec_id, 0);
    ASSERT_NE(first_tas_spec_id, second_tas_spec_id);

    ProgramKindRegistry registry;
    TasMoviePhaseRegistrationConfig config{};
    config.authoring_db = authoring_db;
    config.working_dir_root = temp_root_ / "tasmovie";
    RegisterTasMoviePhaseDescriptor(&registry, execution_db, state_db, analysis_db, config);

    DBWorkflowWorkerCoordinator coordinator(
        execution_db,
        DBWorkflowWorkerCoordinatorConfig{},
        CoordinatorIntegrationConfig{ .workflow_enabled = true },
        &registry);

    auto materialize_for_spec = [&](std::int64_t tas_spec_id, std::string_view suffix) {
        SaveWorkflowGraphResult saved{};
        if (!authoring_db->SaveWorkflowGraph(
            {
                .name = std::string("graph-materialize-tasmovie-") + std::string(suffix),
                .description = "TasMovie launch graph",
                .graph_version = 1,
                .graph_hash = std::string("graph-materialize-tasmovie-hash-") + std::string(suffix),
                .nodes = {
                    {
                        .node_key = "tas_movie_standalone",
                        .unit_kind = "tas_movie",
                        .display_name = "Tas Movie",
                        .authored_ref_kind = std::string("tas_spec"),
                        .authored_ref_id = tas_spec_id,
                        .inputs = {
                            { .input_key = "dtm_artifact", .data_kind = "state_artifact.dtm_artifact_id", .display_name = "DTM artifact" },
                        },
                    },
                },
                .created_at_utc = types::UtcNow(),
                .correlation_id = "test.tasmovie.graph-dedupe",
            },
            &saved,
            &err)) {
            ADD_FAILURE() << err;
            return std::int64_t{0};
        }
        const auto authoring_graph = authoring_db->GetWorkflowGraphRevision(saved.workflow_graph_revision_id);
        if (!authoring_graph.has_value()
            || authoring_graph->nodes.empty()
            || authoring_graph->nodes.front().authored_ref_kind.value_or("") != "tas_spec"
            || authoring_graph->nodes.front().authored_ref_id.value_or(0) != tas_spec_id) {
            ADD_FAILURE() << "saved workflow graph did not preserve tas_spec authored ref";
            return std::int64_t{0};
        }

        std::int64_t workflow_instance_id = 0;
        WorkflowCreateInstanceCommand command{};
        command.workflow_kind = "workflow_graph";
        command.root_scope_kind = "manual";
        command.workflow_graph_revision_id = saved.workflow_graph_revision_id;
        command.created_by = "sqlite-fixture";
        command.created_at_utc = types::UtcNow().time_since_epoch().count();
        command.unit_activations.push_back(TestUnitActivation("tas_movie_standalone", "tas_movie", "TAS Movie", {}, 10, 1));
        command.input_bindings.push_back({
            .node_key = "tas_movie_standalone",
            .input_key = "dtm_artifact",
            .data_kind = "state_artifact.dtm_artifact_id",
            .ref_kind = "state_artifact",
            .ref_id = base_artifact_id,
            .source_kind = "external",
        });
        command.arguments.push_back({
            .node_key = "tas_movie_standalone",
            .argument_key = "rtc",
            .value_type = "integer",
            .integer_value = 0,
            .source_kind = "launcher",
        });
        command.arguments.push_back({
            .node_key = "tas_movie_standalone",
            .argument_key = "headroom",
            .value_type = "integer",
            .integer_value = 50,
            .source_kind = "launcher",
        });
        if (!execution_db->WorkflowCommandService()->CreateWorkflowInstance(command, &workflow_instance_id, &err)) {
            ADD_FAILURE() << err;
            return std::int64_t{0};
        }

        const auto graph = execution_db->WorkflowQueryService()->GetWorkflowGraph(workflow_instance_id);
        if (!graph.has_value()) {
            ADD_FAILURE() << "workflow graph not found";
            return std::int64_t{0};
        }
        if (graph->steps.size() != 1u) {
            ADD_FAILURE() << "expected exactly one workflow step";
            return std::int64_t{0};
        }
        if (graph->instance.workflow_graph_revision_id.value_or(0) != saved.workflow_graph_revision_id) {
            ADD_FAILURE() << "workflow instance revision mismatch";
            return std::int64_t{0};
        }

        const auto scheduled = coordinator.MaterializeWorkflowStep(
            WorkflowReadyStep{
                .workflow_instance_id = workflow_instance_id,
                .workflow_step_id = graph->steps.front().workflow_step_id,
                .step_key = "tas_movie_standalone",
                .step_kind = "tas_movie",
                .priority = 10,
            });
        if (!scheduled.has_value()) {
            ADD_FAILURE() << "TasMovie step did not materialize";
            return std::int64_t{0};
        }
        if (scheduled->job_set_id <= 0) {
            ADD_FAILURE() << "TasMovie step materialized without a job set";
            return std::int64_t{0};
        }
        const auto expected_trace = "tas_spec_id=" + std::to_string(tas_spec_id);
        const auto has_expected_trace = std::any_of(
            scheduled->event_lines.begin(),
            scheduled->event_lines.end(),
            [&](const std::string& line) {
                return line.find(expected_trace) != std::string::npos;
            });
        if (!has_expected_trace) {
            std::ostringstream trace;
            for (const auto& line : scheduled->event_lines) {
                trace << "\n" << line;
            }
            ADD_FAILURE() << "TasMovie materialization trace missing " << expected_trace
                << " for tas_spec_id=" << tas_spec_id
                << trace.str();
            return std::int64_t{0};
        }
        return scheduled->job_set_id;
    };

    const auto first_job_set_id = materialize_for_spec(first_tas_spec_id, "first");
    const auto second_job_set_id = materialize_for_spec(second_tas_spec_id, "second");
    ASSERT_GT(first_job_set_id, 0);
    ASSERT_GT(second_job_set_id, 0);
    ASSERT_NE(first_job_set_id, second_job_set_id);

    sqlite3_stmt* st = nullptr;
    ASSERT_EQ(SQLITE_OK, sqlite3_prepare_v2(
        db_,
        "SELECT COUNT(1) FROM state_tas_movie_variant "
        "WHERE base_dtm_artifact_id=?1 AND rtc_value=0;",
        -1,
        &st,
        nullptr));
    sqlite3_bind_int64(st, 1, base_artifact_id);
    ASSERT_EQ(SQLITE_ROW, sqlite3_step(st));
    EXPECT_EQ(sqlite3_column_int64(st, 0), 2);
    sqlite3_finalize(st);

    ASSERT_EQ(SQLITE_OK, sqlite3_prepare_v2(
        db_,
        "SELECT COUNT(1) FROM exec_job "
        "WHERE program_kind=?1 AND job_set_id IN (?2,?3);",
        -1,
        &st,
        nullptr));
    sqlite3_bind_int(st, 1, static_cast<int>(savor::PK_TasMovie));
    sqlite3_bind_int64(st, 2, first_job_set_id);
    sqlite3_bind_int64(st, 3, second_job_set_id);
    ASSERT_EQ(SQLITE_ROW, sqlite3_step(st));
    EXPECT_EQ(sqlite3_column_int64(st, 0), 2);
    sqlite3_finalize(st);

    ASSERT_EQ(SQLITE_OK, sqlite3_prepare_v2(
        db_,
        "SELECT COUNT(1) FROM exec_job "
        "WHERE fingerprint LIKE ?1 OR fingerprint LIKE ?2;",
        -1,
        &st,
        nullptr));
    const auto first_like = "%;tas_spec_id=" + std::to_string(first_tas_spec_id) + ";%";
    const auto second_like = "%;tas_spec_id=" + std::to_string(second_tas_spec_id) + ";%";
    sqlite3_bind_text(st, 1, first_like.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, second_like.c_str(), -1, SQLITE_TRANSIENT);
    ASSERT_EQ(SQLITE_ROW, sqlite3_step(st));
    EXPECT_EQ(sqlite3_column_int64(st, 0), 2);
    sqlite3_finalize(st);
}

TEST_F(SqliteDbFixture, Stage5CoordinatorBlocksTasMovieGraphNodeWithoutRtcArgument) {
    using namespace savor::db;
    using namespace savor::db::execution::programdb;
    using namespace savor::db::execution::programdb::tasmovie;
    using namespace savor::db::execution::workflow;
    using namespace savor::runner::parallel::savordb;

    auto* authoring_db = db_service_->AuthoringDb();
    auto* state_db = db_service_->StateDb();
    auto* analysis_db = db_service_->AnalysisDb();
    auto* execution_db = db_service_->ExecutionDb();
    ASSERT_NE(authoring_db, nullptr);
    ASSERT_NE(state_db, nullptr);
    ASSERT_NE(analysis_db, nullptr);
    ASSERT_NE(execution_db, nullptr);

    const auto source_path = temp_root_ / "graph-missing-rtc-base.dtm";
    {
        std::ofstream out(source_path, std::ios::binary);
        out << "dtm-bytes";
    }

    std::string err;
    std::int64_t base_artifact_id = 0;
    ASSERT_TRUE(state_db->StoreArtifact(
        {
            .sha256 = "stage5-tasmovie-graph-missing-rtc-base-dtm",
            .size_bytes = static_cast<std::int64_t>(std::filesystem::file_size(source_path)),
            .compression_kind = 0,
            .filename = source_path.string(),
            .file_ext = ".dtm",
            .artifact_kind = "DTM",
            .created_at_utc = types::UtcNow(),
            .correlation_id = "test.tasmovie.missing-rtc",
            .causation_id = "test",
        },
        &base_artifact_id,
        &err))
        << err;

    SaveWorkflowGraphResult saved{};
    ASSERT_TRUE(authoring_db->SaveWorkflowGraph(
        {
            .name = "graph-materialize-tasmovie-missing-rtc",
            .description = "TasMovie launch graph missing required rtc argument",
            .graph_version = 1,
            .graph_hash = "graph-materialize-tasmovie-missing-rtc-hash",
            .nodes = {
                {
                    .node_key = "tas_movie_standalone",
                    .unit_kind = "tas_movie",
                    .display_name = "Tas Movie",
                    .inputs = {
                        { .input_key = "dtm_artifact", .data_kind = "state_artifact.dtm_artifact_id", .display_name = "DTM artifact" },
                    },
                },
            },
            .created_at_utc = types::UtcNow(),
            .correlation_id = "test.tasmovie.missing-rtc",
        },
        &saved,
        &err))
        << err;

    std::int64_t workflow_instance_id = 0;
    WorkflowCreateInstanceCommand command{};
    command.workflow_kind = "workflow_graph";
    command.root_scope_kind = "manual";
    command.workflow_graph_revision_id = saved.workflow_graph_revision_id;
    command.created_by = "sqlite-fixture";
    command.created_at_utc = types::UtcNow().time_since_epoch().count();
    command.unit_activations.push_back(TestUnitActivation("tas_movie_standalone", "tas_movie", "TAS Movie", {}, 10, 1));
    command.input_bindings.push_back({
        .node_key = "tas_movie_standalone",
        .input_key = "dtm_artifact",
        .data_kind = "state_artifact.dtm_artifact_id",
        .ref_kind = "state_artifact",
        .ref_id = base_artifact_id,
        .source_kind = "external",
    });
    ASSERT_TRUE(execution_db->WorkflowCommandService()->CreateWorkflowInstance(command, &workflow_instance_id, &err))
        << err;

    const auto graph = execution_db->WorkflowQueryService()->GetWorkflowGraph(workflow_instance_id);
    ASSERT_TRUE(graph.has_value());
    ASSERT_EQ(graph->steps.size(), 1u);

    ProgramKindRegistry registry;
    TasMoviePhaseRegistrationConfig config{};
    config.authoring_db = authoring_db;
    config.working_dir_root = temp_root_ / "tasmovie-missing-rtc";
    RegisterTasMoviePhaseDescriptor(&registry, execution_db, state_db, analysis_db, config);

    DBWorkflowWorkerCoordinator coordinator(
        execution_db,
        DBWorkflowWorkerCoordinatorConfig{},
        CoordinatorIntegrationConfig{ .workflow_enabled = true },
        &registry);

    const auto scheduled = coordinator.MaterializeWorkflowStep(
        WorkflowReadyStep{
            .workflow_instance_id = workflow_instance_id,
            .workflow_step_id = graph->steps.front().workflow_step_id,
            .step_key = "tas_movie_standalone",
            .step_kind = "tas_movie",
            .priority = 10,
        });
    EXPECT_FALSE(scheduled.has_value());

    sqlite3_stmt* st = nullptr;
    ASSERT_EQ(SQLITE_OK, sqlite3_prepare_v2(
        db_,
        "SELECT COUNT(1) FROM exec_job WHERE program_kind=?1;",
        -1,
        &st,
        nullptr));
    sqlite3_bind_int(st, 1, static_cast<int>(savor::PK_TasMovie));
    ASSERT_EQ(SQLITE_ROW, sqlite3_step(st));
    EXPECT_EQ(sqlite3_column_int64(st, 0), 0);
    sqlite3_finalize(st);

    ASSERT_EQ(SQLITE_OK, sqlite3_prepare_v2(
        db_,
        "SELECT COUNT(1) FROM state_tas_movie_variant WHERE base_dtm_artifact_id=?1;",
        -1,
        &st,
        nullptr));
    sqlite3_bind_int64(st, 1, base_artifact_id);
    ASSERT_EQ(SQLITE_ROW, sqlite3_step(st));
    EXPECT_EQ(sqlite3_column_int64(st, 0), 0);
    sqlite3_finalize(st);
}

TEST_F(SqliteDbFixture, Stage3dAnalysisSeedProbeSetCreateEmitsEventTwentyThree) {
    using namespace savor::db;
    using namespace savor::db::analysis;
    using namespace savor::db::migrations;

    const MigrationSourceOptions embedded_options{ .source_kind = MigrationSourceKind::Embedded };
    std::string err;
    ASSERT_TRUE(ApplyContextMigrations(db_, MigrationContext::AnalysisSeedProbe, embedded_options, &err)) << err;

    SqliteAnalysisDb analysis_db(db_);
    const auto now = types::UtcTimePoint(std::chrono::milliseconds(1712304000000));

    std::int64_t probe_set_id = 0;
    ASSERT_TRUE(analysis_db.CreateSeedProbeSet(
        {
            .name = "stage3d-probe-set",
            .probe_flavor = "BATTLE_PRE",
            .breakpoint_policy_name = "bp-default",
            .segment_source_kind = "FILE",
            .created_at_utc = now,
            .correlation_id = "sp-corr-1",
            .causation_id = "sp-cause-1",
        },
        &probe_set_id,
        &err))
        << err;
    EXPECT_GT(probe_set_id, 0);

    sqlite3_stmt* st = nullptr;
    ASSERT_EQ(SQLITE_OK, sqlite3_prepare_v2(db_, "SELECT COUNT(1) FROM sp_outbox_message WHERE event_type='AnalysisSeedProbe.SetCreated.v1';", -1, &st, nullptr));
    ASSERT_EQ(SQLITE_ROW, sqlite3_step(st));
    EXPECT_EQ(sqlite3_column_int(st, 0), 1);
    sqlite3_finalize(st);

    const auto payload = analysis_db.ResolveSeedProbePayload(1, "probe_set", probe_set_id);
    ASSERT_TRUE(payload.has_value());
    EXPECT_EQ(payload->probe_set_id, probe_set_id);
}

TEST_F(SqliteDbFixture, Stage3dArchiveCommandsEmitEventsFortyFourThroughFortyEight) {
    using namespace savor::db;
    using namespace savor::db::migrations;

    const MigrationSourceOptions embedded_options{ .source_kind = MigrationSourceKind::Embedded };
    std::string err;
    ASSERT_TRUE(ApplyContextMigrations(db_, MigrationContext::Archive, embedded_options, &err)) << err;

    SqliteArchiveDb archive_db(db_);
    const auto now = types::UtcTimePoint(std::chrono::milliseconds(1712304000000));

    std::int64_t archive_package_id = 0;
    ASSERT_TRUE(archive_db.CreateArchivePackage(
        {
            .source_context = "Execution",
            .source_root_job_set_id = 9001,
            .archive_name = "manual archive event test",
            .archive_notes = std::string("event notes"),
            .created_at_utc = now,
            .schema_version = 1,
            .event_catalog_version = 1,
            .time_range_start_utc = now,
            .time_range_end_utc = now,
            .manifest_path = "manifest.json",
            .checksum_status = "PENDING",
            .correlation_id = "ar-corr-1",
            .causation_id = "ar-cause-1",
        },
        &archive_package_id,
        &err))
        << err;
    ASSERT_GT(archive_package_id, 0);
    EXPECT_EQ(ReadText(db_, "SELECT archive_name FROM ar_archive_package WHERE archive_package_id=(SELECT MAX(archive_package_id) FROM ar_archive_package);"), "manual archive event test");
    EXPECT_EQ(ReadText(db_, "SELECT archive_notes FROM ar_archive_package WHERE archive_package_id=(SELECT MAX(archive_package_id) FROM ar_archive_package);"), "event notes");

    std::int64_t archive_item_id = 0;
    ASSERT_TRUE(archive_db.AddArchiveItem(
        {
            .archive_package_id = archive_package_id,
            .item_kind = "exec_job_event",
            .item_count = 4,
            .blob_path = std::string("jobs.jsonl"),
            .checksum = std::string("abc123"),
            .indexed_at_utc = now,
            .correlation_id = "ar-corr-1",
            .causation_id = "ar-cause-2",
        },
        &archive_item_id,
        &err))
        << err;
    ASSERT_GT(archive_item_id, 0);

    std::int64_t rehydrate_request_id = 0;
    ASSERT_TRUE(archive_db.RequestRehydrate(
        {
            .archive_package_id = archive_package_id,
            .status = "REQUESTED",
            .requested_at_utc = now,
            .target_namespace = "test",
            .correlation_id = "ar-corr-1",
            .causation_id = "ar-cause-3",
        },
        &rehydrate_request_id,
        &err))
        << err;

    ASSERT_TRUE(archive_db.CompleteRehydrate(
        {
            .rehydrate_request_id = rehydrate_request_id,
            .status = "COMPLETED",
            .completed_at_utc = now,
            .entity_mappings = {
                { .entity_kind = "job", .old_id = "12", .new_id = "1012" },
            },
            .correlation_id = "ar-corr-1",
            .causation_id = "ar-cause-4",
        },
        &err))
        << err;

    std::int64_t failed_request_id = 0;
    ASSERT_TRUE(archive_db.RequestRehydrate(
        {
            .archive_package_id = archive_package_id,
            .status = "REQUESTED",
            .requested_at_utc = now,
            .target_namespace = "test",
            .correlation_id = "ar-corr-1",
            .causation_id = "ar-cause-5",
        },
        &failed_request_id,
        &err))
        << err;
    ASSERT_TRUE(archive_db.FailRehydrate(
        {
            .rehydrate_request_id = failed_request_id,
            .status = "FAILED",
            .completed_at_utc = now,
            .error_text = "simulated failure",
            .correlation_id = "ar-corr-1",
            .causation_id = "ar-cause-6",
        },
        &err))
        << err;

    sqlite3_stmt* st = nullptr;
    ASSERT_EQ(SQLITE_OK, sqlite3_prepare_v2(db_, "SELECT COUNT(1) FROM ar_outbox_message;", -1, &st, nullptr));
    ASSERT_EQ(SQLITE_ROW, sqlite3_step(st));
    EXPECT_EQ(sqlite3_column_int(st, 0), 6);
    sqlite3_finalize(st);

    const auto payload_package = archive_db.ResolveArchivePayload(1, "archive_package", archive_package_id);
    ASSERT_TRUE(payload_package.has_value());
    EXPECT_EQ(payload_package->archive_package_id, archive_package_id);

    const auto payload_item = archive_db.ResolveArchivePayload(1, "archive_item", archive_item_id);
    ASSERT_TRUE(payload_item.has_value());
    EXPECT_EQ(payload_item->archive_item_id, archive_item_id);
}

TEST_F(SqliteDbFixture, Stage4ArchiveOperatorCommandsPackageCountsAndChecksumValidation) {
    using namespace savor::db;
    using namespace savor::db::migrations;
    using namespace savor::runner::parallel::savordb;

    const MigrationSourceOptions embedded_options{ .source_kind = MigrationSourceKind::Embedded };
    std::string err;
    ASSERT_TRUE(ApplyContextMigrations(db_, MigrationContext::Execution, embedded_options, &err)) << err;
    ASSERT_TRUE(ApplyContextMigrations(db_, MigrationContext::UIRead, embedded_options, &err)) << err;
    ASSERT_TRUE(ApplyContextMigrations(db_, MigrationContext::Archive, embedded_options, &err)) << err;

    ASSERT_TRUE(ExecSql(db_, R"SQL(
INSERT INTO exec_job_set(job_set_id, program_kind, purpose, created_at_utc) VALUES(100,1,'root',1000);
INSERT INTO exec_job(job_id, job_set_id, program_kind, program_version, program_ref_kind, program_ref_id, fingerprint, priority, state, attempts, max_attempts, queued_at_utc, ended_at_utc)
VALUES(200,100,1,1,'workflow',100,'fp-200',0,'COMPLETED',0,1,1000,2000);
INSERT INTO exec_job_event(job_event_id, job_id, event_kind, event_ts_utc, message) VALUES(300,200,'done',2000,'ok');
)SQL"));

    const auto temp_root = std::filesystem::temp_directory_path() / ("savor-archive-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    ASSERT_TRUE(std::filesystem::create_directories(temp_root));

    savor::db::execution::workflow::SqliteExecutionDb execution_db(db_);
    SqliteUiReadDb ui_read_db(db_);
    SqliteArchiveDb archive_db(db_);
    savor::db::archive::SqliteArchivePackageService package_service(
        db_,
        &execution_db,
        &ui_read_db,
        &archive_db,
        DbConfigPaths{ .archive_store_root = temp_root });

    const auto now = types::UtcTimePoint(std::chrono::milliseconds(1712304000000));
    const auto package = package_service.CreatePackage({
        .source_root_job_set_id = 100,
        .created_at_utc = now,
        .correlation_id = "stage4-corr",
        .causation_id = "stage4-cause",
    });
    ASSERT_TRUE(package.success) << package.error.value_or("unknown error");

    savor::db::archive::SqliteRehydrateExecutor rehydrate_executor(db_, db_, &archive_db, temp_root);
    ArchiveWorkflowCommands commands(db_, &archive_db, &package_service, &rehydrate_executor);

    const auto verify_pass = commands.PackageVerify({ .archive_package_id = package.archive_package_id });
    EXPECT_TRUE(verify_pass.success);
    EXPECT_GT(verify_pass.manifest_row_total, 0);
    EXPECT_EQ(verify_pass.manifest_row_total, verify_pass.archive_item_row_total);

    const auto jobs_file = package.package_root / "data" / "jobs.jsonl";
    ASSERT_TRUE(std::filesystem::exists(jobs_file));
    {
        std::ofstream out(jobs_file, std::ios::out | std::ios::trunc);
        out << "{\"job_id\":200}\n";
    }

    const auto verify_fail = commands.PackageVerify({ .archive_package_id = package.archive_package_id });
    EXPECT_FALSE(verify_fail.success);
    EXPECT_FALSE(verify_fail.blocking_reasons.empty());
    EXPECT_NE(std::find_if(
        verify_fail.blocking_reasons.begin(),
        verify_fail.blocking_reasons.end(),
        [](const std::string& r) { return r.find("checksum_mismatch") != std::string::npos; }),
        verify_fail.blocking_reasons.end());

    std::filesystem::remove_all(temp_root);
}

TEST_F(SqliteDbFixture, Stage4ArchiveOperatorCommandsRehydrateNoCollisionAndRoundtripAndCleanup) {
    using namespace savor::db;
    using namespace savor::db::migrations;
    using namespace savor::runner::parallel::savordb;

    const MigrationSourceOptions embedded_options{ .source_kind = MigrationSourceKind::Embedded };
    std::string err;
    ASSERT_TRUE(ApplyContextMigrations(db_, MigrationContext::Execution, embedded_options, &err)) << err;
    ASSERT_TRUE(ApplyContextMigrations(db_, MigrationContext::UIRead, embedded_options, &err)) << err;
    ASSERT_TRUE(ApplyContextMigrations(db_, MigrationContext::Archive, embedded_options, &err)) << err;

    ASSERT_TRUE(ExecSql(db_, R"SQL(
INSERT INTO exec_job_set(job_set_id, program_kind, purpose, created_at_utc) VALUES(100,1,'root',1000);
INSERT INTO exec_job(job_id, job_set_id, program_kind, program_version, program_ref_kind, program_ref_id, fingerprint, priority, state, attempts, max_attempts, queued_at_utc, ended_at_utc)
VALUES(200,100,1,1,'workflow',100,'fp-200',0,'COMPLETED',0,1,1000,2000);
INSERT INTO exec_job_event(job_event_id, job_id, event_kind, event_ts_utc, message) VALUES(300,200,'done',2000,'ok');
)SQL"));

    const auto temp_root = std::filesystem::temp_directory_path() / ("savor-rehydrate-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    ASSERT_TRUE(std::filesystem::create_directories(temp_root));

    savor::db::execution::workflow::SqliteExecutionDb execution_db(db_);
    SqliteUiReadDb ui_read_db(db_);
    SqliteArchiveDb archive_db(db_);
    savor::db::archive::SqliteArchivePackageService package_service(
        db_,
        &execution_db,
        &ui_read_db,
        &archive_db,
        DbConfigPaths{ .archive_store_root = temp_root });

    const auto now = types::UtcTimePoint(std::chrono::milliseconds(1712304000000));
    const auto package = package_service.CreatePackage({
        .source_root_job_set_id = 100,
        .created_at_utc = now,
        .correlation_id = "stage4-roundtrip",
        .causation_id = "stage4-roundtrip",
    });
    ASSERT_TRUE(package.success) << package.error.value_or("unknown error");

    ASSERT_TRUE(ExecSql(
        db_,
        "INSERT INTO exec_job(job_id, job_set_id, program_kind, program_version, program_ref_kind, program_ref_id, fingerprint, priority, state, attempts, max_attempts, queued_at_utc) "
        "VALUES(201,100,1,1,'workflow',101,'fp-live',0,'READY',0,1,3000);"));
    ASSERT_TRUE(ExecSql(db_, "DELETE FROM exec_job_event WHERE job_id=200; DELETE FROM exec_job WHERE job_id=200; DELETE FROM exec_job_set WHERE job_set_id=100;"));

    savor::db::archive::SqliteRehydrateExecutor rehydrate_executor(db_, db_, &archive_db, temp_root);
    ArchiveWorkflowCommands commands(db_, &archive_db, &package_service, &rehydrate_executor);

    const auto rehydrate_summary = commands.RehydrateExecute({
        .archive_package_id = package.archive_package_id,
        .now_utc = now,
        .target_namespace = "stage4",
        .trace_id = "stage4-rh",
    });
    const auto join_messages = [](const std::vector<std::string>& values) {
        std::ostringstream out;
        for (size_t i = 0; i < values.size(); ++i) {
            if (i > 0) out << " | ";
            out << values[i];
        }
        return out.str();
    };

    std::string rehydrate_status;
    std::string rehydrate_error_text;
    {
        sqlite3_stmt* status_st = nullptr;
        ASSERT_EQ(
            SQLITE_OK,
            sqlite3_prepare_v2(
                db_,
                "SELECT status, COALESCE(error_text, '') FROM ar_rehydrate_request ORDER BY rehydrate_request_id DESC LIMIT 1;",
                -1,
                &status_st,
                nullptr));
        if (sqlite3_step(status_st) == SQLITE_ROW) {
            const auto* status = reinterpret_cast<const char*>(sqlite3_column_text(status_st, 0));
            const auto* err_text = reinterpret_cast<const char*>(sqlite3_column_text(status_st, 1));
            if (status != nullptr) rehydrate_status = status;
            if (err_text != nullptr) rehydrate_error_text = err_text;
        }
        sqlite3_finalize(status_st);
    }

    ASSERT_TRUE(rehydrate_summary.success)
        << "RehydrateExecute failed"
        << "\nsummary.errors=" << join_messages(rehydrate_summary.errors)
        << "\nsummary.blocking_reasons=" << join_messages(rehydrate_summary.blocking_reasons)
        << "\nlatest_request.status=" << rehydrate_status
        << "\nlatest_request.error_text=" << rehydrate_error_text;
    ASSERT_EQ(rehydrate_summary.request_ids.size(), 1u);

    sqlite3_stmt* st = nullptr;

    ASSERT_EQ(SQLITE_OK, sqlite3_prepare_v2(db_, "SELECT old_id, new_id FROM ar_rehydrate_map WHERE rehydrate_request_id=?1 AND entity_kind='job';", -1, &st, nullptr));
    sqlite3_bind_int64(st, 1, rehydrate_summary.request_ids.front());
    ASSERT_EQ(SQLITE_ROW, sqlite3_step(st));
    const auto old_id = sqlite3_column_int64(st, 0);
    const auto new_id = sqlite3_column_int64(st, 1);
    EXPECT_EQ(200, old_id);
    EXPECT_NE(200, new_id);
    sqlite3_finalize(st);

    ASSERT_EQ(SQLITE_OK, sqlite3_prepare_v2(db_, "SELECT fingerprint FROM exec_job WHERE job_id=?1;", -1, &st, nullptr));
    sqlite3_bind_int64(st, 1, new_id);
    ASSERT_EQ(SQLITE_ROW, sqlite3_step(st));
    const auto* new_fingerprint_text = reinterpret_cast<const char*>(sqlite3_column_text(st, 0));
    ASSERT_NE(nullptr, new_fingerprint_text);
    const std::string new_fingerprint = new_fingerprint_text;
    EXPECT_FALSE(new_fingerprint.empty());
    EXPECT_NE("fp-200", new_fingerprint);
    EXPECT_NE("fp-live", new_fingerprint);
    sqlite3_finalize(st);

    const auto cleanup = commands.RehydrateCleanup({ .rehydrate_request_id = rehydrate_summary.request_ids.front() });
    EXPECT_TRUE(cleanup.success);

    ASSERT_EQ(SQLITE_OK, sqlite3_prepare_v2(db_, "SELECT COUNT(1) FROM ar_rehydrate_request WHERE rehydrate_request_id=?1;", -1, &st, nullptr));
    sqlite3_bind_int64(st, 1, rehydrate_summary.request_ids.front());
    ASSERT_EQ(SQLITE_ROW, sqlite3_step(st));
    EXPECT_EQ(0, sqlite3_column_int(st, 0));
    sqlite3_finalize(st);

    std::filesystem::remove_all(temp_root);
}

TEST_F(SqliteDbFixture, Stage4ArchiveOperatorCommandsArchivePreviewRespectsSubscriberFloorSafety) {
    using namespace savor::db;
    using namespace savor::db::migrations;
    using namespace savor::runner::parallel::savordb;

    const MigrationSourceOptions embedded_options{ .source_kind = MigrationSourceKind::Embedded };
    std::string err;
    ASSERT_TRUE(ApplyContextMigrations(db_, MigrationContext::Execution, embedded_options, &err)) << err;
    ASSERT_TRUE(ApplyContextMigrations(db_, MigrationContext::UIRead, embedded_options, &err)) << err;
    ASSERT_TRUE(ApplyContextMigrations(db_, MigrationContext::Archive, embedded_options, &err)) << err;

    ASSERT_TRUE(ExecSql(db_, R"SQL(
INSERT INTO exec_job_set(job_set_id, program_kind, purpose, created_at_utc) VALUES(100,1,'root',1000);
INSERT INTO exec_job(job_id, job_set_id, program_kind, program_version, program_ref_kind, program_ref_id, fingerprint, priority, state, attempts, max_attempts, queued_at_utc, ended_at_utc)
VALUES(200,100,1,1,'workflow',100,'fp-200',0,'COMPLETED',0,1,1000,2000);
INSERT INTO exec_outbox_message(outbox_id,event_id,event_type,event_version,context_name,aggregate_kind,aggregate_id,occurred_at_utc,payload_ref_kind,payload_ref_id,published_at_utc)
VALUES(10,'evt-10','Execution.JobQueued.v1',1,'Execution','job','200',1000,'job',200,1100),
      (20,'evt-20','Execution.JobCompleted.v1',1,'Execution','job','200',2000,'job',200,2100);
INSERT INTO ui_projection_subscription(projector_name,source_context,source_outbox_table,last_outbox_id,last_event_id,updated_at_utc,status,last_error)
VALUES('WorkflowProjector','Execution','exec_outbox_message',15,'evt-15',3000,'ACTIVE','');
UPDATE ui_projection_subscription
SET last_outbox_id=15,last_event_id='evt-15',updated_at_utc=3000,status='ACTIVE',last_error=''
WHERE source_context='Execution' AND source_outbox_table='exec_outbox_message';
)SQL"));

    const auto temp_root = std::filesystem::temp_directory_path() / ("savor-floor-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    ASSERT_TRUE(std::filesystem::create_directories(temp_root));

    savor::db::execution::workflow::SqliteExecutionDb execution_db(db_);
    SqliteUiReadDb ui_read_db(db_);
    SqliteArchiveDb archive_db(db_);
    savor::db::archive::SqliteArchivePackageService package_service(
        db_,
        &execution_db,
        &ui_read_db,
        &archive_db,
        DbConfigPaths{ .archive_store_root = temp_root });
    savor::db::archive::SqliteRehydrateExecutor rehydrate_executor(db_, db_, &archive_db, temp_root);
    ArchiveWorkflowCommands commands(db_, &archive_db, &package_service, &rehydrate_executor);

    const auto now = types::UtcTimePoint(std::chrono::milliseconds(1712304000000));
    const auto preview = commands.ArchivePreview({
        .older_than_utc = types::UtcTimePoint(std::chrono::milliseconds(1712305000000)),
        .now_utc = now,
        .max_candidates = 10,
        .outbox_policy = { .paused_or_error_block_threshold = std::chrono::hours(2), .block_when_no_active_subscriptions = true },
    });

    EXPECT_TRUE(preview.success);
    EXPECT_EQ(preview.ui_safe_floor_outbox_id, 15);
    ASSERT_TRUE(preview.retention_safe_floor_outbox_id.has_value());
    EXPECT_EQ(preview.retention_safe_floor_outbox_id.value(), 15);

    std::filesystem::remove_all(temp_root);
}

TEST_F(SqliteDbFixture, Stage4WorkflowArchivePackagesSelectedWorkflowsAndDedupesSavestateZipEntries) {
    using namespace savor::db;
    using namespace savor::db::migrations;

    const MigrationSourceOptions embedded_options{ .source_kind = MigrationSourceKind::Embedded };
    std::string err;
    ASSERT_TRUE(ApplyContextMigrations(db_, MigrationContext::Execution, embedded_options, &err)) << err;
    ASSERT_TRUE(ApplyContextMigrations(db_, MigrationContext::State, embedded_options, &err)) << err;
    ASSERT_TRUE(ApplyContextMigrations(db_, MigrationContext::UIRead, embedded_options, &err)) << err;
    ASSERT_TRUE(ApplyContextMigrations(db_, MigrationContext::Archive, embedded_options, &err)) << err;

    const auto temp_root = std::filesystem::temp_directory_path() / ("savor-workflow-archive-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    ASSERT_TRUE(std::filesystem::create_directories(temp_root));
    const auto sav_path = temp_root / "entry.sav";
    {
        std::ofstream out(sav_path, std::ios::binary | std::ios::trunc);
        out << "shared-entry-savestate";
    }

    const auto sav_sql = std::string("INSERT INTO state_artifact(artifact_id,sha256,size_bytes,compression_kind,filename,file_ext,artifact_kind,created_at_utc) VALUES(10,'sharedsha',21,'NONE','")
        + sav_path.generic_string() + "','.sav','SAV',1000);"
        + "INSERT INTO state_savestate(savestate_id,artifact_id,savestate_type,note,is_complete,created_at_utc) VALUES(101,10,'ENTRY','shared',1,1000);";
    ASSERT_TRUE(ExecSql(db_, sav_sql.c_str()));
    ASSERT_TRUE(ExecSql(db_, R"SQL(
INSERT INTO exec_workflow_instance(workflow_instance_id,workflow_kind,state,root_scope_kind,created_by,created_at_utc,completed_at_utc)
VALUES(1,'BATTLE_RUN','COMPLETED','manual','test',1000,2000),
      (2,'BATTLE_RUN','COMPLETED','manual','test',1000,2000);
INSERT INTO exec_workflow_step(workflow_step_id,workflow_instance_id,step_key,step_kind,state,priority,attempts,max_attempts,input_ref_kind,input_ref_id,created_at_utc,completed_at_utc)
VALUES(11,1,'entry','battle.entry','COMPLETED',0,0,1,'state.savestate_id',101,1000,2000),
      (21,2,'entry','battle.entry','COMPLETED',0,0,1,'state.savestate_id',101,1000,2000);
INSERT INTO ui_workflow_instance(workflow_instance_id,workflow_kind,state,display_state,root_scope_kind,created_by,created_at_utc,completed_at_utc,battle_final_victory_count)
VALUES(1,'BATTLE_RUN','COMPLETED','COMPLETED','manual','test',1000,2000,0),
      (2,'BATTLE_RUN','COMPLETED','COMPLETED','manual','test',1000,2000,0);
)SQL"));

    execution::workflow::SqliteExecutionDb execution_db(db_);
    SqliteUiReadDb ui_read_db(db_);
    SqliteArchiveDb archive_db(db_);
    archive::SqliteArchivePackageService package_service(
        db_,
        &execution_db,
        &ui_read_db,
        &archive_db,
        DbConfigPaths{ .archive_store_root = temp_root },
        db_,
        nullptr,
        db_);

    const auto package = package_service.CreateWorkflowPackage({
        .selection = { .workflow_instance_ids = {1, 2}, .created_by_filter_snapshot = "{\"workflow_kind\":\"BATTLE_RUN\"}" },
        .created_at_utc = types::UtcTimePoint(std::chrono::milliseconds(1712304000000)),
        .archive_name = "No victory archive",
        .archive_notes = std::string("Keep these for later review."),
        .correlation_id = "workflow-archive",
        .causation_id = "workflow-archive",
    });
    ASSERT_TRUE(package.success) << package.error.value_or("unknown error");
    EXPECT_TRUE(std::filesystem::exists(package.package_root / "savestates.zip"));
    EXPECT_NE(package.package_root.filename().string().find("no-victory-archive"), std::string::npos);
    EXPECT_EQ(ReadText(db_, "SELECT archive_name FROM ar_archive_package WHERE archive_package_id=(SELECT MAX(archive_package_id) FROM ar_archive_package);"), "No victory archive");
    EXPECT_EQ(ReadText(db_, "SELECT archive_notes FROM ar_archive_package WHERE archive_package_id=(SELECT MAX(archive_package_id) FROM ar_archive_package);"), "Keep these for later review.");
    EXPECT_EQ(ReadInt64(db_, "SELECT COUNT(1) FROM ar_archive_workflow_package WHERE archive_package_id=(SELECT MAX(archive_package_id) FROM ar_archive_package);"), 2);
    EXPECT_EQ(ReadInt64(db_, "SELECT source_workflow_count FROM ar_archive_package WHERE archive_package_id=(SELECT MAX(archive_package_id) FROM ar_archive_package);"), 2);
    EXPECT_EQ(ReadInt64(db_, "SELECT item_count FROM ar_archive_item WHERE item_kind='state_savestate_zip' AND archive_package_id=(SELECT MAX(archive_package_id) FROM ar_archive_package);"), 1);
    EXPECT_EQ(ReadInt64(db_, "SELECT COUNT(1) FROM ar_archive_item WHERE item_kind='state_artifacts' AND archive_package_id=(SELECT MAX(archive_package_id) FROM ar_archive_package);"), 1);
    {
        std::ifstream manifest(package.package_root / "manifest.json", std::ios::binary);
        ASSERT_TRUE(manifest.is_open());
        std::ostringstream manifest_text;
        manifest_text << manifest.rdbuf();
        EXPECT_NE(manifest_text.str().find("\"archive_name\": \"No victory archive\""), std::string::npos);
        EXPECT_NE(manifest_text.str().find("\"archive_notes\": \"Keep these for later review.\""), std::string::npos);
    }

    std::filesystem::remove_all(temp_root);
}

TEST_F(SqliteDbFixture, Stage4WorkflowArchiveRehydrateReusesExistingSavestateArtifactBySha) {
    using namespace savor::db;
    using namespace savor::db::migrations;

    const MigrationSourceOptions embedded_options{ .source_kind = MigrationSourceKind::Embedded };
    std::string err;
    ASSERT_TRUE(ApplyContextMigrations(db_, MigrationContext::Execution, embedded_options, &err)) << err;
    ASSERT_TRUE(ApplyContextMigrations(db_, MigrationContext::State, embedded_options, &err)) << err;
    ASSERT_TRUE(ApplyContextMigrations(db_, MigrationContext::UIRead, embedded_options, &err)) << err;
    ASSERT_TRUE(ApplyContextMigrations(db_, MigrationContext::Archive, embedded_options, &err)) << err;

    const auto temp_root = std::filesystem::temp_directory_path() / ("savor-workflow-rehydrate-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    ASSERT_TRUE(std::filesystem::create_directories(temp_root));
    const auto sav_path = temp_root / "entry.sav";
    {
        std::ofstream out(sav_path, std::ios::binary | std::ios::trunc);
        out << "dedupe-entry-savestate";
    }

    const auto sav_sql = std::string("INSERT INTO state_artifact(artifact_id,sha256,size_bytes,compression_kind,filename,file_ext,artifact_kind,created_at_utc) VALUES(10,'dedupesha',22,'NONE','")
        + sav_path.generic_string() + "','.sav','SAV',1000);"
        + "INSERT INTO state_savestate(savestate_id,artifact_id,savestate_type,note,is_complete,created_at_utc) VALUES(101,10,'ENTRY','shared',1,1000);";
    ASSERT_TRUE(ExecSql(db_, sav_sql.c_str()));
    ASSERT_TRUE(ExecSql(db_, R"SQL(
INSERT INTO exec_workflow_instance(workflow_instance_id,workflow_kind,state,root_scope_kind,created_by,created_at_utc,completed_at_utc)
VALUES(1,'BATTLE_RUN','COMPLETED','manual','test',1000,2000);
INSERT INTO exec_workflow_step(workflow_step_id,workflow_instance_id,step_key,step_kind,state,priority,attempts,max_attempts,input_ref_kind,input_ref_id,output_ref_kind,output_ref_id,created_at_utc,completed_at_utc)
VALUES(11,1,'entry','battle.entry','COMPLETED',0,0,1,'state.savestate_id',101,'state.savestate_id',101,1000,2000);
INSERT INTO ui_workflow_instance(workflow_instance_id,workflow_kind,state,display_state,root_scope_kind,created_by,created_at_utc,completed_at_utc,battle_final_victory_count)
VALUES(1,'BATTLE_RUN','COMPLETED','COMPLETED','manual','test',1000,2000,0);
)SQL"));

    execution::workflow::SqliteExecutionDb execution_db(db_);
    SqliteUiReadDb ui_read_db(db_);
    SqliteArchiveDb archive_db(db_);
    archive::SqliteArchivePackageService package_service(
        db_,
        &execution_db,
        &ui_read_db,
        &archive_db,
        DbConfigPaths{ .archive_store_root = temp_root },
        db_,
        nullptr,
        db_);

    const auto now = types::UtcTimePoint(std::chrono::milliseconds(1712304000000));
    const auto package = package_service.CreateWorkflowPackage({
        .selection = { .workflow_instance_ids = {1} },
        .created_at_utc = now,
        .correlation_id = "workflow-rehydrate",
        .causation_id = "workflow-rehydrate",
    });
    ASSERT_TRUE(package.success) << package.error.value_or("unknown error");

    std::int64_t request_id = 0;
    ASSERT_TRUE(archive_db.RequestRehydrate(
        {
            .archive_package_id = package.archive_package_id,
            .status = "REQUESTED",
            .requested_at_utc = now,
            .target_namespace = "wf-state",
            .correlation_id = "workflow-rehydrate",
            .causation_id = "workflow-rehydrate",
        },
        &request_id,
        &err))
        << err;

    archive::SqliteRehydrateExecutor rehydrate_executor(db_, db_, &archive_db, temp_root, db_);
    std::vector<archive::ArchiveOperationProgress> rehydrate_progress;
    const auto result = rehydrate_executor.Execute({
        .rehydrate_request_id = request_id,
        .now_utc = now,
        .correlation_id = "workflow-rehydrate",
        .causation_id = "workflow-rehydrate",
        .progress_sink = [&](const archive::ArchiveOperationProgress& progress) { rehydrate_progress.push_back(progress); },
    });
    ASSERT_TRUE(result.success) << result.error.value_or("unknown error");
    EXPECT_TRUE(std::any_of(rehydrate_progress.begin(), rehydrate_progress.end(), [](const archive::ArchiveOperationProgress& progress) {
        return progress.phase == archive::ArchiveOperationPhase::Complete;
    }));
    EXPECT_FALSE(std::any_of(rehydrate_progress.begin(), rehydrate_progress.end(), [](const archive::ArchiveOperationProgress& progress) {
        return progress.phase == archive::ArchiveOperationPhase::Failed;
    }));
    EXPECT_EQ(ReadInt64(db_, "SELECT CAST(new_id AS INTEGER) FROM ar_rehydrate_map WHERE entity_kind='state_artifact' AND old_id='10' ORDER BY rehydrate_map_id DESC LIMIT 1;"), 10);
    EXPECT_EQ(ReadInt64(db_, "SELECT CAST(new_id AS INTEGER) FROM ar_rehydrate_map WHERE entity_kind='state_savestate' AND old_id='101' ORDER BY rehydrate_map_id DESC LIMIT 1;"), 101);
    EXPECT_EQ(ReadInt64(db_, "SELECT COUNT(1) FROM state_artifact WHERE sha256='dedupesha';"), 1);

    std::filesystem::remove_all(temp_root);
}

TEST_F(SqliteDbFixture, Stage4WorkflowArchiveRehydrateRestoresExecutionExtrasAndBattleAnalysis) {
    using namespace savor::db;
    using namespace savor::db::archive;
    using namespace savor::db::migrations;
    using savor::runner::parallel::savordb::ArchiveWorkflowCommands;

    const MigrationSourceOptions embedded_options{ .source_kind = MigrationSourceKind::Embedded };
    std::string err;
    ASSERT_TRUE(ApplyContextMigrations(db_, MigrationContext::Execution, embedded_options, &err)) << err;
    ASSERT_TRUE(ApplyContextMigrations(db_, MigrationContext::State, embedded_options, &err)) << err;
    ASSERT_TRUE(ApplyContextMigrations(db_, MigrationContext::AnalysisBattle, embedded_options, &err)) << err;
    ASSERT_TRUE(ApplyContextMigrations(db_, MigrationContext::UIRead, embedded_options, &err)) << err;
    ASSERT_TRUE(ApplyContextMigrations(db_, MigrationContext::Archive, embedded_options, &err)) << err;

    const auto temp_root = std::filesystem::temp_directory_path() / ("savor-workflow-rehydrate-full-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    ASSERT_TRUE(std::filesystem::create_directories(temp_root));
    const auto sav_path = temp_root / "entry-full.sav";
    {
        std::ofstream out(sav_path, std::ios::binary | std::ios::trunc);
        out << "full-entry-savestate";
    }

    const auto sav_sql = std::string("INSERT INTO state_artifact(artifact_id,sha256,size_bytes,compression_kind,filename,file_ext,artifact_kind,created_at_utc) VALUES(60,'fullsha',20,'NONE','")
        + sav_path.generic_string() + "','.sav','SAV',1000);"
        + "INSERT INTO state_savestate(savestate_id,artifact_id,savestate_type,note,is_complete,created_at_utc) VALUES(601,60,'ENTRY','full',1,1000);";
    ASSERT_TRUE(ExecSql(db_, sav_sql.c_str()));
    ASSERT_TRUE(ExecSql(db_, R"SQL(
INSERT INTO exec_job_set(job_set_id,program_kind,purpose,created_by,created_at_utc,priority_boost,expected_total,domain_ref_kind,domain_ref_id,meta_note)
VALUES(7100,7,'rehydrate full','test',1000,0,1,'analysis_battle.turn_job',8300,'full');
INSERT INTO exec_job(job_id,job_set_id,program_kind,program_version,program_ref_kind,program_ref_id,fingerprint,priority,state,attempts,max_attempts,queued_at_utc,ended_at_utc)
VALUES(7101,7100,7,1,'analysis_battle.turn_job',8300,'rehydrate-full-job',0,'SUCCEEDED',1,1,1000,2000);
INSERT INTO exec_workflow_instance(workflow_instance_id,workflow_kind,state,root_scope_kind,root_scope_id,created_by,created_at_utc,completed_at_utc)
VALUES(7001,'BATTLE_RUN','COMPLETED','job_set',7100,'test',1000,2000);
INSERT INTO exec_workflow_step(workflow_step_id,workflow_instance_id,workflow_unit_activation_id,step_key,graph_node_key,step_kind,state,priority,attempts,max_attempts,job_set_id,input_ref_kind,input_ref_id,output_ref_kind,output_ref_id,created_at_utc,completed_at_utc)
VALUES(7201,7001,NULL,'battle','battle','battle.single_turn','COMPLETED',0,1,1,7100,'state.savestate_id',601,'state.savestate_id',601,1000,2000);
INSERT INTO exec_workflow_step_output(workflow_step_output_id,workflow_instance_id,workflow_step_id,graph_node_key,output_key,data_kind,ref_kind,ref_id,created_at_utc)
VALUES(7202,7001,7201,'battle','output_savestate','state.savestate_id','state.savestate_id',601,2000);
INSERT INTO exec_workflow_instance_input_binding(workflow_instance_input_binding_id,workflow_instance_id,workflow_graph_revision_id,node_key,input_key,data_kind,ref_kind,ref_id,source_kind,created_at_utc)
VALUES(7203,7001,1,'battle','entry','state.savestate_id','state.savestate_id',601,'manual',1000);
INSERT INTO exec_workflow_instance_argument(workflow_instance_argument_id,workflow_instance_id,node_key,argument_key,value_type,integer_value,text_value,source_kind,created_at_utc)
VALUES(7204,7001,'battle','turn_index','integer',1,NULL,'manual',1000);
INSERT INTO ui_workflow_instance(workflow_instance_id,workflow_kind,state,display_state,root_scope_kind,created_by,created_at_utc,completed_at_utc,battle_final_victory_count)
VALUES(7001,'BATTLE_RUN','COMPLETED','COMPLETED','job_set','test',1000,2000,0);
INSERT INTO ab_battle_set(battle_set_id,name,entry_savestate_id,battle_run_spec_id,explorer_settings_id,status,created_at_utc,completed_at_utc)
VALUES(8000,'rehydrate battle',601,1,1,'COMPLETED',1000,2000);
INSERT INTO ab_seed_candidate(seed_candidate_id,battle_set_id,seed_value,source_kind,candidate_status,created_at_utc)
VALUES(8100,8000,123,'MANUAL','SELECTED',1000);
INSERT INTO ab_battle_advancement_pool(battle_advancement_pool_id,battle_set_id,turn_index,pool_name,criterion_kind,created_at_utc)
VALUES(8200,8000,1,'pool','best',1000);
INSERT INTO ab_turn_wave(wave_id,battle_set_id,turn_index,context_probe_id,parent_wave_id,parent_turn_job_id,seed_candidate_id,battle_advancement_pool_id,status,created_at_utc,completed_at_utc)
VALUES(8250,8000,1,NULL,NULL,NULL,8100,8200,'COMPLETED',1000,2000);
INSERT INTO ab_battle_context_probe(context_probe_id,wave_id,source_savestate_id,exec_job_id,probe_status,context_blob,context_version,recorded_at_utc,created_at_utc)
VALUES(8275,8250,601,7101,'COMPLETED','{}',1,1500,1000);
UPDATE ab_turn_wave SET context_probe_id=8275 WHERE wave_id=8250;
INSERT INTO ab_turn_job(turn_job_id,wave_id,exec_job_id,plan_id,fake_attacks_this_turn,fake_attacks_used_before,job_state,started_at_utc,ended_at_utc,has_results,output_savestate_id,recorded_at_utc,source_savestate_id,seed_candidate_id,authored_plan_id,authored_turn_index)
VALUES(8300,8250,7101,1,0,0,'SUCCEEDED',1000,2000,1,601,2000,601,8100,1,1);
INSERT INTO ab_battle_advancement_decision(battle_advancement_decision_id,battle_advancement_pool_id,turn_job_id,decision_kind,decision_reason,created_at_utc)
VALUES(8400,8200,8300,'SELECTED','best',2000);
INSERT INTO ab_manual_followup(manual_followup_id,turn_job_id,manual_followup_status,recorded_sav_artifact_id,note,updated_at_utc)
VALUES(8500,8300,'UNREVIEWED',NULL,'restore me',2000);
)SQL"));

    execution::workflow::SqliteExecutionDb execution_db(db_);
    SqliteUiReadDb ui_read_db(db_);
    SqliteArchiveDb archive_db(db_);
    SqliteArchivePackageService package_service(
        db_,
        &execution_db,
        &ui_read_db,
        &archive_db,
        DbConfigPaths{ .archive_store_root = temp_root },
        db_,
        db_,
        db_);

    const auto now = types::UtcTimePoint(std::chrono::milliseconds(1712304000000));
    const auto package = package_service.CreateWorkflowPackage({
        .selection = { .workflow_instance_ids = {7001} },
        .created_at_utc = now,
        .correlation_id = "workflow-rehydrate-full",
        .causation_id = "workflow-rehydrate-full",
    });
    ASSERT_TRUE(package.success) << package.error.value_or("unknown error");

    SqliteRehydrateExecutor rehydrate_executor(db_, db_, &archive_db, temp_root, db_, db_);
    ArchiveWorkflowCommands commands(db_, &archive_db, &package_service, &rehydrate_executor);
    const auto preview = commands.RehydratePreview({
        .archive_package_id = package.archive_package_id,
        .target_namespace = "wf-full",
    });
    ASSERT_TRUE(preview.success);
    EXPECT_EQ(preview.expected_workflows, 1);
    EXPECT_EQ(preview.expected_jobs, 1);
    EXPECT_GT(preview.execution_row_count, 0);
    EXPECT_GT(preview.analysis_row_count, 0);
    EXPECT_EQ(preview.state_savestate_count, 1);
    EXPECT_EQ(preview.savestate_zip_entry_count, 1);

    std::int64_t request_id = 0;
    ASSERT_TRUE(archive_db.RequestRehydrate(
        {
            .archive_package_id = package.archive_package_id,
            .status = "REQUESTED",
            .requested_at_utc = now,
            .target_namespace = "wf-full",
            .correlation_id = "workflow-rehydrate-full",
            .causation_id = "workflow-rehydrate-full",
        },
        &request_id,
        &err))
        << err;

    const auto result = rehydrate_executor.Execute({
        .rehydrate_request_id = request_id,
        .now_utc = now,
        .correlation_id = "workflow-rehydrate-full",
        .causation_id = "workflow-rehydrate-full",
    });
    ASSERT_TRUE(result.success) << result.error.value_or("unknown error");

    const auto new_workflow_id = ReadInt64(db_, "SELECT CAST(new_id AS INTEGER) FROM ar_rehydrate_map WHERE entity_kind='workflow_instance' AND old_id='7001' ORDER BY rehydrate_map_id DESC LIMIT 1;");
    const auto new_job_id = ReadInt64(db_, "SELECT CAST(new_id AS INTEGER) FROM ar_rehydrate_map WHERE entity_kind='job' AND old_id='7101' ORDER BY rehydrate_map_id DESC LIMIT 1;");
    const auto new_battle_set_id = ReadInt64(db_, "SELECT CAST(new_id AS INTEGER) FROM ar_rehydrate_map WHERE entity_kind='analysis_battle_set' AND old_id='8000' ORDER BY rehydrate_map_id DESC LIMIT 1;");
    const auto new_wave_id = ReadInt64(db_, "SELECT CAST(new_id AS INTEGER) FROM ar_rehydrate_map WHERE entity_kind='analysis_turn_wave' AND old_id='8250' ORDER BY rehydrate_map_id DESC LIMIT 1;");
    const auto new_context_id = ReadInt64(db_, "SELECT CAST(new_id AS INTEGER) FROM ar_rehydrate_map WHERE entity_kind='analysis_battle_context_probe' AND old_id='8275' ORDER BY rehydrate_map_id DESC LIMIT 1;");
    const auto new_turn_job_id = ReadInt64(db_, "SELECT CAST(new_id AS INTEGER) FROM ar_rehydrate_map WHERE entity_kind='analysis_battle_turn_job' AND old_id='8300' ORDER BY rehydrate_map_id DESC LIMIT 1;");
    ASSERT_GT(new_workflow_id, 0);
    ASSERT_GT(new_job_id, 0);
    ASSERT_GT(new_battle_set_id, 0);
    ASSERT_GT(new_wave_id, 0);
    ASSERT_GT(new_context_id, 0);
    ASSERT_GT(new_turn_job_id, 0);

    EXPECT_EQ(ReadInt64(db_, ("SELECT COUNT(1) FROM exec_workflow_step_output WHERE workflow_instance_id=" + std::to_string(new_workflow_id) + " AND ref_id=601;").c_str()), 1);
    EXPECT_EQ(ReadInt64(db_, ("SELECT COUNT(1) FROM exec_workflow_instance_input_binding WHERE workflow_instance_id=" + std::to_string(new_workflow_id) + " AND ref_id=601;").c_str()), 1);
    EXPECT_EQ(ReadInt64(db_, ("SELECT COUNT(1) FROM exec_workflow_instance_argument WHERE workflow_instance_id=" + std::to_string(new_workflow_id) + " AND argument_key='turn_index' AND integer_value=1;").c_str()), 1);
    EXPECT_EQ(ReadInt64(db_, ("SELECT entry_savestate_id FROM ab_battle_set WHERE battle_set_id=" + std::to_string(new_battle_set_id) + ";").c_str()), 601);
    EXPECT_EQ(ReadInt64(db_, ("SELECT exec_job_id FROM ab_turn_job WHERE turn_job_id=" + std::to_string(new_turn_job_id) + ";").c_str()), new_job_id);
    EXPECT_EQ(ReadInt64(db_, ("SELECT context_probe_id FROM ab_turn_wave WHERE wave_id=" + std::to_string(new_wave_id) + ";").c_str()), new_context_id);
    EXPECT_EQ(ReadText(db_, ("SELECT note FROM ab_manual_followup WHERE turn_job_id=" + std::to_string(new_turn_job_id) + ";").c_str()), "restore me");

    const auto collision_preview = commands.RehydratePreview({
        .archive_package_id = package.archive_package_id,
        .target_namespace = "wf-full",
    });
    EXPECT_FALSE(collision_preview.success);
    EXPECT_TRUE(std::any_of(collision_preview.blocking_reasons.begin(), collision_preview.blocking_reasons.end(), [](const std::string& reason) {
        return reason.find("target_id_collision:exec_workflow_instance.workflow_instance_id") != std::string::npos;
    }));

    std::filesystem::remove_all(temp_root);
}

TEST_F(SqliteDbFixture, Stage4WorkflowArchiveRehydrateRestoresSeedProbeAnalysisAndRefs) {
    using namespace savor::db;
    using namespace savor::db::archive;
    using namespace savor::db::migrations;

    const MigrationSourceOptions embedded_options{ .source_kind = MigrationSourceKind::Embedded };
    std::string err;
    ASSERT_TRUE(ApplyContextMigrations(db_, MigrationContext::Execution, embedded_options, &err)) << err;
    ASSERT_TRUE(ApplyContextMigrations(db_, MigrationContext::State, embedded_options, &err)) << err;
    ASSERT_TRUE(ApplyContextMigrations(db_, MigrationContext::AnalysisSeedProbe, embedded_options, &err)) << err;
    ASSERT_TRUE(ApplyContextMigrations(db_, MigrationContext::UIRead, embedded_options, &err)) << err;
    ASSERT_TRUE(ApplyContextMigrations(db_, MigrationContext::Archive, embedded_options, &err)) << err;

    const auto temp_root = std::filesystem::temp_directory_path() / ("savor-workflow-rehydrate-seed-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    ASSERT_TRUE(std::filesystem::create_directories(temp_root));
    const auto sav_path = temp_root / "seed-entry.sav";
    {
        std::ofstream out(sav_path, std::ios::binary | std::ios::trunc);
        out << "seed-entry-savestate";
    }

    const auto sav_sql = std::string("INSERT INTO state_artifact(artifact_id,sha256,size_bytes,compression_kind,filename,file_ext,artifact_kind,created_at_utc) VALUES(70,'seedsha',20,'NONE','")
        + sav_path.generic_string() + "','.sav','SAV',1000);"
        + "INSERT INTO state_savestate(savestate_id,artifact_id,savestate_type,note,is_complete,created_at_utc) VALUES(701,70,'ENTRY','seed',1,1000);";
    ASSERT_TRUE(ExecSql(db_, sav_sql.c_str()));
    ASSERT_TRUE(ExecSql(db_, R"SQL(
INSERT INTO sp_probe_set(probe_set_id,name,probe_flavor,breakpoint_policy_name,segment_source_kind,created_at_utc)
VALUES(9000,'seed probe set','BATTLE_PRE','default','manual',1000);
INSERT INTO an_input_set(input_set_id,content_hash,source_ref_kind,source_ref_id,created_at_utc)
VALUES(9001,'seed-input-hash','manual',1,1000);
INSERT INTO sp_axis_xy(axis_xy_id,x,y) VALUES(9002,128,128),(9003,129,128),(9004,128,129);
INSERT INTO sp_input_frame(input_frame_id,main_axis_xy_id,cstick_axis_xy_id,trigger_axis_xy_id)
VALUES(9005,9002,9003,9004);
INSERT INTO an_input_set_frame(input_set_id,ordinal,input_frame_id,added_at_utc)
VALUES(9001,0,9005,1000);
INSERT INTO sp_probe_run(probe_run_id,probe_set_id,entry_savestate_id,seed_probe_spec_id,codec_version,status,unique_input_set_id,requested_at_utc,completed_at_utc,launch_samples_per_axis)
VALUES(9010,9000,701,1,1,'COMPLETED',9001,1000,2000,1);
INSERT INTO sp_probe_result(probe_result_id,probe_run_id,neutral_seed_value,grid_count,unique_count,result_status,recorded_at_utc)
VALUES(9020,9010,1000,1,1,'COMPLETED',2000);
INSERT INTO sp_neutral_seed(neutral_seed_id,probe_result_id,neutral_seed_value,source_kind,recorded_at_utc)
VALUES(9030,9020,1000,'CALCULATED',2000);
INSERT INTO sp_grid_seed(grid_seed_id,probe_result_id,source_family,axis_xy_id,seed_value,seed_delta,recorded_at_utc)
VALUES(9040,9020,'MAIN',9002,1001,1,2000);
INSERT INTO sp_unique_seed(unique_seed_id,probe_result_id,input_frame_id,seed_value,seed_delta,recorded_at_utc)
VALUES(9050,9020,9005,1002,2,2000);
INSERT INTO sp_encounter_projection(encounter_projection_id,probe_run_id,seed_value,option_ordinal,encounter_id,encounter_frame,movement_required,recorded_at_utc)
VALUES(9060,9010,1002,0,'battle',10,1,2000);
INSERT INTO exec_job_set(job_set_id,program_kind,purpose,created_by,created_at_utc,priority_boost,expected_total,domain_ref_kind,domain_ref_id,meta_note)
VALUES(9100,3,'seed job set','test',1000,0,1,'sp_probe_run',9010,'seed');
INSERT INTO exec_job(job_id,job_set_id,program_kind,program_version,program_ref_kind,program_ref_id,fingerprint,priority,state,attempts,max_attempts,queued_at_utc,ended_at_utc)
VALUES(9101,9100,3,1,'sp_probe_run',9010,'rehydrate-seed-job',0,'SUCCEEDED',1,1,1000,2000);
INSERT INTO exec_workflow_instance(workflow_instance_id,workflow_kind,state,root_scope_kind,root_scope_id,created_by,created_at_utc,completed_at_utc)
VALUES(9102,'SEED_PROBE','COMPLETED','job_set',9100,'test',1000,2000);
INSERT INTO exec_workflow_step(workflow_step_id,workflow_instance_id,step_key,graph_node_key,step_kind,state,priority,attempts,max_attempts,job_set_id,input_ref_kind,input_ref_id,output_ref_kind,output_ref_id,created_at_utc,completed_at_utc)
VALUES(9103,9102,'probe','probe','seed_probe_chain','COMPLETED',0,1,1,9100,'sp_probe_run',9010,'sp_probe_run',9010,1000,2000);
INSERT INTO exec_workflow_step_output(workflow_step_output_id,workflow_instance_id,workflow_step_id,graph_node_key,output_key,data_kind,ref_kind,ref_id,created_at_utc)
VALUES(9104,9102,9103,'probe','run','sp_probe_run','sp_probe_run',9010,2000);
INSERT INTO exec_workflow_instance_input_binding(workflow_instance_input_binding_id,workflow_instance_id,workflow_graph_revision_id,node_key,input_key,data_kind,ref_kind,ref_id,source_kind,created_at_utc)
VALUES(9105,9102,1,'probe','run','sp_probe_run','sp_probe_run',9010,'manual',1000);
INSERT INTO ui_workflow_instance(workflow_instance_id,workflow_kind,state,display_state,root_scope_kind,created_by,created_at_utc,completed_at_utc,battle_final_victory_count)
VALUES(9102,'SEED_PROBE','COMPLETED','COMPLETED','job_set','test',1000,2000,0);
)SQL"));

    execution::workflow::SqliteExecutionDb execution_db(db_);
    SqliteUiReadDb ui_read_db(db_);
    SqliteArchiveDb archive_db(db_);
    SqliteArchivePackageService package_service(
        db_,
        &execution_db,
        &ui_read_db,
        &archive_db,
        DbConfigPaths{ .archive_store_root = temp_root },
        db_,
        db_,
        db_);

    const auto now = types::UtcTimePoint(std::chrono::milliseconds(1712304000000));
    const auto package = package_service.CreateWorkflowPackage({
        .selection = { .workflow_instance_ids = {9102} },
        .created_at_utc = now,
        .correlation_id = "workflow-rehydrate-seed",
        .causation_id = "workflow-rehydrate-seed",
    });
    ASSERT_TRUE(package.success) << package.error.value_or("unknown error");
    EXPECT_EQ(ReadInt64(db_, ("SELECT item_count FROM ar_archive_item WHERE archive_package_id=" + std::to_string(package.archive_package_id) + " AND item_kind='analysis_seed_probe_runs';").c_str()), 1);
    EXPECT_EQ(ReadInt64(db_, ("SELECT item_count FROM ar_archive_item WHERE archive_package_id=" + std::to_string(package.archive_package_id) + " AND item_kind='state_savestates';").c_str()), 1);

    std::int64_t request_id = 0;
    ASSERT_TRUE(archive_db.RequestRehydrate(
        {
            .archive_package_id = package.archive_package_id,
            .status = "REQUESTED",
            .requested_at_utc = now,
            .target_namespace = "wf-seed",
            .correlation_id = "workflow-rehydrate-seed",
            .causation_id = "workflow-rehydrate-seed",
        },
        &request_id,
        &err))
        << err;

    SqliteRehydrateExecutor rehydrate_executor(db_, db_, &archive_db, temp_root, db_, db_);
    const auto result = rehydrate_executor.Execute({
        .rehydrate_request_id = request_id,
        .now_utc = now,
        .correlation_id = "workflow-rehydrate-seed",
        .causation_id = "workflow-rehydrate-seed",
    });
    ASSERT_TRUE(result.success) << result.error.value_or("unknown error");

    const auto new_probe_run_id = ReadInt64(db_, "SELECT CAST(new_id AS INTEGER) FROM ar_rehydrate_map WHERE entity_kind='analysis_seed_probe_run' AND old_id='9010' ORDER BY rehydrate_map_id DESC LIMIT 1;");
    const auto new_job_id = ReadInt64(db_, "SELECT CAST(new_id AS INTEGER) FROM ar_rehydrate_map WHERE entity_kind='job' AND old_id='9101' ORDER BY rehydrate_map_id DESC LIMIT 1;");
    const auto new_step_id = ReadInt64(db_, "SELECT CAST(new_id AS INTEGER) FROM ar_rehydrate_map WHERE entity_kind='workflow_step' AND old_id='9103' ORDER BY rehydrate_map_id DESC LIMIT 1;");
    ASSERT_GT(new_probe_run_id, 0);
    ASSERT_GT(new_job_id, 0);
    ASSERT_GT(new_step_id, 0);
    EXPECT_EQ(ReadInt64(db_, ("SELECT program_ref_id FROM exec_job WHERE job_id=" + std::to_string(new_job_id) + ";").c_str()), new_probe_run_id);
    EXPECT_EQ(ReadInt64(db_, ("SELECT input_ref_id FROM exec_workflow_step WHERE workflow_step_id=" + std::to_string(new_step_id) + ";").c_str()), new_probe_run_id);
    EXPECT_EQ(ReadInt64(db_, ("SELECT COUNT(1) FROM sp_probe_result WHERE probe_run_id=" + std::to_string(new_probe_run_id) + ";").c_str()), 1);
    EXPECT_EQ(ReadInt64(db_, ("SELECT entry_savestate_id FROM sp_probe_run WHERE probe_run_id=" + std::to_string(new_probe_run_id) + ";").c_str()), 701);
    EXPECT_EQ(ReadInt64(db_, ("SELECT COUNT(1) FROM sp_unique_seed u JOIN sp_probe_result r ON r.probe_result_id=u.probe_result_id WHERE r.probe_run_id=" + std::to_string(new_probe_run_id) + ";").c_str()), 1);

    std::filesystem::remove_all(temp_root);
}

TEST_F(SqliteDbFixture, Stage4WorkflowArchiveRehydrateRejectsCorruptSavestateZipBytes) {
    using namespace savor::db;
    using namespace savor::db::archive;
    using namespace savor::db::migrations;

    const MigrationSourceOptions embedded_options{ .source_kind = MigrationSourceKind::Embedded };
    std::string err;
    ASSERT_TRUE(ApplyContextMigrations(db_, MigrationContext::Execution, embedded_options, &err)) << err;
    ASSERT_TRUE(ApplyContextMigrations(db_, MigrationContext::State, embedded_options, &err)) << err;
    ASSERT_TRUE(ApplyContextMigrations(db_, MigrationContext::UIRead, embedded_options, &err)) << err;
    ASSERT_TRUE(ApplyContextMigrations(db_, MigrationContext::Archive, embedded_options, &err)) << err;

    const auto temp_root = std::filesystem::temp_directory_path() / ("savor-workflow-rehydrate-corrupt-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    ASSERT_TRUE(std::filesystem::create_directories(temp_root));
    const auto sav_path = temp_root / "corrupt-entry.sav";
    {
        std::ofstream out(sav_path, std::ios::binary | std::ios::trunc);
        out << "corrupt-entry-savestate";
    }
    const auto sha = hash::sha256_of_file(sav_path.string());

    const auto sav_sql = std::string("INSERT INTO state_artifact(artifact_id,sha256,size_bytes,compression_kind,filename,file_ext,artifact_kind,created_at_utc) VALUES(80,'")
        + sha + "',23,'NONE','" + sav_path.generic_string() + "','.sav','SAV',1000);"
        + "INSERT INTO state_savestate(savestate_id,artifact_id,savestate_type,note,is_complete,created_at_utc) VALUES(801,80,'ENTRY','corrupt',1,1000);";
    ASSERT_TRUE(ExecSql(db_, sav_sql.c_str()));
    ASSERT_TRUE(ExecSql(db_, R"SQL(
INSERT INTO exec_workflow_instance(workflow_instance_id,workflow_kind,state,root_scope_kind,created_by,created_at_utc,completed_at_utc)
VALUES(9801,'BATTLE_RUN','COMPLETED','manual','test',1000,2000);
INSERT INTO exec_workflow_step(workflow_step_id,workflow_instance_id,step_key,step_kind,state,priority,attempts,max_attempts,input_ref_kind,input_ref_id,created_at_utc,completed_at_utc)
VALUES(9802,9801,'entry','battle.entry','COMPLETED',0,0,1,'state.savestate_id',801,1000,2000);
INSERT INTO ui_workflow_instance(workflow_instance_id,workflow_kind,state,display_state,root_scope_kind,created_by,created_at_utc,completed_at_utc,battle_final_victory_count)
VALUES(9801,'BATTLE_RUN','COMPLETED','COMPLETED','manual','test',1000,2000,0);
)SQL"));

    execution::workflow::SqliteExecutionDb execution_db(db_);
    SqliteUiReadDb ui_read_db(db_);
    SqliteArchiveDb archive_db(db_);
    SqliteArchivePackageService package_service(
        db_,
        &execution_db,
        &ui_read_db,
        &archive_db,
        DbConfigPaths{ .archive_store_root = temp_root },
        db_,
        nullptr,
        db_);

    const auto now = types::UtcTimePoint(std::chrono::milliseconds(1712304000000));
    const auto package = package_service.CreateWorkflowPackage({
        .selection = { .workflow_instance_ids = {9801} },
        .created_at_utc = now,
        .correlation_id = "workflow-rehydrate-corrupt",
        .causation_id = "workflow-rehydrate-corrupt",
    });
    ASSERT_TRUE(package.success) << package.error.value_or("unknown error");

    const auto zip_path = package.package_root / "savestates.zip";
    {
        std::string zip_bytes;
        {
            std::ifstream in(zip_path, std::ios::binary);
            ASSERT_TRUE(in.is_open());
            std::ostringstream buffer;
            buffer << in.rdbuf();
            zip_bytes = buffer.str();
        }
        const auto payload_offset = zip_bytes.find("corrupt-entry-savestate");
        ASSERT_NE(payload_offset, std::string::npos);
        std::fstream out(zip_path, std::ios::in | std::ios::out | std::ios::binary);
        ASSERT_TRUE(out.is_open());
        out.seekp(static_cast<std::streamoff>(payload_offset));
        const char corrupted = 'X';
        out.write(&corrupted, 1);
        ASSERT_TRUE(out.good());
    }

    ASSERT_TRUE(ExecSql(db_, "DELETE FROM state_savestate WHERE savestate_id=801;DELETE FROM state_artifact WHERE artifact_id=80;"));

    std::int64_t request_id = 0;
    ASSERT_TRUE(archive_db.RequestRehydrate(
        {
            .archive_package_id = package.archive_package_id,
            .status = "REQUESTED",
            .requested_at_utc = now,
            .target_namespace = "wf-corrupt",
            .correlation_id = "workflow-rehydrate-corrupt",
            .causation_id = "workflow-rehydrate-corrupt",
        },
        &request_id,
        &err))
        << err;

    SqliteRehydrateExecutor rehydrate_executor(db_, db_, &archive_db, temp_root, db_);
    const auto result = rehydrate_executor.Execute({
        .rehydrate_request_id = request_id,
        .now_utc = now,
        .correlation_id = "workflow-rehydrate-corrupt",
        .causation_id = "workflow-rehydrate-corrupt",
    });
    ASSERT_FALSE(result.success);
    ASSERT_TRUE(result.error.has_value());
    EXPECT_NE(result.error->find("sha256 mismatch"), std::string::npos) << *result.error;
    EXPECT_EQ(ReadText(db_, ("SELECT status FROM ar_rehydrate_request WHERE rehydrate_request_id=" + std::to_string(request_id) + ";").c_str()), "FAILED");

    std::filesystem::remove_all(temp_root);
}

TEST_F(SqliteDbFixture, Stage4WorkflowArchiveExecuteReportsProgressPhases) {
    using namespace savor::db;
    using namespace savor::db::archive;
    using namespace savor::db::migrations;
    using savor::runner::parallel::savordb::ArchiveWorkflowCommands;

    const MigrationSourceOptions embedded_options{ .source_kind = MigrationSourceKind::Embedded };
    std::string err;
    ASSERT_TRUE(ApplyContextMigrations(db_, MigrationContext::Execution, embedded_options, &err)) << err;
    ASSERT_TRUE(ApplyContextMigrations(db_, MigrationContext::State, embedded_options, &err)) << err;
    ASSERT_TRUE(ApplyContextMigrations(db_, MigrationContext::UIRead, embedded_options, &err)) << err;
    ASSERT_TRUE(ApplyContextMigrations(db_, MigrationContext::Archive, embedded_options, &err)) << err;

    const auto temp_root = std::filesystem::temp_directory_path() / ("savor-workflow-progress-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    ASSERT_TRUE(std::filesystem::create_directories(temp_root));
    const auto sav_path = temp_root / "progress-entry.sav";
    {
        std::ofstream out(sav_path, std::ios::binary | std::ios::trunc);
        out << "progress-entry-savestate";
    }

    const auto sav_sql = std::string("INSERT INTO state_artifact(artifact_id,sha256,size_bytes,compression_kind,filename,file_ext,artifact_kind,created_at_utc) VALUES(50,'progresssha',23,'NONE','")
        + sav_path.generic_string() + "','.sav','SAV',1000);"
        + "INSERT INTO state_savestate(savestate_id,artifact_id,savestate_type,note,is_complete,created_at_utc) VALUES(501,50,'ENTRY','progress',1,1000);";
    ASSERT_TRUE(ExecSql(db_, sav_sql.c_str()));
    ASSERT_TRUE(ExecSql(db_, R"SQL(
INSERT INTO exec_workflow_instance(workflow_instance_id,workflow_kind,state,root_scope_kind,created_by,created_at_utc,completed_at_utc)
VALUES(5010,'BATTLE_RUN','COMPLETED','manual','test',1000,2000);
INSERT INTO exec_workflow_step(workflow_step_id,workflow_instance_id,step_key,step_kind,state,priority,attempts,max_attempts,input_ref_kind,input_ref_id,created_at_utc,completed_at_utc)
VALUES(5011,5010,'entry','battle.entry','COMPLETED',0,0,1,'state.savestate_id',501,1000,2000);
INSERT INTO ui_workflow_instance(workflow_instance_id,workflow_kind,state,display_state,root_scope_kind,created_by,created_at_utc,completed_at_utc,battle_final_victory_count)
VALUES(5010,'BATTLE_RUN','COMPLETED','COMPLETED','manual','test',1000,2000,0);
)SQL"));

    execution::workflow::SqliteExecutionDb execution_db(db_);
    SqliteUiReadDb ui_read_db(db_);
    SqliteArchiveDb archive_db(db_);
    archive::SqliteArchivePackageService package_service(
        db_,
        &execution_db,
        &ui_read_db,
        &archive_db,
        DbConfigPaths{ .archive_store_root = temp_root },
        db_,
        nullptr,
        db_);
    archive::SqliteRehydrateExecutor rehydrate_executor(db_, db_, &archive_db, temp_root, db_);
    ArchiveWorkflowCommands commands(db_, &archive_db, &package_service, &rehydrate_executor);

    const auto now = types::UtcTimePoint(std::chrono::milliseconds(1712304000000));
    std::vector<ArchiveOperationProgress> package_only_progress;
    auto package_only = commands.WorkflowArchiveExecute({
        .selection = { .workflow_instance_ids = {5010} },
        .now_utc = now,
        .purge_after_verify = false,
        .trace_id = "workflow-progress-package",
        .progress_sink = [&](const ArchiveOperationProgress& progress) { package_only_progress.push_back(progress); },
    });
    ASSERT_TRUE(package_only.success) << (package_only.errors.empty() ? "unknown error" : package_only.errors.front());

    auto has_phase = [](const std::vector<ArchiveOperationProgress>& progress, ArchiveOperationPhase phase) {
        return std::any_of(progress.begin(), progress.end(), [phase](const ArchiveOperationProgress& item) {
            return item.phase == phase;
        });
    };
    auto phase_index = [](const std::vector<ArchiveOperationProgress>& progress, ArchiveOperationPhase phase) {
        for (std::size_t i = 0; i < progress.size(); ++i) {
            if (progress[i].phase == phase) {
                return i;
            }
        }
        return progress.size();
    };

    EXPECT_TRUE(has_phase(package_only_progress, ArchiveOperationPhase::Previewing));
    EXPECT_TRUE(has_phase(package_only_progress, ArchiveOperationPhase::ExportingRows));
    EXPECT_TRUE(has_phase(package_only_progress, ArchiveOperationPhase::WritingSavestates));
    EXPECT_TRUE(has_phase(package_only_progress, ArchiveOperationPhase::RegisteringPackage));
    EXPECT_TRUE(has_phase(package_only_progress, ArchiveOperationPhase::VerifyingPackage));
    EXPECT_TRUE(has_phase(package_only_progress, ArchiveOperationPhase::Complete));
    EXPECT_FALSE(has_phase(package_only_progress, ArchiveOperationPhase::PurgingSource));
    EXPECT_LT(phase_index(package_only_progress, ArchiveOperationPhase::Previewing), phase_index(package_only_progress, ArchiveOperationPhase::ExportingRows));
    EXPECT_LT(phase_index(package_only_progress, ArchiveOperationPhase::ExportingRows), phase_index(package_only_progress, ArchiveOperationPhase::WritingSavestates));
    EXPECT_LT(phase_index(package_only_progress, ArchiveOperationPhase::WritingSavestates), phase_index(package_only_progress, ArchiveOperationPhase::RegisteringPackage));
    EXPECT_LT(phase_index(package_only_progress, ArchiveOperationPhase::RegisteringPackage), phase_index(package_only_progress, ArchiveOperationPhase::VerifyingPackage));
    EXPECT_LT(phase_index(package_only_progress, ArchiveOperationPhase::VerifyingPackage), phase_index(package_only_progress, ArchiveOperationPhase::Complete));
    EXPECT_TRUE(std::any_of(package_only_progress.begin(), package_only_progress.end(), [](const ArchiveOperationProgress& item) {
        return item.phase == ArchiveOperationPhase::WritingSavestates
            && item.completed_units == item.total_units
            && item.total_units == 1
            && item.bytes_completed.has_value()
            && item.bytes_total.has_value()
            && *item.bytes_total > 0;
    }));

    std::vector<ArchiveOperationProgress> purge_progress;
    auto purge = commands.WorkflowArchiveExecute({
        .selection = { .workflow_instance_ids = {5010} },
        .now_utc = now,
        .purge_after_verify = true,
        .trace_id = "workflow-progress-purge",
        .progress_sink = [&](const ArchiveOperationProgress& progress) { purge_progress.push_back(progress); },
    });
    ASSERT_TRUE(purge.success) << (purge.errors.empty() ? "unknown error" : purge.errors.front());
    EXPECT_TRUE(has_phase(purge_progress, ArchiveOperationPhase::PurgingSource));
    EXPECT_TRUE(has_phase(purge_progress, ArchiveOperationPhase::Complete));
    EXPECT_FALSE(has_phase(purge_progress, ArchiveOperationPhase::Failed));

    std::filesystem::remove_all(temp_root);
}

TEST_F(SqliteDbFixture, Stage4WorkflowArchiveCandidateFilterSelectsCompletedBattlesWithoutFinalVictory) {
    using namespace savor::db;
    using namespace savor::db::migrations;

    const MigrationSourceOptions embedded_options{ .source_kind = MigrationSourceKind::Embedded };
    std::string err;
    ASSERT_TRUE(ApplyContextMigrations(db_, MigrationContext::UIRead, embedded_options, &err)) << err;

    ASSERT_TRUE(ExecSql(db_, R"SQL(
INSERT INTO ui_workflow_instance(workflow_instance_id,workflow_kind,state,display_state,root_scope_kind,created_by,created_at_utc,completed_at_utc,battle_final_victory_count)
VALUES(1,'BATTLE_RUN','COMPLETED','COMPLETED','manual','test',3000,4000,0),
      (2,'BATTLE_RUN','COMPLETED','COMPLETED','manual','test',2000,3000,1),
      (3,'SEED_PROBE_CHAIN','COMPLETED','COMPLETED','manual','test',1000,2000,0);
)SQL"));

    SqliteUiReadDb ui_read_db(db_);
    const auto page = ui_read_db.ListWorkflowInstances({
        .limit = 20,
        .state = "COMPLETED",
        .workflow_kind = "BATTLE_RUN",
        .battle_final_victory_absent_only = true,
    });

    ASSERT_EQ(page.items.size(), 1u);
    EXPECT_EQ(page.items.front().workflow_instance_id, 1);
    EXPECT_EQ(page.items.front().battle_final_victory_count, 0);
}

TEST_F(SqliteDbFixture, UiReadArchiveCatalogProjectsArchiveNameAndNotes) {
    namespace migrations = savor::db::migrations;
    namespace projectors = savor::db::uiread::projectors;

    const auto separate_root = temp_root_ / "projection-archive-name-notes";
    ASSERT_TRUE(std::filesystem::create_directories(separate_root));
    const auto paths = MakePhase4DbPaths(separate_root);

    {
        savor::db::core::DBService initializer(
            paths,
            migrations::MigrationSourceOptions{ .source_kind = migrations::MigrationSourceKind::Embedded });
        std::string err;
        ASSERT_TRUE(initializer.Start(&err)) << err;
        initializer.Stop();
    }

    sqlite3* archive_handle = nullptr;
    ASSERT_EQ(SQLITE_OK, sqlite3_open(paths.archive_db_path.string().c_str(), &archive_handle));
    ASSERT_NE(archive_handle, nullptr);
    savor::db::SqliteArchiveDb archive_db(archive_handle);
    const auto now = savor::db::types::UtcTimePoint(std::chrono::milliseconds(1712304000000));
    std::string err;
    std::int64_t archive_package_id = 0;
    ASSERT_TRUE(archive_db.CreateArchivePackage(
        {
            .source_context = "Workflow",
            .source_root_job_set_id = 0,
            .source_scope_kind = "workflow_selection",
            .source_workflow_count = 2,
            .selection_summary = std::string("{\"workflow_count\":2}"),
            .archive_name = "Projected archive name",
            .archive_notes = std::string("Projected archive notes"),
            .created_at_utc = now,
            .schema_version = 1,
            .event_catalog_version = 1,
            .time_range_start_utc = now,
            .time_range_end_utc = now,
            .manifest_path = "manifest.json",
            .checksum_status = "PASS",
            .correlation_id = "archive-projection",
            .causation_id = "archive-projection",
        },
        &archive_package_id,
        &err))
        << err;
    sqlite3_close(archive_handle);
    archive_handle = nullptr;

    projectors::UiReadProjectionService projection(
        projectors::UiReadProjectionConfig{
            .ui_read_db_path = paths.ui_read_db_path,
            .execution_db_path = paths.execution_db_path,
            .state_db_path = paths.state_db_path,
            .analysis_db_path = paths.analysis_db_path,
            .archive_db_path = paths.archive_db_path,
            .max_batch_size = 100,
            .max_attempts = 5,
            .poll_interval = std::chrono::hours{ 24 },
        });
    ASSERT_TRUE(projection.RunOnce(&err)) << err;

    sqlite3* verify_handle = nullptr;
    ASSERT_EQ(SQLITE_OK, sqlite3_open(paths.ui_read_db_path.string().c_str(), &verify_handle));
    ASSERT_NE(verify_handle, nullptr);
    EXPECT_EQ(ReadText(verify_handle, "SELECT archive_name FROM ui_archive_catalog WHERE archive_package_id=(SELECT MAX(archive_package_id) FROM ui_archive_catalog);"), "Projected archive name");
    EXPECT_EQ(ReadText(verify_handle, "SELECT archive_notes FROM ui_archive_catalog WHERE archive_package_id=(SELECT MAX(archive_package_id) FROM ui_archive_catalog);"), "Projected archive notes");
    EXPECT_EQ(ReadInt64(verify_handle, "SELECT source_workflow_count FROM ui_archive_catalog WHERE archive_package_id=(SELECT MAX(archive_package_id) FROM ui_archive_catalog);"), 2);
    sqlite3_close(verify_handle);
}

TEST_F(SqliteDbFixture, UiReadArchiveCatalogAndRehydrateRequestsListForWorkbench) {
    using namespace savor::db;
    using namespace savor::db::migrations;

    const MigrationSourceOptions embedded_options{ .source_kind = MigrationSourceKind::Embedded };
    std::string err;
    ASSERT_TRUE(ApplyContextMigrations(db_, MigrationContext::UIRead, embedded_options, &err)) << err;

    ASSERT_TRUE(ExecSql(db_, R"SQL(
INSERT INTO ui_archive_catalog(
    archive_package_id,source_context,source_root_job_set_id,source_scope_kind,source_workflow_count,
    selection_summary,archive_name,archive_notes,created_at_utc,schema_version,event_catalog_version,
    time_range_start_utc,time_range_end_utc,checksum_status)
VALUES
    (100,'Workflow',0,'workflow_selection',3,'{"workflow_count":3}','No victory workflows','notes',3000,1,1,1000,3000,'PASS'),
    (99,'Root',50,'root_job_set',0,'{}','Old root package','',2000,1,1,1000,2000,'PASS');
INSERT INTO ui_archive_rehydrate_request(
    rehydrate_request_id,archive_package_id,status,target_namespace,requested_at_utc,completed_at_utc,error_text)
VALUES
    (1001,100,'COMPLETED','rehydrate_100_a',4000,4500,NULL),
    (1002,100,'FAILED','rehydrate_100_b',5000,5500,'collision'),
    (1003,99,'COMPLETED','rehydrate_99_a',6000,6500,NULL);
)SQL"));

    SqliteUiReadDb ui_read_db(db_);
    const auto packages = ui_read_db.ListArchiveCatalog({
        .search = "victory",
        .workflow_packages_only = true,
    });
    ASSERT_EQ(packages.size(), 1u);
    EXPECT_EQ(packages.front().archive_package_id, 100);
    EXPECT_EQ(packages.front().archive_name, "No victory workflows");
    EXPECT_EQ(packages.front().source_scope_kind, "workflow_selection");
    EXPECT_EQ(packages.front().source_workflow_count, 3);

    const auto requests = ui_read_db.ListArchiveRehydrateRequests(100);
    ASSERT_EQ(requests.size(), 2u);
    EXPECT_EQ(requests.front().rehydrate_request_id, 1002);
    EXPECT_EQ(requests.front().status, "FAILED");
    EXPECT_EQ(requests.front().error_text, "collision");
    EXPECT_EQ(requests.back().rehydrate_request_id, 1001);
    EXPECT_EQ(requests.back().target_namespace, "rehydrate_100_a");
}

TEST_F(SqliteDbFixture, UiReadPageCursorsMoveForwardToOlderRowsAndBackToNewerRows) {
    using namespace savor::db;
    using namespace savor::db::migrations;

    const MigrationSourceOptions embedded_options{ .source_kind = MigrationSourceKind::Embedded };
    std::string err;
    ASSERT_TRUE(ApplyContextMigrations(db_, MigrationContext::UIRead, embedded_options, &err)) << err;
    ASSERT_TRUE(ExecSql(db_, R"SQL(
INSERT INTO ui_job_summary(job_id,job_set_id,program_kind,state,priority,queued_at_utc)
VALUES(9003,9103,5,'QUEUED',1,3000),
      (9002,9102,5,'QUEUED',1,2000),
      (9001,9101,5,'QUEUED',1,1000);
INSERT INTO ui_artifact_browser(artifact_id,sha256,size_bytes,artifact_kind,filename,created_at_utc)
VALUES(9203,'sha9203',1,'SAV','a3.sav',3000),
      (9202,'sha9202',1,'SAV','a2.sav',2000),
      (9201,'sha9201',1,'SAV','a1.sav',1000);
INSERT INTO ui_workflow_instance(workflow_instance_id,workflow_kind,state,display_state,root_scope_kind,created_by,created_at_utc,completed_at_utc)
VALUES(9303,'cursor-test','COMPLETED','COMPLETED','manual','test',3000,3001),
      (9302,'cursor-test','COMPLETED','COMPLETED','manual','test',2000,2001),
      (9301,'cursor-test','COMPLETED','COMPLETED','manual','test',1000,1001);
INSERT INTO ui_battle_group(battle_set_id,name,status,created_at_utc,completed_at_utc)
VALUES(9403,'battle-3','done',3000,3001),
      (9402,'battle-2','done',2000,2001),
      (9401,'battle-1','done',1000,1001);
INSERT INTO ui_seed_probe_summary(probe_run_id,probe_set_id,status,neutral_seed_value,grid_count,unique_count,requested_at_utc,completed_at_utc)
VALUES(9503,1,'completed',NULL,0,0,3000,3001),
      (9502,1,'completed',NULL,0,0,2000,2001),
      (9501,1,'completed',NULL,0,0,1000,1001);
)SQL"));

    SqliteUiReadDb ui_read_db(db_);

    auto jobs = ui_read_db.ListJobs({ .limit = 2 });
    ASSERT_EQ(jobs.items.size(), 2u);
    EXPECT_EQ(jobs.items[0].job_id, 9003);
    EXPECT_EQ(jobs.items[1].job_id, 9002);
    auto older_jobs = ui_read_db.ListJobs({ .before = jobs.next, .limit = 2 });
    ASSERT_EQ(older_jobs.items.size(), 1u);
    EXPECT_EQ(older_jobs.items[0].job_id, 9001);
    auto newer_jobs = ui_read_db.ListJobs({ .after = older_jobs.prev, .limit = 2 });
    ASSERT_EQ(newer_jobs.items.size(), 2u);
    EXPECT_EQ(newer_jobs.items[0].job_id, 9003);
    EXPECT_EQ(newer_jobs.items[1].job_id, 9002);

    auto job_sets = ui_read_db.ListJobSets({ .limit = 2 });
    ASSERT_EQ(job_sets.items.size(), 2u);
    EXPECT_EQ(job_sets.items[0].job_set_id, 9103);
    EXPECT_EQ(job_sets.items[1].job_set_id, 9102);
    auto older_job_sets = ui_read_db.ListJobSets({ .before = job_sets.next, .limit = 2 });
    ASSERT_EQ(older_job_sets.items.size(), 1u);
    EXPECT_EQ(older_job_sets.items[0].job_set_id, 9101);
    auto newer_job_sets = ui_read_db.ListJobSets({ .after = older_job_sets.prev, .limit = 2 });
    ASSERT_EQ(newer_job_sets.items.size(), 2u);
    EXPECT_EQ(newer_job_sets.items[0].job_set_id, 9103);
    EXPECT_EQ(newer_job_sets.items[1].job_set_id, 9102);

    auto artifacts = ui_read_db.ListArtifacts({ .limit = 2 });
    ASSERT_EQ(artifacts.items.size(), 2u);
    EXPECT_EQ(artifacts.items[0].artifact_id, 9203);
    EXPECT_EQ(artifacts.items[1].artifact_id, 9202);
    auto older_artifacts = ui_read_db.ListArtifacts({ .before = artifacts.next, .limit = 2 });
    ASSERT_EQ(older_artifacts.items.size(), 1u);
    EXPECT_EQ(older_artifacts.items[0].artifact_id, 9201);
    auto newer_artifacts = ui_read_db.ListArtifacts({ .after = older_artifacts.prev, .limit = 2 });
    ASSERT_EQ(newer_artifacts.items.size(), 2u);
    EXPECT_EQ(newer_artifacts.items[0].artifact_id, 9203);
    EXPECT_EQ(newer_artifacts.items[1].artifact_id, 9202);

    auto workflows = ui_read_db.ListWorkflowInstances({ .limit = 2, .workflow_kind = "cursor-test" });
    ASSERT_EQ(workflows.items.size(), 2u);
    EXPECT_EQ(workflows.items[0].workflow_instance_id, 9303);
    EXPECT_EQ(workflows.items[1].workflow_instance_id, 9302);
    auto older_workflows = ui_read_db.ListWorkflowInstances({ .before = workflows.next, .limit = 2, .workflow_kind = "cursor-test" });
    ASSERT_EQ(older_workflows.items.size(), 1u);
    EXPECT_EQ(older_workflows.items[0].workflow_instance_id, 9301);
    auto newer_workflows = ui_read_db.ListWorkflowInstances({ .after = older_workflows.prev, .limit = 2, .workflow_kind = "cursor-test" });
    ASSERT_EQ(newer_workflows.items.size(), 2u);
    EXPECT_EQ(newer_workflows.items[0].workflow_instance_id, 9303);
    EXPECT_EQ(newer_workflows.items[1].workflow_instance_id, 9302);

    auto battle_groups = ui_read_db.ListBattleGroups({ .limit = 2 });
    ASSERT_EQ(battle_groups.items.size(), 2u);
    EXPECT_EQ(battle_groups.items[0].battle_set_id, 9403);
    EXPECT_EQ(battle_groups.items[1].battle_set_id, 9402);
    auto older_battle_groups = ui_read_db.ListBattleGroups({ .before = battle_groups.next, .limit = 2 });
    ASSERT_EQ(older_battle_groups.items.size(), 1u);
    EXPECT_EQ(older_battle_groups.items[0].battle_set_id, 9401);
    auto newer_battle_groups = ui_read_db.ListBattleGroups({ .after = older_battle_groups.prev, .limit = 2 });
    ASSERT_EQ(newer_battle_groups.items.size(), 2u);
    EXPECT_EQ(newer_battle_groups.items[0].battle_set_id, 9403);
    EXPECT_EQ(newer_battle_groups.items[1].battle_set_id, 9402);

    auto seed_probes = ui_read_db.ListSeedProbeRuns({ .limit = 2 });
    ASSERT_EQ(seed_probes.items.size(), 2u);
    EXPECT_EQ(seed_probes.items[0].probe_run_id, 9503);
    EXPECT_EQ(seed_probes.items[1].probe_run_id, 9502);
    auto older_seed_probes = ui_read_db.ListSeedProbeRuns({ .before = seed_probes.next, .limit = 2 });
    ASSERT_EQ(older_seed_probes.items.size(), 1u);
    EXPECT_EQ(older_seed_probes.items[0].probe_run_id, 9501);
    auto newer_seed_probes = ui_read_db.ListSeedProbeRuns({ .after = older_seed_probes.prev, .limit = 2 });
    ASSERT_EQ(newer_seed_probes.items.size(), 2u);
    EXPECT_EQ(newer_seed_probes.items[0].probe_run_id, 9503);
    EXPECT_EQ(newer_seed_probes.items[1].probe_run_id, 9502);
}

TEST_F(SqliteDbFixture, Stage4WorkflowArchiveExecutePurgesExclusiveWorkflowAndSavestateAfterVerify) {
    using namespace savor::db;
    using namespace savor::db::migrations;
    using savor::runner::parallel::savordb::ArchiveWorkflowCommands;

    const MigrationSourceOptions embedded_options{ .source_kind = MigrationSourceKind::Embedded };
    std::string err;
    ASSERT_TRUE(ApplyContextMigrations(db_, MigrationContext::Execution, embedded_options, &err)) << err;
    ASSERT_TRUE(ApplyContextMigrations(db_, MigrationContext::State, embedded_options, &err)) << err;
    ASSERT_TRUE(ApplyContextMigrations(db_, MigrationContext::UIRead, embedded_options, &err)) << err;
    ASSERT_TRUE(ApplyContextMigrations(db_, MigrationContext::Archive, embedded_options, &err)) << err;

    const auto temp_root = std::filesystem::temp_directory_path() / ("savor-workflow-exec-purge-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    ASSERT_TRUE(std::filesystem::create_directories(temp_root));
    const auto sav_path = temp_root / "exclusive-output.sav";
    {
        std::ofstream out(sav_path, std::ios::binary | std::ios::trunc);
        out << "exclusive-output-savestate";
    }

    const auto sav_sql = std::string("INSERT INTO state_artifact(artifact_id,sha256,size_bytes,compression_kind,filename,file_ext,artifact_kind,created_at_utc) VALUES(30,'exclusivesha',25,'NONE','")
        + sav_path.generic_string() + "','.sav','SAV',1000);"
        + "INSERT INTO state_savestate(savestate_id,artifact_id,savestate_type,note,is_complete,created_at_utc) VALUES(301,30,'OUTPUT','exclusive',1,1000);";
    ASSERT_TRUE(ExecSql(db_, sav_sql.c_str()));
    ASSERT_TRUE(ExecSql(db_, R"SQL(
INSERT INTO exec_workflow_instance(workflow_instance_id,workflow_kind,state,root_scope_kind,created_by,created_at_utc,completed_at_utc)
VALUES(3010,'BATTLE_RUN','COMPLETED','manual','test',1000,2000);
INSERT INTO exec_workflow_step(workflow_step_id,workflow_instance_id,step_key,step_kind,state,priority,attempts,max_attempts,output_ref_kind,output_ref_id,created_at_utc,completed_at_utc)
VALUES(3011,3010,'final','battle.final','COMPLETED',0,0,1,'state.savestate_id',301,1000,2000);
INSERT INTO ui_workflow_instance(workflow_instance_id,workflow_kind,state,display_state,root_scope_kind,created_by,created_at_utc,completed_at_utc,battle_final_victory_count)
VALUES(3010,'BATTLE_RUN','COMPLETED','COMPLETED','manual','test',1000,2000,0);
)SQL"));

    execution::workflow::SqliteExecutionDb execution_db(db_);
    SqliteUiReadDb ui_read_db(db_);
    SqliteArchiveDb archive_db(db_);
    archive::SqliteArchivePackageService package_service(
        db_,
        &execution_db,
        &ui_read_db,
        &archive_db,
        DbConfigPaths{ .archive_store_root = temp_root },
        db_,
        nullptr,
        db_);
    archive::SqliteRehydrateExecutor rehydrate_executor(db_, db_, &archive_db, temp_root, db_);
    ArchiveWorkflowCommands commands(db_, &archive_db, &package_service, &rehydrate_executor);

    const auto now = types::UtcTimePoint(std::chrono::milliseconds(1712304000000));
    const auto summary = commands.WorkflowArchiveExecute({
        .selection = { .workflow_instance_ids = {3010} },
        .now_utc = now,
        .purge_after_verify = true,
        .trace_id = "workflow-execute-purge",
    });

    ASSERT_TRUE(summary.success) << (summary.errors.empty() ? "unknown error" : summary.errors.front());
    EXPECT_GT(summary.archive_package_id, 0);
    EXPECT_TRUE(summary.verify.success);
    EXPECT_TRUE(summary.purge.success) << summary.purge.error.value_or("unknown purge error");
    EXPECT_EQ(ReadInt64(db_, "SELECT COUNT(1) FROM exec_workflow_instance WHERE workflow_instance_id=3010;"), 0);
    EXPECT_EQ(ReadInt64(db_, "SELECT COUNT(1) FROM exec_workflow_step WHERE workflow_instance_id=3010;"), 0);
    EXPECT_EQ(ReadInt64(db_, "SELECT COUNT(1) FROM ui_workflow_instance WHERE workflow_instance_id=3010;"), 0);
    EXPECT_EQ(ReadInt64(db_, "SELECT COUNT(1) FROM state_savestate WHERE savestate_id=301;"), 0);
    EXPECT_EQ(ReadInt64(db_, "SELECT COUNT(1) FROM state_artifact WHERE artifact_id=30;"), 0);
    EXPECT_FALSE(std::filesystem::exists(sav_path));

    std::filesystem::remove_all(temp_root);
}

TEST_F(SqliteDbFixture, Stage4WorkflowArchiveExecuteKeepsSharedSavestateWhenRemainingWorkflowReferencesIt) {
    using namespace savor::db;
    using namespace savor::db::migrations;
    using savor::runner::parallel::savordb::ArchiveWorkflowCommands;

    const MigrationSourceOptions embedded_options{ .source_kind = MigrationSourceKind::Embedded };
    std::string err;
    ASSERT_TRUE(ApplyContextMigrations(db_, MigrationContext::Execution, embedded_options, &err)) << err;
    ASSERT_TRUE(ApplyContextMigrations(db_, MigrationContext::State, embedded_options, &err)) << err;
    ASSERT_TRUE(ApplyContextMigrations(db_, MigrationContext::UIRead, embedded_options, &err)) << err;
    ASSERT_TRUE(ApplyContextMigrations(db_, MigrationContext::Archive, embedded_options, &err)) << err;

    const auto temp_root = std::filesystem::temp_directory_path() / ("savor-workflow-shared-purge-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    ASSERT_TRUE(std::filesystem::create_directories(temp_root));
    const auto sav_path = temp_root / "shared-entry.sav";
    {
        std::ofstream out(sav_path, std::ios::binary | std::ios::trunc);
        out << "shared-entry-savestate";
    }

    const auto sav_sql = std::string("INSERT INTO state_artifact(artifact_id,sha256,size_bytes,compression_kind,filename,file_ext,artifact_kind,created_at_utc) VALUES(40,'sharedpurgesha',21,'NONE','")
        + sav_path.generic_string() + "','.sav','SAV',1000);"
        + "INSERT INTO state_savestate(savestate_id,artifact_id,savestate_type,note,is_complete,created_at_utc) VALUES(401,40,'ENTRY','shared',1,1000);";
    ASSERT_TRUE(ExecSql(db_, sav_sql.c_str()));
    ASSERT_TRUE(ExecSql(db_, R"SQL(
INSERT INTO exec_workflow_instance(workflow_instance_id,workflow_kind,state,root_scope_kind,created_by,created_at_utc,completed_at_utc)
VALUES(4010,'BATTLE_RUN','COMPLETED','manual','test',1000,2000),
      (4020,'BATTLE_RUN','COMPLETED','manual','test',1000,2000);
INSERT INTO exec_workflow_step(workflow_step_id,workflow_instance_id,step_key,step_kind,state,priority,attempts,max_attempts,input_ref_kind,input_ref_id,created_at_utc,completed_at_utc)
VALUES(4011,4010,'entry','battle.entry','COMPLETED',0,0,1,'state.savestate_id',401,1000,2000),
      (4021,4020,'entry','battle.entry','COMPLETED',0,0,1,'state.savestate_id',401,1000,2000);
INSERT INTO ui_workflow_instance(workflow_instance_id,workflow_kind,state,display_state,root_scope_kind,created_by,created_at_utc,completed_at_utc,battle_final_victory_count)
VALUES(4010,'BATTLE_RUN','COMPLETED','COMPLETED','manual','test',1000,2000,0),
      (4020,'BATTLE_RUN','COMPLETED','COMPLETED','manual','test',1000,2000,1);
)SQL"));

    execution::workflow::SqliteExecutionDb execution_db(db_);
    SqliteUiReadDb ui_read_db(db_);
    SqliteArchiveDb archive_db(db_);
    archive::SqliteArchivePackageService package_service(
        db_,
        &execution_db,
        &ui_read_db,
        &archive_db,
        DbConfigPaths{ .archive_store_root = temp_root },
        db_,
        nullptr,
        db_);
    archive::SqliteRehydrateExecutor rehydrate_executor(db_, db_, &archive_db, temp_root, db_);
    ArchiveWorkflowCommands commands(db_, &archive_db, &package_service, &rehydrate_executor);

    const auto now = types::UtcTimePoint(std::chrono::milliseconds(1712304000000));
    const auto summary = commands.WorkflowArchiveExecute({
        .selection = { .workflow_instance_ids = {4010}, .explicit_exclusions = {4020} },
        .now_utc = now,
        .purge_after_verify = true,
        .trace_id = "workflow-shared-purge",
    });

    ASSERT_TRUE(summary.success) << (summary.errors.empty() ? "unknown error" : summary.errors.front());
    EXPECT_TRUE(summary.verify.success);
    EXPECT_TRUE(summary.purge.success) << summary.purge.error.value_or("unknown purge error");
    EXPECT_EQ(ReadInt64(db_, "SELECT COUNT(1) FROM exec_workflow_instance WHERE workflow_instance_id=4010;"), 0);
    EXPECT_EQ(ReadInt64(db_, "SELECT COUNT(1) FROM exec_workflow_instance WHERE workflow_instance_id=4020;"), 1);
    EXPECT_EQ(ReadInt64(db_, "SELECT COUNT(1) FROM ui_workflow_instance WHERE workflow_instance_id=4010;"), 0);
    EXPECT_EQ(ReadInt64(db_, "SELECT COUNT(1) FROM ui_workflow_instance WHERE workflow_instance_id=4020;"), 1);
    EXPECT_EQ(ReadInt64(db_, "SELECT COUNT(1) FROM state_savestate WHERE savestate_id=401;"), 1);
    EXPECT_EQ(ReadInt64(db_, "SELECT COUNT(1) FROM state_artifact WHERE artifact_id=40;"), 1);
    EXPECT_TRUE(std::filesystem::exists(sav_path));

    std::filesystem::remove_all(temp_root);
}

TEST_F(SqliteDbFixture, Stage3Phase1DbContracts_TimeoutRetryOnceThenFailedTerminalState) {
    using namespace savor::db::execution::workflow;

    ASSERT_TRUE(ExecSql(db_, R"SQL(
INSERT INTO exec_workflow_instance(workflow_instance_id, workflow_kind, state, root_scope_kind, created_by, created_at_utc, started_at_utc)
VALUES(1401, 'SEED_PROBE_CHAIN', 'RUNNING', 'manual', 'test', unixepoch()*1000, unixepoch()*1000);
INSERT INTO exec_workflow_step(workflow_step_id, workflow_instance_id, step_key, step_kind, state, priority, attempts, max_attempts, created_at_utc, ready_at_utc)
VALUES(1402, 1401, 'Grid', 'seedprobe.grid', 'READY', 1, 0, 2, unixepoch()*1000, unixepoch()*1000);
)SQL"));

    SqliteExecutionDb execution_db(db_);
    auto* commands = execution_db.WorkflowCommandService();
    ASSERT_NE(commands, nullptr);

    std::string err;
    ASSERT_TRUE(commands->MarkStepTerminal(
        {
            .workflow_step_id = 1402,
            .terminal_state = "FAILED",
            .requested_by = "SavorTests-timeout",
        },
        &err))
        << err;

    sqlite3_stmt* st = nullptr;
    ASSERT_EQ(SQLITE_OK, sqlite3_prepare_v2(db_, "SELECT state FROM exec_workflow_step WHERE workflow_step_id=1402;", -1, &st, nullptr));
    ASSERT_EQ(SQLITE_ROW, sqlite3_step(st));
    ASSERT_NE(sqlite3_column_text(st, 0), nullptr);
    EXPECT_EQ(std::string(reinterpret_cast<const char*>(sqlite3_column_text(st, 0))), "FAILED");
    sqlite3_finalize(st);
}

TEST_F(SqliteDbFixture, Stage3cSeedProbeAdaptersUseAuthoringSpecTimingInFingerprints) {
    using namespace savor::db;
    using namespace savor::db::execution::programdb;
    using namespace savor::db::execution::programdb::seedprobe;
    using namespace savor::db::execution::workflow;

    auto* authoring_db = db_service_->AuthoringDb();
    auto* analysis_db = db_service_->AnalysisDb();
    ASSERT_NE(authoring_db, nullptr);
    ASSERT_NE(analysis_db, nullptr);

    std::string err;
    std::int64_t seed_probe_spec_id = 0;
    ASSERT_TRUE(authoring_db->SaveSeedProbeSpec(
        {
            .name = "timing regression",
            .priority = 1,
            .run_ms = 12345,
            .vi_stall_ms = 678,
            .min_value = 47,
            .max_value = 207,
            .cap_trigger_top = true,
            .ignore_trigger_minmax = true,
            .combo_attempts_per_target = 1,
            .combo_sampler_tries = 1,
            .auto_schedule_battle_run = false,
            .created_at_utc = savor::db::types::UtcNow(),
            .correlation_id = "test.seedprobe.timing",
            .causation_id = "test",
        },
        &seed_probe_spec_id,
        &err))
        << err;

    std::int64_t probe_set_id = 0;
    ASSERT_TRUE(analysis_db->CreateSeedProbeSet(
        {
            .name = "timing probe set",
            .probe_flavor = "BATTLE_PRE",
            .breakpoint_policy_name = "default",
            .segment_source_kind = "manual",
            .created_at_utc = savor::db::types::UtcNow(),
            .correlation_id = "test.seedprobe.timing",
            .causation_id = "test",
        },
        &probe_set_id,
        &err))
        << err;

    std::int64_t probe_run_id = 0;
    ASSERT_TRUE(analysis_db->RequestSeedProbeRun(
        {
            .probe_set_id = probe_set_id,
            .entry_savestate_id = 77,
            .seed_probe_spec_id = seed_probe_spec_id,
            .launch_samples_per_axis = 1,
            .codec_version = 1,
            .status = "queued",
            .requested_at_utc = savor::db::types::UtcNow(),
            .correlation_id = "test.seedprobe.timing",
            .causation_id = "test",
        },
        &probe_run_id,
        &err))
        << err;

    SqliteExecutionDb execution_db(db_);
    auto read_first_job_fingerprint = [&](std::int64_t job_set_id) {
        sqlite3_stmt* st = nullptr;
        EXPECT_EQ(SQLITE_OK, sqlite3_prepare_v2(
            db_,
            "SELECT fingerprint FROM exec_job WHERE job_set_id=?1 ORDER BY job_id LIMIT 1;",
            -1,
            &st,
            nullptr));
        sqlite3_bind_int64(st, 1, job_set_id);
        EXPECT_EQ(SQLITE_ROW, sqlite3_step(st));
        std::string fingerprint;
        if (const auto* text = sqlite3_column_text(st, 0)) {
            fingerprint = reinterpret_cast<const char*>(text);
        }
        sqlite3_finalize(st);
        return fingerprint;
    };
    auto expect_pending_jobs = [&](std::int64_t job_set_id) {
        sqlite3_stmt* st = nullptr;
        ASSERT_EQ(SQLITE_OK, sqlite3_prepare_v2(
            db_,
            "WITH RECURSIVE job_set_descendants(job_set_id) AS ("
            "  SELECT job_set_id FROM exec_job_set WHERE job_set_id=?1 "
            "  UNION ALL "
            "  SELECT child.job_set_id FROM exec_job_set child "
            "  JOIN job_set_descendants parent ON parent.job_set_id=child.parent_job_set_id"
            ") "
            "SELECT COUNT(1), "
            "SUM(CASE WHEN state='PENDING_MATERIALIZATION' THEN 1 ELSE 0 END) "
            "FROM exec_job "
            "WHERE job_set_id IN (SELECT job_set_id FROM job_set_descendants);",
            -1,
            &st,
            nullptr));
        sqlite3_bind_int64(st, 1, job_set_id);
        ASSERT_EQ(SQLITE_ROW, sqlite3_step(st));
        const int total = sqlite3_column_int(st, 0);
        const int pending = sqlite3_column_int(st, 1);
        sqlite3_finalize(st);
        EXPECT_GT(total, 0);
        EXPECT_EQ(pending, total);
    };
    auto expect_priority_range = [&](std::int64_t job_set_id, int expected_min, int expected_max) {
        sqlite3_stmt* st = nullptr;
        ASSERT_EQ(SQLITE_OK, sqlite3_prepare_v2(
            db_,
            "WITH RECURSIVE job_set_descendants(job_set_id) AS ("
            "  SELECT job_set_id FROM exec_job_set WHERE job_set_id=?1 "
            "  UNION ALL "
            "  SELECT child.job_set_id FROM exec_job_set child "
            "  JOIN job_set_descendants parent ON parent.job_set_id=child.parent_job_set_id"
            ") "
            "SELECT COUNT(1), COALESCE(MIN(priority), 0), COALESCE(MAX(priority), 0) "
            "FROM exec_job "
            "WHERE job_set_id IN (SELECT job_set_id FROM job_set_descendants);",
            -1,
            &st,
            nullptr));
        sqlite3_bind_int64(st, 1, job_set_id);
        ASSERT_EQ(SQLITE_ROW, sqlite3_step(st));
        const int total = sqlite3_column_int(st, 0);
        const int min_priority = sqlite3_column_int(st, 1);
        const int max_priority = sqlite3_column_int(st, 2);
        sqlite3_finalize(st);
        EXPECT_GT(total, 0);
        EXPECT_EQ(min_priority, expected_min);
        EXPECT_EQ(max_priority, expected_max);
    };
    auto expect_timing = [](const std::string& fingerprint) {
        EXPECT_NE(fingerprint.find(";run_ms=12345"), std::string::npos) << fingerprint;
        EXPECT_NE(fingerprint.find(";vi=678"), std::string::npos) << fingerprint;
    };
    auto schedule_context = [](std::int64_t domain_ref_id, int step_priority = 42) {
        return WorkflowStepScheduleContext{
            .domain_ref_id = domain_ref_id,
            .step_priority = step_priority,
        };
    };

    auto neutral = BuildSeedProbeNeutralDescriptor(&execution_db, analysis_db, authoring_db);
    const auto neutral_scheduled = neutral.job_persistence->EncodeForQueueing(schedule_context(probe_run_id));
    ASSERT_GT(neutral_scheduled.root_job_set_id, 0);
    expect_timing(neutral_scheduled.persistence.fingerprint);
    expect_timing(read_first_job_fingerprint(neutral_scheduled.root_job_set_id));
    expect_pending_jobs(neutral_scheduled.root_job_set_id);
    expect_priority_range(neutral_scheduled.root_job_set_id, 42, 42);

    SeedProbeGridSpec grid_spec{};
    grid_spec.samples_per_axis = 1;
    auto grid = BuildSeedProbeGridDescriptor(
        &execution_db,
        analysis_db,
        SeedProbeGridBlueprintConfig{},
        grid_spec,
        authoring_db);
    const auto grid_scheduled = grid.job_persistence->EncodeForQueueing(schedule_context(probe_run_id));
    ASSERT_GT(grid_scheduled.root_job_set_id, 0);
    expect_timing(grid_scheduled.persistence.fingerprint);
    expect_timing(read_first_job_fingerprint(grid_scheduled.root_job_set_id));
    expect_pending_jobs(grid_scheduled.root_job_set_id);
    expect_priority_range(grid_scheduled.root_job_set_id, 42, 42);

    ASSERT_TRUE(analysis_db->SetSeedProbeRunNeutralSeed(probe_run_id, 1000, &err)) << err;
    const auto probe_result_id = analysis_db->LookupSeedProbeResultId(probe_run_id);
    ASSERT_TRUE(probe_result_id.has_value());
    ASSERT_TRUE(analysis_db->RecordSeedProbeGridSeed(
        {
            .probe_result_id = *probe_result_id,
            .source_family = "MAIN",
            .axis_xy_id = 0x8080,
            .seed_value = 1001,
            .seed_delta = 1,
            .recorded_at_utc = savor::db::types::UtcNow(),
            .correlation_id = "test.seedprobe.timing",
            .causation_id = "test",
        },
        nullptr,
        &err))
        << err;
    ASSERT_TRUE(analysis_db->RecordSeedProbeGridSeed(
        {
            .probe_result_id = *probe_result_id,
            .source_family = "CSTICK",
            .axis_xy_id = 0x8181,
            .seed_value = 1002,
            .seed_delta = 2,
            .recorded_at_utc = savor::db::types::UtcNow(),
            .correlation_id = "test.seedprobe.timing",
            .causation_id = "test",
        },
        nullptr,
        &err))
        << err;
    ASSERT_TRUE(analysis_db->RecordSeedProbeGridSeed(
        {
            .probe_result_id = *probe_result_id,
            .source_family = "TRIGGER",
            .axis_xy_id = 0x8282,
            .seed_value = 1004,
            .seed_delta = 4,
            .recorded_at_utc = savor::db::types::UtcNow(),
            .correlation_id = "test.seedprobe.timing",
            .causation_id = "test",
        },
        nullptr,
        &err))
        << err;

    auto unique = BuildSeedProbeUniqueDescriptor(
        &execution_db,
        analysis_db,
        SeedProbeGridBlueprintConfig{},
        UniqueIni{},
        {},
        authoring_db);
    const auto unique_scheduled = unique.job_persistence->EncodeForQueueing(schedule_context(probe_run_id));
    expect_timing(unique_scheduled.persistence.fingerprint);
    ASSERT_GT(unique_scheduled.root_job_set_id, 0);
    ASSERT_FALSE(unique_scheduled.event_lines.empty());
    expect_pending_jobs(unique_scheduled.root_job_set_id);
    expect_priority_range(unique_scheduled.root_job_set_id, 42, 43);
    EXPECT_NE(unique_scheduled.event_lines.back().find("combo_attempts_per_target=1"), std::string::npos)
        << unique_scheduled.event_lines.back();
    EXPECT_NE(unique_scheduled.event_lines.back().find("combo_sampler_tries=1"), std::string::npos)
        << unique_scheduled.event_lines.back();

    sqlite3_stmt* child_st = nullptr;
    ASSERT_EQ(SQLITE_OK, sqlite3_prepare_v2(
        db_,
        "SELECT COUNT(1), COALESCE(MAX(expected_total), 0) "
        "FROM exec_job_set WHERE parent_job_set_id=?1;",
        -1,
        &child_st,
        nullptr));
    sqlite3_bind_int64(child_st, 1, unique_scheduled.root_job_set_id);
    ASSERT_EQ(SQLITE_ROW, sqlite3_step(child_st));
    const int child_count = sqlite3_column_int(child_st, 0);
    const int max_child_expected = sqlite3_column_int(child_st, 1);
    sqlite3_finalize(child_st);
    EXPECT_GT(child_count, 0);
    EXPECT_LE(max_child_expected, 1);
}

TEST_F(SqliteDbFixture, Stage3cSeedProbeGridResultMapperPersistsGridSeedFromJobFingerprintWithoutInjectedContext) {
    using namespace savor::db;
    using namespace savor::db::execution::programdb::seedprobe;

    auto* analysis_db = db_service_->AnalysisDb();
    auto* execution_db = db_service_->ExecutionDb();
    ASSERT_NE(analysis_db, nullptr);
    ASSERT_NE(execution_db, nullptr);

    std::string err;
    std::int64_t probe_set_id = 0;
    ASSERT_TRUE(analysis_db->CreateSeedProbeSet(
        {
            .name = "grid mapper fallback probe set",
            .probe_flavor = "BATTLE_PRE",
            .breakpoint_policy_name = "default",
            .segment_source_kind = "manual",
            .created_at_utc = savor::db::types::UtcNow(),
            .correlation_id = "test.seedprobe.grid_mapper_fallback",
            .causation_id = "test",
        },
        &probe_set_id,
        &err))
        << err;

    std::int64_t probe_run_id = 0;
    ASSERT_TRUE(analysis_db->RequestSeedProbeRun(
        {
            .probe_set_id = probe_set_id,
            .entry_savestate_id = 77,
            .seed_probe_spec_id = 1,
            .codec_version = 1,
            .status = "queued",
            .requested_at_utc = savor::db::types::UtcNow(),
            .correlation_id = "test.seedprobe.grid_mapper_fallback",
            .causation_id = "test",
        },
        &probe_run_id,
        &err))
        << err;
    ASSERT_TRUE(analysis_db->LookupSeedProbeResultId(probe_run_id).has_value());
    ASSERT_TRUE(analysis_db->SetSeedProbeRunNeutralSeed(probe_run_id, 1000, &err)) << err;
    ASSERT_EQ(analysis_db->LookupSeedProbeNeutralSeed(probe_run_id), 1000);

    std::int64_t job_set_id = 0;
    ASSERT_TRUE(execution_db->CreateJobSet(
        {
            .program_kind = 3,
            .purpose = "SeedProbe Grid",
            .created_by = std::string("test"),
            .expected_total = 1,
            .domain_ref_kind = std::string("sp_probe_run"),
            .domain_ref_id = probe_run_id,
            .meta_note = std::string("phase=Grid"),
        },
        &job_set_id,
        &err))
        << err;

    auto frame = savor::GCInputFrame::new_stk_main(128, 128);
    const auto fingerprint = std::string("PK=3;PV=1;probe_run_id=")
        + std::to_string(probe_run_id)
        + ";family=main;grid_ref=1;run_ms=1;vi=1;frame="
        + frame.to_frame_hex();

    std::int64_t job_id = 0;
    ASSERT_TRUE(execution_db->EnqueueJob(
        {
            .job_set_id = job_set_id,
            .program_kind = 3,
            .program_version = 1,
            .program_ref_kind = "sp_probe_run",
            .program_ref_id = probe_run_id,
            .fingerprint = fingerprint,
            .priority = 0,
            .max_attempts = 1,
        },
        &job_id,
        &err))
        << err;

    SeedProbeGridResultMapper mapper(execution_db, analysis_db);
    const auto payload = mapper.MapPrimaryResult(
        job_id,
        "[SeedProbe.Results]\nw_err=0\ndw_err=0\nrng_seed=1255\nvi_start=1\nvi_end=2\n");
    ASSERT_EQ(payload.result_kind, "analysisseedprobe.grid_seed");
    ASSERT_GT(payload.result_ref_id, 0);

    const auto replay_payload = mapper.MapPrimaryResult(
        job_id,
        "[SeedProbe.Results]\nw_err=0\ndw_err=0\nrng_seed=1255\nvi_start=1\nvi_end=2\n");
    ASSERT_EQ(replay_payload.result_kind, "analysisseedprobe.grid_seed");
    EXPECT_EQ(replay_payload.result_ref_id, payload.result_ref_id);

    const auto rows = analysis_db->ListSeedProbeGridSeeds(probe_run_id);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows.front().source_family, "MAIN");
    EXPECT_EQ(rows.front().axis_x, 128);
    EXPECT_EQ(rows.front().axis_y, 128);
    EXPECT_EQ(rows.front().seed_value, 1255);
    EXPECT_EQ(rows.front().seed_delta, 255);
}

TEST_F(SqliteDbFixture, Stage3cSeedProbeUniqueResultMapperRecordsInputFrameAndSupersedesMatchingChildSet) {
    using namespace savor::db;
    using namespace savor::db::execution::programdb::seedprobe;

    auto* analysis_db = db_service_->AnalysisDb();
    auto* execution_db = db_service_->ExecutionDb();
    ASSERT_NE(analysis_db, nullptr);
    ASSERT_NE(execution_db, nullptr);

    std::string err;
    std::int64_t probe_set_id = 0;
    ASSERT_TRUE(analysis_db->CreateSeedProbeSet(
        {
            .name = "unique mapper supersede probe set",
            .probe_flavor = "BATTLE_PRE",
            .breakpoint_policy_name = "default",
            .segment_source_kind = "manual",
            .created_at_utc = savor::db::types::UtcNow(),
            .correlation_id = "test.seedprobe.unique_mapper_supersede",
            .causation_id = "test",
        },
        &probe_set_id,
        &err))
        << err;

    std::int64_t probe_run_id = 0;
    ASSERT_TRUE(analysis_db->RequestSeedProbeRun(
        {
            .probe_set_id = probe_set_id,
            .entry_savestate_id = 77,
            .seed_probe_spec_id = 1,
            .codec_version = 1,
            .status = "queued",
            .requested_at_utc = savor::db::types::UtcNow(),
            .correlation_id = "test.seedprobe.unique_mapper_supersede",
            .causation_id = "test",
        },
        &probe_run_id,
        &err))
        << err;
    ASSERT_TRUE(analysis_db->SetSeedProbeRunNeutralSeed(probe_run_id, 1000, &err)) << err;
    ASSERT_TRUE(analysis_db->LookupSeedProbeResultId(probe_run_id).has_value());

    std::int64_t root_job_set_id = 0;
    ASSERT_TRUE(execution_db->CreateJobSet(
        {
            .program_kind = 3,
            .purpose = "SeedProbe Unique",
            .created_by = std::string("test"),
            .expected_total = 2,
            .domain_ref_kind = std::string("sp_probe_run"),
            .domain_ref_id = probe_run_id,
            .meta_note = std::string("phase=Unique"),
        },
        &root_job_set_id,
        &err))
        << err;

    std::int64_t child_job_set_id = 0;
    ASSERT_TRUE(execution_db->CreateJobSet(
        {
            .parent_job_set_id = root_job_set_id,
            .program_kind = 3,
            .purpose = "SeedProbe Unique Delta",
            .created_by = std::string("test"),
            .expected_total = 2,
            .domain_ref_kind = std::string("sp_probe_run"),
            .domain_ref_id = probe_run_id,
            .meta_note = std::string("expected_delta=4"),
        },
        &child_job_set_id,
        &err))
        << err;

    auto frame = savor::GCInputFrame::new_stk_main(120, 136);
    const auto fingerprint = std::string("PK=3;PV=1;phase=unique;probe_run_id=")
        + std::to_string(probe_run_id)
        + ";run_ms=1;vi=1;probe_result_id="
        + std::to_string(*analysis_db->LookupSeedProbeResultId(probe_run_id))
        + ";expected_delta=4;frame="
        + frame.to_frame_hex();

    std::int64_t winner_job_id = 0;
    ASSERT_TRUE(execution_db->EnqueueJob(
        {
            .job_set_id = child_job_set_id,
            .program_kind = 3,
            .program_version = 1,
            .program_ref_kind = "sp_probe_run",
            .program_ref_id = probe_run_id,
            .fingerprint = fingerprint + ";case=winner",
            .priority = 0,
            .max_attempts = 1,
        },
        &winner_job_id,
        &err))
        << err;

    std::int64_t sibling_job_id = 0;
    ASSERT_TRUE(execution_db->EnqueueJob(
        {
            .job_set_id = child_job_set_id,
            .program_kind = 3,
            .program_version = 1,
            .program_ref_kind = "sp_probe_run",
            .program_ref_id = probe_run_id,
            .fingerprint = fingerprint + ";case=sibling",
            .priority = 0,
            .max_attempts = 1,
        },
        &sibling_job_id,
        &err))
        << err;

    SeedProbeUniqueResultMapper mapper(execution_db, analysis_db);
    const auto payload = mapper.MapPrimaryResult(
        winner_job_id,
        "[SeedProbe.Results]\nw_err=0\ndw_err=0\nrng_seed=1004\nvi_start=1\nvi_end=2\n");

    ASSERT_EQ(payload.result_kind, "analysisseedprobe.unique.winner");
    ASSERT_GT(payload.result_ref_id, 0);
    ASSERT_GE(payload.event_lines.size(), 1u);
    const auto superseded_line = std::find_if(
        payload.event_lines.begin(),
        payload.event_lines.end(),
        [](const std::string& line) {
            return line.find("[seedprobe-superseded]") != std::string::npos;
        });
    ASSERT_NE(superseded_line, payload.event_lines.end());
    EXPECT_NE(superseded_line->find("expected_delta=4"), std::string::npos);
    EXPECT_NE(superseded_line->find("observed_delta=4"), std::string::npos);
    EXPECT_NE(superseded_line->find("superseded=1"), std::string::npos);

    const auto winner_job = execution_db->GetJob(winner_job_id);
    const auto sibling_job = execution_db->GetJob(sibling_job_id);
    ASSERT_TRUE(winner_job.has_value());
    ASSERT_TRUE(sibling_job.has_value());
    EXPECT_EQ(winner_job->state, "SUCCEEDED_WINNER");
    EXPECT_EQ(sibling_job->state, "SUPERSEDED");

    sqlite3_stmt* st = nullptr;
    ASSERT_EQ(SQLITE_OK, sqlite3_prepare_v2(
        db_,
        "SELECT f.main_axis_xy_id, f.cstick_axis_xy_id, f.trigger_axis_xy_id "
        "FROM sp_unique_seed u "
        "JOIN sp_input_frame f ON f.input_frame_id=u.input_frame_id "
        "WHERE u.unique_seed_id=?1;",
        -1,
        &st,
        nullptr));
    sqlite3_bind_int64(st, 1, payload.result_ref_id);
    ASSERT_EQ(SQLITE_ROW, sqlite3_step(st));
    EXPECT_EQ(sqlite3_column_int64(st, 0), (120 << 8) | 136);
    EXPECT_EQ(sqlite3_column_int64(st, 1), (128 << 8) | 128);
    EXPECT_EQ(sqlite3_column_int64(st, 2), 0);
    sqlite3_finalize(st);
}

TEST_F(SqliteDbFixture, StateDbMaterializesSavestateToExplicitPath) {
    auto* state_db = db_service_->StateDb();
    ASSERT_NE(state_db, nullptr);

    const auto source_path = temp_root_ / "source.sav";
    {
        std::ofstream out(source_path, std::ios::binary);
        out << "savestate-bytes";
    }
    ASSERT_TRUE(std::filesystem::exists(source_path));

    std::string err;
    std::int64_t artifact_id = 0;
    ASSERT_TRUE(state_db->StoreArtifact(
        {
            .sha256 = "state-db-materialize-savestate-test",
            .size_bytes = static_cast<std::int64_t>(std::filesystem::file_size(source_path)),
            .compression_kind = 0,
            .filename = source_path.string(),
            .file_ext = ".sav",
            .artifact_kind = "SAV",
            .created_at_utc = savor::db::types::UtcNow(),
            .correlation_id = "test.state.materialize_savestate",
            .causation_id = "test",
        },
        &artifact_id,
        &err))
        << err;

    std::int64_t savestate_id = 0;
    ASSERT_TRUE(state_db->CreateSavestate(
        {
            .artifact_id = artifact_id,
            .savestate_type = "TRANSITION",
            .note = "materialize test",
            .is_complete = true,
            .created_at_utc = savor::db::types::UtcNow(),
            .correlation_id = "test.state.materialize_savestate",
            .causation_id = "test",
        },
        &savestate_id,
        &err))
        << err;

    const auto destination_path = temp_root_ / "worker" / "savestate" / "current.sav";
    const auto materialized = state_db->MaterializeSavestateToPath(savestate_id, destination_path.string(), &err);
    ASSERT_TRUE(materialized.has_value()) << err;
    EXPECT_EQ(std::filesystem::path(*materialized), destination_path);
    ASSERT_TRUE(std::filesystem::exists(destination_path));

    const auto payload = state_db->ResolveArtifactPayload(1, "savestate", savestate_id);
    ASSERT_TRUE(payload.has_value());
    EXPECT_EQ(payload->savestate_id, savestate_id);
    EXPECT_EQ(payload->artifact_id, artifact_id);

    std::ifstream in(destination_path, std::ios::binary);
    std::stringstream buffer;
    buffer << in.rdbuf();
    EXPECT_EQ(buffer.str(), "savestate-bytes");
}

TEST_F(SqliteDbFixture, StateDbDedupesArtifactAndUiReadListsSummary) {
    auto* state_db = db_service_->StateDb();
    auto* ui_read_db = db_service_->UiReadDb();
    ASSERT_NE(state_db, nullptr);
    ASSERT_NE(ui_read_db, nullptr);

    const auto first_path = temp_root_ / "first.sav";
    const auto second_path = temp_root_ / "second.sav";
    {
        std::ofstream out(first_path, std::ios::binary);
        out << "duplicate-artifact-bytes";
    }
    {
        std::ofstream out(second_path, std::ios::binary);
        out << "duplicate-artifact-bytes";
    }

    std::string err;
    std::int64_t first_artifact_id = 0;
    ASSERT_TRUE(state_db->StoreArtifact(
        {
            .sha256 = "state-db-dedupe-artifact-test",
            .size_bytes = static_cast<std::int64_t>(std::filesystem::file_size(first_path)),
            .compression_kind = 0,
            .filename = first_path.string(),
            .file_ext = ".sav",
            .artifact_kind = "SAV",
            .created_at_utc = savor::db::types::UtcNow(),
            .correlation_id = "test.state.artifact.dedupe",
            .causation_id = "test",
        },
        &first_artifact_id,
        &err))
        << err;

    std::int64_t second_artifact_id = 0;
    ASSERT_TRUE(state_db->StoreArtifact(
        {
            .sha256 = "state-db-dedupe-artifact-test",
            .size_bytes = static_cast<std::int64_t>(std::filesystem::file_size(second_path)),
            .compression_kind = 0,
            .filename = second_path.string(),
            .file_ext = ".sav",
            .artifact_kind = "SAV",
            .created_at_utc = savor::db::types::UtcNow(),
            .correlation_id = "test.state.artifact.dedupe",
            .causation_id = "test",
        },
        &second_artifact_id,
        &err))
        << err;
    EXPECT_EQ(second_artifact_id, first_artifact_id);

    const auto destination_path = temp_root_ / "materialized" / "dedupe.sav";
    const auto materialized = state_db->MaterializeArtifactToPath(first_artifact_id, destination_path.string(), &err);
    ASSERT_TRUE(materialized.has_value()) << err;
    ASSERT_TRUE(std::filesystem::exists(destination_path));

    savor::db::UiReadArtifactListQuery query{};
    query.search = "second";
    query.limit = 10;
    ASSERT_TRUE(db_service_->RunUiReadProjectionOnce(&err)) << err;
    const auto page = ui_read_db->ListArtifacts(query);
    ASSERT_EQ(page.items.size(), 1);
    EXPECT_EQ(page.items.front().artifact_id, first_artifact_id);
    EXPECT_EQ(page.items.front().filename, "second.sav");
}

TEST_F(SqliteDbFixture, UiReadProjectionStreamsSeparateStateDatabaseForArtifactSummary) {
    namespace migrations = savor::db::migrations;

    const auto separate_root = temp_root_ / "separate";
    ASSERT_TRUE(std::filesystem::create_directories(separate_root));

    savor::db::DbConfigPaths config_paths{};
    config_paths.execution_db_path = separate_root / "execution.sqlite";
    config_paths.state_db_path = separate_root / "state.sqlite";
    config_paths.analysis_db_path = separate_root / "analysis.sqlite";
    config_paths.authoring_db_path = separate_root / "authoring.sqlite";
    config_paths.ui_read_db_path = separate_root / "uiread.sqlite";
    config_paths.archive_db_path = separate_root / "archive.sqlite";

    savor::db::core::DBService service(
        config_paths,
        migrations::MigrationSourceOptions{ .source_kind = migrations::MigrationSourceKind::Embedded });

    std::string err;
    ASSERT_TRUE(service.Start(&err)) << err;

    const auto source_path = separate_root / "attached-source.sav";
    {
        std::ofstream out(source_path, std::ios::binary);
        out << "attached-state-artifact";
    }

    auto* state_db = service.StateDb();
    auto* ui_read_db = service.UiReadDb();
    ASSERT_NE(state_db, nullptr);
    ASSERT_NE(ui_read_db, nullptr);

    std::int64_t artifact_id = 0;
    ASSERT_TRUE(state_db->StoreArtifact(
        {
            .sha256 = "state-db-attached-projection-test",
            .size_bytes = static_cast<std::int64_t>(std::filesystem::file_size(source_path)),
            .compression_kind = 0,
            .filename = source_path.string(),
            .file_ext = ".sav",
            .artifact_kind = "SAV",
            .created_at_utc = savor::db::types::UtcNow(),
            .correlation_id = "test.state.artifact.attached_projection",
            .causation_id = "test",
        },
        &artifact_id,
        &err))
        << err;

    ASSERT_TRUE(service.RunUiReadProjectionOnce(&err)) << err;

    savor::db::UiReadArtifactListQuery query{};
    query.search = "attached-source";
    query.limit = 10;
    const auto page = ui_read_db->ListArtifacts(query);
    ASSERT_EQ(page.items.size(), 1);
    EXPECT_EQ(page.items.front().artifact_id, artifact_id);
    EXPECT_EQ(page.items.front().filename, "attached-source.sav");

    const auto perf = service.SnapshotPerformance();
    ASSERT_TRUE(perf.ui_read_projection.running);
    const auto* state_stream = FindProjectionStream(perf, "state");
    ASSERT_NE(state_stream, nullptr);
    EXPECT_EQ(state_stream->source_context, "State");
    EXPECT_GE(state_stream->source_high_water_outbox_id, 1);
    EXPECT_EQ(state_stream->last_outbox_id, state_stream->source_high_water_outbox_id);
    EXPECT_EQ(state_stream->lag_count, 0);
    EXPECT_EQ(state_stream->failed_run_once_count, 0u);
    EXPECT_TRUE(state_stream->last_error.empty());

    service.Stop();
}

TEST_F(SqliteDbFixture, UiReadProjectionStreamsSeparateExecutionDatabaseForWorkflowSummary) {
    namespace migrations = savor::db::migrations;
    namespace workflow = savor::db::execution::workflow;

    const auto separate_root = temp_root_ / "separate-workflow";
    ASSERT_TRUE(std::filesystem::create_directories(separate_root));

    savor::db::DbConfigPaths config_paths{};
    config_paths.execution_db_path = separate_root / "execution.sqlite";
    config_paths.state_db_path = separate_root / "state.sqlite";
    config_paths.analysis_db_path = separate_root / "analysis.sqlite";
    config_paths.authoring_db_path = separate_root / "authoring.sqlite";
    config_paths.ui_read_db_path = separate_root / "uiread.sqlite";
    config_paths.archive_db_path = separate_root / "archive.sqlite";

    savor::db::core::DBService service(
        config_paths,
        migrations::MigrationSourceOptions{ .source_kind = migrations::MigrationSourceKind::Embedded });

    std::string err;
    ASSERT_TRUE(service.Start(&err)) << err;

    auto* execution_db = service.ExecutionDb();
    auto* ui_read_db = service.UiReadDb();
    ASSERT_NE(execution_db, nullptr);
    ASSERT_NE(ui_read_db, nullptr);

    workflow::WorkflowCreateInstanceCommand create{};
    create.workflow_kind = "workflow_graph";
    create.root_scope_kind = "manual";
    create.workflow_graph_revision_id = 1;
    create.created_by = "test";
    create.created_at_utc = savor::db::types::UtcNow().time_since_epoch().count();
    create.unit_activations.push_back(TestUnitActivation("Manual", "test.manual", "Manual Test Unit"));

    std::int64_t workflow_instance_id = 0;
    ASSERT_TRUE(execution_db->WorkflowCommandService()->CreateWorkflowInstance(
        create,
        &workflow_instance_id,
        &err))
        << err;

    ASSERT_TRUE(service.RunUiReadProjectionOnce(&err)) << err;

    savor::db::UiWorkflowInstanceListQuery query{};
    query.limit = 10;
    const auto page = ui_read_db->ListWorkflowInstances(query);
    const auto instance_it = std::find_if(
        page.items.begin(),
        page.items.end(),
        [workflow_instance_id](const auto& row) {
            return row.workflow_instance_id == workflow_instance_id;
        });
    ASSERT_NE(instance_it, page.items.end());
    EXPECT_EQ(instance_it->workflow_kind, "workflow_graph");
    EXPECT_EQ(instance_it->state, "RUNNING");

    const auto detail = ui_read_db->GetWorkflowDetail(workflow_instance_id);
    ASSERT_TRUE(detail.has_value());
    ASSERT_EQ(detail->steps.size(), 1u);
    EXPECT_EQ(detail->steps.front().step_key, "Manual");
    EXPECT_EQ(detail->steps.front().state, "READY");

    const auto perf = service.SnapshotPerformance();
    ASSERT_TRUE(perf.ui_read_projection.running);
    const auto* execution_stream = FindProjectionStream(perf, "execution");
    ASSERT_NE(execution_stream, nullptr);
    EXPECT_EQ(execution_stream->source_context, "Execution");
    EXPECT_GE(execution_stream->source_high_water_outbox_id, 1);
    EXPECT_EQ(execution_stream->last_outbox_id, execution_stream->source_high_water_outbox_id);
    EXPECT_EQ(execution_stream->lag_count, 0);
    EXPECT_EQ(execution_stream->failed_run_once_count, 0u);
    EXPECT_TRUE(execution_stream->last_error.empty());

    service.Stop();
}

TEST_F(SqliteDbFixture, UiReadProjectionAnalysisBattleResultAndStatusEventsRefreshBattleRows) {
    using namespace savor::db;

    auto* analysis_db = db_service_->AnalysisDb();
    ASSERT_NE(analysis_db, nullptr);

    const auto now = types::UtcTimePoint(std::chrono::milliseconds(1712304000000));
    std::string err;
    std::int64_t battle_set_id = 0;
    ASSERT_TRUE(analysis_db->CreateBattleSet(
        {
            .name = "projection-battle-set",
            .entry_savestate_id = 101,
            .battle_run_spec_id = 202,
            .explorer_settings_id = 303,
            .status = BattleSetStatus::Active,
            .created_at_utc = now,
            .correlation_id = "projection-battle",
            .causation_id = "test",
        },
        &battle_set_id,
        &err)) << err;

    std::int64_t seed_candidate_id = 0;
    ASSERT_TRUE(analysis_db->AddBattleSeedCandidate(
        {
            .battle_set_id = battle_set_id,
            .seed_value = 555,
            .source_kind = BattleSeedCandidateSourceKind::Synthetic,
            .candidate_status = BattleSeedCandidateStatus::Ready,
            .created_at_utc = now,
            .correlation_id = "projection-battle",
            .causation_id = "test",
        },
        &seed_candidate_id,
        &err)) << err;

    std::int64_t wave_id = 0;
    ASSERT_TRUE(analysis_db->CreateBattleTurnWave(
        {
            .battle_set_id = battle_set_id,
            .turn_index = 1,
            .seed_candidate_id = seed_candidate_id,
            .status = BattleTurnWaveStatus::Ready,
            .created_at_utc = now,
            .correlation_id = "projection-battle",
            .causation_id = "test",
        },
        &wave_id,
        &err)) << err;

    std::int64_t turn_job_id = 0;
    ASSERT_TRUE(analysis_db->RecordBattleTurnJob(
        {
            .wave_id = wave_id,
            .exec_job_id = 7001,
            .plan_id = 9001,
            .source_savestate_id = 1234,
            .seed_candidate_id = seed_candidate_id,
            .authored_plan_id = 9001,
            .authored_turn_index = 1,
            .resolved_turn_commands_blob = "01000000000004ffff",
            .resolved_turn_variant_key = "variant-a",
            .fake_attacks_this_turn = 2,
            .fake_attacks_used_before = 1,
            .job_state = BattleTurnJobState::Completed,
            .has_results = true,
            .battle_outcome = savor::battle::Outcome::Defeat,
            .recorded_at_utc = now,
            .correlation_id = "projection-battle",
            .causation_id = "test",
        },
        &turn_job_id,
        &err)) << err;

    RunUiReadProjectionUntilCaughtUp(*db_service_, "analysis-battle");
    EXPECT_EQ(ReadText(db_, ("SELECT job_state FROM ui_battle_turn_job WHERE turn_job_id=" + std::to_string(turn_job_id) + ";").c_str()), "COMPLETED");
    EXPECT_EQ(ReadText(db_, ("SELECT resolved_turn_variant_key FROM ui_battle_turn_job_replication WHERE turn_job_id=" + std::to_string(turn_job_id) + ";").c_str()), "variant-a");
    EXPECT_EQ(ReadInt64(db_, ("SELECT source_savestate_id FROM ui_battle_turn_job_replication WHERE turn_job_id=" + std::to_string(turn_job_id) + ";").c_str()), 1234);
    EXPECT_EQ(ReadInt64(db_, ("SELECT seed_candidate_id FROM ui_battle_turn_job_replication WHERE turn_job_id=" + std::to_string(turn_job_id) + ";").c_str()), seed_candidate_id);
    EXPECT_EQ(ReadText(db_, ("SELECT status FROM ui_battle_wave WHERE wave_id=" + std::to_string(wave_id) + ";").c_str()), "READY");
    EXPECT_EQ(ReadText(db_, ("SELECT status FROM ui_battle_group WHERE battle_set_id=" + std::to_string(battle_set_id) + ";").c_str()), "ACTIVE");

    ASSERT_TRUE(analysis_db->UpdateBattleTurnJobResult(
        {
            .exec_job_id = 7001,
            .job_state = BattleTurnJobState::Succeeded,
            .ended_at_utc = now,
            .has_results = true,
            .vi_start = 44,
            .vi_end = 74,
            .delta_vi = 30,
            .battle_outcome = savor::battle::Outcome::Victory,
            .recorded_at_utc = now,
            .correlation_id = "projection-battle",
            .causation_id = "test",
        },
        &err)) << err;

    ASSERT_TRUE(analysis_db->UpdateBattleTurnWaveStatus(wave_id, BattleTurnWaveStatus::Completed, now, &err)) << err;
    ASSERT_TRUE(analysis_db->UpdateBattleSetStatus(battle_set_id, BattleSetStatus::Victory, now, &err)) << err;

    RunUiReadProjectionUntilCaughtUp(*db_service_, "analysis-battle");
    EXPECT_EQ(ReadText(db_, ("SELECT job_state FROM ui_battle_turn_job WHERE turn_job_id=" + std::to_string(turn_job_id) + ";").c_str()), "SUCCEEDED");
    EXPECT_EQ(ReadText(db_, ("SELECT status FROM ui_battle_wave WHERE wave_id=" + std::to_string(wave_id) + ";").c_str()), "COMPLETED");
    EXPECT_EQ(ReadText(db_, ("SELECT status FROM ui_battle_group WHERE battle_set_id=" + std::to_string(battle_set_id) + ";").c_str()), "VICTORY");
    EXPECT_EQ(
        ReadInt64(db_, "SELECT last_outbox_id FROM ui_projection_subscription WHERE stream_id='analysis-battle';"),
        ReadInt64(db_, "SELECT COALESCE(MAX(outbox_id),0) FROM ab_outbox_message;"));
    EXPECT_EQ(
        ReadInt64(db_, ("SELECT COUNT(*) FROM ab_outbox_message WHERE event_type='AnalysisBattle.TurnJobResultUpdated.v1' AND payload_ref_kind='turn_job' AND payload_ref_id=" + std::to_string(turn_job_id) + ";").c_str()),
        1);
    EXPECT_EQ(
        ReadInt64(db_, ("SELECT COUNT(*) FROM ab_outbox_message WHERE event_type='AnalysisBattle.TurnWaveStatusUpdated.v1' AND payload_ref_kind='turn_wave' AND payload_ref_id=" + std::to_string(wave_id) + ";").c_str()),
        1);
    EXPECT_EQ(
        ReadInt64(db_, ("SELECT COUNT(*) FROM ab_outbox_message WHERE event_type='AnalysisBattle.BattleSetStatusUpdated.v1' AND payload_ref_kind='battle_set' AND payload_ref_id=" + std::to_string(battle_set_id) + ";").c_str()),
        1);
    EXPECT_FALSE(analysis_db->UpdateBattleTurnJobResult(
        {
            .exec_job_id = 999999,
            .job_state = BattleTurnJobState::Succeeded,
            .has_results = true,
            .recorded_at_utc = now,
        },
        &err));
    EXPECT_FALSE(analysis_db->UpdateBattleTurnWaveStatus(999999, BattleTurnWaveStatus::Completed, now, &err));
    EXPECT_FALSE(analysis_db->UpdateBattleSetStatus(999999, BattleSetStatus::Victory, now, &err));
    EXPECT_EQ(
        ReadInt64(db_, "SELECT COUNT(*) FROM ab_outbox_message WHERE event_type IN ('AnalysisBattle.TurnJobResultUpdated.v1','AnalysisBattle.TurnWaveStatusUpdated.v1','AnalysisBattle.BattleSetStatusUpdated.v1');"),
        3);
}

TEST_F(SqliteDbFixture, AuthoringPredicateSpecPersistsMultipleRequiredBreakpoints) {
    using namespace savor::db;
    using namespace savor::db::migrations;

    const MigrationSourceOptions embedded_options{ .source_kind = MigrationSourceKind::Embedded };
    std::string err;
    ASSERT_TRUE(ApplyContextMigrations(db_, MigrationContext::Authoring, embedded_options, &err)) << err;

    SqliteAuthoringDb authoring_db(db_);
    const auto now = types::UtcTimePoint(std::chrono::milliseconds(1712304000000));

    std::int64_t predicate_spec_id = 0;
    ASSERT_TRUE(authoring_db.SavePredicateSpec(
        {
            .name = "multi-trigger-predicate",
            .required_breakpoint_ids = { bp::battle::TurnIsReady, bp::battle::EndTurn },
            .lhs_value = 0x1000,
            .rhs_value = 0,
            .cmp_op = PredicateComparisonOp::GT,
            .width = 2,
            .flag_mask = static_cast<std::int64_t>(static_cast<std::uint32_t>(savor::pred::PredFlag::Active)),
            .created_at_utc = now,
            .correlation_id = "multi-bp-save",
        },
        &predicate_spec_id,
        &err)) << err;

    const auto saved = authoring_db.GetPredicateSpec(predicate_spec_id);
    ASSERT_TRUE(saved.has_value());
    EXPECT_EQ(saved->breakpoint_id, bp::battle::TurnIsReady);
    EXPECT_EQ(saved->required_breakpoint_ids, std::vector<BPKey>({ bp::battle::TurnIsReady, bp::battle::EndTurn }));

    const auto listed = authoring_db.ListPredicateSpecs(10);
    ASSERT_FALSE(listed.empty());
    EXPECT_EQ(listed.front().predicate_spec_id, predicate_spec_id);
    EXPECT_EQ(listed.front().breakpoint_id, bp::battle::TurnIsReady);
    EXPECT_EQ(listed.front().required_breakpoint_ids, std::vector<BPKey>({ bp::battle::TurnIsReady, bp::battle::EndTurn }));

    ASSERT_TRUE(authoring_db.UpdatePredicateSpec(
        predicate_spec_id,
        {
            .name = "multi-trigger-predicate-updated",
            .required_breakpoint_ids = { bp::battle::StartAction, bp::battle::EndAction, bp::battle::EndTurn },
            .lhs_value = 0x1000,
            .rhs_value = 0,
            .cmp_op = PredicateComparisonOp::GT,
            .width = 2,
            .flag_mask = static_cast<std::int64_t>(static_cast<std::uint32_t>(savor::pred::PredFlag::Active)),
            .created_at_utc = now,
            .correlation_id = "multi-bp-update",
        },
        &err)) << err;

    const auto updated = authoring_db.GetPredicateSpec(predicate_spec_id);
    ASSERT_TRUE(updated.has_value());
    EXPECT_EQ(updated->breakpoint_id, bp::battle::StartAction);
    EXPECT_EQ(updated->required_breakpoint_ids, std::vector<BPKey>({ bp::battle::StartAction, bp::battle::EndAction, bp::battle::EndTurn }));

    std::int64_t legacy_scalar_predicate_id = 0;
    ASSERT_TRUE(authoring_db.SavePredicateSpec(
        {
            .name = "legacy-scalar-predicate",
            .breakpoint_id = bp::battle::EndTurn,
            .lhs_value = 0x1000,
            .rhs_value = 0,
            .cmp_op = PredicateComparisonOp::GT,
            .width = 2,
            .flag_mask = static_cast<std::int64_t>(static_cast<std::uint32_t>(savor::pred::PredFlag::Active)),
            .created_at_utc = now,
            .correlation_id = "legacy-scalar-save",
        },
        &legacy_scalar_predicate_id,
        &err)) << err;
    const auto legacy_scalar = authoring_db.GetPredicateSpec(legacy_scalar_predicate_id);
    ASSERT_TRUE(legacy_scalar.has_value());
    EXPECT_EQ(legacy_scalar->breakpoint_id, bp::battle::EndTurn);
    EXPECT_EQ(legacy_scalar->required_breakpoint_ids, std::vector<BPKey>({ bp::battle::EndTurn }));

    std::int64_t invalid_id = 0;
    EXPECT_FALSE(authoring_db.SavePredicateSpec(
        {
            .name = "missing-trigger",
            .lhs_value = 0x1000,
            .rhs_value = 0,
            .cmp_op = PredicateComparisonOp::GT,
            .width = 2,
            .flag_mask = static_cast<std::int64_t>(static_cast<std::uint32_t>(savor::pred::PredFlag::Active)),
            .created_at_utc = now,
        },
        &invalid_id,
        &err));
    EXPECT_FALSE(authoring_db.SavePredicateSpec(
        {
            .name = "invalid-trigger",
            .required_breakpoint_ids = { static_cast<BPKey>(0) },
            .lhs_value = 0x1000,
            .rhs_value = 0,
            .cmp_op = PredicateComparisonOp::GT,
            .width = 2,
            .flag_mask = static_cast<std::int64_t>(static_cast<std::uint32_t>(savor::pred::PredFlag::Active)),
            .created_at_utc = now,
        },
        &invalid_id,
        &err));
}

TEST_F(SqliteDbFixture, PredicateTableExpandsRequiredBreakpointList) {
    using namespace savor;

    pred::Spec spec{};
    spec.id = 7;
    spec.required_bp = bp::battle::TurnIsReady;
    spec.required_bps = {
        static_cast<std::uint16_t>(bp::battle::TurnIsReady),
        static_cast<std::uint16_t>(bp::battle::EndTurn),
    };
    spec.width = 2;
    spec.cmp = pred::CmpOp::GT;
    spec.flags = static_cast<std::uint32_t>(pred::PredFlag::Active);
    spec.lhs_addr = 0x1000;
    spec.rhs_value = 0;
    spec.name = "multi-trigger";

    std::vector<pred::PredicateRecord> records;
    std::vector<std::uint8_t> blob;
    ASSERT_TRUE(pred::BuildTable({ spec }, records, blob));
    ASSERT_EQ(records.size(), 2);
    EXPECT_EQ(records[0].id, 7);
    EXPECT_EQ(records[1].id, 7);
    EXPECT_EQ(records[0].required_bp, bp::battle::TurnIsReady);
    EXPECT_EQ(records[1].required_bp, bp::battle::EndTurn);
}

TEST_F(SqliteDbFixture, UiReadProjectionAnalysisBattleSecondTurnSingleTurnJobsRefreshFromResultEvents) {
    using namespace savor::db;

    auto* analysis_db = db_service_->AnalysisDb();
    ASSERT_NE(analysis_db, nullptr);

    const auto now = types::UtcTimePoint(std::chrono::milliseconds(1712304100000));
    std::string err;
    std::int64_t battle_set_id = 0;
    ASSERT_TRUE(analysis_db->CreateBattleSet(
        {
            .name = "projection-turn-two-set",
            .entry_savestate_id = 111,
            .battle_run_spec_id = 222,
            .explorer_settings_id = 333,
            .status = BattleSetStatus::Active,
            .created_at_utc = now,
            .correlation_id = "projection-turn-two",
            .causation_id = "test",
        },
        &battle_set_id,
        &err)) << err;

    std::int64_t seed_candidate_id = 0;
    ASSERT_TRUE(analysis_db->AddBattleSeedCandidate(
        {
            .battle_set_id = battle_set_id,
            .seed_value = 777,
            .source_kind = BattleSeedCandidateSourceKind::Synthetic,
            .candidate_status = BattleSeedCandidateStatus::Ready,
            .created_at_utc = now,
            .correlation_id = "projection-turn-two",
            .causation_id = "test",
        },
        &seed_candidate_id,
        &err)) << err;

    std::int64_t wave_id = 0;
    ASSERT_TRUE(analysis_db->CreateBattleTurnWave(
        {
            .battle_set_id = battle_set_id,
            .turn_index = 2,
            .seed_candidate_id = seed_candidate_id,
            .status = BattleTurnWaveStatus::Running,
            .created_at_utc = now,
            .correlation_id = "projection-turn-two",
            .causation_id = "test",
        },
        &wave_id,
        &err)) << err;

    std::array<std::int64_t, 4> turn_job_ids{};
    for (int index = 0; index < 4; ++index) {
        ASSERT_TRUE(analysis_db->RecordBattleTurnJob(
            {
                .wave_id = wave_id,
                .exec_job_id = 7101 + index,
                .plan_id = 9101 + index,
                .fake_attacks_this_turn = 1,
                .fake_attacks_used_before = 2,
                .job_state = BattleTurnJobState::Queued,
                .has_results = false,
                .recorded_at_utc = now,
                .correlation_id = "projection-turn-two",
                .causation_id = "test",
            },
            &turn_job_ids[index],
            &err)) << err;
    }

    RunUiReadProjectionUntilCaughtUp(*db_service_, "analysis-battle");
    EXPECT_EQ(
        ReadInt64(db_, ("SELECT COUNT(*) FROM ui_battle_turn_job WHERE wave_id=" + std::to_string(wave_id) + " AND job_state='QUEUED';").c_str()),
        4);
    EXPECT_EQ(ReadText(db_, ("SELECT status FROM ui_battle_wave WHERE wave_id=" + std::to_string(wave_id) + ";").c_str()), "RUNNING");

    for (int index = 0; index < 4; ++index) {
        ASSERT_TRUE(analysis_db->UpdateBattleTurnJobResult(
            {
                .exec_job_id = 7101 + index,
                .job_state = BattleTurnJobState::Succeeded,
                .ended_at_utc = now,
                .has_results = true,
                .vi_start = 20 + index,
                .vi_end = 30 + index,
                .delta_vi = 10,
                .battle_outcome = savor::battle::Outcome::Victory,
                .recorded_at_utc = now,
                .correlation_id = "projection-turn-two",
                .causation_id = "test",
            },
            &err)) << err;
    }
    ASSERT_TRUE(analysis_db->UpdateBattleTurnWaveStatus(wave_id, BattleTurnWaveStatus::Completed, now, &err)) << err;
    ASSERT_TRUE(analysis_db->UpdateBattleSetStatus(battle_set_id, BattleSetStatus::Victory, now, &err)) << err;

    RunUiReadProjectionUntilCaughtUp(*db_service_, "analysis-battle");
    EXPECT_EQ(
        ReadInt64(db_, ("SELECT COUNT(*) FROM ui_battle_turn_job WHERE wave_id=" + std::to_string(wave_id) + " AND job_state='SUCCEEDED';").c_str()),
        4);
    EXPECT_EQ(
        ReadInt64(db_, ("SELECT COUNT(*) FROM ui_battle_turn_job WHERE wave_id=" + std::to_string(wave_id) + " AND job_state='QUEUED';").c_str()),
        0);
    EXPECT_EQ(ReadText(db_, ("SELECT status FROM ui_battle_wave WHERE wave_id=" + std::to_string(wave_id) + ";").c_str()), "COMPLETED");
    EXPECT_EQ(ReadText(db_, ("SELECT status FROM ui_battle_group WHERE battle_set_id=" + std::to_string(battle_set_id) + ";").c_str()), "VICTORY");
    EXPECT_EQ(
        ReadInt64(db_, ("SELECT COUNT(*) FROM ab_outbox_message WHERE event_type='AnalysisBattle.TurnJobResultUpdated.v1' AND payload_ref_kind='turn_job' AND aggregate_id='" + std::to_string(battle_set_id) + "';").c_str()),
        4);
}

TEST_F(SqliteDbFixture, UiReadProjectionAnalysisBattleRefreshesParentAggregateForTurnJobAndFollowupRows) {
    using namespace savor::db;

    auto* analysis_db = db_service_->AnalysisDb();
    ASSERT_NE(analysis_db, nullptr);

    const auto now = types::UtcTimePoint(std::chrono::milliseconds(1712305000000));
    std::string err;
    std::int64_t battle_set_id = 0;
    ASSERT_TRUE(analysis_db->CreateBattleSet(
        {
            .name = "projection-incremental-battle",
            .entry_savestate_id = 101,
            .battle_run_spec_id = 202,
            .explorer_settings_id = 303,
            .status = BattleSetStatus::Active,
            .created_at_utc = now,
            .correlation_id = "projection-incremental",
            .causation_id = "test",
        },
        &battle_set_id,
        &err)) << err;

    std::int64_t seed_candidate_id = 0;
    ASSERT_TRUE(analysis_db->AddBattleSeedCandidate(
        {
            .battle_set_id = battle_set_id,
            .seed_value = 777,
            .source_kind = BattleSeedCandidateSourceKind::Synthetic,
            .candidate_status = BattleSeedCandidateStatus::Ready,
            .created_at_utc = now,
            .correlation_id = "projection-incremental",
            .causation_id = "test",
        },
        &seed_candidate_id,
        &err)) << err;

    std::int64_t wave_id = 0;
    ASSERT_TRUE(analysis_db->CreateBattleTurnWave(
        {
            .battle_set_id = battle_set_id,
            .turn_index = 1,
            .seed_candidate_id = seed_candidate_id,
            .status = BattleTurnWaveStatus::Ready,
            .created_at_utc = now,
            .correlation_id = "projection-incremental",
            .causation_id = "test",
        },
        &wave_id,
        &err)) << err;

    std::int64_t first_turn_job_id = 0;
    ASSERT_TRUE(analysis_db->RecordBattleTurnJob(
        {
            .wave_id = wave_id,
            .exec_job_id = 8001,
            .plan_id = 9001,
            .job_state = BattleTurnJobState::Completed,
            .has_results = true,
            .recorded_at_utc = now,
            .correlation_id = "projection-incremental",
            .causation_id = "test",
        },
        &first_turn_job_id,
        &err)) << err;

    RunUiReadProjectionUntilCaughtUp(*db_service_, "analysis-battle");
    EXPECT_EQ(ReadText(db_, ("SELECT status FROM ui_battle_wave WHERE wave_id=" + std::to_string(wave_id) + ";").c_str()), "READY");

    ASSERT_TRUE(ExecSql(db_, ("UPDATE ab_turn_wave SET status='COMPLETED' WHERE wave_id=" + std::to_string(wave_id) + ";").c_str()));

    std::int64_t second_turn_job_id = 0;
    ASSERT_TRUE(analysis_db->RecordBattleTurnJob(
        {
            .wave_id = wave_id,
            .exec_job_id = 8002,
            .plan_id = 9001,
            .job_state = BattleTurnJobState::Succeeded,
            .has_results = true,
            .recorded_at_utc = now,
            .correlation_id = "projection-incremental",
            .causation_id = "test",
        },
        &second_turn_job_id,
        &err)) << err;

    RunUiReadProjectionUntilCaughtUp(*db_service_, "analysis-battle");
    EXPECT_EQ(ReadText(db_, ("SELECT status FROM ui_battle_wave WHERE wave_id=" + std::to_string(wave_id) + ";").c_str()), "COMPLETED");
    EXPECT_EQ(ReadText(db_, ("SELECT job_state FROM ui_battle_turn_job WHERE turn_job_id=" + std::to_string(second_turn_job_id) + ";").c_str()), "SUCCEEDED");

    ASSERT_TRUE(ExecSql(db_, ("UPDATE ab_turn_wave SET status='RUNNING' WHERE wave_id=" + std::to_string(wave_id) + ";").c_str()));

    std::int64_t manual_followup_id = 0;
    ASSERT_TRUE(analysis_db->UpsertBattleManualFollowup(
        {
            .turn_job_id = first_turn_job_id,
            .manual_followup_status = BattleManualFollowupStatus::Recorded,
            .recorded_dtm_artifact_id = 777,
            .note = std::string("incremental-followup"),
            .updated_at_utc = now,
            .correlation_id = "projection-incremental",
            .causation_id = "test",
        },
        &manual_followup_id,
        &err)) << err;

    RunUiReadProjectionUntilCaughtUp(*db_service_, "analysis-battle");
    EXPECT_EQ(ReadText(db_, ("SELECT status FROM ui_battle_wave WHERE wave_id=" + std::to_string(wave_id) + ";").c_str()), "RUNNING");
    EXPECT_EQ(ReadText(db_, ("SELECT note FROM ui_battle_manual_followup WHERE turn_job_id=" + std::to_string(first_turn_job_id) + ";").c_str()), "incremental-followup");
}

TEST_F(SqliteDbFixture, UiReadBattleQueriesListGroupsWavesJobsAndDetail) {
    using namespace savor::db;

    auto* analysis_db = db_service_->AnalysisDb();
    auto* ui_read_db = db_service_->UiReadDb();
    ASSERT_NE(analysis_db, nullptr);
    ASSERT_NE(ui_read_db, nullptr);

    const auto now = types::UtcTimePoint(std::chrono::milliseconds(1712306000000));
    std::string err;
    std::int64_t battle_set_id = 0;
    ASSERT_TRUE(analysis_db->CreateBattleSet(
        {
            .name = "query-battle-run",
            .entry_savestate_id = 101,
            .battle_run_spec_id = 202,
            .explorer_settings_id = 303,
            .status = BattleSetStatus::Active,
            .created_at_utc = now,
            .correlation_id = "query-battle",
            .causation_id = "test",
        },
        &battle_set_id,
        &err)) << err;

    std::int64_t seed_candidate_id = 0;
    ASSERT_TRUE(analysis_db->AddBattleSeedCandidate(
        {
            .battle_set_id = battle_set_id,
            .seed_value = 999,
            .source_kind = BattleSeedCandidateSourceKind::Synthetic,
            .candidate_status = BattleSeedCandidateStatus::Ready,
            .created_at_utc = now,
            .correlation_id = "query-battle",
            .causation_id = "test",
        },
        &seed_candidate_id,
        &err)) << err;

    std::int64_t wave_id = 0;
    ASSERT_TRUE(analysis_db->CreateBattleTurnWave(
        {
            .battle_set_id = battle_set_id,
            .turn_index = 2,
            .seed_candidate_id = seed_candidate_id,
            .status = BattleTurnWaveStatus::Ready,
            .created_at_utc = now,
            .correlation_id = "query-battle",
            .causation_id = "test",
        },
        &wave_id,
        &err)) << err;

    std::int64_t turn_job_id = 0;
    ASSERT_TRUE(analysis_db->RecordBattleTurnJob(
        {
            .wave_id = wave_id,
            .exec_job_id = 9001,
            .plan_id = 42,
            .fake_attacks_this_turn = 4,
            .fake_attacks_used_before = 3,
            .job_state = BattleTurnJobState::Completed,
            .has_results = true,
            .battle_outcome = savor::battle::Outcome::Victory,
            .recorded_at_utc = now,
            .correlation_id = "query-battle",
            .causation_id = "test",
        },
        &turn_job_id,
        &err)) << err;

    std::int64_t candidate_turn_job_id = 0;
    ASSERT_TRUE(analysis_db->RecordBattleTurnJob(
        {
            .wave_id = wave_id,
            .exec_job_id = 9002,
            .plan_id = 42,
            .job_state = BattleTurnJobState::Completed,
            .has_results = true,
            .battle_outcome = savor::battle::Outcome::ReachedNextTurn,
            .recorded_at_utc = now,
            .correlation_id = "query-battle",
            .causation_id = "test",
        },
        &candidate_turn_job_id,
        &err)) << err;

    std::int64_t miss_turn_job_id = 0;
    ASSERT_TRUE(analysis_db->RecordBattleTurnJob(
        {
            .wave_id = wave_id,
            .exec_job_id = 9003,
            .plan_id = 42,
            .job_state = BattleTurnJobState::Completed,
            .has_results = true,
            .battle_outcome = savor::battle::Outcome::Defeat,
            .recorded_at_utc = now,
            .correlation_id = "query-battle",
            .causation_id = "test",
        },
        &miss_turn_job_id,
        &err)) << err;

    std::int64_t pool_id = 0;
    ASSERT_TRUE(analysis_db->CreateBattleAdvancementPool(
        {
            .battle_set_id = battle_set_id,
            .turn_index = 2,
            .pool_name = "query-advancement",
            .criterion_kind = BattleAdvancementCriterionKind::BestFakeAttacksByRngSeed,
            .created_at_utc = now,
            .correlation_id = "query-battle",
            .causation_id = "test",
        },
        &pool_id,
        &err)) << err;

    ASSERT_TRUE(analysis_db->RecordBattleAdvancementDecision(
        {
            .battle_advancement_pool_id = pool_id,
            .turn_job_id = turn_job_id,
            .decision_kind = BattleAdvancementDecisionKind::Selected,
            .decision_reason = std::string("selected"),
            .created_at_utc = now,
            .correlation_id = "query-battle",
            .causation_id = "test",
        },
        nullptr,
        &err)) << err;
    ASSERT_TRUE(analysis_db->RecordBattleAdvancementDecision(
        {
            .battle_advancement_pool_id = pool_id,
            .turn_job_id = candidate_turn_job_id,
            .decision_kind = BattleAdvancementDecisionKind::NotSelected,
            .decision_reason = std::string("candidate"),
            .created_at_utc = now,
            .correlation_id = "query-battle",
            .causation_id = "test",
        },
        nullptr,
        &err)) << err;

    std::int64_t followup_id = 0;
    ASSERT_TRUE(analysis_db->UpsertBattleManualFollowup(
        {
            .turn_job_id = turn_job_id,
            .manual_followup_status = BattleManualFollowupStatus::Recorded,
            .recorded_dtm_artifact_id = 7777,
            .note = std::string("query-followup"),
            .updated_at_utc = now,
            .correlation_id = "query-battle",
            .causation_id = "test",
        },
        &followup_id,
        &err)) << err;

    RunUiReadProjectionUntilCaughtUp(*db_service_, "analysis-battle");

    UiBattleGroupListQuery group_query{};
    group_query.limit = 10;
    const auto groups = ui_read_db->ListBattleGroups(group_query);
    const auto group_it = std::find_if(
        groups.items.begin(),
        groups.items.end(),
        [battle_set_id](const auto& group) { return group.battle_set_id == battle_set_id; });
    ASSERT_NE(group_it, groups.items.end());
    EXPECT_EQ(group_it->name, "query-battle-run");
    EXPECT_EQ(group_it->wave_count, 1);
    EXPECT_EQ(group_it->job_count, 3);
    EXPECT_EQ(group_it->selected_count, 1);
    EXPECT_EQ(group_it->desired_outcome_count, 2);
    EXPECT_EQ(group_it->final_victory_count, 1);
    EXPECT_EQ(group_it->manual_followup_count, 1);
    EXPECT_EQ(group_it->advancement_rank, 2);

    UiBattleGroupListQuery victory_group_query{};
    victory_group_query.limit = 10;
    victory_group_query.final_victory_only = true;
    const auto victory_groups = ui_read_db->ListBattleGroups(victory_group_query);
    EXPECT_NE(
        std::find_if(
            victory_groups.items.begin(),
            victory_groups.items.end(),
            [battle_set_id](const auto& group) { return group.battle_set_id == battle_set_id; }),
        victory_groups.items.end());

    const auto waves = ui_read_db->ListBattleWaves(battle_set_id);
    ASSERT_EQ(waves.size(), 1u);
    EXPECT_EQ(waves.front().wave_id, wave_id);
    EXPECT_EQ(waves.front().turn_index, 2);
    EXPECT_EQ(waves.front().selected_count, 1);
    EXPECT_EQ(waves.front().desired_outcome_count, 2);
    EXPECT_EQ(waves.front().final_victory_count, 1);
    EXPECT_EQ(waves.front().advancement_rank, 2);

    const auto jobs = ui_read_db->ListBattleTurnJobsForWaves({ wave_id });
    ASSERT_EQ(jobs.size(), 3u);
    EXPECT_EQ(jobs.front().turn_job_id, turn_job_id);
    ASSERT_TRUE(jobs.front().exec_job_id.has_value());
    EXPECT_EQ(*jobs.front().exec_job_id, 9001);
    EXPECT_EQ(jobs.front().fake_attacks_this_turn, 4);
    EXPECT_TRUE(jobs.front().selected_for_advancement);
    EXPECT_TRUE(jobs.front().has_desired_outcome);
    EXPECT_TRUE(jobs.front().has_final_victory_outcome);
    EXPECT_EQ(jobs.front().advancement_rank, 2);
    ASSERT_TRUE(jobs.front().advancement_decision.has_value());
    EXPECT_EQ(jobs.front().advancement_decision->decision_kind, "SELECTED");
    ASSERT_TRUE(jobs.front().manual_followup.has_value());
    EXPECT_EQ(jobs.front().manual_followup->note, "query-followup");

    const auto candidate_it = std::find_if(jobs.begin(), jobs.end(), [candidate_turn_job_id](const auto& job) {
        return job.turn_job_id == candidate_turn_job_id;
    });
    ASSERT_NE(candidate_it, jobs.end());
    EXPECT_FALSE(candidate_it->selected_for_advancement);
    EXPECT_TRUE(candidate_it->has_desired_outcome);
    EXPECT_FALSE(candidate_it->has_final_victory_outcome);
    EXPECT_EQ(candidate_it->advancement_rank, 1);
    ASSERT_TRUE(candidate_it->advancement_decision.has_value());
    EXPECT_EQ(candidate_it->advancement_decision->decision_kind, "NOT_SELECTED");

    const auto miss_it = std::find_if(jobs.begin(), jobs.end(), [miss_turn_job_id](const auto& job) {
        return job.turn_job_id == miss_turn_job_id;
    });
    ASSERT_NE(miss_it, jobs.end());
    EXPECT_FALSE(miss_it->selected_for_advancement);
    EXPECT_FALSE(miss_it->has_desired_outcome);
    EXPECT_FALSE(miss_it->has_final_victory_outcome);
    EXPECT_EQ(miss_it->advancement_rank, 0);

    const auto victory_jobs = ui_read_db->ListBattleTurnJobsForWaves({ wave_id }, true);
    ASSERT_EQ(victory_jobs.size(), 1u);
    EXPECT_EQ(victory_jobs.front().turn_job_id, turn_job_id);

    const auto detail = ui_read_db->GetBattleTurnJobDetail(turn_job_id);
    ASSERT_TRUE(detail.has_value());
    ASSERT_TRUE(detail->group.has_value());
    ASSERT_TRUE(detail->wave.has_value());
    EXPECT_EQ(detail->group->battle_set_id, battle_set_id);
    EXPECT_EQ(detail->wave->wave_id, wave_id);
    EXPECT_EQ(detail->summary.turn_job_id, turn_job_id);
}

TEST_F(SqliteDbFixture, UiReadProjectionFailureDiagnosticsRecordPayloadRefsBeforeDeadLetter) {
    namespace migrations = savor::db::migrations;
    namespace projectors = savor::db::uiread::projectors;

    const auto separate_root = temp_root_ / "projection-failure-diagnostic";
    ASSERT_TRUE(std::filesystem::create_directories(separate_root));
    const auto paths = MakePhase4DbPaths(separate_root);

    {
        savor::db::core::DBService initializer(
            paths,
            migrations::MigrationSourceOptions{ .source_kind = migrations::MigrationSourceKind::Embedded });
        std::string err;
        ASSERT_TRUE(initializer.Start(&err)) << err;
        initializer.Stop();
    }

    sqlite3* state_handle = nullptr;
    ASSERT_EQ(SQLITE_OK, sqlite3_open(paths.state_db_path.string().c_str(), &state_handle));
    ASSERT_NE(state_handle, nullptr);
    ASSERT_EQ(SQLITE_OK, sqlite3_busy_timeout(state_handle, 5000));
    savor::db::state::SqliteStateDb state_db(state_handle);

    sqlite3* ui_handle = nullptr;
    ASSERT_EQ(SQLITE_OK, sqlite3_open(paths.ui_read_db_path.string().c_str(), &ui_handle));
    ASSERT_NE(ui_handle, nullptr);
    ASSERT_TRUE(ExecSql(ui_handle, "DROP TABLE ui_artifact_browser;"));
    sqlite3_close(ui_handle);
    ui_handle = nullptr;

    projectors::UiReadProjectionService projection(
        projectors::UiReadProjectionConfig{
            .ui_read_db_path = paths.ui_read_db_path,
            .execution_db_path = paths.execution_db_path,
            .state_db_path = paths.state_db_path,
            .analysis_db_path = paths.analysis_db_path,
            .archive_db_path = paths.archive_db_path,
            .max_batch_size = 100,
            .max_attempts = 5,
            .poll_interval = std::chrono::hours{ 24 },
        });
    std::string err;
    ASSERT_TRUE(projection.Start(&err)) << err;

    std::int64_t artifact_id = 0;
    ASSERT_TRUE(state_db.StoreArtifact(
        {
            .sha256 = "projection-diagnostic-artifact",
            .size_bytes = 12,
            .compression_kind = 0,
            .filename = "diagnostic.sav",
            .file_ext = ".sav",
            .artifact_kind = "SAV",
            .created_at_utc = savor::db::types::UtcNow(),
            .correlation_id = "projection-diagnostic",
            .causation_id = "test",
        },
        &artifact_id,
        &err)) << err;

    EXPECT_FALSE(projection.RunOnce(&err));
    EXPECT_FALSE(err.empty());
    projection.Stop();
    sqlite3_close(state_handle);
    state_handle = nullptr;

    sqlite3* verify_handle = nullptr;
    ASSERT_EQ(SQLITE_OK, sqlite3_open(paths.ui_read_db_path.string().c_str(), &verify_handle));
    ASSERT_NE(verify_handle, nullptr);
    EXPECT_EQ(ReadInt64(verify_handle, "SELECT COUNT(1) FROM ui_projection_dead_letter WHERE stream_id='state';"), 1);
    EXPECT_EQ(ReadText(verify_handle, "SELECT event_type FROM ui_projection_dead_letter WHERE stream_id='state';"), "State.ArtifactStored.v1");
    EXPECT_EQ(ReadText(verify_handle, "SELECT payload_ref_kind FROM ui_projection_dead_letter WHERE stream_id='state';"), "artifact");
    EXPECT_EQ(ReadInt64(verify_handle, "SELECT payload_ref_id FROM ui_projection_dead_letter WHERE stream_id='state';"), artifact_id);
    EXPECT_GE(ReadInt64(verify_handle, "SELECT failure_count FROM ui_projection_dead_letter WHERE stream_id='state';"), 1);
    EXPECT_EQ(ReadInt64(verify_handle, "SELECT is_dead_letter FROM ui_projection_dead_letter WHERE stream_id='state';"), 0);
    EXPECT_NE(ReadText(verify_handle, "SELECT error_text FROM ui_projection_dead_letter WHERE stream_id='state';").find("ui_artifact_browser"), std::string::npos);
    EXPECT_EQ(ReadInt64(verify_handle, "SELECT dead_letter_count FROM ui_projection_subscription WHERE stream_id='state';"), 0);
    sqlite3_close(verify_handle);
}

TEST_F(SqliteDbFixture, UiReadProjectionAdvancesStateCursorOncePerBatch) {
    namespace migrations = savor::db::migrations;
    namespace projectors = savor::db::uiread::projectors;

    const auto separate_root = temp_root_ / "projection-batch-cursor";
    ASSERT_TRUE(std::filesystem::create_directories(separate_root));
    const auto paths = MakePhase4DbPaths(separate_root);

    {
        savor::db::core::DBService initializer(
            paths,
            migrations::MigrationSourceOptions{ .source_kind = migrations::MigrationSourceKind::Embedded });
        std::string err;
        ASSERT_TRUE(initializer.Start(&err)) << err;
        initializer.Stop();
    }

    sqlite3* state_handle = nullptr;
    ASSERT_EQ(SQLITE_OK, sqlite3_open(paths.state_db_path.string().c_str(), &state_handle));
    ASSERT_NE(state_handle, nullptr);
    ASSERT_EQ(SQLITE_OK, sqlite3_busy_timeout(state_handle, 5000));
    savor::db::state::SqliteStateDb state_db(state_handle);

    std::string err;
    for (int i = 0; i < 3; ++i) {
        std::int64_t artifact_id = 0;
        ASSERT_TRUE(state_db.StoreArtifact(
            {
                .sha256 = "projection-batch-artifact-" + std::to_string(i),
                .size_bytes = 12 + i,
                .compression_kind = 0,
                .filename = "batch-" + std::to_string(i) + ".sav",
                .file_ext = ".sav",
                .artifact_kind = "SAV",
                .created_at_utc = savor::db::types::UtcNow(),
                .correlation_id = "projection-batch",
                .causation_id = "test",
            },
            &artifact_id,
            &err)) << err;
    }

    projectors::UiReadProjectionService projection(
        projectors::UiReadProjectionConfig{
            .ui_read_db_path = paths.ui_read_db_path,
            .execution_db_path = paths.execution_db_path,
            .state_db_path = paths.state_db_path,
            .analysis_db_path = paths.analysis_db_path,
            .archive_db_path = paths.archive_db_path,
            .max_batch_size = 3,
            .max_attempts = 5,
            .poll_interval = std::chrono::hours{ 24 },
        });
    ASSERT_TRUE(projection.Start(&err)) << err;
    ASSERT_TRUE(projection.RunOnce(&err)) << err;
    projection.Stop();
    sqlite3_close(state_handle);
    state_handle = nullptr;

    sqlite3* verify_handle = nullptr;
    ASSERT_EQ(SQLITE_OK, sqlite3_open(paths.ui_read_db_path.string().c_str(), &verify_handle));
    ASSERT_NE(verify_handle, nullptr);
    EXPECT_EQ(ReadInt64(verify_handle, "SELECT COUNT(1) FROM ui_artifact_browser;"), 3);
    EXPECT_EQ(ReadInt64(verify_handle, "SELECT last_outbox_id FROM ui_projection_subscription WHERE stream_id='state';"), 3);
    EXPECT_EQ(ReadInt64(verify_handle, "SELECT source_high_water_outbox_id FROM ui_projection_subscription WHERE stream_id='state';"), 3);
    EXPECT_EQ(ReadInt64(verify_handle, "SELECT lag_count FROM ui_projection_subscription WHERE stream_id='state';"), 0);
    EXPECT_EQ(ReadInt64(verify_handle, "SELECT COUNT(1) FROM ui_projection_subscription_audit WHERE source_context='State';"), 1);
    EXPECT_EQ(ReadInt64(verify_handle, "SELECT from_outbox_id FROM ui_projection_subscription_audit WHERE source_context='State';"), 0);
    EXPECT_EQ(ReadInt64(verify_handle, "SELECT to_outbox_id FROM ui_projection_subscription_audit WHERE source_context='State';"), 3);
    EXPECT_EQ(ReadInt64(verify_handle, "SELECT processed_count FROM ui_projection_subscription_audit WHERE source_context='State';"), 3);
    sqlite3_close(verify_handle);
}

TEST_F(SqliteDbFixture, UiReadProjectionCoalescesProgressFloodToOneDirtyJob) {
    namespace migrations = savor::db::migrations;
    namespace projectors = savor::db::uiread::projectors;

    const auto separate_root = temp_root_ / "projection-progress-flood";
    ASSERT_TRUE(std::filesystem::create_directories(separate_root));
    const auto paths = MakePhase4DbPaths(separate_root);

    {
        savor::db::core::DBService initializer(
            paths,
            migrations::MigrationSourceOptions{ .source_kind = migrations::MigrationSourceKind::Embedded });
        std::string err;
        ASSERT_TRUE(initializer.Start(&err)) << err;
        initializer.Stop();
    }

    sqlite3* exec_handle = nullptr;
    ASSERT_EQ(SQLITE_OK, sqlite3_open(paths.execution_db_path.string().c_str(), &exec_handle));
    ASSERT_NE(exec_handle, nullptr);
    ASSERT_TRUE(ExecSql(exec_handle,
        "INSERT INTO exec_job_set(job_set_id,parent_job_set_id,program_kind,purpose,created_by,created_at_utc,priority_boost,expected_total,domain_ref_kind,domain_ref_id,meta_note) "
        "VALUES(1,NULL,42,'progress-flood','test',1000,0,1,NULL,NULL,NULL);"));
    ASSERT_TRUE(ExecSql(exec_handle,
        "INSERT INTO exec_job(job_id,job_set_id,parent_job_id,program_kind,program_version,program_ref_kind,program_ref_id,fingerprint,priority,state,attempts,max_attempts,claimed_by_token,lease_expires_at_utc,queued_at_utc,started_at_utc,ended_at_utc,error_code,error_text) "
        "VALUES(17401,1,NULL,42,1,'test',1,'progress-flood-job',1,'RUNNING',1,1,'worker-1',NULL,1000,1100,NULL,NULL,NULL);"));
    for (int i = 1; i <= 1000; ++i) {
        ASSERT_TRUE(ExecSql(exec_handle,
            ("INSERT INTO exec_outbox_message(outbox_id,event_id,event_type,event_version,context_name,aggregate_kind,aggregate_id,correlation_id,causation_id,occurred_at_utc,payload_ref_kind,payload_ref_id,published_at_utc,attempt_count,last_error) "
             "VALUES(" + std::to_string(i) + ",'progress-" + std::to_string(i) + "','Execution.JobProgressed.v1',1,'Execution','job','17401','test','test'," + std::to_string(1000 + i) + ",'job',17401,NULL,0,NULL);")
                .c_str()));
    }
    sqlite3_close(exec_handle);
    exec_handle = nullptr;

    projectors::UiReadProjectionService projection(
        projectors::UiReadProjectionConfig{
            .ui_read_db_path = paths.ui_read_db_path,
            .execution_db_path = paths.execution_db_path,
            .state_db_path = paths.state_db_path,
            .analysis_db_path = paths.analysis_db_path,
            .archive_db_path = paths.archive_db_path,
            .max_batch_size = 5000,
            .max_dirty_materialization_batch_size = 1000,
            .max_attempts = 5,
            .poll_interval = std::chrono::hours{ 24 },
        });
    std::string err;
    ASSERT_TRUE(projection.Start(&err)) << err;
    ASSERT_TRUE(projection.RunOnce(&err)) << err;
    projection.Stop();

    sqlite3* verify_handle = nullptr;
    ASSERT_EQ(SQLITE_OK, sqlite3_open(paths.ui_read_db_path.string().c_str(), &verify_handle));
    ASSERT_NE(verify_handle, nullptr);
    EXPECT_EQ(ReadInt64(verify_handle, "SELECT COUNT(1) FROM ui_job_summary;"), 1);
    EXPECT_EQ(ReadText(verify_handle, "SELECT state FROM ui_job_summary WHERE job_id=17401;"), "RUNNING");
    EXPECT_EQ(ReadInt64(verify_handle, "SELECT COUNT(1) FROM ui_projection_dirty_entity WHERE stream_id='execution';"), 0);
    EXPECT_EQ(ReadInt64(verify_handle, "SELECT last_outbox_id FROM ui_projection_subscription WHERE stream_id='execution';"), 1000);
    EXPECT_EQ(ReadInt64(verify_handle, "SELECT lag_count FROM ui_projection_subscription WHERE stream_id='execution';"), 0);
    EXPECT_EQ(ReadInt64(verify_handle, "SELECT processed_count FROM ui_projection_subscription_audit WHERE source_context='Execution';"), 1000);
    sqlite3_close(verify_handle);
}

TEST_F(SqliteDbFixture, UiReadProjectionTerminalJobEventRefreshesWorkflowLaneCounts) {
    namespace migrations = savor::db::migrations;
    namespace projectors = savor::db::uiread::projectors;

    const auto separate_root = temp_root_ / "projection-terminal-job-workflow";
    ASSERT_TRUE(std::filesystem::create_directories(separate_root));
    const auto paths = MakePhase4DbPaths(separate_root);

    {
        savor::db::core::DBService initializer(
            paths,
            migrations::MigrationSourceOptions{ .source_kind = migrations::MigrationSourceKind::Embedded });
        std::string err;
        ASSERT_TRUE(initializer.Start(&err)) << err;
        initializer.Stop();
    }

    sqlite3* exec_handle = nullptr;
    ASSERT_EQ(SQLITE_OK, sqlite3_open(paths.execution_db_path.string().c_str(), &exec_handle));
    ASSERT_NE(exec_handle, nullptr);
    ASSERT_TRUE(ExecSql(exec_handle,
        "INSERT INTO exec_workflow_instance(workflow_instance_id,workflow_kind,state,root_scope_kind,root_scope_id,created_by,created_at_utc,started_at_utc) "
        "VALUES(25001,'terminal-job-test','RUNNING','job_set',25002,'test',1000,1100);"));
    ASSERT_TRUE(ExecSql(exec_handle,
        "INSERT INTO exec_job_set(job_set_id,parent_job_set_id,program_kind,purpose,created_by,created_at_utc,priority_boost,expected_total,domain_ref_kind,domain_ref_id,meta_note) "
        "VALUES(25002,NULL,42,'terminal-job-test','test',1000,0,2,NULL,NULL,NULL);"));
    ASSERT_TRUE(ExecSql(exec_handle,
        "INSERT INTO exec_workflow_step(workflow_step_id,workflow_instance_id,step_key,step_kind,state,priority,attempts,max_attempts,job_set_id,created_at_utc,started_at_utc) "
        "VALUES(25003,25001,'terminal-step','job_set','RUNNING',5,1,1,25002,1000,1100);"));
    ASSERT_TRUE(ExecSql(exec_handle,
        "INSERT INTO exec_job(job_id,job_set_id,parent_job_id,program_kind,program_version,program_ref_kind,program_ref_id,fingerprint,priority,state,attempts,max_attempts,claimed_by_token,lease_expires_at_utc,queued_at_utc,started_at_utc,ended_at_utc,error_code,error_text) "
        "VALUES(25004,25002,NULL,42,1,'test',1,'terminal-job-completed',5,'COMPLETED',1,1,'worker-1',NULL,1000,1100,1200,NULL,NULL);"));
    ASSERT_TRUE(ExecSql(exec_handle,
        "INSERT INTO exec_job(job_id,job_set_id,parent_job_id,program_kind,program_version,program_ref_kind,program_ref_id,fingerprint,priority,state,attempts,max_attempts,claimed_by_token,lease_expires_at_utc,queued_at_utc,started_at_utc,ended_at_utc,error_code,error_text) "
        "VALUES(25005,25002,NULL,42,1,'test',2,'terminal-job-running',5,'RUNNING',1,1,'worker-1',NULL,1000,1100,NULL,NULL,NULL);"));
    ASSERT_TRUE(ExecSql(exec_handle,
        "INSERT INTO exec_outbox_message(outbox_id,event_id,event_type,event_version,context_name,aggregate_kind,aggregate_id,correlation_id,causation_id,occurred_at_utc,payload_ref_kind,payload_ref_id,published_at_utc,attempt_count,last_error) "
        "VALUES(1,'terminal-job-completed-event','Execution.JobCompleted.v1',1,'Execution','job','25004','test','test',1200,'job',25004,NULL,0,NULL);"));
    sqlite3_close(exec_handle);
    exec_handle = nullptr;

    projectors::UiReadProjectionService projection(
        projectors::UiReadProjectionConfig{
            .ui_read_db_path = paths.ui_read_db_path,
            .execution_db_path = paths.execution_db_path,
            .state_db_path = paths.state_db_path,
            .analysis_db_path = paths.analysis_db_path,
            .archive_db_path = paths.archive_db_path,
            .max_batch_size = 5000,
            .max_dirty_materialization_batch_size = 1000,
            .max_attempts = 5,
            .poll_interval = std::chrono::hours{ 24 },
        });
    std::string err;
    ASSERT_TRUE(projection.Start(&err)) << err;
    ASSERT_TRUE(projection.RunOnce(&err)) << err;
    projection.Stop();

    sqlite3* verify_handle = nullptr;
    ASSERT_EQ(SQLITE_OK, sqlite3_open(paths.ui_read_db_path.string().c_str(), &verify_handle));
    ASSERT_NE(verify_handle, nullptr);
    EXPECT_EQ(ReadText(verify_handle, "SELECT state FROM ui_job_summary WHERE job_id=25004;"), "COMPLETED");
    EXPECT_EQ(ReadText(verify_handle, "SELECT state FROM ui_workflow_instance WHERE workflow_instance_id=25001;"), "RUNNING");
    EXPECT_EQ(ReadInt64(verify_handle, "SELECT job_count FROM ui_workflow_step WHERE workflow_step_id=25003;"), 2);
    EXPECT_EQ(ReadInt64(verify_handle, "SELECT job_completed_count FROM ui_workflow_step WHERE workflow_step_id=25003;"), 1);
    EXPECT_EQ(ReadInt64(verify_handle, "SELECT job_failed_count FROM ui_workflow_step WHERE workflow_step_id=25003;"), 0);
    EXPECT_EQ(ReadInt64(verify_handle, "SELECT COUNT(1) FROM ui_projection_dirty_entity WHERE stream_id='execution';"), 0);
    EXPECT_EQ(ReadInt64(verify_handle, "SELECT last_outbox_id FROM ui_projection_subscription WHERE stream_id='execution';"), 1);
    sqlite3_close(verify_handle);
}

TEST_F(SqliteDbFixture, UiReadProjectionProjectsWorkflowDisplayStateFromStepActivity) {
    namespace migrations = savor::db::migrations;
    namespace projectors = savor::db::uiread::projectors;

    const auto separate_root = temp_root_ / "projection-workflow-display-state";
    ASSERT_TRUE(std::filesystem::create_directories(separate_root));
    const auto paths = MakePhase4DbPaths(separate_root);

    {
        savor::db::core::DBService initializer(
            paths,
            migrations::MigrationSourceOptions{ .source_kind = migrations::MigrationSourceKind::Embedded });
        std::string err;
        ASSERT_TRUE(initializer.Start(&err)) << err;
        initializer.Stop();
    }

    sqlite3* exec_handle = nullptr;
    ASSERT_EQ(SQLITE_OK, sqlite3_open(paths.execution_db_path.string().c_str(), &exec_handle));
    ASSERT_NE(exec_handle, nullptr);
    ASSERT_TRUE(ExecSql(exec_handle,
        "INSERT INTO exec_workflow_instance(workflow_instance_id,workflow_kind,state,root_scope_kind,root_scope_id,created_by,created_at_utc,started_at_utc,completed_at_utc) "
        "VALUES"
        "(26001,'display-test','RUNNING','manual',NULL,'test',1000,1100,NULL),"
        "(26002,'display-test','RUNNING','manual',NULL,'test',1001,1101,NULL),"
        "(26003,'display-test','RUNNING','manual',NULL,'test',1002,1102,NULL),"
        "(26004,'display-test','COMPLETED','manual',NULL,'test',1003,1103,1203),"
        "(26005,'display-test','RUNNING','manual',NULL,'test',1004,1104,NULL);"));
    ASSERT_TRUE(ExecSql(exec_handle,
        "INSERT INTO exec_job_set(job_set_id,parent_job_set_id,program_kind,purpose,created_by,created_at_utc,priority_boost,expected_total,domain_ref_kind,domain_ref_id,meta_note) "
        "VALUES(26050,NULL,42,'display-active-job','test',1000,0,1,NULL,NULL,NULL);"));
    ASSERT_TRUE(ExecSql(exec_handle,
        "INSERT INTO exec_workflow_step(workflow_step_id,workflow_instance_id,step_key,step_kind,state,priority,attempts,max_attempts,job_set_id,ready_at_utc,created_at_utc) "
        "VALUES"
        "(26011,26001,'ready-step','test','READY',1,0,1,NULL,1150,1000),"
        "(26012,26002,'waiting-step','test','WAITING',1,0,1,NULL,NULL,1000),"
        "(26013,26003,'materialized-step','test','MATERIALIZED',1,0,1,NULL,NULL,1000),"
        "(26014,26004,'terminal-step','test','RUNNING',1,0,1,NULL,NULL,1000),"
        "(26015,26005,'active-job-step','test','WAITING',1,0,1,26050,NULL,1000);"));
    ASSERT_TRUE(ExecSql(exec_handle,
        "INSERT INTO exec_job(job_id,job_set_id,parent_job_id,program_kind,program_version,program_ref_kind,program_ref_id,fingerprint,priority,state,attempts,max_attempts,queued_at_utc) "
        "VALUES(26051,26050,NULL,42,1,'test',1,'display-active-job',1,'QUEUED',0,1,1160);"));
    for (int i = 0; i < 5; ++i) {
        const auto workflow_id = 26001 + i;
        ASSERT_TRUE(ExecSql(exec_handle,
            ("INSERT INTO exec_outbox_message(outbox_id,event_id,event_type,event_version,context_name,aggregate_kind,aggregate_id,correlation_id,causation_id,occurred_at_utc,payload_ref_kind,payload_ref_id,published_at_utc,attempt_count,last_error) "
             "VALUES(" + std::to_string(i + 1) + ",'display-workflow-" + std::to_string(workflow_id) + "','Execution.WorkflowInstanceCreated.v1',1,'Execution','workflow','" + std::to_string(workflow_id) + "','test','test'," + std::to_string(1200 + i) + ",'workflow'," + std::to_string(workflow_id) + ",NULL,0,NULL);")
                .c_str()));
    }
    sqlite3_close(exec_handle);
    exec_handle = nullptr;

    projectors::UiReadProjectionService projection(
        projectors::UiReadProjectionConfig{
            .ui_read_db_path = paths.ui_read_db_path,
            .execution_db_path = paths.execution_db_path,
            .state_db_path = paths.state_db_path,
            .analysis_db_path = paths.analysis_db_path,
            .archive_db_path = paths.archive_db_path,
            .max_batch_size = 5000,
            .max_dirty_materialization_batch_size = 1000,
            .max_attempts = 5,
            .poll_interval = std::chrono::hours{ 24 },
        });
    std::string err;
    ASSERT_TRUE(projection.RunOnce(&err)) << err;

    sqlite3* verify_handle = nullptr;
    ASSERT_EQ(SQLITE_OK, sqlite3_open(paths.ui_read_db_path.string().c_str(), &verify_handle));
    ASSERT_NE(verify_handle, nullptr);
    EXPECT_EQ(ReadText(verify_handle, "SELECT display_state FROM ui_workflow_instance WHERE workflow_instance_id=26001;"), "QUEUED");
    EXPECT_EQ(ReadText(verify_handle, "SELECT display_state FROM ui_workflow_instance WHERE workflow_instance_id=26002;"), "WAITING");
    EXPECT_EQ(ReadText(verify_handle, "SELECT display_state FROM ui_workflow_instance WHERE workflow_instance_id=26003;"), "RUNNING");
    EXPECT_EQ(ReadText(verify_handle, "SELECT display_state FROM ui_workflow_instance WHERE workflow_instance_id=26004;"), "COMPLETED");
    EXPECT_EQ(ReadText(verify_handle, "SELECT display_state FROM ui_workflow_instance WHERE workflow_instance_id=26005;"), "RUNNING");
    EXPECT_EQ(ReadText(verify_handle, "SELECT state FROM ui_workflow_instance WHERE workflow_instance_id=26001;"), "RUNNING");
    savor::db::SqliteUiReadDb ui_read(verify_handle);
    savor::db::UiWorkflowInstanceListQuery query{};
    query.display_state = "QUEUED";
    query.limit = 10;
    const auto queued_page = ui_read.ListWorkflowInstances(query);
    ASSERT_EQ(queued_page.items.size(), 1u);
    EXPECT_EQ(queued_page.items[0].workflow_instance_id, 26001);
    const auto counts = ui_read.CountWorkflowDisplayStates();
    EXPECT_EQ(counts.running, 2);
    EXPECT_EQ(counts.queued, 1);
    EXPECT_EQ(counts.waiting, 1);
    EXPECT_EQ(counts.completed, 1);
    EXPECT_EQ(counts.terminal, 1);
    EXPECT_EQ(counts.total, 5);
    sqlite3_close(verify_handle);
}

TEST_F(SqliteDbFixture, UiReadProjectionRefreshesJobsSupersededByBatchUpdate) {
    namespace migrations = savor::db::migrations;
    namespace projectors = savor::db::uiread::projectors;

    const auto separate_root = temp_root_ / "projection-superseded-jobs";
    ASSERT_TRUE(std::filesystem::create_directories(separate_root));
    const auto paths = MakePhase4DbPaths(separate_root);

    {
        savor::db::core::DBService initializer(
            paths,
            migrations::MigrationSourceOptions{ .source_kind = migrations::MigrationSourceKind::Embedded });
        std::string err;
        ASSERT_TRUE(initializer.Start(&err)) << err;
        initializer.Stop();
    }

    sqlite3* exec_handle = nullptr;
    ASSERT_EQ(SQLITE_OK, sqlite3_open(paths.execution_db_path.string().c_str(), &exec_handle));
    ASSERT_NE(exec_handle, nullptr);
    ASSERT_TRUE(ExecSql(exec_handle, R"SQL(
INSERT INTO exec_workflow_instance(workflow_instance_id,workflow_kind,state,root_scope_kind,root_scope_id,created_by,created_at_utc,started_at_utc)
VALUES(27001,'supersede-projection','RUNNING','manual',NULL,'test',1000,1100);
INSERT INTO exec_job_set(job_set_id,parent_job_set_id,program_kind,purpose,created_by,created_at_utc,priority_boost,expected_total,domain_ref_kind,domain_ref_id,meta_note)
VALUES(27010,NULL,7,'supersede-projection','test',1000,0,3,NULL,NULL,NULL);
INSERT INTO exec_workflow_step(workflow_step_id,workflow_instance_id,step_key,step_kind,state,priority,attempts,max_attempts,job_set_id,ready_at_utc,created_at_utc)
VALUES(27011,27001,'Unique','seedprobe.unique','MATERIALIZED',1,0,1,27010,1100,1000);
INSERT INTO exec_job(job_id,job_set_id,parent_job_id,program_kind,program_version,program_ref_kind,program_ref_id,fingerprint,priority,state,attempts,max_attempts,queued_at_utc)
VALUES
(27021,27010,NULL,7,1,'seed_probe',1,'projection-supersede-keep',1,'QUEUED',0,1,1200),
(27022,27010,NULL,7,1,'seed_probe',1,'projection-supersede-a',1,'QUEUED',0,1,1200),
(27023,27010,NULL,7,1,'seed_probe',1,'projection-supersede-b',1,'QUEUED',0,1,1200);
INSERT INTO exec_outbox_message(outbox_id,event_id,event_type,event_version,context_name,aggregate_kind,aggregate_id,correlation_id,causation_id,occurred_at_utc,payload_ref_kind,payload_ref_id,published_at_utc,attempt_count,last_error)
VALUES
(1,'projection-supersede-job-27021','Execution.JobQueued.v1',1,'Execution','job','27021','test','test',1200,'job',27021,NULL,0,NULL),
(2,'projection-supersede-job-27022','Execution.JobQueued.v1',1,'Execution','job','27022','test','test',1201,'job',27022,NULL,0,NULL),
(3,'projection-supersede-job-27023','Execution.JobQueued.v1',1,'Execution','job','27023','test','test',1202,'job',27023,NULL,0,NULL);
)SQL"));
    sqlite3_close(exec_handle);
    exec_handle = nullptr;

    projectors::UiReadProjectionService projection(
        projectors::UiReadProjectionConfig{
            .ui_read_db_path = paths.ui_read_db_path,
            .execution_db_path = paths.execution_db_path,
            .state_db_path = paths.state_db_path,
            .analysis_db_path = paths.analysis_db_path,
            .archive_db_path = paths.archive_db_path,
            .max_batch_size = 5000,
            .max_dirty_materialization_batch_size = 1000,
            .max_attempts = 5,
            .poll_interval = std::chrono::hours{ 24 },
        });
    std::string err;
    ASSERT_TRUE(projection.RunOnce(&err)) << err;

    sqlite3* verify_handle = nullptr;
    ASSERT_EQ(SQLITE_OK, sqlite3_open(paths.ui_read_db_path.string().c_str(), &verify_handle));
    ASSERT_NE(verify_handle, nullptr);
    EXPECT_EQ(ReadText(verify_handle, "SELECT state FROM ui_job_summary WHERE job_id=27021;"), "QUEUED");
    EXPECT_EQ(ReadText(verify_handle, "SELECT state FROM ui_job_summary WHERE job_id=27022;"), "QUEUED");
    EXPECT_EQ(ReadText(verify_handle, "SELECT state FROM ui_job_summary WHERE job_id=27023;"), "QUEUED");
    sqlite3_close(verify_handle);
    verify_handle = nullptr;

    ASSERT_EQ(SQLITE_OK, sqlite3_open(paths.execution_db_path.string().c_str(), &exec_handle));
    ASSERT_NE(exec_handle, nullptr);
    savor::db::execution::workflow::SqliteExecutionDb execution_db(exec_handle);
    int rows_superseded = 0;
    ASSERT_TRUE(execution_db.MarkQueuedJobsSuperseded(27010, 27021, &err, &rows_superseded)) << err;
    EXPECT_EQ(rows_superseded, 2);
    sqlite3_close(exec_handle);
    exec_handle = nullptr;

    for (int i = 0; i < 3; ++i) {
        ASSERT_TRUE(projection.RunOnce(&err)) << err;
    }

    ASSERT_EQ(SQLITE_OK, sqlite3_open(paths.ui_read_db_path.string().c_str(), &verify_handle));
    ASSERT_NE(verify_handle, nullptr);
    EXPECT_EQ(ReadText(verify_handle, "SELECT state FROM ui_job_summary WHERE job_id=27021;"), "QUEUED");
    EXPECT_EQ(ReadText(verify_handle, "SELECT state FROM ui_job_summary WHERE job_id=27022;"), "SUPERSEDED");
    EXPECT_EQ(ReadText(verify_handle, "SELECT state FROM ui_job_summary WHERE job_id=27023;"), "SUPERSEDED");
    EXPECT_EQ(ReadInt64(verify_handle, "SELECT COUNT(1) FROM ui_job_summary WHERE job_set_id=27010 AND state IN ('QUEUED','PENDING_MATERIALIZATION');"), 1);
    sqlite3_close(verify_handle);
}

TEST_F(SqliteDbFixture, UiReadProjectionPrioritizesWorkflowDirtyRowsOverOlderJobs) {
    namespace migrations = savor::db::migrations;
    namespace projectors = savor::db::uiread::projectors;

    const auto separate_root = temp_root_ / "projection-prioritize-workflow";
    ASSERT_TRUE(std::filesystem::create_directories(separate_root));
    const auto paths = MakePhase4DbPaths(separate_root);

    {
        savor::db::core::DBService initializer(
            paths,
            migrations::MigrationSourceOptions{ .source_kind = migrations::MigrationSourceKind::Embedded });
        std::string err;
        ASSERT_TRUE(initializer.Start(&err)) << err;
        initializer.Stop();
    }

    sqlite3* exec_handle = nullptr;
    ASSERT_EQ(SQLITE_OK, sqlite3_open(paths.execution_db_path.string().c_str(), &exec_handle));
    ASSERT_NE(exec_handle, nullptr);
    ASSERT_TRUE(ExecSql(exec_handle,
        "INSERT INTO exec_job_set(job_set_id,parent_job_set_id,program_kind,purpose,created_by,created_at_utc,priority_boost,expected_total,domain_ref_kind,domain_ref_id,meta_note) "
        "VALUES(32001,NULL,42,'priority-old-jobs','test',1000,0,5,NULL,NULL,NULL);"));
    for (int i = 0; i < 5; ++i) {
        const auto job_id = 32010 + i;
        ASSERT_TRUE(ExecSql(exec_handle,
            ("INSERT INTO exec_job(job_id,job_set_id,parent_job_id,program_kind,program_version,program_ref_kind,program_ref_id,fingerprint,priority,state,attempts,max_attempts,claimed_by_token,lease_expires_at_utc,queued_at_utc,started_at_utc,ended_at_utc,error_code,error_text) "
             "VALUES(" + std::to_string(job_id) + ",32001,NULL,42,1,'test',1,'priority-old-job-" + std::to_string(i) + "',5,'RUNNING',1,1,'worker-1',NULL,1000,1100,NULL,NULL,NULL);")
                .c_str()));
        ASSERT_TRUE(ExecSql(exec_handle,
            ("INSERT INTO exec_outbox_message(outbox_id,event_id,event_type,event_version,context_name,aggregate_kind,aggregate_id,correlation_id,causation_id,occurred_at_utc,payload_ref_kind,payload_ref_id,published_at_utc,attempt_count,last_error) "
             "VALUES(" + std::to_string(i + 1) + ",'priority-old-job-event-" + std::to_string(i) + "','Execution.JobStarted.v1',1,'Execution','job','" + std::to_string(job_id) + "','test','test'," + std::to_string(1100 + i) + ",'job'," + std::to_string(job_id) + ",NULL,0,NULL);")
                .c_str()));
    }
    ASSERT_TRUE(ExecSql(exec_handle,
        "INSERT INTO exec_workflow_instance(workflow_instance_id,workflow_kind,state,root_scope_kind,root_scope_id,created_by,created_at_utc,started_at_utc) "
        "VALUES(32100,'priority-workflow','RUNNING','manual',NULL,'test',2000,2100);"));
    ASSERT_TRUE(ExecSql(exec_handle,
        "INSERT INTO exec_outbox_message(outbox_id,event_id,event_type,event_version,context_name,aggregate_kind,aggregate_id,correlation_id,causation_id,occurred_at_utc,payload_ref_kind,payload_ref_id,published_at_utc,attempt_count,last_error) "
        "VALUES(100,'priority-workflow-event','Execution.WorkflowInstanceCreated.v1',1,'Execution','workflow','32100','test','test',2100,'workflow',32100,NULL,0,NULL);"));
    sqlite3_close(exec_handle);
    exec_handle = nullptr;

    projectors::UiReadProjectionService projection(
        projectors::UiReadProjectionConfig{
            .ui_read_db_path = paths.ui_read_db_path,
            .execution_db_path = paths.execution_db_path,
            .state_db_path = paths.state_db_path,
            .analysis_db_path = paths.analysis_db_path,
            .archive_db_path = paths.archive_db_path,
            .max_batch_size = 5000,
            .max_dirty_materialization_batch_size = 1,
            .max_attempts = 5,
            .poll_interval = std::chrono::hours{ 24 },
        });
    std::string err;
    ASSERT_TRUE(projection.RunOnce(&err)) << err;

    sqlite3* verify_handle = nullptr;
    ASSERT_EQ(SQLITE_OK, sqlite3_open(paths.ui_read_db_path.string().c_str(), &verify_handle));
    ASSERT_NE(verify_handle, nullptr);
    EXPECT_EQ(ReadText(verify_handle, "SELECT state FROM ui_workflow_instance WHERE workflow_instance_id=32100;"), "RUNNING");
    EXPECT_EQ(ReadInt64(verify_handle, "SELECT COUNT(1) FROM ui_job_summary WHERE job_id BETWEEN 32010 AND 32014;"), 0);
    EXPECT_EQ(ReadInt64(verify_handle, "SELECT COUNT(1) FROM ui_projection_dirty_entity WHERE stream_id='execution' AND entity_kind='workflow';"), 0);
    EXPECT_EQ(ReadInt64(verify_handle, "SELECT COUNT(1) FROM ui_projection_dirty_entity WHERE stream_id='execution' AND entity_kind='job';"), 5);
    sqlite3_close(verify_handle);
}

TEST_F(SqliteDbFixture, UiReadProjectionExecutionDirtyPriorityPreservesWorkflowFifoThenJobSetBeforeJob) {
    namespace migrations = savor::db::migrations;
    namespace projectors = savor::db::uiread::projectors;

    const auto separate_root = temp_root_ / "projection-priority-order";
    ASSERT_TRUE(std::filesystem::create_directories(separate_root));
    const auto paths = MakePhase4DbPaths(separate_root);

    {
        savor::db::core::DBService initializer(
            paths,
            migrations::MigrationSourceOptions{ .source_kind = migrations::MigrationSourceKind::Embedded });
        std::string err;
        ASSERT_TRUE(initializer.Start(&err)) << err;
        initializer.Stop();
    }

    sqlite3* exec_handle = nullptr;
    ASSERT_EQ(SQLITE_OK, sqlite3_open(paths.execution_db_path.string().c_str(), &exec_handle));
    ASSERT_NE(exec_handle, nullptr);
    ASSERT_TRUE(ExecSql(exec_handle,
        "INSERT INTO exec_workflow_instance(workflow_instance_id,workflow_kind,state,root_scope_kind,root_scope_id,created_by,created_at_utc,started_at_utc) "
        "VALUES(33100,'priority-order','RUNNING','manual',NULL,'test',2000,2100);"));
    ASSERT_TRUE(ExecSql(exec_handle,
        "INSERT INTO exec_workflow_instance(workflow_instance_id,workflow_kind,state,root_scope_kind,root_scope_id,created_by,created_at_utc,started_at_utc) "
        "VALUES(33101,'priority-order','RUNNING','manual',NULL,'test',2001,2101);"));
    ASSERT_TRUE(ExecSql(exec_handle,
        "INSERT INTO exec_job_set(job_set_id,parent_job_set_id,program_kind,purpose,created_by,created_at_utc,priority_boost,expected_total,domain_ref_kind,domain_ref_id,meta_note) "
        "VALUES(33110,NULL,42,'priority-job-set','test',1000,0,1,NULL,NULL,NULL);"));
    ASSERT_TRUE(ExecSql(exec_handle,
        "INSERT INTO exec_job_set(job_set_id,parent_job_set_id,program_kind,purpose,created_by,created_at_utc,priority_boost,expected_total,domain_ref_kind,domain_ref_id,meta_note) "
        "VALUES(33120,NULL,42,'priority-plain-job','test',1000,0,1,NULL,NULL,NULL);"));
    ASSERT_TRUE(ExecSql(exec_handle,
        "INSERT INTO exec_job(job_id,job_set_id,parent_job_id,program_kind,program_version,program_ref_kind,program_ref_id,fingerprint,priority,state,attempts,max_attempts,claimed_by_token,lease_expires_at_utc,queued_at_utc,started_at_utc,ended_at_utc,error_code,error_text) "
        "VALUES(33111,33110,NULL,42,1,'test',1,'priority-job-set-job',5,'RUNNING',1,1,'worker-1',NULL,1000,1100,NULL,NULL,NULL);"));
    ASSERT_TRUE(ExecSql(exec_handle,
        "INSERT INTO exec_job(job_id,job_set_id,parent_job_id,program_kind,program_version,program_ref_kind,program_ref_id,fingerprint,priority,state,attempts,max_attempts,claimed_by_token,lease_expires_at_utc,queued_at_utc,started_at_utc,ended_at_utc,error_code,error_text) "
        "VALUES(33121,33120,NULL,42,1,'test',1,'priority-plain-job',5,'RUNNING',1,1,'worker-1',NULL,1000,1100,NULL,NULL,NULL);"));
    ASSERT_TRUE(ExecSql(exec_handle,
        "INSERT INTO exec_outbox_message(outbox_id,event_id,event_type,event_version,context_name,aggregate_kind,aggregate_id,correlation_id,causation_id,occurred_at_utc,payload_ref_kind,payload_ref_id,published_at_utc,attempt_count,last_error) "
        "VALUES(1,'priority-order-plain-job','Execution.JobStarted.v1',1,'Execution','job','33121','test','test',1100,'job',33121,NULL,0,NULL);"));
    ASSERT_TRUE(ExecSql(exec_handle,
        "INSERT INTO exec_outbox_message(outbox_id,event_id,event_type,event_version,context_name,aggregate_kind,aggregate_id,correlation_id,causation_id,occurred_at_utc,payload_ref_kind,payload_ref_id,published_at_utc,attempt_count,last_error) "
        "VALUES(2,'priority-order-job-set','Execution.JobSetCreated.v1',1,'Execution','job_set','33110','test','test',1101,'job_set',33110,NULL,0,NULL);"));
    ASSERT_TRUE(ExecSql(exec_handle,
        "INSERT INTO exec_outbox_message(outbox_id,event_id,event_type,event_version,context_name,aggregate_kind,aggregate_id,correlation_id,causation_id,occurred_at_utc,payload_ref_kind,payload_ref_id,published_at_utc,attempt_count,last_error) "
        "VALUES(3,'priority-order-workflow-a','Execution.WorkflowInstanceCreated.v1',1,'Execution','workflow','33100','test','test',2100,'workflow',33100,NULL,0,NULL);"));
    ASSERT_TRUE(ExecSql(exec_handle,
        "INSERT INTO exec_outbox_message(outbox_id,event_id,event_type,event_version,context_name,aggregate_kind,aggregate_id,correlation_id,causation_id,occurred_at_utc,payload_ref_kind,payload_ref_id,published_at_utc,attempt_count,last_error) "
        "VALUES(4,'priority-order-workflow-b','Execution.WorkflowInstanceCreated.v1',1,'Execution','workflow','33101','test','test',2101,'workflow',33101,NULL,0,NULL);"));
    sqlite3_close(exec_handle);
    exec_handle = nullptr;

    projectors::UiReadProjectionService projection(
        projectors::UiReadProjectionConfig{
            .ui_read_db_path = paths.ui_read_db_path,
            .execution_db_path = paths.execution_db_path,
            .state_db_path = paths.state_db_path,
            .analysis_db_path = paths.analysis_db_path,
            .archive_db_path = paths.archive_db_path,
            .max_batch_size = 5000,
            .max_dirty_materialization_batch_size = 1,
            .max_attempts = 5,
            .poll_interval = std::chrono::hours{ 24 },
        });
    std::string err;
    ASSERT_TRUE(projection.RunOnce(&err)) << err;

    sqlite3* verify_handle = nullptr;
    ASSERT_EQ(SQLITE_OK, sqlite3_open(paths.ui_read_db_path.string().c_str(), &verify_handle));
    ASSERT_NE(verify_handle, nullptr);
    EXPECT_EQ(ReadText(verify_handle, "SELECT state FROM ui_workflow_instance WHERE workflow_instance_id=33100;"), "RUNNING");
    EXPECT_EQ(ReadInt64(verify_handle, "SELECT COUNT(1) FROM ui_workflow_instance WHERE workflow_instance_id=33101;"), 0);

    ASSERT_TRUE(projection.RunOnce(&err)) << err;
    EXPECT_EQ(ReadText(verify_handle, "SELECT state FROM ui_workflow_instance WHERE workflow_instance_id=33101;"), "RUNNING");
    EXPECT_EQ(ReadInt64(verify_handle, "SELECT COUNT(1) FROM ui_job_summary WHERE job_id IN (33111,33121);"), 0);

    ASSERT_TRUE(projection.RunOnce(&err)) << err;
    EXPECT_EQ(ReadText(verify_handle, "SELECT state FROM ui_job_summary WHERE job_id=33111;"), "RUNNING");
    EXPECT_EQ(ReadInt64(verify_handle, "SELECT COUNT(1) FROM ui_job_summary WHERE job_id=33121;"), 0);
    EXPECT_EQ(ReadInt64(verify_handle, "SELECT COUNT(1) FROM ui_projection_dirty_entity WHERE stream_id='execution' AND entity_kind='job_set';"), 0);
    EXPECT_EQ(ReadInt64(verify_handle, "SELECT COUNT(1) FROM ui_projection_dirty_entity WHERE stream_id='execution' AND entity_kind='job';"), 1);
    sqlite3_close(verify_handle);
}

TEST_F(SqliteDbFixture, UiReadWorkflowBattleRollupQueriesUseJobSetIndexes) {
    EXPECT_TRUE(QueryPlanContains(db_,
        "EXPLAIN QUERY PLAN "
        "SELECT job_event_id,job_id,artifact_id,event_kind,event_ts_utc "
        "FROM exec_job_event WHERE job_id=1 AND artifact_id IS NOT NULL ORDER BY job_event_id ASC;",
        "ix_exec_job_event_job_event_id"));
    EXPECT_TRUE(QueryPlanContains(db_,
        "EXPLAIN QUERY PLAN "
        "SELECT COALESCE(MAX(b.advancement_rank),0) "
        "FROM ui_job_summary j INDEXED BY ix_ui_job_summary_job_set_job "
        "JOIN ui_battle_turn_job b ON b.exec_job_id=j.job_id "
        "WHERE j.job_set_id=1;",
        "ix_ui_job_summary_job_set_job"));
    EXPECT_TRUE(QueryPlanContains(db_,
        "EXPLAIN QUERY PLAN "
        "SELECT DISTINCT s.job_set_id "
        "FROM ui_job_summary j "
        "JOIN ui_workflow_step s INDEXED BY ix_ui_workflow_step_job_set_instance ON s.job_set_id=j.job_set_id "
        "WHERE j.job_id=1 AND s.job_set_id IS NOT NULL;",
        "ix_ui_workflow_step_job_set_instance"));
}

TEST_F(SqliteDbFixture, UiReadProjectionDirtyJobSetRefreshesWorkflowBattleRollupOnceAfterProjectingJobs) {
    namespace migrations = savor::db::migrations;
    namespace projectors = savor::db::uiread::projectors;

    const auto separate_root = temp_root_ / "projection-job-set-battle-rollup";
    ASSERT_TRUE(std::filesystem::create_directories(separate_root));
    const auto paths = MakePhase4DbPaths(separate_root);

    {
        savor::db::core::DBService initializer(
            paths,
            migrations::MigrationSourceOptions{ .source_kind = migrations::MigrationSourceKind::Embedded });
        std::string err;
        ASSERT_TRUE(initializer.Start(&err)) << err;
        initializer.Stop();
    }

    sqlite3* ui_handle = nullptr;
    ASSERT_EQ(SQLITE_OK, sqlite3_open(paths.ui_read_db_path.string().c_str(), &ui_handle));
    ASSERT_NE(ui_handle, nullptr);
    ASSERT_TRUE(ExecSql(ui_handle,
        "INSERT INTO ui_workflow_instance(workflow_instance_id,workflow_kind,state,root_scope_kind,root_scope_id,created_by,created_at_utc,started_at_utc) "
        "VALUES(34100,'battle-rollup','RUNNING','manual',NULL,'test',1000,1100);"));
    ASSERT_TRUE(ExecSql(ui_handle,
        "INSERT INTO ui_workflow_step(workflow_step_id,workflow_instance_id,step_key,step_kind,state,job_set_id,priority,attempts,max_attempts,created_at_utc) "
        "VALUES(34101,34100,'battle-step','battle_single_turn','MATERIALIZED',34110,5,1,1,1000);"));
    for (int i = 0; i < 24; ++i) {
        const auto job_id = 34120 + i;
        const auto turn_job_id = 34220 + i;
        const int selected = i == 0 ? 1 : 0;
        const int desired = i < 6 ? 1 : 0;
        const int victory = i == 1 ? 1 : 0;
        const int rank = selected ? 2 : (desired ? 1 : 0);
        ASSERT_TRUE(ExecSql(ui_handle,
            ("INSERT INTO ui_battle_turn_job(turn_job_id,wave_id,exec_job_id,job_state,fake_attacks_this_turn,fake_attacks_used_before,"
             "has_desired_outcome,selected_for_advancement,advancement_decision_kind,advancement_rank,has_final_victory_outcome) "
             "VALUES(" + std::to_string(turn_job_id) + ",34150," + std::to_string(job_id) + ",'SUCCEEDED',0,0,"
             + std::to_string(desired) + "," + std::to_string(selected) + ","
             + (selected ? "'SELECTED'" : (desired ? "'NOT_SELECTED'" : "NULL")) + ","
             + std::to_string(rank) + "," + std::to_string(victory) + ");")
                .c_str()));
    }
    sqlite3_close(ui_handle);
    ui_handle = nullptr;

    sqlite3* exec_handle = nullptr;
    ASSERT_EQ(SQLITE_OK, sqlite3_open(paths.execution_db_path.string().c_str(), &exec_handle));
    ASSERT_NE(exec_handle, nullptr);
    ASSERT_TRUE(ExecSql(exec_handle,
        "INSERT INTO exec_job_set(job_set_id,parent_job_set_id,program_kind,purpose,created_by,created_at_utc,priority_boost,expected_total,domain_ref_kind,domain_ref_id,meta_note) "
        "VALUES(34110,NULL,42,'rollup-job-set','test',1000,0,24,NULL,NULL,NULL);"));
    for (int i = 0; i < 24; ++i) {
        const auto job_id = 34120 + i;
        ASSERT_TRUE(ExecSql(exec_handle,
            ("INSERT INTO exec_job(job_id,job_set_id,parent_job_id,program_kind,program_version,program_ref_kind,program_ref_id,fingerprint,priority,state,attempts,max_attempts,claimed_by_token,lease_expires_at_utc,queued_at_utc,started_at_utc,ended_at_utc,error_code,error_text) "
             "VALUES(" + std::to_string(job_id) + ",34110,NULL,42,1,'test',1,'rollup-job-" + std::to_string(i) + "',5,'COMPLETED',1,1,'worker-1',NULL,1000,1100,1200,NULL,NULL);")
                .c_str()));
    }
    ASSERT_TRUE(ExecSql(exec_handle,
        "INSERT INTO exec_outbox_message(outbox_id,event_id,event_type,event_version,context_name,aggregate_kind,aggregate_id,correlation_id,causation_id,occurred_at_utc,payload_ref_kind,payload_ref_id,published_at_utc,attempt_count,last_error) "
        "VALUES(1,'rollup-job-set-created','Execution.JobSetCreated.v1',1,'Execution','job_set','34110','test','test',1300,'job_set',34110,NULL,0,NULL);"));
    sqlite3_close(exec_handle);
    exec_handle = nullptr;

    projectors::UiReadProjectionService projection(
        projectors::UiReadProjectionConfig{
            .ui_read_db_path = paths.ui_read_db_path,
            .execution_db_path = paths.execution_db_path,
            .state_db_path = paths.state_db_path,
            .analysis_db_path = paths.analysis_db_path,
            .archive_db_path = paths.archive_db_path,
            .max_batch_size = 5000,
            .max_dirty_materialization_batch_size = 1,
            .max_attempts = 5,
            .poll_interval = std::chrono::hours{ 24 },
        });
    std::string err;
    ASSERT_TRUE(projection.RunOnce(&err)) << err;

    sqlite3* verify_handle = nullptr;
    ASSERT_EQ(SQLITE_OK, sqlite3_open(paths.ui_read_db_path.string().c_str(), &verify_handle));
    ASSERT_NE(verify_handle, nullptr);
    EXPECT_EQ(ReadInt64(verify_handle, "SELECT COUNT(1) FROM ui_job_summary WHERE job_set_id=34110;"), 24);
    EXPECT_EQ(ReadInt64(verify_handle, "SELECT battle_advancement_rank FROM ui_workflow_step WHERE workflow_step_id=34101;"), 2);
    EXPECT_EQ(ReadInt64(verify_handle, "SELECT battle_desired_outcome_count FROM ui_workflow_step WHERE workflow_step_id=34101;"), 6);
    EXPECT_EQ(ReadInt64(verify_handle, "SELECT battle_final_victory_count FROM ui_workflow_step WHERE workflow_step_id=34101;"), 1);
    EXPECT_EQ(ReadInt64(verify_handle, "SELECT battle_selected_count FROM ui_workflow_step WHERE workflow_step_id=34101;"), 1);
    EXPECT_EQ(ReadInt64(verify_handle, "SELECT battle_advancement_rank FROM ui_workflow_instance WHERE workflow_instance_id=34100;"), 2);
    EXPECT_EQ(ReadInt64(verify_handle, "SELECT battle_desired_outcome_count FROM ui_workflow_instance WHERE workflow_instance_id=34100;"), 6);
    EXPECT_EQ(ReadInt64(verify_handle, "SELECT battle_final_victory_count FROM ui_workflow_instance WHERE workflow_instance_id=34100;"), 1);
    EXPECT_EQ(ReadInt64(verify_handle, "SELECT battle_selected_count FROM ui_workflow_instance WHERE workflow_instance_id=34100;"), 1);
    EXPECT_EQ(ReadInt64(verify_handle, "SELECT COUNT(1) FROM ui_projection_dirty_entity WHERE stream_id='execution' AND entity_kind='job_set';"), 0);
    sqlite3_close(verify_handle);
}

}

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
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
#include "State/ArtifactObjectStore.h"
#include "State/SqliteStateDb.h"
#include "UIRead/SqliteUiReadDb.h"
#include "Execution/ArchiveWorkflowCommands.h"
#include "Execution/Workflow/SqliteExecutionDb.h"
#include "Execution/Jobs/JobEventOrchestration.h"
#include "Execution/Workflow/WorkflowIntegrityChecks.h"
#include "Execution/Workflow/WorkflowProjector.h"
#include "Execution/Workflow/WorkflowRecoveryService.h"
#include "Execution/Workflow/WorkflowCoordinatorService.h"
#include "Execution/Workflow/WorkflowComposition.h"
#include "Execution/Workflow/WorkflowGraphRoutingService.h"
#include "Execution/Workflow/WorkflowStepSettlementGate.h"
#include "Execution/Workflow/WorkflowSettlementAdvancementService.h"
#include "Execution/ProgramDB/BattleSingleTurn/BattleSingleTurnProgram.h"
#include "Execution/ProgramDB/ProductionProgramKindRegistry.h"
#include "Execution/ProgramDB/SeedProbe/SeedProbeExecutionAdapters.h"
#include "Execution/ProgramDB/SeedProbe/SeedProbeJobSpec.h"
#include "UIRead/Projectors/ProjectorContract.h"
#include "Execution/WorkflowCoordinatorBridge.h"
#include "Execution/WorkflowSchedulerAdapter.h"
#include "Execution/StepInputAggregationService.h"
#include "Execution/CoordinatorRuntime.h"
#include "Runner/IPC/DurableWorkerTerminalEnvelope.h"
#include "Utils/Hash.h"
#include "Core/Memory/Soa/Battle/BattleContextCodec.h"
#include "Phases/Programs/BattleRecord/BattleRecordModule.h"
#include "Runner/Breakpoints/BpRegistry.h"
#include "Runner/Runtime/ProgramKind.h"
#include "Runner/Runtime/ProgramRuntime/Capabilities/SourceCapabilityPacks.h"
#include "Runner/Runtime/Worksets/WorksetWireCodec.h"

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

struct ConfirmedSeedProbeFixture {
    std::int64_t probe_run_id = 0;
    std::int64_t probe_result_id = 0;
    std::int64_t input_frame_id = 0;
};

std::optional<ConfirmedSeedProbeFixture> CreateConfirmedSeedProbeFixture(
    savor::db::IAnalysisDb* analysis_db,
    const std::string& fixture_name,
    std::int64_t entry_savestate_id,
    std::int64_t seed_probe_spec_id,
    std::int64_t representative_job_id,
    std::uint32_t seed_value,
    savor::db::types::UtcTimePoint now,
    std::string* error_out) {
    using namespace savor::db;

    if (analysis_db == nullptr) {
        if (error_out != nullptr) {
            *error_out = "analysis DB is required";
        }
        return std::nullopt;
    }

    std::int64_t probe_set_id = 0;
    if (!analysis_db->CreateSeedProbeSet(
            {
                .name = fixture_name + "-probe-set",
                .probe_flavor = "BATTLE_PRE",
                .breakpoint_policy_name = "fixture",
                .segment_source_kind = "sqlite_fixture",
                .created_at_utc = now,
                .correlation_id = fixture_name,
                .causation_id = fixture_name,
            },
            &probe_set_id,
            error_out)) {
        return std::nullopt;
    }

    std::int64_t probe_run_id = 0;
    if (!analysis_db->RequestSeedProbeRun(
            {
                .materialization_key =
                    "sqlite-fixture." + fixture_name,
                .probe_set_id = probe_set_id,
                .entry_savestate_id = entry_savestate_id,
                .seed_probe_spec_id = seed_probe_spec_id,
                .launch_samples_per_axis = 1,
                .codec_version = 1,
                .status = SeedProbeRunStatus::Survey,
                .requested_at_utc = now,
                .correlation_id = fixture_name,
                .causation_id = fixture_name,
            },
            &probe_run_id,
            error_out)) {
        return std::nullopt;
    }

    std::int64_t input_frame_id = 0;
    if (!analysis_db->EnsureSeedProbeInputFrame(
            0x8080,
            0x8080,
            0x0000,
            &input_frame_id,
            error_out)) {
        return std::nullopt;
    }

    RecordSeedProbeObservationReceipt observation_receipt{};
    if (!analysis_db->RecordSeedProbeObservation(
            {
                .probe_run_id = probe_run_id,
                .input_frame_id = input_frame_id,
                .source_job_id = representative_job_id,
                .seed_value = seed_value,
                .origin_worker_id = 1,
                .origin_process_generation = 1,
                .origin_workset_epoch = 1,
                .terminal_sha256 = std::string(64, 'a'),
                .endpoint = SeedProbeEndpoint::AfterRandSeedSet,
                .recorded_at_utc = now,
                .correlation_id = fixture_name,
                .causation_id = fixture_name,
            },
            &observation_receipt,
            error_out)
        || !observation_receipt.inserted) {
        return std::nullopt;
    }
    const auto representative_result_id =
        observation_receipt.observation.probe_result_id;

    bool changed = false;
    if (!analysis_db->TransitionSeedProbeEvidence(
            {
                .probe_result_id = representative_result_id,
                .expected_state = SeedProbeEvidenceState::Observed,
                .new_state = SeedProbeEvidenceState::Provisional,
                .changed_at_utc = now,
                .correlation_id = fixture_name,
                .causation_id = fixture_name,
            },
            &changed,
            error_out)
        || !changed) {
        return std::nullopt;
    }

    observation_receipt = {};
    if (!analysis_db->RecordSeedProbeObservation(
            {
                .probe_run_id = probe_run_id,
                .input_frame_id = input_frame_id,
                .source_job_id = representative_job_id + 1,
                .seed_value = seed_value,
                .origin_worker_id = 1,
                .origin_process_generation = 1,
                .origin_workset_epoch = 2,
                .terminal_sha256 = std::string(64, 'b'),
                .confirmation_of_probe_result_id = representative_result_id,
                .endpoint = SeedProbeEndpoint::AfterRandSeedSet,
                .recorded_at_utc = now,
                .correlation_id = fixture_name,
                .causation_id = fixture_name,
            },
            &observation_receipt,
            error_out)
        || !observation_receipt.inserted) {
        return std::nullopt;
    }

    changed = false;
    if (!analysis_db->TransitionSeedProbeEvidence(
            {
                .probe_result_id = representative_result_id,
                .expected_state = SeedProbeEvidenceState::Provisional,
                .new_state = SeedProbeEvidenceState::Confirmed,
                .changed_at_utc = now,
                .correlation_id = fixture_name,
                .causation_id = fixture_name,
            },
            &changed,
            error_out)
        || !changed) {
        return std::nullopt;
    }

    return ConfirmedSeedProbeFixture{
        .probe_run_id = probe_run_id,
        .probe_result_id = representative_result_id,
        .input_frame_id = input_frame_id,
    };
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

TEST_F(SqliteDbFixture, BattlePlanRelationalTurnMigrationPreservesAuthoredRows) {
    using namespace savor::db::migrations;

    sqlite3* raw_legacy_db = nullptr;
    ASSERT_EQ(sqlite3_open(":memory:", &raw_legacy_db), SQLITE_OK);
    const std::unique_ptr<sqlite3, decltype(&sqlite3_close)> legacy_db(
        raw_legacy_db, &sqlite3_close);

    const auto migrations = LoadContextMigrations(
        MigrationContext::Authoring,
        {.source_kind = MigrationSourceKind::Embedded});
    const auto base = std::ranges::find(
        migrations, "202604051000_authoring_stage2_schema.sql",
        &MigrationEntry::name);
    const auto relational = std::ranges::find(
        migrations, "202608111000_authoring_battle_plan_relational_turns.sql",
        &MigrationEntry::name);
    const auto symbolic_target_hard_cut = std::ranges::find(
        migrations,
        "202608121000_authoring_remove_battle_target_expression.sql",
        &MigrationEntry::name);
    ASSERT_NE(base, migrations.end());
    ASSERT_NE(relational, migrations.end());
    ASSERT_NE(symbolic_target_hard_cut, migrations.end());
    ASSERT_TRUE(ExecSql(legacy_db.get(), base->sql.c_str()));
    ASSERT_TRUE(ExecSql(legacy_db.get(),
        "INSERT INTO au_battle_plan(plan_id,name,fingerprint,num_turns,created_at_utc) "
        "VALUES(101,'migration-plan','migration-plan-fingerprint',2,1);"));
    ASSERT_TRUE(ExecSql(legacy_db.get(),
        "INSERT INTO au_battle_plan_action_preset("
        "action_preset_id,name,macro,target_kind,target_single_slot,target_expr_ini,flags,created_at_utc) "
        "VALUES(201,'migration-attack',1,1,4,'legacy-target-expression',0,1);"));
    ASSERT_TRUE(ExecSql(legacy_db.get(),
        "INSERT INTO au_battle_plan_turn(plan_turn_id,plan_id,turn_index) "
        "VALUES(301,101,1),(302,101,2);"));
    ASSERT_TRUE(ExecSql(legacy_db.get(),
        "INSERT INTO au_battle_plan_action("
        "plan_action_id,plan_turn_id,actor_slot,action_preset_id,ordinal) "
        "VALUES(401,301,0,201,0),(402,302,0,201,0);"));

    ASSERT_TRUE(ExecSql(legacy_db.get(), relational->sql.c_str()));
    ASSERT_TRUE(ExecSql(
        legacy_db.get(), symbolic_target_hard_cut->sql.c_str()));
    EXPECT_EQ(ReadInt64(legacy_db.get(),
        "SELECT COUNT(1) FROM pragma_table_info('au_battle_plan') WHERE name='num_turns';"), 0);
    EXPECT_EQ(ReadInt64(legacy_db.get(), "SELECT COUNT(1) FROM au_battle_plan WHERE plan_id=101;"), 1);
    EXPECT_EQ(ReadInt64(legacy_db.get(), "SELECT COUNT(1) FROM au_battle_plan_turn WHERE plan_id=101;"), 2);
    EXPECT_EQ(ReadInt64(legacy_db.get(),
        "SELECT COUNT(1) FROM au_battle_plan_action WHERE plan_turn_id IN (301,302);"), 2);
    EXPECT_EQ(ReadInt64(legacy_db.get(),
        "SELECT COUNT(1) FROM au_battle_plan_action_preset WHERE action_preset_id=201;"), 1);
    EXPECT_EQ(ReadInt64(legacy_db.get(),
        "SELECT COUNT(1) FROM pragma_table_info('au_battle_plan_action_preset') WHERE name='target_expr_ini';"), 0);
}

TEST_F(SqliteDbFixture, WorkflowLaunchContractMigrationBackfillsExactKindsArgumentsAndEmptyPredicateNull) {
    using namespace savor::db::migrations;

    const auto migrations = LoadContextMigrations(
        MigrationContext::Authoring,
        {.source_kind = MigrationSourceKind::Embedded});
    const auto contract = std::ranges::find(
        migrations,
        "202608161000_authoring_workflow_launch_contracts.sql",
        &MigrationEntry::name);
    ASSERT_NE(contract, migrations.end());
    const auto adaptive = std::ranges::find(
        migrations,
        "202608161200_authoring_adaptive_seed_probe.sql",
        &MigrationEntry::name);
    ASSERT_NE(adaptive, migrations.end());
    ASSERT_LT(contract, adaptive);

    sqlite3* raw_legacy_db = nullptr;
    ASSERT_EQ(sqlite3_open(":memory:", &raw_legacy_db), SQLITE_OK);
    const std::unique_ptr<sqlite3, decltype(&sqlite3_close)> legacy_db(
        raw_legacy_db, &sqlite3_close);
    for (auto migration = migrations.begin(); migration != contract; ++migration) {
        ASSERT_TRUE(ExecSql(legacy_db.get(), migration->sql.c_str()))
            << migration->name;
    }

    ASSERT_TRUE(ExecSql(legacy_db.get(), R"SQL(
INSERT INTO au_battle_plan(plan_id,name,fingerprint,created_at_utc)
VALUES(101,'contract-plan','contract-plan-fingerprint',1);
INSERT INTO au_battle_plan_turn(
    plan_turn_id,plan_id,turn_index,default_predicate_bundle_revision_id)
VALUES(102,101,1,1);
INSERT INTO au_workflow_graph(
    workflow_graph_id,name,description,active_revision_id,created_at_utc)
VALUES(201,'contract-graph','migration fixture',NULL,1);
INSERT INTO au_workflow_graph_revision(
    workflow_graph_revision_id,workflow_graph_id,graph_version,graph_hash,created_at_utc)
VALUES(202,201,1,'contract-graph-hash',1);
UPDATE au_workflow_graph SET active_revision_id=202 WHERE workflow_graph_id=201;
INSERT INTO au_workflow_graph_revision_node(
    workflow_graph_revision_node_id,workflow_graph_revision_id,node_key,unit_kind,
    display_name,authored_ref_kind,authored_ref_id,ordinal)
VALUES(203,202,'probe','seed_probe_chain','Seed Probe','seed_probe_spec',9,0);
INSERT INTO au_workflow_graph_revision_node(
    workflow_graph_revision_node_id,workflow_graph_revision_id,node_key,unit_kind,
    display_name,authored_ref_kind,authored_ref_id,ordinal)
VALUES
    (206,202,'battle-probe','battle_seed_probe','Battle Probe','seed_probe_spec',9,1),
    (207,202,'dungeon-probe','dungeon_seed_probe','Dungeon Probe','seed_probe_spec',9,2),
    (208,202,'overworld-probe','overworld_seed_probe','Overworld Probe','seed_probe_spec',9,3);
INSERT INTO au_workflow_graph_revision_node_input(
    workflow_graph_revision_node_input_id,workflow_graph_revision_node_id,
    input_key,data_kind,display_name,required,ordinal)
VALUES(204,203,'entry_savestate','state.movie_inactive_savestate_id','Entry',1,0);
INSERT INTO au_workflow_graph_revision_node_output(
    workflow_graph_revision_node_output_id,workflow_graph_revision_node_id,
    output_key,data_kind,display_name,ordinal)
VALUES(205,203,'seed_probe_run','analysis.seed_probe_run','Run',0);
INSERT INTO au_workflow_graph_revision_edge(
    workflow_graph_revision_edge_id,workflow_graph_revision_id,
    from_revision_node_id,output_key,to_revision_node_id,input_key,ordinal)
VALUES(209,202,203,'seed_probe_run',206,'entry_savestate',0);
)SQL"));

    ASSERT_TRUE(ExecSql(legacy_db.get(), contract->sql.c_str()));
    EXPECT_EQ(ReadText(legacy_db.get(),
        "SELECT ref_kind FROM au_workflow_graph_revision_node_input WHERE workflow_graph_revision_node_input_id=204;"),
        "state.savestate");
    EXPECT_EQ(ReadText(legacy_db.get(),
        "SELECT ref_kind FROM au_workflow_graph_revision_node_output WHERE workflow_graph_revision_node_output_id=205;"),
        "sp_probe_run");
    EXPECT_EQ(ReadInt64(legacy_db.get(),
        "SELECT COUNT(1) FROM au_workflow_graph_revision_node_argument WHERE workflow_graph_revision_node_id=203 AND argument_key='samples_per_axis' AND default_value='5';"),
        1);
    EXPECT_EQ(ReadInt64(legacy_db.get(),
        "SELECT COUNT(1) FROM au_battle_plan_turn WHERE plan_turn_id=102 AND default_predicate_bundle_revision_id IS NULL;"),
        1);

    ASSERT_TRUE(ExecSql(legacy_db.get(), adaptive->sql.c_str()));
    EXPECT_EQ(ReadInt64(legacy_db.get(),
        "SELECT COUNT(1) FROM au_workflow_graph_revision_node WHERE workflow_graph_revision_id=202 AND unit_kind='seed_probe';"),
        4);
    EXPECT_EQ(ReadInt64(legacy_db.get(),
        "SELECT COUNT(1) FROM au_workflow_graph_revision_node WHERE workflow_graph_revision_node_id IN (203,206,207,208);"),
        4);
    EXPECT_EQ(ReadInt64(legacy_db.get(),
        "SELECT COUNT(1) FROM au_workflow_graph_revision_edge WHERE workflow_graph_revision_edge_id=209 AND from_revision_node_id=203 AND to_revision_node_id=206;"),
        1);
    EXPECT_EQ(ReadInt64(legacy_db.get(),
        "SELECT COUNT(1) FROM au_workflow_graph_revision_node_argument WHERE workflow_graph_revision_node_id IN (203,206,207,208) AND argument_key='samples_per_axis';"),
        4);
    EXPECT_EQ(ReadInt64(legacy_db.get(),
        "SELECT active_revision_id FROM au_workflow_graph WHERE workflow_graph_id=201;"),
        202);
    EXPECT_EQ(ReadText(legacy_db.get(),
        "SELECT graph_hash FROM au_workflow_graph_revision WHERE workflow_graph_revision_id=202;"),
        "adaptive-seedprobe-v1:contract-graph-hash");

    const auto direct_battle = std::ranges::find(
        migrations,
        "202608171500_authoring_direct_battle_workflow.sql",
        &MigrationEntry::name);
    ASSERT_NE(direct_battle, migrations.end());
    ASSERT_LT(adaptive, direct_battle);
    ASSERT_TRUE(ExecSql(legacy_db.get(), R"SQL(
INSERT INTO au_workflow_graph_revision_node(
    workflow_graph_revision_node_id,workflow_graph_revision_id,node_key,unit_kind,
    display_name,authored_ref_kind,authored_ref_id,ordinal)
VALUES(210,202,'battle','battle_chain','Battle Chain',
       'authoring.battle_chain_spec',77,4);
)SQL"));
    for (auto migration = std::next(adaptive);
         migration != std::next(direct_battle);
         ++migration) {
        ASSERT_TRUE(ExecSql(legacy_db.get(), migration->sql.c_str()))
            << migration->name;
    }
    EXPECT_EQ(ReadText(legacy_db.get(),
        "SELECT unit_kind FROM au_workflow_graph_revision_node WHERE workflow_graph_revision_node_id=210;"),
        "battle");
    EXPECT_EQ(ReadText(legacy_db.get(),
        "SELECT authored_ref_kind FROM au_workflow_graph_revision_node WHERE workflow_graph_revision_node_id=210;"),
        "authoring.battle_plan");
    EXPECT_EQ(ReadInt64(legacy_db.get(),
        "SELECT authored_ref_id IS NULL FROM au_workflow_graph_revision_node WHERE workflow_graph_revision_node_id=210;"),
        1);
    EXPECT_EQ(ReadText(legacy_db.get(),
        "SELECT graph_hash FROM au_workflow_graph_revision WHERE workflow_graph_revision_id=202;"),
        "direct-battle-plan-v1:adaptive-seedprobe-v1:contract-graph-hash");
    EXPECT_EQ(ReadInt64(legacy_db.get(),
        "SELECT COUNT(1) FROM au_workflow_graph_revision_node_argument WHERE workflow_graph_revision_node_id=210;"),
        3);
    EXPECT_EQ(ReadInt64(legacy_db.get(),
        "SELECT COUNT(1) FROM au_workflow_graph_revision_node_argument_choice c "
        "JOIN au_workflow_graph_revision_node_argument a "
        "ON a.workflow_graph_revision_node_argument_id=c.workflow_graph_revision_node_argument_id "
        "WHERE a.workflow_graph_revision_node_id=210 AND a.argument_key='continuation_mode';"),
        2);
    EXPECT_EQ(ReadText(legacy_db.get(),
        "SELECT choice_value FROM au_workflow_graph_revision_node_argument_choice c "
        "JOIN au_workflow_graph_revision_node_argument a "
        "ON a.workflow_graph_revision_node_argument_id=c.workflow_graph_revision_node_argument_id "
        "WHERE a.workflow_graph_revision_node_id=210 AND c.ordinal=0;"),
        "manual_selection");
    EXPECT_EQ(ReadInt64(legacy_db.get(),
        "SELECT COUNT(1) FROM sqlite_master WHERE type='table' "
        "AND name IN ('au_battle_chain_spec','au_explorer_settings','au_battle_run_spec');"),
        0);

    sqlite3* raw_unknown_db = nullptr;
    ASSERT_EQ(sqlite3_open(":memory:", &raw_unknown_db), SQLITE_OK);
    const std::unique_ptr<sqlite3, decltype(&sqlite3_close)> unknown_db(
        raw_unknown_db, &sqlite3_close);
    for (auto migration = migrations.begin(); migration != contract; ++migration) {
        ASSERT_TRUE(ExecSql(unknown_db.get(), migration->sql.c_str()))
            << migration->name;
    }
    ASSERT_TRUE(ExecSql(unknown_db.get(), R"SQL(
INSERT INTO au_workflow_graph(workflow_graph_id,name,active_revision_id,created_at_utc)
VALUES(301,'unknown-contract',NULL,1);
INSERT INTO au_workflow_graph_revision(
    workflow_graph_revision_id,workflow_graph_id,graph_version,graph_hash,status,created_at_utc)
VALUES(302,301,1,'unknown-contract-hash','active',1);
INSERT INTO au_workflow_graph_revision_node(
    workflow_graph_revision_node_id,workflow_graph_revision_id,node_key,unit_kind,ordinal)
VALUES(303,302,'unknown','seed_probe_chain',0);
INSERT INTO au_workflow_graph_revision_node_input(
    workflow_graph_revision_node_input_id,workflow_graph_revision_node_id,
    input_key,data_kind,required,ordinal)
VALUES(304,303,'unknown_input','unknown.data.kind',1,0);
)SQL"));
    EXPECT_FALSE(ExecSql(unknown_db.get(), contract->sql.c_str()));
    (void)sqlite3_exec(unknown_db.get(), "ROLLBACK;", nullptr, nullptr, nullptr);
}

TEST_F(SqliteDbFixture, PredicateIdentityHardCutClearsInterimAuthoringAndRemovesBundleModel) {
    using namespace savor::db::migrations;

    const auto migrations = LoadContextMigrations(
        MigrationContext::Authoring,
        {.source_kind = MigrationSourceKind::Embedded});
    const auto hard_cut = std::ranges::find(
        migrations,
        "202608161400_authoring_predicate_groups.sql",
        &MigrationEntry::name);
    ASSERT_NE(hard_cut, migrations.end());

    sqlite3* raw_legacy_db = nullptr;
    ASSERT_EQ(sqlite3_open(":memory:", &raw_legacy_db), SQLITE_OK);
    const std::unique_ptr<sqlite3, decltype(&sqlite3_close)> legacy_db(
        raw_legacy_db, &sqlite3_close);
    for (auto migration = migrations.begin(); migration != hard_cut; ++migration) {
        ASSERT_TRUE(ExecSql(legacy_db.get(), migration->sql.c_str()))
            << migration->name;
    }

    ASSERT_TRUE(ExecSql(legacy_db.get(), R"SQL(
INSERT INTO au_predicate_definition_v2(
    predicate_definition_id,stable_key,name,description,created_at_utc)
VALUES(91,'predicate.kept','Kept Predicate','pure definition',1);
INSERT INTO au_predicate_definition_revision_v2(
    predicate_definition_revision_id,predicate_definition_id,revision_number,
    revision_state,root_node_ordinal,content_sha256,created_at_utc,published_at_utc)
VALUES(92,91,1,'DRAFT',0,
    '1111111111111111111111111111111111111111111111111111111111111111',1,NULL);
INSERT INTO au_predicate_expression_node_v2(
    predicate_definition_revision_id,node_ordinal,node_kind,result_builtin_type,
    literal_kind,literal_integer,source_label)
VALUES(92,0,'LITERAL',1,'BOOL',1,'true');
UPDATE au_predicate_definition_revision_v2
SET revision_state='PUBLISHED',published_at_utc=1
WHERE predicate_definition_revision_id=92;
INSERT INTO au_predicate_bundle_v2(
    predicate_bundle_id,stable_key,name,description,created_at_utc)
VALUES(93,'predicate.bundle.removed','Removed bundle','obsolete',1);
INSERT INTO au_predicate_bundle_revision_v2(
    predicate_bundle_revision_id,predicate_bundle_id,revision_number,
    revision_state,content_sha256,created_at_utc,published_at_utc)
VALUES(94,93,1,'PUBLISHED',
    '2222222222222222222222222222222222222222222222222222222222222222',1,1);
INSERT INTO au_battle_plan(plan_id,name,fingerprint,created_at_utc)
VALUES(95,'hard-cut-plan','hard-cut-plan',1);
INSERT INTO au_battle_plan_turn(
    plan_turn_id,plan_id,turn_index,default_predicate_bundle_revision_id)
VALUES(96,95,1,94);
)SQL"));

    ASSERT_TRUE(ExecSql(legacy_db.get(), hard_cut->sql.c_str()));
    EXPECT_EQ(ReadInt64(legacy_db.get(),
        "SELECT COUNT(1) FROM au_predicate_definition_v2;"), 0);
    EXPECT_EQ(ReadInt64(legacy_db.get(),
        "SELECT COUNT(1) FROM au_predicate_definition_revision_v2;"), 0);
    EXPECT_EQ(ReadInt64(legacy_db.get(),
        "SELECT COUNT(1) FROM sqlite_master WHERE type='table' "
        "AND name LIKE 'au_predicate_bundle%';"), 0);
    EXPECT_EQ(ReadInt64(legacy_db.get(),
        "SELECT COUNT(1) FROM sqlite_master WHERE type='table' "
        "AND name='au_predicate_check_use_v2';"), 0);
    EXPECT_EQ(ReadInt64(legacy_db.get(),
        "SELECT COUNT(1) FROM pragma_table_info('au_battle_plan_turn') "
        "WHERE name='default_predicate_bundle_revision_id';"), 0);
    EXPECT_EQ(ReadInt64(legacy_db.get(),
        "SELECT COUNT(1) FROM au_battle_plan_turn WHERE plan_turn_id=96 "
        "AND default_predicate_group_revision_id IS NULL;"), 1);
    EXPECT_EQ(ReadInt64(legacy_db.get(),
        "SELECT COUNT(1) FROM sqlite_master WHERE type='table' AND name IN ("
        "'au_predicate_execution_binding','au_predicate_execution_binding_revision',"
        "'au_predicate_group','au_predicate_group_revision','au_predicate_group_member',"
        "'au_predicate_group_member_hook');"), 6);
    EXPECT_EQ(ReadInt64(legacy_db.get(),
        "SELECT COUNT(1) FROM sqlite_master WHERE type='table' "
        "AND name='au_predicate_authoring_request';"), 1);
    EXPECT_EQ(ReadInt64(legacy_db.get(),
        "SELECT COUNT(1) FROM pragma_table_info('au_predicate_definition_v2') "
        "WHERE name='updated_at_utc' AND [notnull]=1;"), 1);
    EXPECT_EQ(ReadInt64(legacy_db.get(),
        "SELECT COUNT(1) FROM pragma_table_info('au_predicate_definition_revision_v2') "
        "WHERE name='semantic_sha256' AND [notnull]=1;"), 1);
}

TEST_F(SqliteDbFixture, PredicateIdentitySchemaRepairRebuildsStaleAppliedSchemaWithoutTouchingBattlePlans) {
    using namespace savor::db::migrations;
    const auto migrations = LoadContextMigrations(
        MigrationContext::Authoring,
        {.source_kind = MigrationSourceKind::Embedded});
    const auto hard_cut = std::ranges::find(migrations,
        "202608161400_authoring_predicate_groups.sql", &MigrationEntry::name);
    const auto repair = std::ranges::find(migrations,
        "202608162300_authoring_predicate_identity_schema_repair.sql", &MigrationEntry::name);
    const auto observation_planning = std::ranges::find(migrations,
        "202608171130_authoring_predicate_observation_planning.sql",
        &MigrationEntry::name);
    ASSERT_NE(hard_cut, migrations.end());
    ASSERT_NE(repair, migrations.end());
    ASSERT_NE(observation_planning, migrations.end());
    ASSERT_LT(hard_cut, repair);
    ASSERT_LT(repair, observation_planning);

    sqlite3* raw = nullptr;
    ASSERT_EQ(sqlite3_open(":memory:", &raw), SQLITE_OK);
    const std::unique_ptr<sqlite3, decltype(&sqlite3_close)> stale(raw, &sqlite3_close);
    for (auto migration = migrations.begin(); migration != repair; ++migration)
        ASSERT_TRUE(ExecSql(stale.get(), migration->sql.c_str())) << migration->name;

    ASSERT_TRUE(ExecSql(stale.get(), R"SQL(
INSERT INTO au_battle_plan(plan_id,name,fingerprint,created_at_utc)
VALUES(710,'preserved plan','preserved-plan',1);
INSERT INTO au_battle_plan_turn(plan_turn_id,plan_id,turn_index,default_predicate_group_revision_id)
VALUES(711,710,1,NULL);
DROP TRIGGER IF EXISTS au_predicate_definition_published_immutable;
DROP TRIGGER IF EXISTS au_predicate_definition_published_delete_immutable;
DROP TRIGGER IF EXISTS au_predicate_definition_child_insert_immutable;
DROP TRIGGER IF EXISTS au_predicate_definition_child_update_immutable;
DROP TRIGGER IF EXISTS au_predicate_definition_child_delete_immutable;
DROP TRIGGER IF EXISTS au_predicate_definition_node_insert_immutable;
DROP TRIGGER IF EXISTS au_predicate_definition_node_update_immutable;
DROP TRIGGER IF EXISTS au_predicate_definition_node_delete_immutable;
DROP TRIGGER IF EXISTS au_predicate_definition_edge_insert_immutable;
DROP TRIGGER IF EXISTS au_predicate_definition_edge_update_immutable;
DROP TRIGGER IF EXISTS au_predicate_definition_edge_delete_immutable;
DROP TRIGGER IF EXISTS au_predicate_execution_binding_published_immutable;
DROP TRIGGER IF EXISTS au_predicate_execution_binding_published_delete_immutable;
DROP TRIGGER IF EXISTS au_predicate_execution_binding_source_published_insert_immutable;
DROP TRIGGER IF EXISTS au_predicate_execution_binding_source_published_update_immutable;
DROP TRIGGER IF EXISTS au_predicate_execution_binding_source_published_delete_immutable;
DROP TRIGGER IF EXISTS au_predicate_group_published_immutable;
DROP TRIGGER IF EXISTS au_predicate_group_published_delete_immutable;
DROP TRIGGER IF EXISTS au_predicate_group_member_published_insert_immutable;
DROP TRIGGER IF EXISTS au_predicate_group_member_published_update_immutable;
DROP TRIGGER IF EXISTS au_predicate_group_member_published_delete_immutable;
DROP TRIGGER IF EXISTS au_predicate_group_hook_published_insert_immutable;
DROP TRIGGER IF EXISTS au_predicate_group_hook_published_update_immutable;
DROP TRIGGER IF EXISTS au_predicate_group_hook_published_delete_immutable;
DROP TABLE au_predicate_group_member_hook;
DROP TABLE au_predicate_group_member;
DROP TABLE au_predicate_group_revision;
DROP TABLE au_predicate_group;
DROP TABLE au_predicate_execution_binding_witness_source;
DROP TABLE au_predicate_execution_binding_revision;
DROP TABLE au_predicate_execution_binding;
DROP TABLE au_predicate_expression_edge_v2;
DROP TABLE au_predicate_expression_node_v2;
DROP TABLE au_predicate_witness_v2;
DROP TABLE au_predicate_definition_revision_v2;
DROP TABLE au_predicate_definition_v2;
DROP TABLE au_predicate_authoring_request;
CREATE TABLE au_predicate_definition_v2(
 predicate_definition_id INTEGER PRIMARY KEY,stable_key TEXT NOT NULL UNIQUE,
 name TEXT NOT NULL,description TEXT,created_at_utc INTEGER NOT NULL);
CREATE TABLE au_predicate_definition_revision_v2(
 predicate_definition_revision_id INTEGER PRIMARY KEY,predicate_definition_id INTEGER NOT NULL,
 revision_number INTEGER NOT NULL,revision_state TEXT NOT NULL,root_node_ordinal INTEGER NOT NULL,
 content_sha256 TEXT,created_at_utc INTEGER NOT NULL,published_at_utc INTEGER);
CREATE TABLE au_predicate_group(
 predicate_group_id INTEGER PRIMARY KEY,stable_key TEXT NOT NULL UNIQUE,
 name TEXT NOT NULL,description TEXT NOT NULL DEFAULT '',created_at_utc INTEGER NOT NULL);
CREATE TABLE au_predicate_group_revision(
 predicate_group_revision_id INTEGER PRIMARY KEY,predicate_group_id INTEGER NOT NULL,
 revision_number INTEGER NOT NULL,revision_state TEXT NOT NULL,content_sha256 TEXT,
 created_at_utc INTEGER NOT NULL,published_at_utc INTEGER,
 FOREIGN KEY(predicate_group_id) REFERENCES au_predicate_group(predicate_group_id));
INSERT INTO au_predicate_definition_v2 VALUES(720,'stale','discarded','stale row',1);
INSERT INTO au_predicate_definition_revision_v2 VALUES(721,720,1,'DRAFT',0,NULL,1,NULL);
)SQL"));

    ASSERT_TRUE(ExecSql(stale.get(), repair->sql.c_str()))
        << sqlite3_errmsg(stale.get());
    ASSERT_TRUE(ExecSql(stale.get(), observation_planning->sql.c_str()))
        << sqlite3_errmsg(stale.get());
    EXPECT_EQ(ReadInt64(stale.get(), "SELECT COUNT(*) FROM au_battle_plan WHERE plan_id=710;"), 1);
    EXPECT_EQ(ReadInt64(stale.get(), "SELECT COUNT(*) FROM au_battle_plan_turn WHERE plan_turn_id=711 AND default_predicate_group_revision_id IS NULL;"), 1);
    EXPECT_EQ(ReadInt64(stale.get(), "SELECT COUNT(*) FROM au_predicate_definition_v2;"), 0);
    EXPECT_EQ(ReadInt64(stale.get(), "SELECT COUNT(*) FROM pragma_table_info('au_predicate_definition_v2') WHERE name='updated_at_utc' AND [notnull]=1;"), 1);
    EXPECT_EQ(ReadInt64(stale.get(), "SELECT COUNT(*) FROM pragma_table_info('au_predicate_definition_revision_v2') WHERE name='semantic_sha256' AND [notnull]=1;"), 1);
    EXPECT_EQ(ReadInt64(stale.get(), "SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name='au_predicate_authoring_request';"), 1);
    EXPECT_EQ(ReadInt64(stale.get(),
        "SELECT COUNT(*) FROM sqlite_master WHERE type='table' "
        "AND name='au_predicate_execution_binding_witness_source' "
        "AND sql LIKE '%DERIVED_STATE_QUERY%' "
        "AND sql NOT LIKE '%CURRENT_HOOK_QUERY%';"), 1);
    EXPECT_EQ(ReadInt64(stale.get(),
        "SELECT COUNT(*) FROM sqlite_master WHERE type='trigger' "
        "AND name LIKE 'au_predicate_execution_binding_source_published_%';"), 3);
    EXPECT_EQ(ReadText(stale.get(), "PRAGMA integrity_check;"), "ok");

    sqlite3* raw_fresh = nullptr;
    ASSERT_EQ(sqlite3_open(":memory:", &raw_fresh), SQLITE_OK);
    const std::unique_ptr<sqlite3, decltype(&sqlite3_close)> fresh(raw_fresh, &sqlite3_close);
    for (auto migration = migrations.begin(); migration != repair; ++migration)
        ASSERT_TRUE(ExecSql(fresh.get(), migration->sql.c_str())) << migration->name;
    ASSERT_TRUE(ExecSql(fresh.get(), repair->sql.c_str()))
        << sqlite3_errmsg(fresh.get());
    ASSERT_TRUE(ExecSql(fresh.get(), observation_planning->sql.c_str()))
        << sqlite3_errmsg(fresh.get());
    EXPECT_EQ(ReadText(fresh.get(), "PRAGMA integrity_check;"), "ok");
    EXPECT_EQ(ReadInt64(fresh.get(), "SELECT COUNT(*) FROM pragma_table_info('au_predicate_group') WHERE name='updated_at_utc' AND [notnull]=1;"), 1);
}

TEST_F(SqliteDbFixture, SeedProbeEntryQualifiedFlavorMigrationPreservesExistingSets) {
    using namespace savor::db::migrations;
    const auto migrations = LoadContextMigrations(
        MigrationContext::AnalysisSeedProbe,
        {.source_kind = MigrationSourceKind::Embedded});
    const auto entry_qualified = std::ranges::find(
        migrations,
        "202608161200_analysisseedprobe_entry_qualified_flavor.sql",
        &MigrationEntry::name);
    ASSERT_NE(entry_qualified, migrations.end());

    sqlite3* raw = nullptr;
    ASSERT_EQ(sqlite3_open(":memory:", &raw), SQLITE_OK);
    const std::unique_ptr<sqlite3, decltype(&sqlite3_close)> db(
        raw, &sqlite3_close);
    for (auto migration = migrations.begin(); migration != entry_qualified; ++migration) {
        ASSERT_TRUE(ExecSql(db.get(), migration->sql.c_str())) << migration->name;
    }
    ASSERT_TRUE(ExecSql(db.get(), R"SQL(
INSERT INTO sp_probe_set(
    probe_set_id,name,probe_flavor,breakpoint_policy_name,
    segment_source_kind,created_at_utc)
VALUES(1,'existing-battle','BATTLE_PRE','seedprobe.battle','test',1);
)SQL"));

    ASSERT_TRUE(ExecSql(db.get(), entry_qualified->sql.c_str()));
    EXPECT_EQ(ReadInt64(db.get(),
        "SELECT COUNT(1) FROM sp_probe_set WHERE probe_set_id=1 AND probe_flavor='BATTLE_PRE';"), 1);
    ASSERT_TRUE(ExecSql(db.get(), R"SQL(
INSERT INTO sp_probe_set(
    probe_set_id,name,probe_flavor,breakpoint_policy_name,
    segment_source_kind,created_at_utc)
VALUES(2,'entry-qualified','ENTRY_QUALIFIED','seedprobe.entry_pc.v1','test',2);
)SQL"));
    EXPECT_EQ(ReadInt64(db.get(),
        "SELECT COUNT(1) FROM sp_probe_set WHERE probe_set_id=2 AND probe_flavor='ENTRY_QUALIFIED';"), 1);
}

TEST_F(SqliteDbFixture, BattleCompletionArtifactMigrationPreservesExistingArtifacts) {
    using namespace savor::db::migrations;

    sqlite3* raw_legacy_db = nullptr;
    ASSERT_EQ(sqlite3_open(":memory:", &raw_legacy_db), SQLITE_OK);
    const std::unique_ptr<sqlite3, decltype(&sqlite3_close)> legacy_db(
        raw_legacy_db, &sqlite3_close);

    const auto migrations = LoadContextMigrations(
        MigrationContext::State,
        {.source_kind = MigrationSourceKind::Embedded});
    const auto battle_context = std::ranges::find(
        migrations,
        "202608081000_state_battle_context_artifact.sql",
        &MigrationEntry::name);
    const auto battle_completion = std::ranges::find(
        migrations,
        "202608131100_state_battle_completion_artifact.sql",
        &MigrationEntry::name);
    ASSERT_NE(battle_context, migrations.end());
    ASSERT_NE(battle_completion, migrations.end());
    ASSERT_LT(battle_context, battle_completion);

    for (auto migration = migrations.begin();
         migration != std::next(battle_context);
         ++migration)
    {
        ASSERT_TRUE(ExecSql(legacy_db.get(), migration->sql.c_str()))
            << migration->name;
    }
    ASSERT_TRUE(ExecSql(legacy_db.get(),
        "INSERT INTO state_artifact(artifact_id,sha256,size_bytes,"
        "compression_kind,filename,file_ext,artifact_kind,created_at_utc) "
        "VALUES(1,'existing-context',4,0,'context.bctx','.bctx',"
        "'BATTLE_CONTEXT',1);"));

    ASSERT_TRUE(ExecSql(
        legacy_db.get(), battle_completion->sql.c_str()));
    EXPECT_EQ(ReadText(legacy_db.get(),
        "SELECT artifact_kind FROM state_artifact WHERE artifact_id=1;"),
        "BATTLE_CONTEXT");
    EXPECT_TRUE(ExecSql(legacy_db.get(),
        "INSERT INTO state_artifact(artifact_id,sha256,size_bytes,"
        "compression_kind,filename,file_ext,artifact_kind,created_at_utc) "
        "VALUES(2,'completion-manifest',4,0,'completion.bcmb','.bcmb',"
        "'BATTLE_COMPLETION',2);"));
    EXPECT_FALSE(ExecSql(legacy_db.get(),
        "INSERT INTO state_artifact(artifact_id,sha256,size_bytes,"
        "compression_kind,filename,file_ext,artifact_kind,created_at_utc) "
        "VALUES(3,'invalid-kind',4,0,'invalid.bin','.bin',"
        "'INVALID_KIND',3);"));
}

TEST_F(SqliteDbFixture, DerivedAddressRegionMigrationDropsOnlyLegacyRows) {
    using namespace savor::db::migrations;

    sqlite3* raw_legacy_db = nullptr;
    ASSERT_EQ(sqlite3_open(":memory:", &raw_legacy_db), SQLITE_OK);
    const std::unique_ptr<sqlite3, decltype(&sqlite3_close)> legacy_db(
        raw_legacy_db, &sqlite3_close);
    const auto migrations = LoadContextMigrations(
        MigrationContext::Authoring,
        {.source_kind = MigrationSourceKind::Embedded});
    const auto symbols = std::ranges::find(
        migrations, "202606101300_authoring_runtime_symbol_packs.sql",
        &MigrationEntry::name);
    const auto removal = std::ranges::find(
        migrations, "202608111100_authoring_remove_derived_address_region.sql",
        &MigrationEntry::name);
    ASSERT_NE(symbols, migrations.end());
    ASSERT_NE(removal, migrations.end());
    ASSERT_TRUE(ExecSql(legacy_db.get(), symbols->sql.c_str()));
    ASSERT_TRUE(ExecSql(legacy_db.get(),
        "INSERT INTO au_runtime_symbol_pack("
        "runtime_symbol_pack_id,pack_id,schema_name,schema_version,name,"
        "created_at_utc,imported_at_utc) VALUES(1,'test.pack',"
        "'savor.runtime-symbol-pack',1,'test',1,1);"
        "INSERT INTO au_address_symbol("
        "address_symbol_id,runtime_symbol_pack_id,stable_id,name,region,"
        "base_address,ordinal) VALUES"
        "(1,1,'user.addr.mem1','mem1','MEM1',1,0),"
        "(2,1,'user.addr.mem2','mem2','MEM2',2,1),"
        "(3,1,'user.addr.derived','derived','DERIVED',3,2);"));

    ASSERT_TRUE(ExecSql(legacy_db.get(), removal->sql.c_str()));
    EXPECT_EQ(ReadInt64(legacy_db.get(),
        "SELECT COUNT(1) FROM au_address_symbol;"), 2);
    EXPECT_EQ(ReadInt64(legacy_db.get(),
        "SELECT COUNT(1) FROM au_address_symbol WHERE region='DERIVED';"), 0);
    EXPECT_FALSE(ExecSql(legacy_db.get(),
        "INSERT INTO au_address_symbol("
        "address_symbol_id,runtime_symbol_pack_id,stable_id,name,region,"
        "base_address,ordinal) VALUES"
        "(4,1,'user.addr.rejected','rejected','DERIVED',4,3);"));
}

TEST_F(SqliteDbFixture, DerivedWorksetMigrationBackfillsCanonicalEmptyBinding) {
    using namespace savor::db::migrations;

    sqlite3* raw_legacy_db = nullptr;
    ASSERT_EQ(sqlite3_open(":memory:", &raw_legacy_db), SQLITE_OK);
    const std::unique_ptr<sqlite3, decltype(&sqlite3_close)> legacy_db(
        raw_legacy_db, &sqlite3_close);
    ASSERT_TRUE(ExecSql(legacy_db.get(),
        "CREATE TABLE exec_workset(workset_id INTEGER PRIMARY KEY);"
        "INSERT INTO exec_workset(workset_id) VALUES(1);"));
    const auto migrations = LoadContextMigrations(
        MigrationContext::Execution,
        {.source_kind = MigrationSourceKind::Embedded});
    const auto migration = std::ranges::find(
        migrations, "202608111000_execution_derived_state_binding.sql",
        &MigrationEntry::name);
    ASSERT_NE(migration, migrations.end());
    ASSERT_TRUE(ExecSql(legacy_db.get(), migration->sql.c_str()));
    EXPECT_EQ(ReadText(legacy_db.get(),
        "SELECT derived_state_binding_sha256 FROM exec_workset WHERE workset_id=1;"),
        "4bf4d7c8b3d9d28238f46029b93c4649ec76fdfdd387a49565b630560ab8240f");
    EXPECT_EQ(ReadText(legacy_db.get(),
        "SELECT lower(hex(derived_state_binding_payload)) FROM exec_workset WHERE workset_id=1;"),
        "0100000001000000000000004000000034626634643763386233643964323832333866343630323962393363343634396563373666646664643338376134393536356236333035363061623832343066");
}

TEST_F(
    SqliteDbFixture,
    CoordinatorRuntimeOpensCancellationAdmissionOnlyAfterAtomicStartup) {
    using namespace savor::runner::parallel::savordb;

    savor::db::execution::programdb::ProgramKindRegistry registry;
    CoordinatorRuntime runtime;
    CoordinatorRuntimeConfig config{
        .worker = WorkerCoordinatorConfig{.desired_workers = 0},
        .poll_interval = std::chrono::milliseconds(2),
        .state_compatibility = {
            .game_id = "TEST00",
            .iso_sha256 = std::string(64, '0'),
            .emulator_build = "test-emulator",
            .runtime_revision = "coordinator-runtime-test",
        },
        .initially_paused = true,
        .object_store_root = temp_root_ / "coordinator-object-store",
    };

    std::string error;
    ASSERT_TRUE(runtime.Start(
        db_service_->ExecutionDb(),
        db_service_->AuthoringDb(),
        &registry,
        std::move(config),
        &error)) << error;
    EXPECT_TRUE(runtime.IsStarted());
    EXPECT_TRUE(runtime.IsExecutionPaused());

    auto telemetry = runtime.SnapshotTelemetry();
    EXPECT_TRUE(telemetry.execution.blob_store_ready);
    EXPECT_TRUE(telemetry.execution.cancellation_admission_open);
    EXPECT_TRUE(telemetry.execution.user_admission_paused);
    EXPECT_TRUE(telemetry.worker_admission_paused);

    runtime.SetExecutionPaused(false);
    EXPECT_FALSE(runtime.IsExecutionPaused());
    telemetry = runtime.SnapshotTelemetry();
    EXPECT_TRUE(telemetry.execution.cancellation_admission_open);
    EXPECT_FALSE(telemetry.execution.user_admission_paused);
    EXPECT_FALSE(telemetry.worker_admission_paused);

    EXPECT_TRUE(runtime.Stop(&error)) << error;
    EXPECT_FALSE(runtime.IsStarted());
    EXPECT_TRUE(runtime.Stop(&error)) << error;
}

TEST_F(
    SqliteDbFixture,
    CoordinatorRuntimeStartupFailureUnwindsEarlierServices) {
    using namespace savor::runner::parallel::savordb;

    const auto object_store = temp_root_ / "blocked-object-store";
    ASSERT_TRUE(std::filesystem::create_directories(object_store));
    {
        std::ofstream blocker(
            object_store / "worker_results",
            std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(blocker);
        blocker << "not a directory";
    }

    savor::db::execution::programdb::ProgramKindRegistry registry;
    CoordinatorRuntime runtime;
    CoordinatorRuntimeConfig config{
        .worker = WorkerCoordinatorConfig{.desired_workers = 0},
        .poll_interval = std::chrono::milliseconds(2),
        .state_compatibility = {
            .game_id = "TEST00",
            .iso_sha256 = std::string(64, '0'),
            .emulator_build = "test-emulator",
            .runtime_revision = "coordinator-runtime-test",
        },
        .object_store_root = object_store,
    };

    std::string error;
    EXPECT_FALSE(runtime.Start(
        db_service_->ExecutionDb(),
        db_service_->AuthoringDb(),
        &registry,
        std::move(config),
        &error));
    EXPECT_NE(error.find("job execution coordinator startup failed"),
              std::string::npos) << error;
    EXPECT_FALSE(runtime.IsStarted());
    EXPECT_TRUE(runtime.Stop(&error)) << error;
}

TEST_F(SqliteDbFixture, ProductionProgramKindRegistryBuildsCompleteCatalogAtomicallyWithoutSideEffects) {
    using namespace savor::db::execution::programdb;

    const ProductionProgramKindRegistryDependencies dependencies{
        .execution_db = db_service_->ExecutionDb(),
        .state_db = db_service_->StateDb(),
        .analysis_db = db_service_->AnalysisDb(),
        .authoring_db = db_service_->AuthoringDb(),
    };
    ASSERT_NE(dependencies.execution_db, nullptr);
    ASSERT_NE(dependencies.state_db, nullptr);
    ASSERT_NE(dependencies.analysis_db, nullptr);
    ASSERT_NE(dependencies.authoring_db, nullptr);

    const auto runtime_root_a = temp_root_ / "composition-runtime-a";
    const auto runtime_root_b = temp_root_ / "composition-runtime-b";
    ASSERT_FALSE(std::filesystem::exists(runtime_root_a));
    ASSERT_FALSE(std::filesystem::exists(runtime_root_b));

    const auto schema_rows_before = ReadInt64(
        db_,
        "SELECT COUNT(*) FROM sqlite_schema;");
    const auto migration_rows_before = ReadInt64(
        db_,
        "SELECT COUNT(*) FROM migration_history;");
    const auto tas_movie_requests_before = ReadInt64(
        db_,
        "SELECT COUNT(*) FROM tmv_validation_request;");
    const auto job_sets_before = ReadInt64(
        db_,
        "SELECT COUNT(*) FROM exec_job_set;");

    constexpr std::int32_t kSentinelProgramKind = 31337;
    ProgramKindDescriptor sentinel{};
    sentinel.program_kind = kSentinelProgramKind;
    sentinel.program_name = "atomic-output-sentinel";
    sentinel.default_progress_library_ids = std::vector<std::string>{};
    sentinel.default_derived_state_block_ids = std::vector<std::string>{};

    ProgramKindRegistry output;
    ASSERT_TRUE(output.Register(sentinel));
    ASSERT_TRUE(output.RegisterForStepKind("atomic-output-sentinel", sentinel));

    const auto expect_atomic_failure =
        [&](ProductionProgramKindRegistryDependencies invalid_dependencies) {
            std::string error;
            EXPECT_FALSE(BuildProductionProgramKindRegistry(
                invalid_dependencies,
                MakeProductionProgramKindRegistryConfig(runtime_root_a),
                &output,
                &error));
            EXPECT_FALSE(error.empty());
            const auto* numeric_sentinel = output.Find(kSentinelProgramKind);
            ASSERT_NE(numeric_sentinel, nullptr);
            EXPECT_EQ(numeric_sentinel->program_name, "atomic-output-sentinel");
            const auto* step_sentinel =
                output.FindForStepKind("atomic-output-sentinel");
            ASSERT_NE(step_sentinel, nullptr);
            EXPECT_EQ(step_sentinel->program_name, "atomic-output-sentinel");
            EXPECT_EQ(
                output.Find(static_cast<std::int32_t>(savor::PK_TasMovie)),
                nullptr);
        };

    auto invalid_dependencies = dependencies;
    invalid_dependencies.execution_db = nullptr;
    expect_atomic_failure(invalid_dependencies);
    invalid_dependencies = dependencies;
    invalid_dependencies.state_db = nullptr;
    expect_atomic_failure(invalid_dependencies);
    invalid_dependencies = dependencies;
    invalid_dependencies.analysis_db = nullptr;
    expect_atomic_failure(invalid_dependencies);
    invalid_dependencies = dependencies;
    invalid_dependencies.authoring_db = nullptr;
    expect_atomic_failure(invalid_dependencies);

    std::string error = "must be cleared on success";
    ASSERT_TRUE(BuildProductionProgramKindRegistry(
        dependencies,
        MakeProductionProgramKindRegistryConfig(runtime_root_a),
        &output,
        &error)) << error;
    EXPECT_TRUE(error.empty());
    EXPECT_EQ(output.Find(kSentinelProgramKind), nullptr);
    EXPECT_EQ(output.FindForStepKind("atomic-output-sentinel"), nullptr);

    struct ExpectedDescriptor {
        std::int32_t program_kind;
        const char* program_name;
    };
    constexpr std::array<ExpectedDescriptor, 8> canonical_descriptors{{
        {static_cast<std::int32_t>(savor::PK_TasMovie), "TAS Movie Complete Validation"},
        {static_cast<std::int32_t>(savor::PK_TasMovieCheckpointSterilize), "TAS Movie Checkpoint Sterilization"},
        {static_cast<std::int32_t>(savor::PK_SeedProbe), "SeedProbe"},
        {static_cast<std::int32_t>(savor::PK_BattleContext), "Battle Context"},
        {static_cast<std::int32_t>(savor::PK_BattleSingleTurnRunner), "Battle Single Turn"},
        {static_cast<std::int32_t>(savor::PK_BattleCompletion), "Battle Completion"},
        {static_cast<std::int32_t>(savor::PK_BattleRecord), "Battle Recording"},
        {static_cast<std::int32_t>(savor::PK_BattleReplay), "Battle Replay"},
    }};
    struct ExpectedStepDescriptor {
        const char* step_kind;
        std::int32_t program_kind;
        const char* program_name;
    };
    constexpr std::array<ExpectedStepDescriptor, 11> step_descriptors{{
        {"tasmovie.establish_root_cursor", static_cast<std::int32_t>(savor::PK_TasMovie), "TAS Movie Complete Validation"},
        {"tasmovie.validate_root", static_cast<std::int32_t>(savor::PK_TasMovie), "TAS Movie Complete Validation"},
        {"tasmovie.validate_tree", static_cast<std::int32_t>(savor::PK_TasMovie), "TAS Movie Complete Validation"},
        {"tasmovie.checkpoint_sterilize", static_cast<std::int32_t>(savor::PK_TasMovieCheckpointSterilize), "TAS Movie Checkpoint Sterilization"},
        {"seedprobe.survey", static_cast<std::int32_t>(savor::PK_SeedProbe), "SeedProbe"},
        {"battle.context", static_cast<std::int32_t>(savor::PK_BattleContext), "Battle Context"},
        {"battle.start", static_cast<std::int32_t>(savor::PK_BattleSingleTurnRunner), "Battle Single Turn"},
        {"battle.single_turn", static_cast<std::int32_t>(savor::PK_BattleSingleTurnRunner), "Battle Single Turn"},
        {"battle.completion", static_cast<std::int32_t>(savor::PK_BattleCompletion), "Battle Completion"},
        {"battle.record", static_cast<std::int32_t>(savor::PK_BattleRecord), "Battle Recording"},
        {"battle.replay", static_cast<std::int32_t>(savor::PK_BattleReplay), "Battle Replay"},
    }};

    const auto expect_complete_descriptor =
        [](const ProgramKindDescriptor* descriptor,
           std::int32_t expected_program_kind,
           const char* expected_program_name) {
            ASSERT_NE(descriptor, nullptr);
            EXPECT_EQ(descriptor->program_kind, expected_program_kind);
            EXPECT_EQ(descriptor->program_name, expected_program_name);
            EXPECT_NE(descriptor->job_materializer, nullptr);
            EXPECT_NE(descriptor->workset_reconstruction, nullptr);
            EXPECT_NE(descriptor->result_handler, nullptr);
            EXPECT_TRUE(descriptor->supports_workflow_orchestration);
        };

    for (const auto& expected : canonical_descriptors) {
        expect_complete_descriptor(
            output.Find(expected.program_kind),
            expected.program_kind,
            expected.program_name);
    }
    for (const auto& expected : step_descriptors) {
        expect_complete_descriptor(
            output.FindForStepKind(expected.step_kind),
            expected.program_kind,
            expected.program_name);
    }

    const auto* seed_probe =
        output.Find(static_cast<std::int32_t>(savor::PK_SeedProbe));
    ASSERT_NE(seed_probe, nullptr);
    EXPECT_EQ(seed_probe->program_name, "SeedProbe");
    const auto* completion =
        output.Find(static_cast<std::int32_t>(savor::PK_BattleCompletion));
    ASSERT_NE(completion, nullptr);
    EXPECT_NE(completion->full_phase_identity, std::nullopt);
    ASSERT_TRUE(completion->default_progress_library_ids.has_value());
    EXPECT_TRUE(completion->default_progress_library_ids->empty());
    ASSERT_TRUE(completion->default_derived_state_block_ids.has_value());
    EXPECT_TRUE(completion->default_derived_state_block_ids->empty());
    const auto* recording =
        output.Find(static_cast<std::int32_t>(savor::PK_BattleRecord));
    ASSERT_NE(recording, nullptr);
    EXPECT_NE(recording->full_phase_identity, std::nullopt);
    EXPECT_NE(recording->workflow_transition, nullptr);
    EXPECT_EQ(recording->default_progress_library_ids,
              std::optional<std::vector<std::string>>(
                  std::vector<std::string>{
                      "soa.progress.battle.events/1"}));
    ASSERT_TRUE(recording->default_derived_state_block_ids.has_value());
    EXPECT_TRUE(recording->default_derived_state_block_ids->empty());
    const auto* replay =
        output.Find(static_cast<std::int32_t>(savor::PK_BattleReplay));
    ASSERT_NE(replay, nullptr);
    EXPECT_NE(replay->full_phase_identity, std::nullopt);
    EXPECT_EQ(replay->workflow_transition, nullptr);
    EXPECT_EQ(replay->default_progress_library_ids,
              recording->default_progress_library_ids);
    ASSERT_TRUE(replay->default_derived_state_block_ids.has_value());
    EXPECT_TRUE(replay->default_derived_state_block_ids->empty());
    EXPECT_EQ(output.FindForStepKind("tas_movie"), nullptr);
    EXPECT_EQ(output.FindForStepKind("tasmovie.play"), nullptr);
    EXPECT_EQ(output.FindForStepKind("battle.results_screen"), nullptr);

    ProgramKindRegistry second_registry;
    ASSERT_TRUE(BuildProductionProgramKindRegistry(
        dependencies,
        MakeProductionProgramKindRegistryConfig(runtime_root_b),
        &second_registry,
        &error)) << error;
    for (const auto& expected : canonical_descriptors) {
        expect_complete_descriptor(
            second_registry.Find(expected.program_kind),
            expected.program_kind,
            expected.program_name);
    }
    for (const auto& expected : step_descriptors) {
        expect_complete_descriptor(
            second_registry.FindForStepKind(expected.step_kind),
            expected.program_kind,
            expected.program_name);
    }

    EXPECT_FALSE(std::filesystem::exists(runtime_root_a));
    EXPECT_FALSE(std::filesystem::exists(runtime_root_b));
    EXPECT_EQ(
        ReadInt64(db_, "SELECT COUNT(*) FROM sqlite_schema;"),
        schema_rows_before);
    EXPECT_EQ(
        ReadInt64(db_, "SELECT COUNT(*) FROM migration_history;"),
        migration_rows_before);
    EXPECT_EQ(
        ReadInt64(db_, "SELECT COUNT(*) FROM tmv_validation_request;"),
        tas_movie_requests_before);
    EXPECT_EQ(
        ReadInt64(db_, "SELECT COUNT(*) FROM exec_job_set;"),
        job_sets_before);

    error.clear();
    EXPECT_FALSE(BuildProductionProgramKindRegistry(
        dependencies,
        MakeProductionProgramKindRegistryConfig(runtime_root_a),
        nullptr,
        &error));
    EXPECT_FALSE(error.empty());
}

TEST_F(SqliteDbFixture, BattleCompletionAndRecordingPersistExactIdempotentLineage) {
    using namespace savor::db;

    ASSERT_TRUE(ExecSql(db_, R"SQL(
INSERT INTO ab_battle_set(
    battle_set_id,name,entry_savestate_id,battle_plan_id,
    battle_plan_fingerprint,continuation_mode,status,created_at_utc,
    launch_fake_attack_min,launch_fake_attack_max)
VALUES(8101,'completion-recording-fixture',7001,13,
       'completion-recording-plan-v1','automatic_best_per_ending_rng',
       'VICTORY',1000,0,0);
INSERT INTO ab_seed_candidate(
    seed_candidate_id,battle_set_id,source_probe_result_id,
    source_input_frame_id,seed_value,source_kind,candidate_status,
    created_at_utc)
VALUES(8102,8101,NULL,NULL,1234,'MANUAL','SELECTED',1000);
INSERT INTO ab_turn_wave(
    wave_id,battle_set_id,turn_index,seed_candidate_id,status,
    created_at_utc)
VALUES(8103,8101,1,8102,'COMPLETED',1000);
INSERT INTO ab_turn_job(
    turn_job_id,wave_id,exec_job_id,plan_id,fake_attacks_this_turn,
    fake_attacks_used_before,job_state,has_results,battle_outcome,
    output_savestate_id)
VALUES(8104,8103,8105,13,0,0,'SUCCEEDED',1,2,7002);
)SQL"));

    auto* analysis = db_service_->AnalysisDb();
    ASSERT_NE(analysis, nullptr);
    const auto now = types::UtcTimePoint(std::chrono::milliseconds(2000));
    const std::string completion_terminal(64, 'a');
    const std::string manifest_blob("BCM1\0fixture", 12);
    const std::string manifest_sha = hash::sha256(
        manifest_blob.data(), manifest_blob.size());
    std::string error;
    std::int64_t completion_id = 0;
    const CreateBattleCompletionCommand completion_create{
        .workflow_instance_id = 8201,
        .workflow_step_id = 8202,
        .battle_set_id = 8101,
        .wave_id = 8103,
        .selected_turn_job_id = 8104,
        .selected_execution_job_id = 8105,
        .entry_savestate_id = 7002,
        .status = "QUEUED",
        .created_at_utc = now,
        .correlation_id = "completion-recording-fixture",
        .causation_id = "completion-recording-fixture",
    };
    ASSERT_TRUE(analysis->CreateBattleCompletion(
        completion_create, &completion_id, &error)) << error;
    ASSERT_GT(completion_id, 0);
    const auto queued_completion =
        analysis->GetBattleCompletion(completion_id);
    ASSERT_TRUE(queued_completion.has_value());
    EXPECT_FALSE(queued_completion->completion_savestate_id.has_value());
    EXPECT_FALSE(queued_completion->manifest_artifact_id.has_value());

    std::int64_t repeated_completion_id = 0;
    ASSERT_TRUE(analysis->CreateBattleCompletion(
        completion_create, &repeated_completion_id, &error)) << error;
    EXPECT_EQ(repeated_completion_id, completion_id);
    auto conflicting_completion = completion_create;
    conflicting_completion.entry_savestate_id = 7999;
    EXPECT_FALSE(analysis->CreateBattleCompletion(
        conflicting_completion, nullptr, &error));
    EXPECT_NE(error.find("different immutable lineage"), std::string::npos);

    ASSERT_TRUE(analysis->BindBattleCompletionExecutionJob({
        .battle_completion_id = completion_id,
        .workflow_instance_id = 8201,
        .workflow_step_id = 8202,
        .exec_job_id = 8203,
    }, &error)) << error;
    ASSERT_TRUE(analysis->BindBattleCompletionExecutionJob({
        .battle_completion_id = completion_id,
        .workflow_instance_id = 8201,
        .workflow_step_id = 8202,
        .exec_job_id = 8203,
    }, &error)) << error;

    auto* state = db_service_->StateDb();
    ASSERT_NE(state, nullptr);
    const auto manifest_path = temp_root_ / "completion-result.bcmb";
    {
        std::ofstream out(manifest_path, std::ios::binary);
        out.write(manifest_blob.data(),
                  static_cast<std::streamsize>(manifest_blob.size()));
    }
    std::int64_t manifest_artifact_id = 0;
    ASSERT_TRUE(state->ImportExternalArtifact({
        .absolute_source_path = manifest_path,
        .artifact = {
            .sha256 = manifest_sha,
            .size_bytes = static_cast<std::int64_t>(manifest_blob.size()),
            .compression_kind = 0,
            .display_filename = manifest_path.filename().string(),
            .file_ext = ".bcmb",
            .artifact_kind = "BATTLE_COMPLETION",
            .created_at_utc = now,
            .correlation_id = "completion-recording-fixture",
            .causation_id = "completion-recording-fixture",
        },
    }, &manifest_artifact_id, &error)) << error;

    const CompleteBattleCompletionCommand completion_result{
        .battle_completion_id = completion_id,
        .completion_savestate_id = 7003,
        .manifest_version = 1,
        .manifest_blob = manifest_blob,
        .manifest_sha256 = manifest_sha,
        .manifest_artifact_id = manifest_artifact_id,
        .route_kind = "FIELD_NAVIGATION",
        .transition_filename = "me123a.sct",
        .worker_terminal_sha256 = completion_terminal,
        .status = "COMPLETED",
        .completed_at_utc = now,
        .correlation_id = "completion-recording-fixture",
        .causation_id = "completion-recording-fixture",
    };
    ASSERT_TRUE(analysis->CompleteBattleCompletion(
        completion_result, &error)) << error;
    ASSERT_TRUE(analysis->CompleteBattleCompletion(
        completion_result, &error)) << error;

    const auto completion = analysis->GetBattleCompletion(completion_id);
    ASSERT_TRUE(completion.has_value());
    EXPECT_EQ(completion->exec_job_id, std::optional<std::int64_t>(8203));
    EXPECT_EQ(completion->selected_turn_job_id, 8104);
    EXPECT_EQ(completion->completion_savestate_id,
              std::optional<std::int64_t>(7003));
    EXPECT_EQ(completion->manifest_blob,
              std::optional<std::string>(completion_result.manifest_blob));
    EXPECT_EQ(completion->route_kind,
              std::optional<std::string>("FIELD_NAVIGATION"));
    EXPECT_EQ(completion->transition_filename,
              std::optional<std::string>("me123a.sct"));
    const auto completion_by_job =
        analysis->GetBattleCompletionForExecJob(8203);
    ASSERT_TRUE(completion_by_job.has_value());
    EXPECT_EQ(completion_by_job->battle_completion_id, completion_id);

    auto conflicting_result = completion_result;
    conflicting_result.transition_filename = "me124a.sct";
    EXPECT_FALSE(analysis->CompleteBattleCompletion(
        conflicting_result, &error));
    EXPECT_NE(error.find("different durable result"), std::string::npos);

    ASSERT_TRUE(ExecSql(db_, ("DELETE FROM state_artifact WHERE artifact_id=" +
        std::to_string(manifest_artifact_id) + ";").c_str()));
    const auto completion_after_artifact_deletion =
        analysis->GetBattleCompletion(completion_id);
    ASSERT_TRUE(completion_after_artifact_deletion.has_value());
    EXPECT_EQ(completion_after_artifact_deletion->status, "COMPLETED");
    EXPECT_EQ(completion_after_artifact_deletion->manifest_blob,
              std::optional<std::string>(manifest_blob));
    EXPECT_EQ(completion_after_artifact_deletion->manifest_artifact_id,
              std::optional<std::int64_t>(manifest_artifact_id));

    const std::string replay_plan_sha(64, 'c');
    savor::runtime::battlerecord::BattleReplaySourceBindingV1 paired_binding{
        .source_savestate_id = 7001,
        .source_dtm_artifact_id = 7401,
        .source_itinerary_artifact_id = 7402,
        .source_savestate_sha256 = std::string(64, '1'),
        .source_dtm_sha256 = std::string(64, '2'),
        .source_itinerary_sha256 = std::string(64, '3'),
    };
    paired_binding.canonical_sha256 =
        savor::runtime::battlerecord::ComputeBattleReplaySourceBindingHashV1(
            paired_binding);
    const auto paired_binding_bytes =
        savor::runtime::battlerecord::EncodeBattleReplaySourceBindingV1(
            paired_binding);
    ASSERT_FALSE(paired_binding_bytes.empty());
    const std::string paired_binding_blob(
        reinterpret_cast<const char*>(paired_binding_bytes.data()),
        paired_binding_bytes.size());
    const CreateBattleRecordingCommand recording_create{
        .battle_completion_id = completion_id,
        .workflow_instance_id = 8301,
        .workflow_step_id = 8302,
        .source_savestate_id = 7001,
        .source_dtm_artifact_id = 7401,
        .source_itinerary_artifact_id = 7402,
        .source_binding_version = 1,
        .source_binding_blob = paired_binding_blob,
        .source_binding_sha256 = paired_binding.canonical_sha256,
        .replay_plan_version = 1,
        .replay_plan_blob = std::string("BRP1\0fixture", 12),
        .replay_plan_sha256 = replay_plan_sha,
        .status = "QUEUED",
        .created_at_utc = now,
        .correlation_id = "completion-recording-fixture",
        .causation_id = "completion-recording-fixture",
    };
    std::int64_t recording_id = 0;
    ASSERT_TRUE(analysis->CreateBattleRecording(
        recording_create, &recording_id, &error)) << error;
    ASSERT_GT(recording_id, 0);
    std::int64_t repeated_recording_id = 0;
    ASSERT_TRUE(analysis->CreateBattleRecording(
        recording_create, &repeated_recording_id, &error)) << error;
    EXPECT_EQ(repeated_recording_id, recording_id);

    auto conflicting_recording = recording_create;
    conflicting_recording.replay_plan_blob = "different-plan";
    EXPECT_FALSE(analysis->CreateBattleRecording(
        conflicting_recording, nullptr, &error));
    EXPECT_NE(error.find("different immutable plan"), std::string::npos);

    ASSERT_TRUE(analysis->BindBattleRecordingExecutionJob({
        .battle_recording_id = recording_id,
        .workflow_instance_id = 8301,
        .workflow_step_id = 8302,
        .exec_job_id = 8303,
    }, &error)) << error;
    ASSERT_TRUE(analysis->BindBattleRecordingExecutionJob({
        .battle_recording_id = recording_id,
        .workflow_instance_id = 8301,
        .workflow_step_id = 8302,
        .exec_job_id = 8303,
    }, &error)) << error;

    EXPECT_FALSE(analysis->CompleteBattleRecording({
        .battle_recording_id = recording_id,
        .outcome = "REPLAY_MISMATCH",
        .recorded_dtm_artifact_id = 7500,
        .worker_terminal_sha256 = std::string(64, 'e'),
        .status = "REPLAY_MISMATCH",
        .completed_at_utc = now,
        .correlation_id = "completion-recording-fixture",
        .causation_id = "completion-recording-fixture",
    }, &error));
    const auto still_queued = analysis->GetBattleRecording(recording_id);
    ASSERT_TRUE(still_queued.has_value());
    EXPECT_EQ(still_queued->status, "QUEUED");

    const CompleteBattleRecordingCommand recording_result{
        .battle_recording_id = recording_id,
        .outcome = "RECORDED",
        .recorded_dtm_artifact_id = 7501,
        .recorded_itinerary_artifact_id = 7502,
        .paired_checkpoint_savestate_id = 7004,
        .timing_anchor_version = 1,
        .timing_anchor_blob = std::string("BTA1\0fixture", 12),
        .tas_movie_tree_id = 7601,
        .worker_terminal_sha256 = std::string(64, 'd'),
        .status = "COMPLETED",
        .completed_at_utc = now,
        .correlation_id = "completion-recording-fixture",
        .causation_id = "completion-recording-fixture",
    };
    ASSERT_TRUE(analysis->CompleteBattleRecording(recording_result, &error))
        << error;
    ASSERT_TRUE(analysis->CompleteBattleRecording(recording_result, &error))
        << error;

    ASSERT_TRUE(analysis->BindBattleRecordingValidation({
        .battle_recording_id = recording_id,
        .tas_movie_tree_id = 7601,
        .validation_request_id = 7701,
    }, &error)) << error;
    ASSERT_TRUE(analysis->BindBattleRecordingValidation({
        .battle_recording_id = recording_id,
        .tas_movie_tree_id = 7601,
        .validation_request_id = 7701,
    }, &error)) << error;
    EXPECT_FALSE(analysis->BindBattleRecordingValidation({
        .battle_recording_id = recording_id,
        .tas_movie_tree_id = 7601,
        .validation_request_id = 7702,
    }, &error));

    ASSERT_TRUE(analysis->BindBattleRecordingSterilization({
        .battle_recording_id = recording_id,
        .tas_movie_tree_id = 7601,
        .sterilization_request_id = 7801,
    }, &error)) << error;
    ASSERT_TRUE(analysis->BindBattleRecordingSterilization({
        .battle_recording_id = recording_id,
        .tas_movie_tree_id = 7601,
        .sterilization_request_id = 7801,
    }, &error)) << error;
    EXPECT_FALSE(analysis->BindBattleRecordingSterilization({
        .battle_recording_id = recording_id,
        .tas_movie_tree_id = 7601,
        .sterilization_request_id = 7802,
    }, &error));

    const auto recording = analysis->GetBattleRecording(recording_id);
    ASSERT_TRUE(recording.has_value());
    EXPECT_EQ(recording->exec_job_id, std::optional<std::int64_t>(8303));
    EXPECT_EQ(recording->outcome, std::optional<std::string>("RECORDED"));
    EXPECT_EQ(recording->recorded_dtm_artifact_id,
              std::optional<std::int64_t>(7501));
    EXPECT_EQ(recording->recorded_itinerary_artifact_id,
              std::optional<std::int64_t>(7502));
    EXPECT_EQ(recording->paired_checkpoint_savestate_id,
              std::optional<std::int64_t>(7004));
    EXPECT_EQ(recording->validation_request_id,
              std::optional<std::int64_t>(7701));
    EXPECT_EQ(recording->sterilization_request_id,
              std::optional<std::int64_t>(7801));
    const auto recording_by_job =
        analysis->GetBattleRecordingForExecJob(8303);
    const auto recording_by_tree =
        analysis->GetBattleRecordingForTasMovieTree(7601);
    const auto recording_by_checkpoint =
        analysis->GetBattleRecordingForPairedCheckpoint(7004);
    ASSERT_TRUE(recording_by_job.has_value());
    ASSERT_TRUE(recording_by_tree.has_value());
    ASSERT_TRUE(recording_by_checkpoint.has_value());
    EXPECT_EQ(recording_by_job->battle_recording_id, recording_id);
    EXPECT_EQ(recording_by_tree->battle_recording_id, recording_id);
    EXPECT_EQ(recording_by_checkpoint->battle_recording_id, recording_id);

    const CreateBattleReplayCommand replay_create{
        .battle_completion_id = completion_id,
        .workflow_instance_id = 8401,
        .workflow_step_id = 8402,
        .source_savestate_id = 7001,
        .source_dtm_artifact_id = 7401,
        .source_itinerary_artifact_id = 7402,
        .source_binding_version = 1,
        .source_binding_blob = paired_binding_blob,
        .source_binding_sha256 = paired_binding.canonical_sha256,
        .replay_plan_version = 1,
        .replay_plan_blob = std::string("BRP1\0fixture", 12),
        .replay_plan_sha256 = replay_plan_sha,
        .status = "QUEUED",
        .created_at_utc = now,
        .correlation_id = "completion-replay-fixture",
        .causation_id = "completion-replay-fixture",
    };
    std::int64_t replay_id = 0;
    ASSERT_TRUE(analysis->CreateBattleReplay(
        replay_create, &replay_id, &error)) << error;
    ASSERT_GT(replay_id, 0);
    std::int64_t repeated_replay_id = 0;
    ASSERT_TRUE(analysis->CreateBattleReplay(
        replay_create, &repeated_replay_id, &error)) << error;
    EXPECT_EQ(repeated_replay_id, replay_id);
    auto conflicting_replay = replay_create;
    conflicting_replay.replay_plan_blob = "different-plan";
    EXPECT_FALSE(analysis->CreateBattleReplay(
        conflicting_replay, nullptr, &error));
    ASSERT_TRUE(analysis->BindBattleReplayExecutionJob({
        .battle_replay_id = replay_id,
        .workflow_instance_id = 8401,
        .workflow_step_id = 8402,
        .exec_job_id = 8403,
    }, &error)) << error;
    const std::string observed_transition("FTC1\0fixture", 12);
    const CompleteBattleReplayCommand replay_result{
        .battle_replay_id = replay_id,
        .outcome = "MATCHED",
        .observed_completion_blob = manifest_blob,
        .observed_completion_sha256 = manifest_sha,
        .observed_transition_blob = observed_transition,
        .observed_transition_sha256 = hash::sha256(
            observed_transition.data(), observed_transition.size()),
        .worker_terminal_sha256 = std::string(64, 'a'),
        .status = "MATCHED",
        .completed_at_utc = now,
        .correlation_id = "completion-replay-fixture",
        .causation_id = "completion-replay-fixture",
    };
    ASSERT_TRUE(analysis->CompleteBattleReplay(replay_result, &error)) << error;
    ASSERT_TRUE(analysis->CompleteBattleReplay(replay_result, &error)) << error;
    const auto replay_row = analysis->GetBattleReplay(replay_id);
    const auto replay_by_job = analysis->GetBattleReplayForExecJob(8403);
    ASSERT_TRUE(replay_row.has_value());
    ASSERT_TRUE(replay_by_job.has_value());
    EXPECT_EQ(replay_row->status, "MATCHED");
    EXPECT_EQ(replay_row->outcome, std::optional<std::string>("MATCHED"));
    EXPECT_EQ(replay_row->observed_completion_blob,
              std::optional<std::string>(manifest_blob));
    EXPECT_EQ(replay_by_job->battle_replay_id, replay_id);
    ASSERT_TRUE(ExecSql(db_, ("DELETE FROM ab_battle_replay WHERE battle_replay_id=" +
        std::to_string(replay_id) + ";").c_str()));

    savor::runtime::battlerecord::BattleReplaySourceBindingV1 inactive_binding{
        .source_savestate_id = 7002,
        .source_savestate_sha256 = std::string(64, '4'),
    };
    inactive_binding.canonical_sha256 =
        savor::runtime::battlerecord::ComputeBattleReplaySourceBindingHashV1(
            inactive_binding);
    const auto inactive_binding_bytes =
        savor::runtime::battlerecord::EncodeBattleReplaySourceBindingV1(
            inactive_binding);
    ASSERT_FALSE(inactive_binding_bytes.empty());
    auto inactive_replay_create = replay_create;
    inactive_replay_create.workflow_instance_id = 8411;
    inactive_replay_create.workflow_step_id = 8412;
    inactive_replay_create.source_savestate_id = 7002;
    inactive_replay_create.source_dtm_artifact_id.reset();
    inactive_replay_create.source_itinerary_artifact_id.reset();
    inactive_replay_create.source_binding_blob = std::string(
        reinterpret_cast<const char*>(inactive_binding_bytes.data()),
        inactive_binding_bytes.size());
    inactive_replay_create.source_binding_sha256 =
        inactive_binding.canonical_sha256;
    inactive_replay_create.correlation_id = "completion-replay-inactive";
    inactive_replay_create.causation_id = "completion-replay-inactive";
    std::int64_t inactive_replay_id = 0;
    ASSERT_TRUE(analysis->CreateBattleReplay(
        inactive_replay_create, &inactive_replay_id, &error)) << error;
    const auto inactive_replay = analysis->GetBattleReplay(inactive_replay_id);
    ASSERT_TRUE(inactive_replay.has_value());
    EXPECT_EQ(inactive_replay->source_savestate_id, 7002);
    EXPECT_FALSE(inactive_replay->source_dtm_artifact_id.has_value());
    EXPECT_FALSE(inactive_replay->source_itinerary_artifact_id.has_value());
    EXPECT_EQ(inactive_replay->source_binding_sha256,
              inactive_binding.canonical_sha256);
}

TEST_F(SqliteDbFixture, WorkflowCatalogContainsCompletionRecordingAndReplayButNoResultsPhase) {
    using namespace savor::db::execution::workflow;

    const auto registry = BuildDefaultWorkflowUnitRegistry();
    const auto* completion = registry.Find("battle_completion");
    ASSERT_NE(completion, nullptr);
    EXPECT_TRUE(completion->hidden);
    ASSERT_EQ(completion->required_inputs.size(), 1u);
    EXPECT_EQ(completion->required_inputs.front().key, "victory_turn_job");
    EXPECT_EQ(completion->required_inputs.front().data_kind,
              "analysis_battle.battle_turn_job");
    ASSERT_EQ(completion->internal_step_kinds.size(), 1u);
    EXPECT_EQ(completion->internal_step_kinds.front(), "battle.completion");

    const auto* recording = registry.Find("battle_recording");
    ASSERT_NE(recording, nullptr);
    EXPECT_TRUE(recording->hidden);
    ASSERT_EQ(recording->required_inputs.size(), 1u);
    EXPECT_EQ(recording->required_inputs.front().data_kind,
              "analysis_battle.battle_completion");
    ASSERT_EQ(recording->internal_step_kinds.size(), 1u);
    EXPECT_EQ(recording->internal_step_kinds.front(), "battle.record");

    const auto* replay = registry.Find("battle_replay");
    ASSERT_NE(replay, nullptr);
    EXPECT_TRUE(replay->hidden);
    ASSERT_EQ(replay->required_inputs.size(), 1u);
    EXPECT_EQ(replay->required_inputs.front().data_kind,
              "analysis_battle.battle_completion");
    ASSERT_EQ(replay->internal_step_kinds.size(), 1u);
    EXPECT_EQ(replay->internal_step_kinds.front(), "battle.replay");
    ASSERT_EQ(replay->possible_outputs.size(), 1u);
    EXPECT_EQ(replay->possible_outputs.front().data_kind,
              "analysis_battle.battle_replay");

    EXPECT_EQ(registry.Find("battle_results_screen"), nullptr);
    const auto units = registry.ListUnits();
    EXPECT_TRUE(std::ranges::none_of(
        units,
        [](const WorkflowUnitDefinition& unit) {
            return std::ranges::find(
                unit.internal_step_kinds,
                "battle.results_screen") != unit.internal_step_kinds.end();
        }));
}

TEST_F(SqliteDbFixture, BattlePredicateExecutionPackageAndSingleTurnResultRoundTripExactly) {
    using namespace savor::db;
    auto* analysis = db_service_->AnalysisDb();
    ASSERT_NE(analysis, nullptr);
    const auto now = types::UtcNow();
    std::string error;
    std::int64_t battle_set_id = 0;
    ASSERT_TRUE(analysis->CreateBattleSet({
        .name = "battle-v2-persistence-fixture",
        .entry_savestate_id = 101,
        .battle_plan_id = 201,
        .battle_plan_fingerprint = "battle-v2-plan",
        .continuation_mode = BattleContinuationMode::AutomaticBestPerEndingRng,
        .status = BattleSetStatus::Active,
        .created_at_utc = now,
        .correlation_id = "battle-v2",
        .causation_id = "test",
    }, &battle_set_id, &error)) << error;
    std::int64_t candidate_id = 0;
    ASSERT_TRUE(analysis->AddBattleSeedCandidate({
        .battle_set_id = battle_set_id,
        .seed_value = 0x12345678,
        .source_kind = BattleSeedCandidateSourceKind::Manual,
        .candidate_status = BattleSeedCandidateStatus::Ready,
        .created_at_utc = now,
        .correlation_id = "battle-v2",
        .causation_id = "test",
    }, &candidate_id, &error)) << error;
    std::int64_t wave_id = 0;
    ASSERT_TRUE(analysis->CreateBattleTurnWave({
        .battle_set_id = battle_set_id,
        .turn_index = 2,
        .seed_candidate_id = candidate_id,
        .status = BattleTurnWaveStatus::Ready,
        .created_at_utc = now,
        .correlation_id = "battle-v2",
        .causation_id = "test",
    }, &wave_id, &error)) << error;

    const auto package = savor::runtime::predicates::EmptyPredicateExecutionPackageV1();
    std::vector<std::uint8_t> package_blob;
    ASSERT_TRUE(savor::runtime::predicates::EncodePredicateExecutionPackageV1(
        package, package_blob, &error)) << error;
    const std::string phase_sha(64, 'd');
    std::int64_t binding_id = 0;
    ASSERT_TRUE(analysis->BindBattlePredicateExecutionPackage({
        .wave_id = wave_id,
        .predicate_group_revision_id = std::nullopt,
        .predicate_group_sha256 = package.group.content_sha256,
        .execution_package_sha256 = package.content_sha256,
        .execution_package_blob = package_blob,
        .phase_program_kind = static_cast<std::int32_t>(savor::PK_BattleSingleTurnRunner),
        .phase_program_version = 1,
        .phase_canonical_id = "savor.full_phase.battle_single_turn",
        .phase_revision = 1,
        .phase_sha256 = phase_sha,
        .hook_contract_canonical_id = package.hook_contract.canonical_id,
        .hook_contract_revision = package.hook_contract.revision,
        .hook_contract_sha256 = package.hook_contract.content_sha256,
        .created_at_utc = now,
    }, &binding_id, &error)) << error;
    const auto binding = analysis->GetBattlePredicateExecutionPackageForWave(wave_id);
    ASSERT_TRUE(binding.has_value());
    EXPECT_EQ(binding->predicate_execution_package_id, binding_id);
    EXPECT_EQ(binding->execution_package_sha256, package.content_sha256);
    EXPECT_EQ(binding->execution_package_blob, package_blob);

    std::int64_t turn_job_id = 0;
    constexpr std::int64_t exec_job_id = 70001;
    ASSERT_TRUE(analysis->RecordBattleTurnJob({
        .wave_id = wave_id,
        .exec_job_id = exec_job_id,
        .plan_id = 501,
        .fake_attacks_this_turn = 2,
        .fake_attacks_used_before = 3,
        .job_state = BattleTurnJobState::Succeeded,
        .has_results = true,
        .recorded_at_utc = now,
        .correlation_id = "battle-v2",
        .causation_id = "test",
    }, &turn_job_id, &error)) << error;
    const std::vector<std::uint8_t> evidence{1, 2, 3, 4};
    std::int64_t result_id = 0;
    ASSERT_TRUE(analysis->RecordBattleSingleTurnResult({
        .turn_job_id = turn_job_id,
        .exec_job_id = exec_job_id,
        .worker_terminal_sha256 = std::string(64, 'f'),
        .terminal_kind = "SUCCEEDED",
        .domain_outcome = "PredicateRejected",
        .ending_rng = 1234,
        .vi_start = 100,
        .vi_end = 120,
        .pred_passed = 5,
        .pred_total = 6,
        .cumulative_fake_attacks = 5,
        .predicate_group_revision_id = std::nullopt,
        .predicate_group_sha256 = package.group.content_sha256,
        .predicate_execution_package_sha256 = package.content_sha256,
        .predicate_evidence_blob = evidence,
        .recorded_at_utc = now,
    }, &result_id, &error)) << error;
    const auto result = analysis->GetBattleSingleTurnResultForExecJob(exec_job_id);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->battle_single_turn_result_id, result_id);
    EXPECT_EQ(result->domain_outcome, "PredicateRejected");
    EXPECT_EQ(result->pred_passed, 5u);
    EXPECT_EQ(result->pred_total, 6u);
    EXPECT_EQ(result->predicate_execution_package_sha256,
              package.content_sha256);
    EXPECT_EQ(result->predicate_evidence_blob, evidence);

    EXPECT_TRUE(analysis->RecordBattleSingleTurnResult({
        .turn_job_id = turn_job_id,
        .exec_job_id = exec_job_id,
        .worker_terminal_sha256 = std::string(64, 'f'),
        .terminal_kind = "SUCCEEDED",
        .recorded_at_utc = now,
    }, nullptr, &error)) << error;
    EXPECT_FALSE(analysis->ReplaceFailedBattleSingleTurnResult({
        .expected_worker_terminal_sha256 = std::string(64, 'f'),
        .replacement_worker_terminal_sha256 = std::string(64, 'e'),
        .result = {
            .turn_job_id = turn_job_id,
            .exec_job_id = exec_job_id,
            .worker_terminal_sha256 = std::string(64, 'e'),
            .terminal_kind = "SUCCEEDED",
            .recorded_at_utc = now,
        },
        .superseded_at_utc = now,
    }, nullptr, &error));

    const auto record_failed_job = [&](std::int64_t exec_job_id_value) {
        EXPECT_TRUE(analysis->RecordBattleTurnJob({
            .wave_id = wave_id,
            .exec_job_id = exec_job_id_value,
            .plan_id = 501,
            .fake_attacks_this_turn = 2,
            .fake_attacks_used_before = 3,
            .job_state = BattleTurnJobState::Failed,
            .has_results = false,
            .recorded_at_utc = now,
            .correlation_id = "battle-v2-failed",
            .causation_id = "test",
        }, nullptr, &error)) << error;
        const auto turn_job_sql =
            "SELECT turn_job_id FROM ab_turn_job WHERE exec_job_id=" +
            std::to_string(exec_job_id_value) + ";";
        return ReadInt64(db_, turn_job_sql.c_str());
    };
    const auto replace_failed = [&](std::int64_t failed_turn_job_id,
                                    std::int64_t failed_exec_job_id,
                                    char expected_terminal,
                                    char replacement_terminal,
                                    std::string replacement_kind = "FAILED") {
        ReplaceFailedBattleSingleTurnResultCommand replacement;
        replacement.expected_worker_terminal_sha256 = std::string(64, expected_terminal);
        replacement.replacement_worker_terminal_sha256 =
            std::string(64, replacement_terminal);
        replacement.result.turn_job_id = failed_turn_job_id;
        replacement.result.exec_job_id = failed_exec_job_id;
        replacement.result.worker_terminal_sha256 =
            std::string(64, replacement_terminal);
        replacement.result.terminal_kind = std::move(replacement_kind);
        replacement.result.error_code = "RETRY_FAILURE";
        replacement.result.error_text = "replacement terminal";
        return analysis->ReplaceFailedBattleSingleTurnResult(
            replacement, nullptr, &error);
    };

    constexpr std::int64_t failed_exec_job_id = 70002;
    const auto failed_turn_job_id = record_failed_job(failed_exec_job_id);
    ASSERT_GT(failed_turn_job_id, 0);
    std::int64_t failed_result_id = 0;
    RecordBattleSingleTurnResultCommand initial_failed_result;
    initial_failed_result.turn_job_id = failed_turn_job_id;
    initial_failed_result.exec_job_id = failed_exec_job_id;
    initial_failed_result.worker_terminal_sha256 = std::string(64, 'a');
    initial_failed_result.terminal_kind = "FAILED";
    initial_failed_result.error_code = "INITIAL_FAILURE";
    initial_failed_result.error_text = "initial terminal";
    ASSERT_GT(initial_failed_result.turn_job_id, 0);
    ASSERT_GT(initial_failed_result.exec_job_id, 0);
    ASSERT_EQ(initial_failed_result.worker_terminal_sha256.size(), 64u);
    ASSERT_FALSE(initial_failed_result.terminal_kind.empty());
    ASSERT_TRUE(analysis->RecordBattleSingleTurnResult(
        initial_failed_result, &failed_result_id, &error)) << error;
    ASSERT_TRUE(replace_failed(failed_turn_job_id, failed_exec_job_id, 'a', 'b')) << error;
    ASSERT_TRUE(replace_failed(failed_turn_job_id, failed_exec_job_id, 'b', 'c')) << error;
    const auto retried = analysis->GetBattleSingleTurnResultForExecJob(failed_exec_job_id);
    ASSERT_TRUE(retried.has_value());
    EXPECT_EQ(retried->battle_single_turn_result_id, failed_result_id);
    EXPECT_EQ(retried->worker_terminal_sha256, std::string(64, 'c'));
    EXPECT_EQ(ReadInt64(db_,
        "SELECT COUNT(1) FROM ab_historical_failed_battle_single_turn_results "
        "WHERE exec_job_id=70002;"), 2);
    EXPECT_EQ(ReadText(db_,
        "SELECT prior_worker_terminal_sha256 FROM "
        "ab_historical_failed_battle_single_turn_results WHERE exec_job_id=70002 "
        "ORDER BY historical_failed_battle_single_turn_result_id ASC LIMIT 1;"),
        std::string(64, 'a'));
    ASSERT_TRUE(replace_failed(
        failed_turn_job_id, failed_exec_job_id, 'c', 'd', "SUCCEEDED")) << error;
    EXPECT_FALSE(replace_failed(failed_turn_job_id, failed_exec_job_id, 'd', 'e'));

    constexpr std::int64_t handler_exec_job_id = 70004;
    const auto handler_turn_job_id = record_failed_job(handler_exec_job_id);
    ASSERT_GT(handler_turn_job_id, 0);
    auto descriptor = savor::db::execution::programdb::battle::
        BuildBattleSingleTurnProgramDescriptor(
            db_service_->ExecutionDb(),
            db_service_->StateDb(),
            analysis,
            db_service_->AuthoringDb(),
            {.working_dir_root = temp_root_});
    ASSERT_TRUE(descriptor.result_handler);
    const auto handler_failure_context = [&](char terminal_sha) {
        constexpr std::uint64_t dispatch_attempt_id = 970004;
        savor::runtime::DurableWorkerTerminalEnvelope envelope{
            .envelope_version = savor::runtime::
                kDurableWorkerTerminalEnvelopeVersion,
            .wrms_protocol_version = savor::wrms::ProtocolVersion,
            .worker_id = 1,
            .process_generation = 1,
            .terminal = {
                .outbound_sequence = 1,
                .workset_id = dispatch_attempt_id,
                .item_id = static_cast<std::uint64_t>(handler_exec_job_id),
                .item_ordinal = 0,
                .invocation_id = static_cast<std::uint64_t>(handler_exec_job_id),
                .attempt_id = 1,
                .terminal_id = 1,
                .terminal_order = 1,
                .status = savor::wrms::InvocationTerminalStatus::Failed,
                .session_disposition = savor::wrms::SessionDispositionCode::Clean,
                .workset_epoch = 1,
                .unstarted = false,
                .rejection_code = savor::wrms::RejectionCode::None,
                .error_code = "TEST_FAILED_TERMINAL",
                .message = "fixture failure",
            },
        };
        std::vector<std::uint8_t> encoded_envelope;
        std::string encode_error;
        if (!savor::runtime::EncodeDurableWorkerTerminalEnvelope(
                envelope, &encoded_envelope, &encode_error)) {
            throw std::logic_error(encode_error);
        }
        return savor::db::execution::programdb::ProgramResultProcessingContext{
            .job_id = handler_exec_job_id,
            .job_set_id = 1,
            .program_kind = static_cast<std::int32_t>(savor::PK_BattleSingleTurnRunner),
            .program_version = savor::runtime::battlesingleturn::ProgramVersion,
            .program_ref_kind = "analysis_battle.turn_job",
            .program_ref_id = handler_turn_job_id,
            .terminal = {
                .job_id = handler_exec_job_id,
                .workset_id = 1,
                .dispatch_attempt_id = static_cast<std::int64_t>(dispatch_attempt_id),
                .reserved_attempt_id = 1,
                .format = "savor.worker-terminal-envelope.v1",
                .sha256 = std::string(64, terminal_sha),
                .envelope = std::move(encoded_envelope),
            },
        };
    };
    const auto first_handler_decision = descriptor.result_handler->Process(
        handler_failure_context('h'));
    EXPECT_EQ(first_handler_decision.final_job_state, "FAILED");
    const auto retried_handler_decision = descriptor.result_handler->Process(
        handler_failure_context('i'));
    EXPECT_EQ(retried_handler_decision.final_job_state, "FAILED");
    const auto handler_result =
        analysis->GetBattleSingleTurnResultForExecJob(handler_exec_job_id);
    ASSERT_TRUE(handler_result.has_value());
    EXPECT_EQ(handler_result->worker_terminal_sha256, std::string(64, 'i'));
    EXPECT_EQ(ReadInt64(db_,
        "SELECT COUNT(1) FROM ab_historical_failed_battle_single_turn_results "
        "WHERE exec_job_id=70004;"), 1);

    constexpr std::int64_t rollback_exec_job_id = 70003;
    const auto rollback_turn_job_id = record_failed_job(rollback_exec_job_id);
    ASSERT_GT(rollback_turn_job_id, 0);
    RecordBattleSingleTurnResultCommand rollback_failed_result;
    rollback_failed_result.turn_job_id = rollback_turn_job_id;
    rollback_failed_result.exec_job_id = rollback_exec_job_id;
    rollback_failed_result.worker_terminal_sha256 = std::string(64, '1');
    rollback_failed_result.terminal_kind = "FAILED";
    ASSERT_TRUE(analysis->RecordBattleSingleTurnResult(
        rollback_failed_result, nullptr, &error)) << error;
    ASSERT_TRUE(ExecSql(db_, R"SQL(
CREATE TRIGGER fail_battle_single_turn_result_retry
BEFORE UPDATE ON ab_battle_single_turn_result_v1
WHEN OLD.exec_job_id=70003
BEGIN
    SELECT RAISE(ABORT, 'injected retry update failure');
END;
)SQL"));
    EXPECT_FALSE(replace_failed(
        rollback_turn_job_id, rollback_exec_job_id, '1', '2'));
    EXPECT_EQ(ReadInt64(db_,
        "SELECT COUNT(1) FROM ab_historical_failed_battle_single_turn_results "
        "WHERE exec_job_id=70003;"), 0);
    const auto rolled_back =
        analysis->GetBattleSingleTurnResultForExecJob(rollback_exec_job_id);
    ASSERT_TRUE(rolled_back.has_value());
    EXPECT_EQ(rolled_back->worker_terminal_sha256, std::string(64, '1'));
}

TEST_F(SqliteDbFixture, PredicateAuthoringPublishesDefinitionBindingAndAtomicMultiHookGroup) {
    using namespace savor::db;
    using namespace savor::runtime::predicates;
    using namespace savor::runtime::program;
    using namespace savor::runtime::program::composition;

    auto* authoring = db_service_->AuthoringDb();
    ASSERT_NE(authoring, nullptr);
    const auto now = types::UtcNow();
    std::string error;

    const auto u16 = TypeRef::Builtin(BuiltinType::U16);
    const auto boolean = TypeRef::Builtin(BuiltinType::Bool);
    PredicateDefinition definition{
        .canonical_id = "test.predicate.item_is_273",
        .revision = 1,
        .source_name = "sqlite-fixture",
        .witnesses = {{"item_id", u16}},
        .expression = {
            {.kind = PredicateExpressionKind::Witness,
             .witness_index = 0, .result_type = u16,
             .source_label = "item id"},
            {.kind = PredicateExpressionKind::Literal,
             .literal = LiteralValue{.type = u16,
                                     .payload = std::uint16_t{273}},
             .result_type = u16, .source_label = "Electri Box"},
            {.kind = PredicateExpressionKind::Equal,
             .operands = {0, 1}, .result_type = boolean,
             .source_label = "selected item"},
        },
        .root_expression = 2,
    };
    PredicateAuthoringRevisionReceipt definition_receipt;
    ASSERT_TRUE(authoring->CreatePredicateDefinitionDraft({
        .creation_request_key = "00000000000000000000000000000001",
        .name = "Item is Electri Box",
        .body = {definition.witnesses, definition.expression,
                 definition.root_expression},
        .created_at_utc = now,
    }, &definition_receipt, &error)) << error;
    const auto definition_revision_id = definition_receipt.revision_id;
    EXPECT_TRUE(definition_receipt.stable_key.starts_with(
        "predicate.definition/"));
    ASSERT_TRUE(authoring->PublishPredicateDefinitionRevisionV2(
        definition_revision_id, now, nullptr, &error)) << error;
    const auto published_definition =
        authoring->GetPredicateDefinitionRevisionV2(definition_revision_id);
    ASSERT_TRUE(published_definition.has_value());
    EXPECT_EQ(published_definition->revision_state, "PUBLISHED");
    EXPECT_EQ(published_definition->definition.expression, definition.expression);
    EXPECT_EQ(published_definition->content_sha256.size(), 64u);

    PredicateExecutionBindingV1 execution_binding{
        .canonical_id = "test.predicate_binding.item_273",
        .revision = 1,
        .definition = {
            definition_revision_id,
            published_definition->content_sha256,
            published_definition->definition,
        },
        .witnesses = {{
            .witness_ordinal = 0,
            .source_kind = PredicateWitnessSourceKindV1::ConcreteValue,
            .value_type = u16,
            .concrete_value = LiteralValue{
                .type = u16, .payload = std::uint16_t{273}},
        }},
    };
    PredicateAuthoringRevisionReceipt binding_receipt;
    ASSERT_TRUE(authoring->CreatePredicateExecutionBindingDraft({
        .creation_request_key = "00000000000000000000000000000002",
        .name = "Electri Box binding",
        .body = {definition_revision_id, execution_binding.witnesses},
        .created_at_utc = now,
    }, &binding_receipt, &error)) << error;
    const auto binding_revision_id = binding_receipt.revision_id;
    EXPECT_TRUE(binding_receipt.stable_key.starts_with("predicate.binding/"));
    ASSERT_EQ(binding_receipt.stable_key.size(), 50u);
    EXPECT_TRUE(std::ranges::all_of(
        std::string_view(binding_receipt.stable_key).substr(18),
        [](const char value) {
            return (value >= '0' && value <= '9')
                || (value >= 'a' && value <= 'f');
        }));
    PredicateAuthoringRevisionReceipt binding_replay;
    ASSERT_TRUE(authoring->CreatePredicateExecutionBindingDraft({
        .creation_request_key = "00000000000000000000000000000002",
        .name = "Electri Box binding",
        .body = {definition_revision_id, execution_binding.witnesses},
        .created_at_utc = now,
    }, &binding_replay, &error)) << error;
    EXPECT_EQ(binding_replay.revision_id, binding_revision_id);
    EXPECT_FALSE(binding_replay.identity_created);
    auto conflicting_binding_create = CreatePredicateExecutionBindingDraftCommand{
        .creation_request_key = "00000000000000000000000000000002",
        .name = "Different binding request",
        .body = {definition_revision_id, execution_binding.witnesses},
        .created_at_utc = now,
    };
    error.clear();
    EXPECT_FALSE(authoring->CreatePredicateExecutionBindingDraft(
        conflicting_binding_create, nullptr, &error));
    EXPECT_NE(error.find("reused with different content"), std::string::npos)
        << error;
    ASSERT_TRUE(authoring->PublishPredicateExecutionBindingRevision(
        binding_revision_id, now, nullptr, &error)) << error;
    PredicateAuthoringRevisionReceipt binding_republish;
    ASSERT_TRUE(authoring->PublishPredicateExecutionBindingRevision(
        binding_revision_id, now, &binding_republish, &error)) << error;
    EXPECT_EQ(binding_republish.revision_id, binding_revision_id);
    const auto published_binding =
        authoring->GetPredicateExecutionBindingRevision(binding_revision_id);
    ASSERT_TRUE(published_binding.has_value());
    EXPECT_EQ(published_binding->revision_state, "PUBLISHED");
    ASSERT_EQ(published_binding->binding.witnesses.size(), 1u);
    EXPECT_EQ(std::get<std::uint16_t>(
        published_binding->binding.witnesses.front().concrete_value->payload),
        273u);
    bool binding_metadata_changed = false;
    ASSERT_TRUE(authoring->UpdatePredicateAuthoringMetadata({
        .object_kind = PredicateAuthoringObjectKind::ExecutionBinding,
        .parent_id = binding_receipt.parent_id,
        .name = "Renamed Electri Box binding",
        .description = "presentation metadata",
        .updated_at_utc = now + std::chrono::milliseconds(1),
    }, &binding_metadata_changed, &error)) << error;
    EXPECT_TRUE(binding_metadata_changed);
    const auto renamed_binding =
        authoring->GetPredicateExecutionBindingRevision(binding_revision_id);
    ASSERT_TRUE(renamed_binding.has_value());
    EXPECT_EQ(renamed_binding->binding.execution_binding_revision_id,
              binding_revision_id);
    EXPECT_EQ(renamed_binding->semantic_sha256,
              published_binding->semantic_sha256);
    EXPECT_EQ(renamed_binding->binding.content_sha256,
              published_binding->binding.content_sha256);
    EXPECT_EQ(ReadInt64(db_,
        ("SELECT COUNT(1) FROM au_predicate_execution_binding_revision "
         "WHERE predicate_execution_binding_id="
         + std::to_string(binding_receipt.parent_id) + ";").c_str()), 1);

    PredicateAuthoringRevisionReceipt unchanged_binding;
    ASSERT_TRUE(authoring->SavePredicateExecutionBindingDraft({
        .predicate_execution_binding_id = binding_receipt.parent_id,
        .body = {definition_revision_id, execution_binding.witnesses},
        .saved_at_utc = now + std::chrono::milliseconds(2),
    }, &unchanged_binding, &error)) << error;
    EXPECT_EQ(unchanged_binding.revision_id, binding_revision_id);
    EXPECT_FALSE(unchanged_binding.semantic_changed);

    auto changed_binding_witnesses = execution_binding.witnesses;
    changed_binding_witnesses.front().concrete_value->payload =
        std::uint16_t{274};
    PredicateAuthoringRevisionReceipt changed_binding;
    ASSERT_TRUE(authoring->SavePredicateExecutionBindingDraft({
        .predicate_execution_binding_id = binding_receipt.parent_id,
        .body = {definition_revision_id, changed_binding_witnesses},
        .saved_at_utc = now + std::chrono::milliseconds(3),
    }, &changed_binding, &error)) << error;
    EXPECT_EQ(changed_binding.parent_id, binding_receipt.parent_id);
    EXPECT_EQ(changed_binding.stable_key, binding_receipt.stable_key);
    EXPECT_EQ(changed_binding.revision_number, 2);
    EXPECT_EQ(changed_binding.revision_state, "DRAFT");
    EXPECT_TRUE(changed_binding.semantic_changed);
    EXPECT_NE(changed_binding.semantic_sha256,
              binding_receipt.semantic_sha256);

    auto alternate_binding = execution_binding;
    alternate_binding.canonical_id = "test.predicate_binding.item_274";
    alternate_binding.witnesses.front().concrete_value->payload =
        std::uint16_t{274};
    PredicateAuthoringRevisionReceipt alternate_binding_receipt;
    ASSERT_TRUE(authoring->CreatePredicateExecutionBindingDraft({
        .creation_request_key = "00000000000000000000000000000003",
        .name = "Alternate item binding",
        .body = {definition_revision_id, alternate_binding.witnesses},
        .created_at_utc = now,
    }, &alternate_binding_receipt, &error)) << error;
    const auto alternate_binding_revision_id =
        alternate_binding_receipt.revision_id;
    ASSERT_TRUE(authoring->PublishPredicateExecutionBindingRevision(
        alternate_binding_revision_id, now, nullptr, &error)) << error;
    const auto published_alternate =
        authoring->GetPredicateExecutionBindingRevision(
            alternate_binding_revision_id);
    ASSERT_TRUE(published_alternate.has_value());
    EXPECT_NE(published_binding->binding.content_sha256,
              published_alternate->binding.content_sha256);
    EXPECT_EQ(published_binding->binding.definition.revision_id,
              published_alternate->binding.definition.revision_id);

    const auto hooks = BattlePredicateHookContractV1();
    std::vector<std::string> terminal_hooks;
    for (const auto& hook : hooks.points) {
        if (hook.canonical_id.ends_with(".EndTurn")
            || hook.canonical_id.ends_with(".EndBattleVictory"))
            terminal_hooks.push_back(hook.canonical_id);
    }
    std::ranges::sort(terminal_hooks);
    ASSERT_EQ(terminal_hooks.size(), 2u);
    ResolvedPredicateGroupV1 group{
        .canonical_id = "test.predicate_group.terminal_item",
        .revision = 1,
        .members = {{
            .ordinal = 0,
            .execution_binding_revision_id = binding_revision_id,
            .semantic_hook_ids = terminal_hooks,
            .occurrence = PredicateOccurrencePolicyV1::First,
            .reaction = PredicateReaction::AbortOnFail,
            .participates_in_aggregation = true,
            .emit_evidence = true,
        }},
    };
    PredicateAuthoringRevisionReceipt group_receipt;
    ASSERT_TRUE(authoring->CreatePredicateGroupDraft({
        .creation_request_key = "00000000000000000000000000000004",
        .name = "Terminal Electri Box predicate",
        .body = {group.members},
        .created_at_utc = now,
    }, &group_receipt, &error)) << error;
    const auto group_revision_id = group_receipt.revision_id;
    EXPECT_TRUE(group_receipt.stable_key.starts_with("predicate.group/"));
    ASSERT_EQ(group_receipt.stable_key.size(), 48u);
    EXPECT_TRUE(std::ranges::all_of(
        std::string_view(group_receipt.stable_key).substr(16),
        [](const char value) {
            return (value >= '0' && value <= '9')
                || (value >= 'a' && value <= 'f');
        }));
    PredicateAuthoringRevisionReceipt group_replay;
    ASSERT_TRUE(authoring->CreatePredicateGroupDraft({
        .creation_request_key = "00000000000000000000000000000004",
        .name = "Terminal Electri Box predicate",
        .body = {group.members},
        .created_at_utc = now,
    }, &group_replay, &error)) << error;
    EXPECT_EQ(group_replay.revision_id, group_revision_id);
    EXPECT_FALSE(group_replay.identity_created);
    error.clear();
    EXPECT_FALSE(authoring->CreatePredicateGroupDraft({
        .creation_request_key = "00000000000000000000000000000004",
        .name = "Different group request",
        .body = {group.members},
        .created_at_utc = now,
    }, nullptr, &error));
    EXPECT_NE(error.find("reused with different content"), std::string::npos)
        << error;
    ASSERT_TRUE(authoring->PublishPredicateGroupRevision(
        group_revision_id, now, nullptr, &error)) << error;
    PredicateAuthoringRevisionReceipt group_republish;
    ASSERT_TRUE(authoring->PublishPredicateGroupRevision(
        group_revision_id, now, &group_republish, &error)) << error;
    EXPECT_EQ(group_republish.revision_id, group_revision_id);
    const auto published_group = authoring->GetPredicateGroupRevision(group_revision_id);
    ASSERT_TRUE(published_group.has_value());
    EXPECT_EQ(published_group->revision_state, "PUBLISHED");
    ASSERT_EQ(published_group->group.members.size(), 1u);
    EXPECT_EQ(published_group->group.members.front().semantic_hook_ids,
              terminal_hooks);
    EXPECT_EQ(published_group->group.content_sha256.size(), 64u);
    bool group_metadata_changed = false;
    ASSERT_TRUE(authoring->UpdatePredicateAuthoringMetadata({
        .object_kind = PredicateAuthoringObjectKind::Group,
        .parent_id = group_receipt.parent_id,
        .name = "Renamed terminal predicate group",
        .description = "presentation metadata",
        .updated_at_utc = now + std::chrono::milliseconds(2),
    }, &group_metadata_changed, &error)) << error;
    EXPECT_TRUE(group_metadata_changed);
    const auto renamed_group =
        authoring->GetPredicateGroupRevision(group_revision_id);
    ASSERT_TRUE(renamed_group.has_value());
    EXPECT_EQ(renamed_group->semantic_sha256,
              published_group->semantic_sha256);
    EXPECT_EQ(renamed_group->group.content_sha256,
              published_group->group.content_sha256);
    EXPECT_EQ(ReadInt64(db_,
        ("SELECT COUNT(1) FROM au_predicate_group_revision "
         "WHERE predicate_group_id=" + std::to_string(group_receipt.parent_id)
         + ";").c_str()), 1);

    PredicateAuthoringRevisionReceipt unchanged_group;
    ASSERT_TRUE(authoring->SavePredicateGroupDraft({
        .predicate_group_id = group_receipt.parent_id,
        .body = {group.members},
        .saved_at_utc = now + std::chrono::milliseconds(4),
    }, &unchanged_group, &error)) << error;
    EXPECT_EQ(unchanged_group.revision_id, group_revision_id);
    EXPECT_FALSE(unchanged_group.semantic_changed);

    auto changed_group_members = group.members;
    changed_group_members.front().emit_evidence = false;
    PredicateAuthoringRevisionReceipt changed_group;
    ASSERT_TRUE(authoring->SavePredicateGroupDraft({
        .predicate_group_id = group_receipt.parent_id,
        .body = {changed_group_members},
        .saved_at_utc = now + std::chrono::milliseconds(5),
    }, &changed_group, &error)) << error;
    EXPECT_EQ(changed_group.parent_id, group_receipt.parent_id);
    EXPECT_EQ(changed_group.stable_key, group_receipt.stable_key);
    EXPECT_EQ(changed_group.revision_number, 2);
    EXPECT_EQ(changed_group.revision_state, "DRAFT");
    EXPECT_TRUE(changed_group.semantic_changed);
    EXPECT_NE(changed_group.semantic_sha256, group_receipt.semantic_sha256);

    EXPECT_EQ(SQLITE_CONSTRAINT, sqlite3_exec(db_,
        ("UPDATE au_predicate_group_member SET emit_evidence=0 "
         "WHERE predicate_group_revision_id="
         + std::to_string(group_revision_id)).c_str(),
        nullptr, nullptr, nullptr));

    const auto published_page = authoring->ListPredicateGroupRevisions({
        .revision_state = std::string("PUBLISHED"),
        .search_text = "Renamed terminal",
        .limit = 1,
    });
    ASSERT_EQ(published_page.items.size(), 1u);
    EXPECT_EQ(published_page.items.front().predicate_group_revision_id,
              group_revision_id);
    EXPECT_EQ(published_page.items.front().member_count, 1);
    EXPECT_EQ(published_page.items.front().hook_count, 2);
    EXPECT_FALSE(published_page.next_before_revision_id.has_value());

    const auto definition_page = authoring->ListPredicateDefinitionRevisionsV2({
        .revision_state = std::string("PUBLISHED"),
        .search_text = "Electri Box",
        .limit = 50,
    });
    ASSERT_EQ(definition_page.items.size(), 1u);
    EXPECT_EQ(
        definition_page.items.front().predicate_definition_revision_id,
        definition_revision_id);

    PredicateAuthoringRevisionReceipt duplicated_binding;
    ASSERT_TRUE(authoring->DuplicatePredicateExecutionBinding({
        .source_revision_id = binding_revision_id,
        .creation_request_key = "00000000000000000000000000000005",
        .name = "Duplicated binding",
        .description = "same semantics, separate identity",
        .created_at_utc = now,
    }, &duplicated_binding, &error)) << error;
    EXPECT_NE(duplicated_binding.parent_id, binding_receipt.parent_id);
    EXPECT_NE(duplicated_binding.stable_key, binding_receipt.stable_key);
    EXPECT_EQ(duplicated_binding.semantic_sha256,
              binding_receipt.semantic_sha256);
    PredicateAuthoringRevisionReceipt duplicate_binding_replay;
    ASSERT_TRUE(authoring->DuplicatePredicateExecutionBinding({
        .source_revision_id = binding_revision_id,
        .creation_request_key = "00000000000000000000000000000005",
        .name = "Duplicated binding",
        .description = "same semantics, separate identity",
        .created_at_utc = now,
    }, &duplicate_binding_replay, &error)) << error;
    EXPECT_EQ(duplicate_binding_replay.revision_id,
              duplicated_binding.revision_id);

    PredicateAuthoringRevisionReceipt duplicated_group;
    ASSERT_TRUE(authoring->DuplicatePredicateGroup({
        .source_revision_id = group_revision_id,
        .creation_request_key = "00000000000000000000000000000006",
        .name = "Duplicated predicate group",
        .description = "same semantics, separate identity",
        .created_at_utc = now,
    }, &duplicated_group, &error)) << error;
    EXPECT_NE(duplicated_group.parent_id, group_receipt.parent_id);
    EXPECT_NE(duplicated_group.stable_key, group_receipt.stable_key);
    EXPECT_EQ(duplicated_group.semantic_sha256, group_receipt.semantic_sha256);
    PredicateAuthoringRevisionReceipt duplicate_group_replay;
    ASSERT_TRUE(authoring->DuplicatePredicateGroup({
        .source_revision_id = group_revision_id,
        .creation_request_key = "00000000000000000000000000000006",
        .name = "Duplicated predicate group",
        .description = "same semantics, separate identity",
        .created_at_utc = now,
    }, &duplicate_group_replay, &error)) << error;
    EXPECT_EQ(duplicate_group_replay.revision_id,
              duplicated_group.revision_id);
}

TEST_F(SqliteDbFixture, PredicateAuthoringCreatesListsReloadsAndPublishesFirstBattleTurnOrderRule) {
    using namespace savor::db;
    using namespace savor::runtime::predicates;
    using namespace savor::runtime::program;
    auto* authoring = db_service_->AuthoringDb();
    ASSERT_NE(authoring, nullptr);
    const auto catalog = BattlePredicateAuthoringCatalogV2();
    const PredicateGuidedNodeV1 rule{
        .kind = PredicateGuidedNodeKindV1::Less,
        .children = {
            {.kind = PredicateGuidedNodeKindV1::CatalogValue,
             .key = "battle.turn_order.player_max_position"},
            {.kind = PredicateGuidedNodeKindV1::CatalogValue,
             .key = "battle.turn_order.enemy_min_position"},
        },
    };
    const auto compiled = CompileGuidedPredicateDefinitionV1(rule, catalog);
    ASSERT_TRUE(compiled);
    std::string error;
    PredicateAuthoringRevisionReceipt created;
    ASSERT_TRUE(authoring->CreatePredicateDefinitionDraft({
        .creation_request_key = "f1000000000000000000000000000001",
        .name = "PCs before enemies",
        .description = "All player characters act before enemies",
        .body = {compiled.definition.witnesses, compiled.definition.expression,
                 compiled.definition.root_expression},
        .created_at_utc = types::UtcNow(),
    }, &created, &error)) << error;
    const auto drafts = authoring->ListPredicateDefinitionRevisionsV2({
        .revision_state = std::string("DRAFT"), .search_text = "PCs before"});
    ASSERT_EQ(drafts.items.size(), 1u);
    EXPECT_EQ(drafts.items.front().predicate_definition_revision_id, created.revision_id);
    const auto loaded = authoring->GetPredicateDefinitionRevisionV2(created.revision_id);
    ASSERT_TRUE(loaded);
    EXPECT_EQ(loaded->definition.expression.back().kind,
              savor::runtime::program::composition::PredicateExpressionKind::Less);
    ASSERT_TRUE(authoring->PublishPredicateDefinitionRevisionV2(
        created.revision_id, types::UtcNow(), nullptr, &error)) << error;
    const auto published = authoring->GetPredicateDefinitionRevisionV2(created.revision_id);
    ASSERT_TRUE(published);
    EXPECT_EQ(published->revision_state, "PUBLISHED");
    EXPECT_EQ(published->definition.witnesses.size(), 1u);
    EXPECT_EQ(published->definition.witnesses.front().name, "turn_order");

    PredicateAuthoringRevisionReceipt binding;
    ASSERT_TRUE(authoring->CreatePredicateExecutionBindingDraft({
        .creation_request_key = "f1000000000000000000000000000011",
        .name = "Automatic turn-order data",
        .body = {created.revision_id, {}},
        .created_at_utc = types::UtcNow(),
    }, &binding, &error)) << error;
    const auto loaded_binding =
        authoring->GetPredicateExecutionBindingRevision(binding.revision_id);
    ASSERT_TRUE(loaded_binding);
    ASSERT_EQ(loaded_binding->binding.witnesses.size(), 1u);
    EXPECT_EQ(loaded_binding->binding.witnesses.front().source_kind,
              PredicateWitnessSourceKindV1::DerivedStateQuery);
    EXPECT_EQ(loaded_binding->binding.witnesses.front().source,
              capabilities::BattleDerivedTurnOrderActionIdentity());
    ASSERT_TRUE(authoring->PublishPredicateExecutionBindingRevision(
        binding.revision_id, types::UtcNow(), nullptr, &error)) << error;

    const auto hooks = BattlePredicateHookContractV1();
    std::vector<std::string> terminal_hooks;
    for (const auto& hook : hooks.points) {
        if (hook.canonical_id.ends_with(".EndTurn") ||
            hook.canonical_id.ends_with(".EndBattleVictory"))
            terminal_hooks.push_back(hook.canonical_id);
    }
    std::ranges::sort(terminal_hooks);
    PredicateAuthoringRevisionReceipt group;
    ASSERT_TRUE(authoring->CreatePredicateGroupDraft({
        .creation_request_key = "f1000000000000000000000000000012",
        .name = "Evaluate turn order at terminal",
        .body = {{PredicateGroupMemberV1{
            .ordinal = 0,
            .execution_binding_revision_id = binding.revision_id,
            .semantic_hook_ids = terminal_hooks,
            .occurrence = PredicateOccurrencePolicyV1::First,
        }}},
        .created_at_utc = types::UtcNow(),
    }, &group, &error)) << error;
    ASSERT_TRUE(authoring->PublishPredicateGroupRevision(
        group.revision_id, types::UtcNow(), nullptr, &error)) << error;
}

TEST_F(SqliteDbFixture, PredicateAuthoringSchemaFailureReportsItsExactStage) {
    using namespace savor::db;
    using namespace savor::runtime::program;
    using namespace savor::runtime::program::composition;

    sqlite3* raw = nullptr;
    ASSERT_EQ(sqlite3_open(":memory:", &raw), SQLITE_OK);
    const std::unique_ptr<sqlite3, decltype(&sqlite3_close)> missing_schema(
        raw, &sqlite3_close);
    SqliteAuthoringDb authoring(missing_schema.get());
    PredicateDefinitionDraftBody body{
        .expression = {{
            .kind = PredicateExpressionKind::Literal,
            .literal = LiteralValue{
                .type = TypeRef::Builtin(BuiltinType::Bool),
                .payload = true,
            },
            .result_type = TypeRef::Builtin(BuiltinType::Bool),
            .source_label = "true",
        }},
        .root_expression = 0,
    };
    std::string error;
    EXPECT_FALSE(authoring.CreatePredicateDefinitionDraft({
        .creation_request_key = "f1000000000000000000000000000002",
        .name = "Schema diagnostic",
        .body = body,
        .created_at_utc = types::UtcNow(),
    }, nullptr, &error));
    EXPECT_NE(error.find("predicate authoring request lookup preparation failed"),
              std::string::npos) << error;
    EXPECT_EQ(error.find("not an error"), std::string::npos) << error;
}

TEST_F(SqliteDbFixture, PredicateDefinitionIdentityMetadataAndAuthoringRequestsAreIdempotent) {
    using namespace savor::db;
    using namespace savor::runtime::program;
    using namespace savor::runtime::program::composition;

    auto* authoring = db_service_->AuthoringDb();
    ASSERT_NE(authoring, nullptr);
    const auto now = types::UtcNow();
    std::string error;

    PredicateDefinitionDraftBody body{
        .expression = {{
            .kind = PredicateExpressionKind::Literal,
            .literal = LiteralValue{
                .type = TypeRef::Builtin(BuiltinType::Bool),
                .payload = true,
            },
            .result_type = TypeRef::Builtin(BuiltinType::Bool),
            .source_label = "true",
        }},
        .root_expression = 0,
    };
    const CreatePredicateDefinitionDraftCommand create{
        .creation_request_key = "10000000000000000000000000000001",
        .name = "Idempotent definition",
        .description = "initial presentation",
        .body = body,
        .created_at_utc = now,
    };

    PredicateAuthoringRevisionReceipt created;
    ASSERT_TRUE(authoring->CreatePredicateDefinitionDraft(
        create, &created, &error)) << error;
    EXPECT_TRUE(created.identity_created);
    EXPECT_TRUE(created.semantic_changed);
    EXPECT_FALSE(created.metadata_changed);
    EXPECT_TRUE(created.stable_key.starts_with("predicate.definition/"));
    ASSERT_EQ(created.stable_key.size(), 53u);
    EXPECT_TRUE(std::ranges::all_of(
        std::string_view(created.stable_key).substr(21), [](const char value) {
            return (value >= '0' && value <= '9')
                || (value >= 'a' && value <= 'f');
        }));

    PredicateAuthoringRevisionReceipt replayed;
    ASSERT_TRUE(authoring->CreatePredicateDefinitionDraft(
        create, &replayed, &error)) << error;
    EXPECT_EQ(replayed.parent_id, created.parent_id);
    EXPECT_EQ(replayed.revision_id, created.revision_id);
    EXPECT_EQ(replayed.stable_key, created.stable_key);
    EXPECT_FALSE(replayed.identity_created);
    EXPECT_FALSE(replayed.semantic_changed);
    EXPECT_EQ(ReadInt64(db_, "SELECT COUNT(1) FROM au_predicate_definition_v2;"), 1);
    EXPECT_EQ(ReadInt64(db_, "SELECT COUNT(1) FROM au_predicate_authoring_request;"), 1);

    auto conflicting_create = create;
    conflicting_create.description = "different payload";
    error.clear();
    EXPECT_FALSE(authoring->CreatePredicateDefinitionDraft(
        conflicting_create, nullptr, &error));
    EXPECT_NE(error.find("reused with different content"), std::string::npos)
        << error;

    bool metadata_changed = true;
    ASSERT_TRUE(authoring->UpdatePredicateAuthoringMetadata({
        .object_kind = PredicateAuthoringObjectKind::Definition,
        .parent_id = created.parent_id,
        .name = create.name,
        .description = create.description,
        .updated_at_utc = now + std::chrono::milliseconds(1),
    }, &metadata_changed, &error)) << error;
    EXPECT_FALSE(metadata_changed);
    EXPECT_EQ(ReadInt64(db_,
        "SELECT updated_at_utc FROM au_predicate_definition_v2;"),
        now.time_since_epoch().count());

    ASSERT_TRUE(authoring->UpdatePredicateAuthoringMetadata({
        .object_kind = PredicateAuthoringObjectKind::Definition,
        .parent_id = created.parent_id,
        .name = "Renamed definition",
        .description = "new presentation only",
        .updated_at_utc = now + std::chrono::milliseconds(2),
    }, &metadata_changed, &error)) << error;
    EXPECT_TRUE(metadata_changed);
    const auto renamed = authoring->GetPredicateDefinitionRevisionV2(
        created.revision_id);
    ASSERT_TRUE(renamed.has_value());
    EXPECT_EQ(renamed->stable_key, created.stable_key);
    EXPECT_EQ(renamed->semantic_sha256, created.semantic_sha256);
    EXPECT_EQ(renamed->name, "Renamed definition");
    EXPECT_EQ(ReadInt64(db_,
        "SELECT COUNT(1) FROM au_predicate_definition_revision_v2;"), 1);

    PredicateAuthoringRevisionReceipt published;
    ASSERT_TRUE(authoring->PublishPredicateDefinitionRevisionV2(
        created.revision_id, now, &published, &error)) << error;
    PredicateAuthoringRevisionReceipt republished;
    ASSERT_TRUE(authoring->PublishPredicateDefinitionRevisionV2(
        created.revision_id, now + std::chrono::milliseconds(3),
        &republished, &error)) << error;
    EXPECT_EQ(republished.revision_id, published.revision_id);
    EXPECT_EQ(republished.semantic_sha256, published.semantic_sha256);

    const auto published_before_metadata =
        authoring->GetPredicateDefinitionRevisionV2(created.revision_id);
    ASSERT_TRUE(published_before_metadata.has_value());
    ASSERT_TRUE(authoring->UpdatePredicateAuthoringMetadata({
        .object_kind = PredicateAuthoringObjectKind::Definition,
        .parent_id = created.parent_id,
        .name = "Published renamed definition",
        .description = "published presentation only",
        .updated_at_utc = now + std::chrono::milliseconds(4),
    }, &metadata_changed, &error)) << error;
    EXPECT_TRUE(metadata_changed);
    const auto published_after_metadata =
        authoring->GetPredicateDefinitionRevisionV2(created.revision_id);
    ASSERT_TRUE(published_after_metadata.has_value());
    EXPECT_EQ(published_after_metadata->content_sha256,
              published_before_metadata->content_sha256);
    EXPECT_EQ(published_after_metadata->semantic_sha256,
              published_before_metadata->semantic_sha256);
    EXPECT_EQ(ReadInt64(db_,
        "SELECT COUNT(1) FROM au_predicate_definition_revision_v2;"), 1);

    PredicateAuthoringRevisionReceipt unchanged;
    ASSERT_TRUE(authoring->SavePredicateDefinitionDraft({
        .predicate_definition_id = created.parent_id,
        .body = body,
        .saved_at_utc = now + std::chrono::milliseconds(5),
    }, &unchanged, &error)) << error;
    EXPECT_EQ(unchanged.revision_id, created.revision_id);
    EXPECT_FALSE(unchanged.semantic_changed);

    auto changed_body = body;
    changed_body.expression.front().literal->payload = false;
    PredicateAuthoringRevisionReceipt draft;
    ASSERT_TRUE(authoring->SavePredicateDefinitionDraft({
        .predicate_definition_id = created.parent_id,
        .body = changed_body,
        .saved_at_utc = now + std::chrono::milliseconds(6),
    }, &draft, &error)) << error;
    EXPECT_EQ(draft.parent_id, created.parent_id);
    EXPECT_EQ(draft.stable_key, created.stable_key);
    EXPECT_EQ(draft.revision_number, 2);
    EXPECT_EQ(draft.revision_state, "DRAFT");
    EXPECT_TRUE(draft.semantic_changed);
    EXPECT_NE(draft.semantic_sha256, created.semantic_sha256);
    const auto draft_node_query =
        "SELECT rowid FROM au_predicate_expression_node_v2 WHERE "
        "predicate_definition_revision_id=" + std::to_string(draft.revision_id)
        + " AND node_ordinal=0;";
    const auto draft_node_rowid = ReadInt64(db_, draft_node_query.c_str());

    PredicateAuthoringRevisionReceipt unchanged_draft;
    ASSERT_TRUE(authoring->SavePredicateDefinitionDraft({
        .predicate_definition_id = created.parent_id,
        .body = changed_body,
        .saved_at_utc = now + std::chrono::milliseconds(7),
    }, &unchanged_draft, &error)) << error;
    EXPECT_EQ(unchanged_draft.revision_id, draft.revision_id);
    EXPECT_FALSE(unchanged_draft.semantic_changed);
    EXPECT_EQ(ReadInt64(db_, draft_node_query.c_str()), draft_node_rowid);

    const DuplicatePredicateDefinitionCommand duplicate{
        .source_revision_id = created.revision_id,
        .creation_request_key = "10000000000000000000000000000002",
        .name = "Duplicated definition",
        .description = "separate logical identity",
        .created_at_utc = now,
    };
    PredicateAuthoringRevisionReceipt duplicated;
    ASSERT_TRUE(authoring->DuplicatePredicateDefinition(
        duplicate, &duplicated, &error)) << error;
    EXPECT_NE(duplicated.parent_id, created.parent_id);
    EXPECT_NE(duplicated.stable_key, created.stable_key);
    EXPECT_EQ(duplicated.semantic_sha256, created.semantic_sha256);
    EXPECT_EQ(duplicated.revision_number, 1);

    PredicateAuthoringRevisionReceipt duplicate_replay;
    ASSERT_TRUE(authoring->DuplicatePredicateDefinition(
        duplicate, &duplicate_replay, &error)) << error;
    EXPECT_EQ(duplicate_replay.parent_id, duplicated.parent_id);
    EXPECT_EQ(duplicate_replay.revision_id, duplicated.revision_id);
    EXPECT_FALSE(duplicate_replay.identity_created);

    auto conflicting_duplicate = duplicate;
    conflicting_duplicate.name = "Different duplicate request";
    error.clear();
    EXPECT_FALSE(authoring->DuplicatePredicateDefinition(
        conflicting_duplicate, nullptr, &error));
    EXPECT_NE(error.find("reused with different content"), std::string::npos)
        << error;
    EXPECT_EQ(ReadInt64(db_, "SELECT COUNT(1) FROM au_predicate_definition_v2;"), 2);
}

TEST_F(SqliteDbFixture, PredicateGroupRejectsUnavailableBattleHookAtPublication) {
    using namespace savor::db;
    using namespace savor::runtime::predicates;
    using namespace savor::runtime::program;
    using namespace savor::runtime::program::composition;

    auto* authoring = db_service_->AuthoringDb();
    ASSERT_NE(authoring, nullptr);
    const auto now = types::UtcNow();
    std::string error;
    PredicateDefinition definition{
        .canonical_id = "test.predicate.unavailable_hook",
        .revision = 1,
        .source_name = "sqlite-fixture",
        .expression = {{
            .kind = PredicateExpressionKind::Literal,
            .literal = LiteralValue{
                .type = TypeRef::Builtin(BuiltinType::Bool),
                .payload = true,
            },
            .result_type = TypeRef::Builtin(BuiltinType::Bool),
            .source_label = "true",
        }},
        .root_expression = 0,
    };
    PredicateAuthoringRevisionReceipt definition_receipt;
    ASSERT_TRUE(authoring->CreatePredicateDefinitionDraft({
        .creation_request_key = "00000000000000000000000000000011",
        .name = "Unavailable hook predicate",
        .body = {definition.witnesses, definition.expression,
                 definition.root_expression},
        .created_at_utc = now,
    }, &definition_receipt, &error)) << error;
    const auto definition_revision_id = definition_receipt.revision_id;
    ASSERT_TRUE(authoring->PublishPredicateDefinitionRevisionV2(
        definition_revision_id, now, nullptr, &error)) << error;

    const auto published_definition =
        authoring->GetPredicateDefinitionRevisionV2(definition_revision_id);
    ASSERT_TRUE(published_definition.has_value());
    PredicateExecutionBindingV1 binding{
        .canonical_id = "test.predicate_binding.unavailable_hook",
        .revision = 1,
        .definition = {definition_revision_id,
                       published_definition->content_sha256,
                       published_definition->definition},
    };
    PredicateAuthoringRevisionReceipt binding_receipt;
    ASSERT_TRUE(authoring->CreatePredicateExecutionBindingDraft({
        .creation_request_key = "00000000000000000000000000000012",
        .name = "Unavailable hook binding",
        .body = {definition_revision_id, binding.witnesses},
        .created_at_utc = now,
    }, &binding_receipt, &error)) << error;
    const auto binding_revision_id = binding_receipt.revision_id;
    ASSERT_TRUE(authoring->PublishPredicateExecutionBindingRevision(
        binding_revision_id, now, nullptr, &error)) << error;
    ResolvedPredicateGroupV1 group{
        .canonical_id = "test.predicate_group.unavailable_hook",
        .revision = 1,
        .members = {{
            .ordinal = 0,
            .execution_binding_revision_id = binding_revision_id,
            .semantic_hook_ids = {"soa.battle.point.StartAction"},
            .occurrence = PredicateOccurrencePolicyV1::First,
        }},
    };
    PredicateAuthoringRevisionReceipt group_receipt;
    ASSERT_TRUE(authoring->CreatePredicateGroupDraft({
        .creation_request_key = "00000000000000000000000000000013",
        .name = "Unavailable hook group",
        .body = {group.members},
        .created_at_utc = now,
    }, &group_receipt, &error)) << error;
    const auto group_revision_id = group_receipt.revision_id;
    EXPECT_FALSE(authoring->PublishPredicateGroupRevision(
        group_revision_id, now, nullptr, &error));
    EXPECT_NE(error.find("predicate.hook_unavailable"), std::string::npos)
        << error;
    const auto rejected = authoring->GetPredicateGroupRevision(group_revision_id);
    ASSERT_TRUE(rejected.has_value());
    EXPECT_EQ(rejected->revision_state, "DRAFT");
}

TEST_F(SqliteDbFixture, BattleStartAtomicallyJoinsContextAndConfirmedSeedProbeFrames) {
    using namespace savor::db;

    auto* analysis = db_service_->AnalysisDb();
    ASSERT_NE(analysis, nullptr);
    const auto now = types::UtcNow();
    constexpr std::int64_t entry_savestate_id = 81001;
    constexpr std::int64_t workflow_instance_id = 81002;
    constexpr std::int64_t workflow_step_id = 81003;
    std::string error;
    const auto confirmed = CreateConfirmedSeedProbeFixture(
        analysis, "battle-start-join", entry_savestate_id, 81004,
        81005, 0x12345678u, now, &error);
    ASSERT_TRUE(confirmed.has_value()) << error;
    ASSERT_TRUE(analysis->ReplaceSeedProbeAcceptedInputFrames({
        .probe_run_id = confirmed->probe_run_id,
        .input_frame_ids = {confirmed->input_frame_id},
        .replaced_at_utc = now,
        .correlation_id = "battle-start-join",
        .causation_id = "test",
    }, &error)) << error;
    bool changed = false;
    ASSERT_TRUE(analysis->UpdateSeedProbeRunStatus({
        .probe_run_id = confirmed->probe_run_id,
        .expected_status = SeedProbeRunStatus::Survey,
        .new_status = SeedProbeRunStatus::Confirm,
        .changed_at_utc = now,
        .correlation_id = "battle-start-join",
        .causation_id = "test",
    }, &changed, &error)) << error;
    ASSERT_TRUE(changed);
    changed = false;
    ASSERT_TRUE(analysis->UpdateSeedProbeRunStatus({
        .probe_run_id = confirmed->probe_run_id,
        .expected_status = SeedProbeRunStatus::Confirm,
        .new_status = SeedProbeRunStatus::Completed,
        .completed_at_utc = now,
        .changed_at_utc = now,
        .correlation_id = "battle-start-join",
        .causation_id = "test",
    }, &changed, &error)) << error;
    ASSERT_TRUE(changed);

    std::int64_t context_probe_id = 0;
    ASSERT_TRUE(analysis->CreateBattleContextProbe({
        .source_savestate_id = entry_savestate_id,
        .materialization_key = "battle-start-context",
        .workflow_instance_id = workflow_instance_id,
        .workflow_step_id = 81006,
        .probe_status = BattleContextProbeStatus::Queued,
        .created_at_utc = now,
        .correlation_id = "battle-start-join",
        .causation_id = "test",
    }, &context_probe_id, &error)) << error;
    ASSERT_TRUE(analysis->SetBattleContextProbeExecJobId(
        context_probe_id, 81007, &error)) << error;
    ASSERT_TRUE(analysis->CompleteBattleContextProbe({
        .exec_job_id = 81007,
        .probe_status = BattleContextProbeStatus::Succeeded,
        .context_version = 1,
        .recorded_at_utc = now,
    }, &error)) << error;

    EnsureBattleStartCommand command{
        .workflow_instance_id = workflow_instance_id,
        .workflow_step_id = workflow_step_id,
        .probe_run_id = confirmed->probe_run_id,
        .context_probe_id = context_probe_id,
        .battle_set_name = "battle.start.step.81003",
        .entry_savestate_id = entry_savestate_id,
        .battle_plan_id = 81008,
        .battle_plan_fingerprint = "battle-start-plan",
        .continuation_mode = BattleContinuationMode::AutomaticBestPerEndingRng,
        .launch_fake_attack_min = 0,
        .launch_fake_attack_max = 2,
        .created_at_utc = now,
        .correlation_id = "battle-start-join",
        .causation_id = "test",
    };
    EnsureBattleStartReceipt created{};
    ASSERT_TRUE(analysis->EnsureBattleStart(command, &created, &error)) << error;
    EXPECT_TRUE(created.created);
    ASSERT_GT(created.battle_set_id, 0);
    ASSERT_EQ(created.first_wave_ids.size(), 1u);
    const auto wave = analysis->GetBattleTurnWave(created.first_wave_ids.front());
    ASSERT_TRUE(wave.has_value());
    EXPECT_EQ(wave->turn_index, 1);
    EXPECT_EQ(wave->context_probe_id, context_probe_id);
    const auto frozen_set = analysis->GetBattleSet(created.battle_set_id);
    ASSERT_TRUE(frozen_set.has_value());
    EXPECT_EQ(frozen_set->battle_plan_id, 81008);
    EXPECT_EQ(frozen_set->battle_plan_fingerprint, "battle-start-plan");
    EXPECT_EQ(
        frozen_set->continuation_mode,
        BattleContinuationMode::AutomaticBestPerEndingRng);
    EXPECT_EQ(frozen_set->launch_fake_attack_min, 0);
    EXPECT_EQ(frozen_set->launch_fake_attack_max, 2);

    EnsureBattleStartReceipt replayed{};
    ASSERT_TRUE(analysis->EnsureBattleStart(command, &replayed, &error)) << error;
    EXPECT_FALSE(replayed.created);
    EXPECT_EQ(replayed.battle_start_id, created.battle_start_id);
    EXPECT_EQ(replayed.battle_set_id, created.battle_set_id);
    EXPECT_EQ(replayed.first_wave_ids, created.first_wave_ids);
    auto drifted = command;
    drifted.battle_plan_fingerprint = "drifted-plan";
    EXPECT_FALSE(analysis->EnsureBattleStart(drifted, nullptr, &error));
    EXPECT_EQ(ReadInt64(db_, "SELECT COUNT(*) FROM ab_battle_set WHERE name='battle.start.step.81003';"), 1);
    EXPECT_EQ(ReadInt64(db_, "SELECT COUNT(*) FROM ab_seed_candidate WHERE battle_set_id=(SELECT battle_set_id FROM ab_battle_start WHERE workflow_step_id=81003);"), 1);
    EXPECT_EQ(ReadInt64(db_, "SELECT COUNT(*) FROM ab_turn_wave WHERE battle_set_id=(SELECT battle_set_id FROM ab_battle_start WHERE workflow_step_id=81003);"), 1);
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
            .min_value = 0,
            .max_value = 255,
            .created_at_utc = t1,
            .correlation_id = "repeat-authoring-analysis",
            .causation_id = "test",
        },
        &spec_a,
        &error)) << error;
    const auto seed_probe_zero_sql =
        "SELECT COUNT(1) FROM au_seed_probe_spec WHERE seed_probe_spec_id="
        + std::to_string(spec_a) + " AND run_ms=0 AND vi_stall_ms=0;";
    EXPECT_EQ(ReadInt64(db_, seed_probe_zero_sql.c_str()), 1);
    const auto seed_probe_legacy_update =
        "UPDATE au_seed_probe_spec SET run_ms=40000,vi_stall_ms=3000"
        " WHERE seed_probe_spec_id=" + std::to_string(spec_a) + ";";
    ASSERT_TRUE(ExecSql(db_, seed_probe_legacy_update.c_str()));

    std::int64_t spec_b = 0;
    ASSERT_TRUE(authoring_db->SaveSeedProbeSpec(
        {
            .name = "repeat-authoring-b",
            .priority = 1,
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
                        { .input_key = "entry_savestate", .data_kind = "state.savestate_id", .ref_kind = "state.savestate", .display_name = "Entry savestate" },
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
                        { .input_key = "entry_savestate", .data_kind = "state.savestate_id", .ref_kind = "state.savestate", .display_name = "Entry savestate" },
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
        const std::string artifact_bytes(
            static_cast<std::size_t>(99 + i), static_cast<char>('A' + i));
        const auto artifact_path = temp_root_
            / ("all-scenario-style-" + std::to_string(i) + ".sav");
        {
            std::ofstream out(artifact_path, std::ios::binary);
            out.write(
                artifact_bytes.data(),
                static_cast<std::streamsize>(artifact_bytes.size()));
        }
        std::int64_t artifact_id = 0;
        ASSERT_TRUE(state_db->ImportExternalArtifact(
            {
                .absolute_source_path = artifact_path,
                .artifact = {
                    .sha256 = hash::sha256(
                        artifact_bytes.data(), artifact_bytes.size()),
                    .size_bytes = static_cast<std::int64_t>(artifact_bytes.size()),
                    .compression_kind = 0,
                    .display_filename = artifact_path.filename().string(),
                    .file_ext = ".sav",
                    .artifact_kind = "SAV",
                    .created_at_utc = now,
                    .correlation_id = "all-scenario-style",
                    .causation_id = "test",
                },
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
    EXPECT_TRUE(TableExists(db_, "exec_work_availability"));
    EXPECT_TRUE(ColumnExists(
        db_, "exec_work_availability", "has_ready_worksets"));
    EXPECT_TRUE(ColumnExists(
        db_, "exec_work_availability", "has_execution_finished_results"));
    EXPECT_FALSE(ColumnExists(
        db_, "exec_work_availability", "has_requested_cancellations"));
    EXPECT_FALSE(ColumnExists(db_, "exec_job", "result_processor_token"));
    EXPECT_FALSE(ColumnExists(
        db_, "exec_job", "result_processing_lease_expires_at_utc"));
    EXPECT_FALSE(ColumnExists(
        db_, "exec_job", "result_processing_retry_after_utc"));
    EXPECT_FALSE(ColumnExists(
        db_, "exec_job_cancellation_request", "delivery_token"));
    EXPECT_FALSE(ColumnExists(
        db_, "exec_job_cancellation_request", "delivery_lease_expires_at_utc"));
    EXPECT_EQ(
        TableCreateSql(db_, "exec_job_cancellation_request").find(
            "DELIVERY_IN_PROGRESS"),
        std::string::npos);
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
    EXPECT_TRUE(execution_db.WorkflowCommandService()->CompleteWorkflowStep(
        { .workflow_step_id = 2001, .completion_state = "COMPLETED", .requested_by = "test" },
        &command_error))
        << command_error;
    // Idempotent duplicate terminal callback.
    EXPECT_TRUE(execution_db.WorkflowCommandService()->CompleteWorkflowStep(
        { .workflow_step_id = 2001, .completion_state = "COMPLETED", .requested_by = "test" },
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
INSERT INTO ui_workflow_step(
    workflow_step_id, workflow_instance_id, step_key, step_kind, state,
    job_set_id, job_count, priority, attempts, max_attempts, created_at_utc)
VALUES
  (7101, 7001, 'retryable', 'test', 'FAILED', 7201, 4, 1, 1, 2, 2000),
  (7102, 7002, 'complete', 'test', 'COMPLETED', 7202, 1, 1, 1, 1, 1000);
INSERT INTO ui_job_summary(job_id, job_set_id, program_kind, state, priority, queued_at_utc)
VALUES
  (7301, 7201, 1, 'FAILED', 1, 2000),
  (7302, 7201, 1, 'INTERRUPTED', 1, 2000),
  (7303, 7201, 1, 'CANCELED', 1, 2000),
  (7304, 7201, 1, 'SUPERSEDED', 1, 2000),
  (7305, 7202, 1, 'SUCCEEDED', 1, 1000);
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

    UiWorkflowInstanceListQuery focused_query{};
    focused_query.workflow_instance_id = 7001;
    focused_query.limit = 10;
    const auto focused = ui_read_db.ListWorkflowInstances(focused_query);
    ASSERT_EQ(focused.items.size(), 1u);
    EXPECT_EQ(focused.items.front().workflow_instance_id, 7001);
    EXPECT_EQ(focused.items.front().retryable_job_count, 2);
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
    EXPECT_GT(report.non_settled_step_in_completed_instance_count, 0);
    EXPECT_GT(report.non_settled_step_in_terminal_instance_count, 0);
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
    EXPECT_EQ(report.non_settled_step_in_terminal_instance_count, 0);
    EXPECT_EQ(report.completed_step_missing_completion_ts_count, 0);
}

TEST_F(SqliteDbFixture, Stage3cTerminalAdvancementServiceMarksNextStepReadyFromTerminalJob) {
    using namespace savor::db::execution::workflow;

    const savor::db::migrations::MigrationSourceOptions embedded_options{ .source_kind = savor::db::migrations::MigrationSourceKind::Embedded };
    std::string err;
    ASSERT_TRUE(savor::db::migrations::ApplyContextMigrations(db_, savor::db::migrations::MigrationContext::Execution, embedded_options, &err)) << err;

    ASSERT_TRUE(ExecSql(db_, R"SQL(
INSERT INTO exec_workflow_instance(workflow_instance_id, workflow_kind, state, root_scope_kind, created_by, created_at_utc)
VALUES(2, 'SEED_PROBE_CHAIN', 'RUNNING', 'manual', 'test', unixepoch()*1000);
INSERT INTO exec_job_set(
    job_set_id, program_kind, purpose, created_at_utc, expected_total,
    materialization_state)
VALUES(20, 1, 'workflow', unixepoch()*1000, 1, 'WORKSET_PUBLICATION_COMPLETE');
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
    descriptor.default_progress_library_ids = std::vector<std::string>{};
    descriptor.default_derived_state_block_ids = std::vector<std::string>{};
    descriptor.workflow_transition = std::make_shared<AlwaysAdvanceTransitionHandler>();
    ASSERT_TRUE(registry.RegisterForStepKind("seedprobe.neutral", descriptor));
    StepSettlementGateService gate;
    SqliteWorkflowOrchestrationQueryService query_service(db_);
    SqliteWorkflowOrchestrationCommandService command_service(db_);
    WorkflowSettlementAdvancementService advancement(
        &registry,
        &gate,
        &query_service,
        &command_service);

    const auto terminal_ready = query_service.ListSettlementReadyStepSnapshots(10);
    ASSERT_EQ(terminal_ready.size(), 1u);
    EXPECT_EQ(terminal_ready.front().workflow_step_id, 200);
    EXPECT_EQ(terminal_ready.front().expected_total, 1);
    EXPECT_EQ(terminal_ready.front().discovered_total, 1);
    EXPECT_EQ(terminal_ready.front().settled_total, 1);

    WorkflowSettlementAdvancementResult result{};
    ASSERT_TRUE(advancement.AdvanceForSettledJob(2000, &result, &err)) << err;
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

TEST_F(SqliteDbFixture, WorksetTerminalCommitWakesTargetedAdvancementBeforeRecoveryScan) {
    using namespace savor::db::execution::programdb;
    using namespace savor::db::execution::workflow;
    using namespace savor::db::migrations;

    const MigrationSourceOptions embedded_options{
        .source_kind = MigrationSourceKind::Embedded,
    };
    std::string err;
    ASSERT_TRUE(ApplyContextMigrations(
        db_,
        MigrationContext::Execution,
        embedded_options,
        &err)) << err;
    ASSERT_TRUE(ExecSql(db_, R"SQL(
INSERT INTO exec_workflow_instance(
    workflow_instance_id, workflow_kind, state, root_scope_kind,
    created_by, created_at_utc)
VALUES(20200, 'WORKSET_TARGETED', 'RUNNING', 'manual', 'test', 1);
INSERT INTO exec_job_set(
    job_set_id, program_kind, purpose, created_at_utc, expected_total,
    materialization_state)
VALUES(
    20201, 1, 'targeted-terminal', 1, 1,
    'WORKSET_PUBLICATION_COMPLETE');
INSERT INTO exec_workflow_step(
    workflow_step_id, workflow_instance_id, step_key, step_kind, state,
    job_set_id, priority, attempts, max_attempts, created_at_utc)
VALUES(
    20202, 20200, 'source', 'workset.targeted', 'MATERIALIZED',
    20201, 4, 0, 1, 1);
INSERT INTO exec_workflow_step(
    workflow_step_id, workflow_instance_id, step_key, step_kind, state,
    priority, attempts, max_attempts, created_at_utc)
VALUES(
    20203, 20200, 'next', 'workset.next', 'WAITING',
    0, 0, 1, 1);
INSERT INTO exec_job(
    job_id, job_set_id, program_kind, program_version, program_ref_kind,
    program_ref_id, fingerprint, priority, state, attempts, max_attempts,
    queued_at_utc)
VALUES(
    20204, 20201, 1, 1, 'workset_test', 1, 'targeted-job', 4,
    'RUNNING', 0, 1, 1);
)SQL"));

    class TerminalOnlyMaterializer final : public IProgramJobMaterializer {
    public:
        bool MaterializeJobs(
            const ProgramJobMaterializationContext&,
            WorkflowStepScheduleResult*,
            std::string* error_out) const override {
            if (error_out != nullptr) {
                *error_out =
                    "terminal-only fixture does not materialize jobs";
            }
            return false;
        }
    };

    ProgramKindRegistry registry;
    ProgramKindDescriptor descriptor{};
    descriptor.program_kind = 1;
    descriptor.program_name = "workset.targeted";
    descriptor.default_progress_library_ids = std::vector<std::string>{};
    descriptor.default_derived_state_block_ids = std::vector<std::string>{};
    descriptor.job_materializer =
        std::make_shared<TerminalOnlyMaterializer>();
    descriptor.workflow_transition =
        std::make_shared<AlwaysAdvanceTransitionHandler>();
    ASSERT_TRUE(registry.RegisterForStepKind(
        "workset.targeted",
        descriptor));

    SqliteExecutionDb execution_db(db_);
    WorkflowCoordinatorService coordinator(
        &execution_db,
        &registry,
        WorkflowCoordinatorConfig{
            .poll_interval = std::chrono::seconds(5),
            .settlement_repair_interval = std::chrono::seconds(5),
        });
    ASSERT_TRUE(coordinator.Start(&err)) << err;

    const auto initial_scan_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (coordinator.SnapshotTelemetry().settlement_scan_count == 0
        && std::chrono::steady_clock::now() < initial_scan_deadline) {
        std::this_thread::yield();
    }
    ASSERT_GE(coordinator.SnapshotTelemetry().settlement_scan_count, 1);
    ASSERT_TRUE(ExecSql(
        db_,
        "UPDATE exec_job SET state='SUCCEEDED', ended_at_utc=2 "
        "WHERE job_id=20204;"));
    ASSERT_TRUE(coordinator.PublishSettlementCommit(
        {
            .commit_sequence = 7,
            .workflow_step_id = 20202,
            .job_id = 20204,
        }));

    const auto targeted_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (coordinator.SnapshotTelemetry()
               .targeted_settlement_advancement_count
               == 0
        && std::chrono::steady_clock::now() < targeted_deadline) {
        std::this_thread::yield();
    }
    coordinator.Stop();

    const auto telemetry = coordinator.SnapshotTelemetry();
    EXPECT_EQ(telemetry.targeted_settlement_notification_count, 1);
    EXPECT_EQ(telemetry.targeted_settlement_advancement_count, 1);
    EXPECT_EQ(
        ReadText(
            db_,
            "SELECT state FROM exec_workflow_step "
            "WHERE workflow_step_id=20202;"),
        "COMPLETED");
    EXPECT_EQ(
        ReadText(
            db_,
            "SELECT state FROM exec_workflow_step "
            "WHERE workflow_step_id=20203;"),
        "READY");
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
            decision.spawn_steps.push_back({
                .parent_workflow_step_id = 89,
                .step_key = "spawned-sibling-next",
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
    descriptor.default_progress_library_ids = std::vector<std::string>{};
    descriptor.default_derived_state_block_ids = std::vector<std::string>{};
    descriptor.workflow_transition = std::make_shared<SpawnStepTransitionHandler>();
    ASSERT_TRUE(registry.RegisterForStepKind("mock.spawn", descriptor));
    StepSettlementGateService gate;
    RecordingWorkflowCommandService command_service;
    WorkflowSettlementAdvancementService advancement(
        &registry,
        &gate,
        nullptr,
        &command_service);

    WorkflowStepSettlementSnapshot snapshot{};
    snapshot.workflow_instance_id = 77;
    snapshot.workflow_step_id = 88;
    snapshot.job_set_id = 99;
    snapshot.expected_total = 1;
    snapshot.discovered_total = 1;
    snapshot.settled_total = 1;
    snapshot.failed_total = 0;
    snapshot.priority = 20;
    snapshot.workflow_kind = "mock";
    snapshot.step_key = "source";
    snapshot.step_kind = "mock.spawn";

    std::string err;
    WorkflowSettlementAdvancementResult result{};
    ASSERT_TRUE(advancement.AdvanceSnapshot(snapshot, &result, &err)) << err;
    EXPECT_TRUE(result.advanced_next_step);
    EXPECT_EQ(result.spawned_step_count, 2);
    ASSERT_EQ(command_service.dynamic_step_calls.size(), 2u);
    ASSERT_EQ(command_service.dynamic_step_calls[0].steps.size(), 1u);
    EXPECT_EQ(command_service.dynamic_step_calls[0].steps[0].step_key, "spawned-next");
    EXPECT_EQ(command_service.dynamic_step_calls[0].steps[0].priority, 34);
    EXPECT_EQ(command_service.dynamic_step_calls[0].parent_workflow_step_id, 88);
    ASSERT_EQ(command_service.dynamic_step_calls[1].steps.size(), 1u);
    EXPECT_EQ(command_service.dynamic_step_calls[1].steps[0].step_key, "spawned-sibling-next");
    EXPECT_EQ(command_service.dynamic_step_calls[1].parent_workflow_step_id, 89);
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
INSERT INTO exec_job_set(
    job_set_id, program_kind, purpose, created_at_utc, expected_total,
    materialization_state)
VALUES(220, 1, 'workflow', unixepoch()*1000, 1, 'WORKSET_PUBLICATION_COMPLETE');
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
    descriptor.default_progress_library_ids = std::vector<std::string>{};
    descriptor.default_derived_state_block_ids = std::vector<std::string>{};
    descriptor.workflow_transition = std::make_shared<OutputAdvanceTransitionHandler>();
    ASSERT_TRUE(registry.RegisterForStepKind("mock.source", descriptor));
    StepSettlementGateService gate;
    WorkflowSettlementAdvancementService advancement(
        &registry,
        &gate,
        &query_service,
        &command_service);

    WorkflowSettlementAdvancementResult result{};
    ASSERT_TRUE(advancement.AdvanceForSettledJob(22000, &result, &err)) << err;
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

TEST_F(SqliteDbFixture, Stage5WorkflowStepRecordsDistinctNamedOutputs) {
    using namespace savor::db::execution::workflow;

    const savor::db::migrations::MigrationSourceOptions embedded_options{
        .source_kind = savor::db::migrations::MigrationSourceKind::Embedded,
    };
    std::string err;
    ASSERT_TRUE(savor::db::migrations::ApplyContextMigrations(
        db_,
        savor::db::migrations::MigrationContext::Execution,
        embedded_options,
        &err)) << err;
    ASSERT_TRUE(ExecSql(db_, R"SQL(
INSERT INTO exec_workflow_instance(
    workflow_instance_id, workflow_kind, state, root_scope_kind,
    created_by, created_at_utc)
VALUES(23, 'workflow_graph', 'RUNNING', 'manual', 'test', unixepoch()*1000);
INSERT INTO exec_workflow_step(
    workflow_step_id, workflow_instance_id, step_key, step_kind, state,
    priority, attempts, max_attempts, created_at_utc)
VALUES(2300, 23, 'source', 'mock.source', 'READY', 0, 0, 1, unixepoch()*1000);
)SQL"));

    SqliteWorkflowOrchestrationQueryService query_service(db_);
    SqliteWorkflowOrchestrationCommandService command_service(db_);
    ASSERT_TRUE(command_service.RecordStepOutput(
        {
            .workflow_step_id = 2300,
            .output_key = "attempt",
            .output_data_kind = "analysis.attempt_id",
            .output_ref_kind = "analysis.attempt",
            .output_ref_id = 2222,
            .requested_by = "test",
        },
        &err)) << err;
    ASSERT_TRUE(command_service.RecordStepOutput(
        {
            .workflow_step_id = 2300,
            .output_key = "checkpoint",
            .output_data_kind = "state.savestate_id",
            .output_ref_kind = "state.savestate",
            .output_ref_id = 3333,
            .requested_by = "test",
        },
        &err)) << err;

    const auto outputs = query_service.ListStepOutputs(23);
    ASSERT_EQ(outputs.size(), 2u);
    EXPECT_EQ(outputs[0].output_key, "attempt");
    EXPECT_EQ(outputs[0].ref_kind, "analysis.attempt");
    EXPECT_EQ(outputs[0].ref_id, 2222);
    EXPECT_EQ(outputs[1].output_key, "checkpoint");
    EXPECT_EQ(outputs[1].ref_kind, "state.savestate");
    EXPECT_EQ(outputs[1].ref_id, 3333);

    sqlite3_stmt* st = nullptr;
    ASSERT_EQ(SQLITE_OK, sqlite3_prepare_v2(
        db_,
        "SELECT output_ref_kind, output_ref_id "
        "FROM exec_workflow_step WHERE workflow_step_id=2300;",
        -1,
        &st,
        nullptr));
    ASSERT_EQ(SQLITE_ROW, sqlite3_step(st));
    EXPECT_STREQ(
        reinterpret_cast<const char*>(sqlite3_column_text(st, 0)),
        "analysis.attempt");
    EXPECT_EQ(sqlite3_column_int64(st, 1), 2222);
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
INSERT INTO exec_job_set(
    job_set_id, program_kind, purpose, created_at_utc, expected_total,
    materialization_state)
VALUES(30, 1, 'workflow', unixepoch()*1000, 1, 'WORKSET_PUBLICATION_COMPLETE');
INSERT INTO exec_workflow_step(workflow_step_id, workflow_instance_id, step_key, step_kind, state, job_set_id, priority, attempts, max_attempts, created_at_utc)
VALUES(300, 3, 'Unique', 'seedprobe.unique', 'MATERIALIZED', 30, 0, 0, 1, unixepoch()*1000);
INSERT INTO exec_job(job_id, job_set_id, program_kind, program_version, program_ref_kind, program_ref_id, fingerprint, priority, state, attempts, max_attempts, queued_at_utc)
VALUES(3000, 30, 1, 1, 'seedprobe_spec', 44, 'fp-3', 0, 'SUCCEEDED', 0, 1, unixepoch()*1000);
)SQL"));

    savor::db::execution::programdb::ProgramKindRegistry registry;
    savor::db::execution::programdb::ProgramKindDescriptor descriptor{};
    descriptor.program_kind = 1;
    descriptor.program_name = "seedprobe.unique";
    descriptor.default_progress_library_ids = std::vector<std::string>{};
    descriptor.default_derived_state_block_ids = std::vector<std::string>{};
    descriptor.workflow_transition = std::make_shared<FinalStepTransitionHandler>();
    ASSERT_TRUE(registry.RegisterForStepKind("seedprobe.unique", descriptor));
    StepSettlementGateService gate;
    SqliteWorkflowOrchestrationQueryService query_service(db_);
    SqliteWorkflowOrchestrationCommandService command_service(db_);
    WorkflowSettlementAdvancementService advancement(
        &registry,
        &gate,
        &query_service,
        &command_service);

    WorkflowSettlementAdvancementResult result{};
    ASSERT_TRUE(advancement.AdvanceForSettledJob(3000, &result, &err)) << err;
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

TEST_F(SqliteDbFixture, Stage3cSettlementAdvancementParksMixedSuccessAndFailureBeforeTransition) {
    using namespace savor::db::execution::workflow;

    const savor::db::migrations::MigrationSourceOptions embedded_options{ .source_kind = savor::db::migrations::MigrationSourceKind::Embedded };
    std::string err;
    ASSERT_TRUE(savor::db::migrations::ApplyContextMigrations(db_, savor::db::migrations::MigrationContext::Execution, embedded_options, &err)) << err;

    ASSERT_TRUE(ExecSql(db_, R"SQL(
INSERT INTO exec_workflow_instance(workflow_instance_id, workflow_kind, state, root_scope_kind, created_by, created_at_utc)
VALUES(4, 'SEED_PROBE_CHAIN', 'RUNNING', 'manual', 'test', unixepoch()*1000);
INSERT INTO exec_job_set(
    job_set_id, program_kind, purpose, created_at_utc, expected_total,
    materialization_state)
VALUES(40, 1, 'workflow', unixepoch()*1000, 2, 'WORKSET_PUBLICATION_COMPLETE');
INSERT INTO exec_workflow_step(workflow_step_id, workflow_instance_id, step_key, step_kind, state, job_set_id, priority, attempts, max_attempts, created_at_utc)
VALUES(400, 4, 'Neutral', 'seedprobe.neutral', 'MATERIALIZED', 40, 0, 0, 1, unixepoch()*1000);
INSERT INTO exec_workflow_step(workflow_step_id, workflow_instance_id, step_key, step_kind, state, priority, attempts, max_attempts, created_at_utc)
VALUES(401, 4, 'next', 'seedprobe.next', 'WAITING', 0, 0, 1, unixepoch()*1000);
INSERT INTO exec_job(job_id, job_set_id, program_kind, program_version, program_ref_kind, program_ref_id, fingerprint, priority, state, attempts, max_attempts, queued_at_utc)
VALUES
(3999, 40, 1, 1, 'seedprobe_spec', 43, 'fp-3', 0, 'SUCCEEDED', 0, 1, unixepoch()*1000),
(4000, 40, 1, 1, 'seedprobe_spec', 44, 'fp-4', 0, 'FAILED', 0, 1, unixepoch()*1000);
)SQL"));

    savor::db::execution::programdb::ProgramKindRegistry registry;
    savor::db::execution::programdb::ProgramKindDescriptor descriptor{};
    descriptor.program_kind = 1;
    descriptor.program_name = "seedprobe.neutral";
    descriptor.default_progress_library_ids = std::vector<std::string>{};
    descriptor.default_derived_state_block_ids = std::vector<std::string>{};
    descriptor.workflow_transition = std::make_shared<AlwaysAdvanceTransitionHandler>();
    ASSERT_TRUE(registry.RegisterForStepKind("seedprobe.neutral", descriptor));
    StepSettlementGateService gate;
    SqliteWorkflowOrchestrationQueryService query_service(db_);
    SqliteWorkflowOrchestrationCommandService command_service(db_);
    WorkflowSettlementAdvancementService advancement(
        &registry,
        &gate,
        &query_service,
        &command_service);

    WorkflowSettlementAdvancementResult result{};
    ASSERT_TRUE(advancement.AdvanceForSettledJob(4000, &result, &err)) << err;
    EXPECT_TRUE(result.snapshot_found);
    EXPECT_TRUE(result.gate_can_transition);
    EXPECT_TRUE(result.step_marked_terminal);
    EXPECT_FALSE(result.transition_evaluated);
    EXPECT_FALSE(result.advanced_next_step);
    EXPECT_FALSE(result.workflow_completed);
    EXPECT_TRUE(result.workflow_failed);

    sqlite3_stmt* st = nullptr;
    ASSERT_EQ(SQLITE_OK, sqlite3_prepare_v2(
        db_,
        "SELECT i.state, i.completed_at_utc, source.state, source.blocked_reason, next.state "
        "FROM exec_workflow_instance i "
        "JOIN exec_workflow_step source ON source.workflow_instance_id=i.workflow_instance_id AND source.workflow_step_id=400 "
        "JOIN exec_workflow_step next ON next.workflow_instance_id=i.workflow_instance_id AND next.step_key='next' "
        "WHERE i.workflow_instance_id=4;",
        -1,
        &st,
        nullptr));
    ASSERT_EQ(SQLITE_ROW, sqlite3_step(st));
    EXPECT_STREQ(reinterpret_cast<const char*>(sqlite3_column_text(st, 0)), "FAILED");
    EXPECT_EQ(sqlite3_column_type(st, 1), SQLITE_NULL);
    EXPECT_STREQ(reinterpret_cast<const char*>(sqlite3_column_text(st, 2)), "FAILED");
    EXPECT_EQ(sqlite3_column_type(st, 3), SQLITE_NULL);
    EXPECT_STREQ(reinterpret_cast<const char*>(sqlite3_column_text(st, 4)), "WAITING");
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
    ASSERT_TRUE(execution_db.WorkflowCommandService()->CompleteWorkflowStep(
        { .workflow_step_id = 9401, .completion_state = "COMPLETED", .requested_by = "projector-replay-test" },
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
    5001,'evt-workflow-relay-missing-payload','Execution.WorkflowStepReady.v1',1,'Execution','workflow_instance','999',unixepoch()*1000,'workflow_event',0
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
    EXPECT_NE(first_error.find("payload_ref_id"), std::string::npos);
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
    EXPECT_EQ(dead_letter_error.find("dead-letter: "), 0u);
    EXPECT_NE(dead_letter_error.find("payload_ref_id"), std::string::npos);
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

TEST_F(SqliteDbFixture, Stage3dExecutionJobCommandServiceEmitsLifecycleEvents) {
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
    ASSERT_TRUE(job_commands->AppendLifecycleEvent({ .kind = JobLifecycleEventKind::JobCompleted, .job_id = 601, .terminal_state = std::string("SUCCEEDED") }, &err)) << err;
    ASSERT_TRUE(job_commands->AppendLifecycleEvent({ .kind = JobLifecycleEventKind::JobEventArchived, .job_id = 601, .message = std::string("archived") }, &err)) << err;
    ASSERT_TRUE(job_commands->AppendLifecycleEvent({ .kind = JobLifecycleEventKind::JobRestored, .job_id = 601, .message = std::string("restored") }, &err)) << err;

    sqlite3_stmt* st = nullptr;
    ASSERT_EQ(SQLITE_OK, sqlite3_prepare_v2(db_, "SELECT COUNT(1) FROM exec_outbox_message WHERE event_type LIKE 'Execution.Job%.v1';", -1, &st, nullptr));
    ASSERT_EQ(SQLITE_ROW, sqlite3_step(st));
    EXPECT_EQ(sqlite3_column_int(st, 0), 8);
    sqlite3_finalize(st);

    ASSERT_EQ(SQLITE_OK, sqlite3_prepare_v2(db_, "SELECT COUNT(1) FROM exec_outbox_message WHERE payload_ref_kind='job' AND payload_ref_id=601;", -1, &st, nullptr));
    ASSERT_EQ(SQLITE_ROW, sqlite3_step(st));
    EXPECT_EQ(sqlite3_column_int(st, 0), 7);
    sqlite3_finalize(st);

    ASSERT_EQ(SQLITE_OK, sqlite3_prepare_v2(db_, "SELECT COUNT(1) FROM exec_job_event WHERE job_id=601;", -1, &st, nullptr));
    ASSERT_EQ(SQLITE_ROW, sqlite3_step(st));
    EXPECT_EQ(sqlite3_column_int(st, 0), 7);
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
    ASSERT_TRUE(execution_db->RequeueJob(requeue_job_id, &err)) << err;

    const auto restarted = execution_db->GetExecutionJob(restart_job_id);
    const auto requeued = execution_db->GetExecutionJob(requeue_job_id);
    ASSERT_TRUE(restarted.has_value());
    ASSERT_TRUE(requeued.has_value());
    EXPECT_EQ(restarted->state, "QUEUED");
    EXPECT_EQ(restarted->attempts, 0);
    EXPECT_EQ(restarted->input_ini, "[Job]\nmode=new\n");
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
    EXPECT_EQ(sqlite3_column_int(st, 0), 4);
    sqlite3_finalize(st);

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

    const auto record = execution_db.GetExecutionJob(job_id);
    ASSERT_TRUE(record.has_value());
    EXPECT_EQ(record->program_kind, 7);
    EXPECT_EQ(record->state, "QUEUED");

    std::string claim_error;
    const auto claimed = execution_db.ClaimNextReadyExecutionJob("worker-default-queued", 30000, &claim_error);
    EXPECT_TRUE(claim_error.empty()) << claim_error;
    ASSERT_TRUE(claimed.has_value());
    EXPECT_EQ(claimed->job_id, job_id);
    EXPECT_EQ(claimed->workflow_step_id, 1830);
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

TEST_F(SqliteDbFixture, WorksetBatchClaimsAreOrderedAtomicAndUseExactAuthorityTokens) {
    using namespace savor::db;
    using namespace savor::db::execution::workflow;
    using namespace savor::db::migrations;

    const MigrationSourceOptions embedded_options{
        .source_kind = MigrationSourceKind::Embedded,
    };
    std::string err;
    ASSERT_TRUE(ApplyContextMigrations(
        db_,
        MigrationContext::Execution,
        embedded_options,
        &err)) << err;
    ASSERT_TRUE(ExecSql(db_, R"SQL(
INSERT INTO exec_workflow_instance(
    workflow_instance_id, workflow_kind, state, root_scope_kind,
    created_by, created_at_utc)
VALUES(19100, 'WORKSET_TEST', 'RUNNING', 'manual', 'test', 1);
INSERT INTO exec_job_set(
    job_set_id, program_kind, purpose, created_at_utc)
VALUES(19101, 7, 'workset-batch-claim', 1);
INSERT INTO exec_workflow_step(
    workflow_step_id, workflow_instance_id, step_key, step_kind, state,
    job_set_id, priority, attempts, max_attempts, created_at_utc)
VALUES(
    19102, 19100, 'Batch', 'seedprobe.grid', 'MATERIALIZED',
    19101, 10, 0, 2, 1);
INSERT INTO exec_job(
    job_id, job_set_id, program_kind, program_version, program_ref_kind,
    program_ref_id, fingerprint, priority, state, attempts, max_attempts,
    queued_at_utc)
VALUES
    (19103, 19101, 7, 1, 'seed_probe', 1, 'batch-low', 5,
     'QUEUED', 0, 3, 300),
    (19104, 19101, 7, 1, 'seed_probe', 1, 'batch-first', 10,
     'QUEUED', 0, 3, 100),
    (19105, 19101, 7, 1, 'seed_probe', 1, 'batch-second', 10,
     'QUEUED', 0, 3, 200),
    (19106, 19101, 7, 1, 'seed_probe', 1, 'attempts-exhausted', 20,
     'QUEUED', 1, 1, 50);
)SQL"));

    SqliteExecutionDb execution_db(db_);
    const auto claimed = execution_db.ClaimBatchReadyExecutionJobs(
        "coordinator-run-77",
        4,
        30000,
        &err);
    ASSERT_TRUE(err.empty()) << err;
    ASSERT_EQ(claimed.size(), 3u);
    EXPECT_EQ(claimed[0].job_id, 19104);
    EXPECT_EQ(claimed[1].job_id, 19105);
    EXPECT_EQ(claimed[2].job_id, 19103);
    EXPECT_EQ(claimed[0].claimed_by_token, "coordinator-run-77:19104");
    EXPECT_EQ(claimed[1].claimed_by_token, "coordinator-run-77:19105");
    EXPECT_EQ(claimed[2].claimed_by_token, "coordinator-run-77:19103");
    EXPECT_EQ(claimed[0].durable_attempt_id, 1u);
    EXPECT_EQ(claimed[1].durable_attempt_id, 1u);
    EXPECT_EQ(claimed[2].durable_attempt_id, 1u);
    EXPECT_NE(claimed[0].claimed_by_token, claimed[1].claimed_by_token);
    EXPECT_EQ(ReadInt64(
        db_,
        "SELECT COUNT(1) FROM exec_job WHERE state='CLAIMED';"),
        3);
    EXPECT_EQ(ReadInt64(
        db_,
        "SELECT SUM(attempts) FROM exec_job WHERE job_set_id=19101;"),
        1);
    EXPECT_EQ(ReadInt64(
        db_,
        "SELECT COUNT(1) FROM exec_job "
        "WHERE job_id=19106 AND state='QUEUED';"),
        1);

    const auto renewal = execution_db.RenewExecutionJobLeases(
        {
            {
                .job_id = 19104,
                .claimed_by_token = claimed[0].claimed_by_token,
            },
            {
                .job_id = 19105,
                .claimed_by_token = "wrong-token",
            },
            {
                .job_id = 999999,
                .claimed_by_token = "missing-token",
            },
        },
        30000,
        &err);
    ASSERT_TRUE(err.empty()) << err;
    ASSERT_EQ(renewal.size(), 3u);
    EXPECT_EQ(
        renewal[0].disposition,
        ExecutionJobLeaseRenewalDisposition::Renewed);
    EXPECT_EQ(
        renewal[1].disposition,
        ExecutionJobLeaseRenewalDisposition::TokenMismatch);
    EXPECT_EQ(
        renewal[2].disposition,
        ExecutionJobLeaseRenewalDisposition::Missing);

    ExecutionJobStartAuthoritySetReceipt authority{};
    ASSERT_TRUE(execution_db.ValidateExecutionJobStartAuthoritySet(
        {
            {
                .job_id = 19104,
                .claimed_by_token = claimed[0].claimed_by_token,
            },
            {
                .job_id = 19105,
                .claimed_by_token = claimed[1].claimed_by_token,
            },
            {
                .job_id = 19103,
                .claimed_by_token = claimed[2].claimed_by_token,
            },
        },
        &authority,
        &err)) << err;
    ASSERT_TRUE(authority.all_valid);
    ASSERT_EQ(authority.items.size(), 3u);
    for (const auto& item : authority.items) {
        EXPECT_EQ(
            item.disposition,
            ExecutionJobStartAuthorityDisposition::Valid);
    }
    EXPECT_EQ(ReadInt64(
        db_,
        "SELECT COUNT(1) FROM exec_job WHERE state='CLAIMED';"),
        3);
    EXPECT_EQ(ReadInt64(
        db_,
        "SELECT COUNT(1) FROM exec_job_event "
        "WHERE event_kind='Execution.JobStarted.v1';"),
        0);

    ASSERT_TRUE(execution_db.ValidateExecutionJobStartAuthoritySet(
        {
            {
                .job_id = 19104,
                .claimed_by_token = claimed[0].claimed_by_token,
            },
            {
                .job_id = 19105,
                .claimed_by_token = "wrong-token",
            },
            {
                .job_id = 19104,
                .claimed_by_token = claimed[0].claimed_by_token,
            },
        },
        &authority,
        &err)) << err;
    EXPECT_FALSE(authority.all_valid);
    ASSERT_EQ(authority.items.size(), 3u);
    EXPECT_EQ(
        authority.items[0].disposition,
        ExecutionJobStartAuthorityDisposition::Valid);
    EXPECT_EQ(
        authority.items[1].disposition,
        ExecutionJobStartAuthorityDisposition::TokenMismatch);
    EXPECT_EQ(
        authority.items[2].disposition,
        ExecutionJobStartAuthorityDisposition::DuplicateJob);
    EXPECT_EQ(ReadInt64(
        db_,
        "SELECT COUNT(1) FROM exec_job WHERE state='CLAIMED';"),
        3);

    ExecutionJobStartReceipt start{};
    ASSERT_TRUE(execution_db.MarkExecutionJobStarted(
        19104,
        claimed[0].claimed_by_token,
        "workset-item-start",
        &start,
        &err)) << err;
    EXPECT_EQ(start.disposition, ExecutionJobStartDisposition::Started);
    ASSERT_TRUE(start.durable_attempt_id.has_value());
    EXPECT_EQ(*start.durable_attempt_id, claimed[0].durable_attempt_id);
    EXPECT_EQ(ReadInt64(
        db_,
        "SELECT attempts FROM exec_job WHERE job_id=19104;"),
        1);
    ASSERT_TRUE(execution_db.MarkExecutionJobStarted(
        19104,
        claimed[0].claimed_by_token,
        "workset-item-start-retry",
        &start,
        &err)) << err;
    EXPECT_EQ(
        start.disposition,
        ExecutionJobStartDisposition::AlreadyRunning);
    ASSERT_TRUE(start.durable_attempt_id.has_value());
    EXPECT_EQ(*start.durable_attempt_id, claimed[0].durable_attempt_id);
    EXPECT_EQ(ReadInt64(
        db_,
        "SELECT attempts FROM exec_job WHERE job_id=19104;"),
        1);
    ASSERT_TRUE(execution_db.MarkExecutionJobStarted(
        19105,
        "wrong-token",
        "workset-item-start",
        &start,
        &err)) << err;
    EXPECT_EQ(start.disposition, ExecutionJobStartDisposition::TokenMismatch);
    EXPECT_EQ(ReadInt64(
        db_,
        "SELECT COUNT(1) FROM exec_job_event "
        "WHERE job_id=19104 "
        "AND event_kind='Execution.JobStarted.v1';"),
        1);

    ASSERT_TRUE(execution_db.RequeueClaimedExecutionJob(
        19104,
        claimed[0].claimed_by_token,
        "WORKSET_INVOCATION_RECOVERY",
        &err)) << err;
    const auto recovered_job = execution_db.GetExecutionJob(19104);
    ASSERT_TRUE(recovered_job.has_value());
    EXPECT_EQ(recovered_job->state, "QUEUED");
    EXPECT_EQ(ReadInt64(
        db_,
        "SELECT attempts FROM exec_job WHERE job_id=19104;"),
        1);
    const auto reclaimed = execution_db.ClaimBatchReadyExecutionJobs(
        "coordinator-run-78",
        1,
        30000,
        &err);
    ASSERT_TRUE(err.empty()) << err;
    ASSERT_EQ(reclaimed.size(), 1u);
    EXPECT_EQ(reclaimed[0].job_id, 19104);
    EXPECT_EQ(reclaimed[0].durable_attempt_id, 2u);
}

TEST_F(SqliteDbFixture, WorksetTerminalAuthorityRequiresExactLiveAttemptAndRenewsLease) {
    using namespace savor::db;
    using namespace savor::db::execution::workflow;
    using namespace savor::db::migrations;

    const MigrationSourceOptions embedded_options{
        .source_kind = MigrationSourceKind::Embedded,
    };
    std::string err;
    ASSERT_TRUE(ApplyContextMigrations(
        db_,
        MigrationContext::Execution,
        embedded_options,
        &err)) << err;
    ASSERT_TRUE(ExecSql(db_, R"SQL(
INSERT INTO exec_job_set(
    job_set_id, program_kind, purpose, created_at_utc)
VALUES(19301, 7, 'terminal-authority', 1);
INSERT INTO exec_job(
    job_id, job_set_id, program_kind, program_version, program_ref_kind,
    program_ref_id, fingerprint, priority, state, attempts, max_attempts,
    claimed_by_token, lease_expires_at_utc, queued_at_utc, started_at_utc)
VALUES(
    19302, 19301, 7, 1, 'seed_probe', 1, 'terminal-authority', 10,
    'RUNNING', 2, 3, 'terminal-owner',
    (unixepoch()*1000)+5000, 1, 2);
)SQL"));

    SqliteExecutionDb execution_db(db_);
    ExecutionJobTerminalAuthorityReceipt receipt{};
    const auto original_expiry = ReadInt64(
        db_,
        "SELECT lease_expires_at_utc FROM exec_job WHERE job_id=19302;");

    ASSERT_TRUE(execution_db.ConfirmExecutionJobTerminalAuthority(
        19302,
        "terminal-owner",
        1,
        30000,
        &receipt,
        &err)) << err;
    EXPECT_EQ(
        receipt.disposition,
        ExecutionJobTerminalAuthorityDisposition::AttemptMismatch);
    EXPECT_EQ(
        ReadInt64(
            db_,
            "SELECT lease_expires_at_utc FROM exec_job "
            "WHERE job_id=19302;"),
        original_expiry);

    ASSERT_TRUE(execution_db.ConfirmExecutionJobTerminalAuthority(
        19302,
        "different-owner",
        2,
        30000,
        &receipt,
        &err)) << err;
    EXPECT_EQ(
        receipt.disposition,
        ExecutionJobTerminalAuthorityDisposition::TokenMismatch);
    EXPECT_EQ(
        ReadInt64(
            db_,
            "SELECT lease_expires_at_utc FROM exec_job "
            "WHERE job_id=19302;"),
        original_expiry);

    ASSERT_TRUE(execution_db.ConfirmExecutionJobTerminalAuthority(
        19302,
        "terminal-owner",
        2,
        30000,
        &receipt,
        &err)) << err;
    EXPECT_EQ(
        receipt.disposition,
        ExecutionJobTerminalAuthorityDisposition::Valid);
    ASSERT_TRUE(receipt.durable_attempt_id.has_value());
    EXPECT_EQ(*receipt.durable_attempt_id, 2u);
    ASSERT_TRUE(receipt.lease_expires_at_utc.has_value());
    EXPECT_GT(*receipt.lease_expires_at_utc, original_expiry);

    ASSERT_TRUE(ExecSql(
        db_,
        "UPDATE exec_job SET lease_expires_at_utc=1 "
        "WHERE job_id=19302;"));
    ASSERT_TRUE(execution_db.ConfirmExecutionJobTerminalAuthority(
        19302,
        "terminal-owner",
        2,
        30000,
        &receipt,
        &err)) << err;
    EXPECT_EQ(
        receipt.disposition,
        ExecutionJobTerminalAuthorityDisposition::Expired);
}

TEST_F(SqliteDbFixture, WorksetWorkerLossRecoveryIsExactAndTerminalizesFinalAttempt) {
    using namespace savor::db;
    using namespace savor::db::execution::workflow;
    using namespace savor::db::migrations;

    const MigrationSourceOptions embedded_options{
        .source_kind = MigrationSourceKind::Embedded,
    };
    std::string err;
    ASSERT_TRUE(ApplyContextMigrations(
        db_,
        MigrationContext::Execution,
        embedded_options,
        &err)) << err;
    ASSERT_TRUE(ExecSql(db_, R"SQL(
INSERT INTO exec_job_set(
    job_set_id, program_kind, purpose, created_at_utc)
VALUES(19401, 7, 'worker-loss', 1);
INSERT INTO exec_job(
    job_id, job_set_id, program_kind, program_version, program_ref_kind,
    program_ref_id, fingerprint, priority, state, attempts, max_attempts,
    claimed_by_token, lease_expires_at_utc, queued_at_utc, started_at_utc)
VALUES
    (19402, 19401, 7, 1, 'seed_probe', 1, 'claimed-loss', 10,
     'CLAIMED', 0, 3, 'claimed-owner',
     (unixepoch()*1000)+60000, 1, NULL),
    (19403, 19401, 7, 1, 'seed_probe', 1, 'running-loss', 9,
     'RUNNING', 1, 3, 'running-owner',
     (unixepoch()*1000)+60000, 1, 2),
    (19404, 19401, 7, 1, 'seed_probe', 1, 'final-loss', 8,
     'RUNNING', 1, 1, 'final-owner',
     (unixepoch()*1000)+60000, 1, 2);
)SQL"));

    SqliteExecutionDb execution_db(db_);
    ExecutionJobWorkerLossRecoveryReceipt receipt{};

    ASSERT_TRUE(execution_db.RecoverExecutionJobAfterWorkerLoss(
        19403,
        "stale-owner",
        1,
        "worker lost",
        &receipt,
        &err)) << err;
    EXPECT_EQ(
        receipt.disposition,
        ExecutionJobWorkerLossRecoveryDisposition::TokenMismatch);
    EXPECT_EQ(
        ReadText(
            db_,
            "SELECT state FROM exec_job WHERE job_id=19403;"),
        "RUNNING");
    EXPECT_EQ(
        ReadText(
            db_,
            "SELECT claimed_by_token FROM exec_job WHERE job_id=19403;"),
        "running-owner");

    ASSERT_TRUE(execution_db.RecoverExecutionJobAfterWorkerLoss(
        19403,
        "running-owner",
        2,
        "worker lost",
        &receipt,
        &err)) << err;
    EXPECT_EQ(
        receipt.disposition,
        ExecutionJobWorkerLossRecoveryDisposition::AttemptMismatch);
    EXPECT_EQ(
        ReadText(
            db_,
            "SELECT state FROM exec_job WHERE job_id=19403;"),
        "RUNNING");

    ASSERT_TRUE(execution_db.RecoverExecutionJobAfterWorkerLoss(
        19402,
        "claimed-owner",
        1,
        "worker lost before start",
        &receipt,
        &err)) << err;
    EXPECT_EQ(
        receipt.disposition,
        ExecutionJobWorkerLossRecoveryDisposition::Interrupted);
    EXPECT_EQ(
        ReadText(
            db_,
            "SELECT state FROM exec_job WHERE job_id=19402;"),
        "INTERRUPTED");
    EXPECT_EQ(
        ReadInt64(
            db_,
            "SELECT attempts FROM exec_job WHERE job_id=19402;"),
        0);

    ASSERT_TRUE(execution_db.RecoverExecutionJobAfterWorkerLoss(
        19403,
        "running-owner",
        1,
        "worker lost during execution",
        &receipt,
        &err)) << err;
    EXPECT_EQ(
        receipt.disposition,
        ExecutionJobWorkerLossRecoveryDisposition::Interrupted);
    EXPECT_EQ(
        ReadText(
            db_,
            "SELECT state FROM exec_job WHERE job_id=19403;"),
        "INTERRUPTED");
    EXPECT_EQ(
        ReadInt64(
            db_,
            "SELECT attempts FROM exec_job WHERE job_id=19403;"),
        1);

    ASSERT_TRUE(execution_db.RecoverExecutionJobAfterWorkerLoss(
        19404,
        "final-owner",
        1,
        "worker lost on final attempt",
        &receipt,
        &err)) << err;
    EXPECT_EQ(
        receipt.disposition,
        ExecutionJobWorkerLossRecoveryDisposition::Interrupted);
    EXPECT_EQ(
        ReadText(
            db_,
            "SELECT state FROM exec_job WHERE job_id=19404;"),
        "INTERRUPTED");
    EXPECT_EQ(
        ReadText(
            db_,
            "SELECT error_code FROM exec_job WHERE job_id=19404;"),
        "WORKER_LOSS");
    EXPECT_EQ(
        ReadInt64(
            db_,
            "SELECT COUNT(1) FROM exec_job_event "
            "WHERE job_id=19404 "
            "AND event_kind='Execution.JobCompleted.v1';"),
        1);
    EXPECT_EQ(
        ReadInt64(
            db_,
            "SELECT COUNT(1) FROM exec_job "
            "WHERE job_id=19404 AND state='QUEUED' "
            "AND attempts>=max_attempts;"),
        0);
}

TEST_F(SqliteDbFixture, WorksetBatchClaimRollsBackWhenAnyCandidateCannotResolveItsWorkflowStep) {
    using namespace savor::db::execution::workflow;
    using namespace savor::db::migrations;

    const MigrationSourceOptions embedded_options{
        .source_kind = MigrationSourceKind::Embedded,
    };
    std::string err;
    ASSERT_TRUE(ApplyContextMigrations(
        db_,
        MigrationContext::Execution,
        embedded_options,
        &err)) << err;
    ASSERT_TRUE(ExecSql(db_, R"SQL(
INSERT INTO exec_workflow_instance(
    workflow_instance_id, workflow_kind, state, root_scope_kind,
    created_by, created_at_utc)
VALUES(19200, 'WORKSET_TEST', 'RUNNING', 'manual', 'test', 1);
INSERT INTO exec_job_set(
    job_set_id, program_kind, purpose, created_at_utc)
VALUES
    (19201, 7, 'mapped', 1),
    (19202, 7, 'unmapped', 1);
INSERT INTO exec_workflow_step(
    workflow_step_id, workflow_instance_id, step_key, step_kind, state,
    job_set_id, priority, attempts, max_attempts, created_at_utc)
VALUES(
    19203, 19200, 'Mapped', 'seedprobe.grid', 'MATERIALIZED',
    19201, 10, 0, 2, 1);
INSERT INTO exec_job(
    job_id, job_set_id, program_kind, program_version, program_ref_kind,
    program_ref_id, fingerprint, priority, state, attempts, max_attempts,
    queued_at_utc)
VALUES
    (19204, 19201, 7, 1, 'seed_probe', 1, 'mapped', 10,
     'QUEUED', 0, 3, 100),
    (19205, 19202, 7, 1, 'seed_probe', 1, 'unmapped', 10,
     'QUEUED', 0, 3, 200);
)SQL"));

    SqliteExecutionDb execution_db(db_);
    const auto claimed = execution_db.ClaimBatchReadyExecutionJobs(
        "coordinator-run-rollback",
        2,
        30000,
        &err);
    EXPECT_TRUE(claimed.empty());
    EXPECT_FALSE(err.empty());
    EXPECT_EQ(ReadInt64(
        db_,
        "SELECT COUNT(1) FROM exec_job "
        "WHERE job_id IN (19204,19205) AND state='QUEUED' "
        "AND claimed_by_token IS NULL;"),
        2);
    EXPECT_EQ(ReadInt64(
        db_,
        "SELECT COUNT(1) FROM exec_job_event "
        "WHERE job_id IN (19204,19205) "
        "AND event_kind='Execution.JobClaimed.v1';"),
        0);
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

TEST_F(SqliteDbFixture, Stage3dMarkQueuedJobsSupersededEmitsJobCompletedOutbox) {
    using namespace savor::db::execution::workflow;
    using namespace savor::db::migrations;

    const MigrationSourceOptions embedded_options{ .source_kind = MigrationSourceKind::Embedded };
    std::string err;
    ASSERT_TRUE(ApplyContextMigrations(db_, MigrationContext::Execution, embedded_options, &err)) << err;

    ASSERT_TRUE(ExecSql(db_, R"SQL(
INSERT INTO exec_job_set(job_set_id, program_kind, purpose, expected_total, created_at_utc)
VALUES(1910, 7, 'supersede-event-root', 4, 1000);
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
            .battle_plan_id = 202,
            .battle_plan_fingerprint = "stage3d-plan",
            .continuation_mode = BattleContinuationMode::AutomaticBestPerEndingRng,
            .status = savor::db::BattleSetStatus::Active,
            .created_at_utc = now,
            .correlation_id = "ab-corr-1",
            .causation_id = "ab-cause-1",
        },
        &battle_set_id,
        &err))
        << err;
    ASSERT_GT(battle_set_id, 0);

    const auto confirmed_seed = CreateConfirmedSeedProbeFixture(
        &analysis_db,
        "stage3d-analysis-battle-events",
        101,
        202,
        444,
        555,
        now,
        &err);
    ASSERT_TRUE(confirmed_seed.has_value()) << err;

    std::int64_t seed_candidate_id = 0;
    ASSERT_TRUE(analysis_db.AddBattleSeedCandidate(
        {
            .battle_set_id = battle_set_id,
            .source_probe_result_id = confirmed_seed->probe_result_id,
            .source_input_frame_id = confirmed_seed->input_frame_id,
            .seed_value = 555,
            .source_kind = savor::db::BattleSeedCandidateSourceKind::SeedProbeConfirmedResult,
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

    std::int64_t plan_id = 0;
    ASSERT_TRUE(authoring_db.SavePlan(
        {
            .name = "single-turn-plan",
            .description = "Single-turn persistence fixture",
            .fingerprint = "plan-fp-1",
            .created_at_utc = now,
            .correlation_id = "au-corr",
            .causation_id = "au-cause-2",
        },
        &plan_id,
        &err)) << err;

    sqlite3_stmt* battle_plan_turn_count_column = nullptr;
    ASSERT_EQ(SQLITE_OK, sqlite3_prepare_v2(
        db_,
        "SELECT COUNT(1) FROM pragma_table_info('au_battle_plan') WHERE name='num_turns';",
        -1,
        &battle_plan_turn_count_column,
        nullptr));
    ASSERT_EQ(SQLITE_ROW, sqlite3_step(battle_plan_turn_count_column));
    EXPECT_EQ(sqlite3_column_int(battle_plan_turn_count_column, 0), 0);
    sqlite3_finalize(battle_plan_turn_count_column);
    EXPECT_FALSE(ColumnExists(
        db_, "au_battle_plan_action_preset", "target_expr_ini"));

    EXPECT_FALSE(authoring_db.SaveBattlePlanTurn(
        {
            .plan_id = plan_id,
            .turn_index = 0,
            .created_at_utc = now,
            .correlation_id = "au-corr",
            .causation_id = "invalid-turn-zero",
        }, nullptr, &err));
    err.clear();

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

    const auto plan = authoring_db.GetBattlePlan(plan_id);
    ASSERT_TRUE(plan.has_value());
    EXPECT_EQ(plan->description, "Single-turn persistence fixture");
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

    std::int64_t battle_set_id = 0;
    ASSERT_TRUE(analysis_db.CreateBattleSet(
        {
            .name = "battle-set-roundtrip",
            .entry_savestate_id = 501,
            .battle_plan_id = plan_id,
            .battle_plan_fingerprint = "plan-fp-1",
            .continuation_mode = BattleContinuationMode::AutomaticBestPerEndingRng,
            .status = savor::db::BattleSetStatus::Active,
            .created_at_utc = now,
            .correlation_id = "ab-corr",
            .causation_id = "ab-cause-1",
        },
        &battle_set_id,
        &err)) << err;

    const auto confirmed_seed = CreateConfirmedSeedProbeFixture(
        &analysis_db,
        "stage3d-battle-roundtrip",
        501,
        plan_id,
        2001,
        7777,
        now,
        &err);
    ASSERT_TRUE(confirmed_seed.has_value()) << err;

    std::int64_t seed_candidate_id = 0;
    ASSERT_TRUE(analysis_db.AddBattleSeedCandidate(
        {
            .battle_set_id = battle_set_id,
            .source_probe_result_id = confirmed_seed->probe_result_id,
            .source_input_frame_id = confirmed_seed->input_frame_id,
            .seed_value = 7777,
            .source_kind = savor::db::BattleSeedCandidateSourceKind::SeedProbeConfirmedResult,
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
    EXPECT_EQ(battle_set->battle_plan_id, plan_id);
    EXPECT_EQ(battle_set->battle_plan_fingerprint, "plan-fp-1");
    EXPECT_EQ(battle_set->continuation_mode, BattleContinuationMode::AutomaticBestPerEndingRng);

    const auto candidates = analysis_db.ListBattleSeedCandidates(battle_set_id);
    ASSERT_EQ(candidates.size(), 1);
    EXPECT_EQ(candidates[0].source_probe_result_id.value_or(0), confirmed_seed->probe_result_id);
    EXPECT_EQ(candidates[0].source_input_frame_id.value_or(0), confirmed_seed->input_frame_id);
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

TEST_F(SqliteDbFixture, SeedProbeNeutralSelectionRequiresSurveySourceJob) {
    using namespace savor::db;
    using namespace savor::db::execution::programdb::seedprobe;

    auto* analysis_db = db_service_->AnalysisDb();
    auto* execution_db = db_service_->ExecutionDb();
    ASSERT_NE(analysis_db, nullptr);
    ASSERT_NE(execution_db, nullptr);

    ASSERT_TRUE(ExecSql(db_, R"SQL(
INSERT INTO sp_probe_set(
    probe_set_id,name,probe_flavor,breakpoint_policy_name,
    segment_source_kind,created_at_utc)
VALUES(77601,'neutral-stage-filter','BATTLE_PRE','test','test',1000);
INSERT INTO an_input_set(
    input_set_id,source_ref_kind,source_ref_id,created_at_utc)
VALUES(77602,'sp_probe_run',77605,1000);
INSERT INTO sp_axis_xy(axis_xy_id,x,y)
VALUES(77603,128,128),(77604,0,0);
INSERT INTO sp_input_frame(
    input_frame_id,main_axis_xy_id,cstick_axis_xy_id,trigger_axis_xy_id)
VALUES(77606,77603,77603,77604);
INSERT INTO sp_probe_run(
    probe_run_id,materialization_key,probe_set_id,entry_savestate_id,
    seed_probe_spec_id,launch_samples_per_axis,codec_version,status,
    accepted_input_set_id,requested_at_utc)
VALUES(
    77605,'fixture.neutral-stage-filter',77601,1,
    1,1,1,'SEARCH',77602,1000);
INSERT INTO exec_job_set(
    job_set_id,program_kind,purpose,created_by,created_at_utc,
    priority_boost,expected_total,domain_ref_kind,domain_ref_id)
VALUES(
    77610,1,'SeedProbe Search','test',1000,
    0,1,'sp_probe_run',77605);
INSERT INTO exec_job(
    job_id,job_set_id,program_kind,program_version,program_ref_kind,
    program_ref_id,fingerprint,priority,state,attempts,max_attempts,
    queued_at_utc,input_ini)
VALUES(
    77611,77610,1,4,'sp_probe_run',
    77605,'neutral-stage-filter-search',0,'SUCCEEDED',1,1,1000,
    '[SeedProbe.Request]
desired_delta=1
input_frame_id=77606
sample_ordinal=0
stage=SEARCH
version=1
');
INSERT INTO sp_probe_result(
    probe_result_id,probe_run_id,input_frame_id,source_job_id,seed_value,
    origin_worker_id,origin_process_generation,origin_workset_epoch,
    terminal_sha256,confirmation_of_probe_result_id,
    evidence_state,recorded_at_utc)
VALUES(
    77621,77605,77606,77611,100,1,1,1,
    '1111111111111111111111111111111111111111111111111111111111111111',
    NULL,'PROVISIONAL',1000);
)SQL")) << sqlite3_errmsg(db_);

    EXPECT_FALSE(
        FindSurveyNeutralSeedProbeResultId(
            execution_db,
            analysis_db,
            77605)
            .has_value());

    ASSERT_TRUE(ExecSql(db_, R"SQL(
INSERT INTO exec_job(
    job_id,job_set_id,program_kind,program_version,program_ref_kind,
    program_ref_id,fingerprint,priority,state,attempts,max_attempts,
    queued_at_utc,input_ini)
VALUES(
    77612,77610,1,4,'sp_probe_run',
    77605,'neutral-stage-filter-survey',0,'SUCCEEDED',1,1,1000,
    '[SeedProbe.Request]
input_frame_id=77606
sample_ordinal=0
stage=SURVEY
version=1
');
INSERT INTO sp_probe_result(
    probe_result_id,probe_run_id,input_frame_id,source_job_id,seed_value,
    origin_worker_id,origin_process_generation,origin_workset_epoch,
    terminal_sha256,confirmation_of_probe_result_id,
    evidence_state,recorded_at_utc)
VALUES(
    77622,77605,77606,77612,101,1,1,2,
    '2222222222222222222222222222222222222222222222222222222222222222',
    NULL,'PROVISIONAL',1000);
)SQL")) << sqlite3_errmsg(db_);

    const auto neutral_result_id =
        FindSurveyNeutralSeedProbeResultId(
            execution_db,
            analysis_db,
            77605);
    ASSERT_TRUE(neutral_result_id.has_value());
    EXPECT_EQ(*neutral_result_id, 77622);
}

TEST_F(SqliteDbFixture, Stage5SeedProbeRunAcceptsSameNumericEpochFromDifferentWorkerAndPublishesConfirmedFrames) {
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
            .materialization_key =
                "fixture.input-set-owner",
            .probe_set_id = probe_set_id,
            .entry_savestate_id = 77001,
            .seed_probe_spec_id = seed_probe_spec_id,
            .launch_samples_per_axis = 1,
            .codec_version = 1,
            .status = SeedProbeRunStatus::Survey,
            .requested_at_utc = now,
            .correlation_id = "an-probe-run-input-set-owner",
        },
        &probe_run_id,
        &err)) << err;
    std::int64_t retried_probe_run_id = 0;
    ASSERT_TRUE(analysis_db->RequestSeedProbeRun(
        {
            .materialization_key =
                "fixture.input-set-owner",
            .probe_set_id = probe_set_id,
            .entry_savestate_id = 77001,
            .seed_probe_spec_id = seed_probe_spec_id,
            .launch_samples_per_axis = 1,
            .codec_version = 1,
            .status = SeedProbeRunStatus::Survey,
            .requested_at_utc = now,
            .correlation_id =
                "an-probe-run-input-set-owner-retry",
        },
        &retried_probe_run_id,
        &err)) << err;
    EXPECT_EQ(retried_probe_run_id, probe_run_id);
    EXPECT_EQ(
        ReadInt64(
            db_,
            "SELECT COUNT(1) FROM sp_probe_run "
            "WHERE materialization_key='fixture.input-set-owner';"),
        1);

    const auto probe_run = analysis_db->GetSeedProbeRun(probe_run_id);
    ASSERT_TRUE(probe_run.has_value());
    ASSERT_GT(probe_run->accepted_input_set_id, 0);
    EXPECT_TRUE(analysis_db->ListAnalysisInputSetFrames(probe_run->accepted_input_set_id).empty());

    std::int64_t input_frame_id = 0;
    ASSERT_TRUE(analysis_db->EnsureSeedProbeInputFrame(0x8080, 0x8080, 0x0000, &input_frame_id, &err)) << err;

    const RecordSeedProbeObservationCommand representative{
        .probe_run_id = probe_run_id,
        .input_frame_id = input_frame_id,
        .source_job_id = 77010,
        .seed_value = 0,
        .origin_worker_id = 1,
        .origin_process_generation = 1,
        .origin_workset_epoch = 1,
        .terminal_sha256 = std::string(64, 'a'),
        .endpoint = SeedProbeEndpoint::AfterRandSeedSet,
        .recorded_at_utc = now,
        .correlation_id = "an-probe-observation-input-set-owner",
        .causation_id = "an-probe-run-input-set-owner",
    };
    RecordSeedProbeObservationReceipt observation_receipt{};
    ASSERT_TRUE(analysis_db->RecordSeedProbeObservation(
        representative,
        &observation_receipt,
        &err)) << err;
    EXPECT_TRUE(observation_receipt.inserted);
    const auto probe_result_id =
        observation_receipt.observation.probe_result_id;
    EXPECT_GT(probe_result_id, 0);
    observation_receipt = {};
    ASSERT_TRUE(analysis_db->RecordSeedProbeObservation(
        representative,
        &observation_receipt,
        &err)) << err;
    EXPECT_FALSE(observation_receipt.inserted);
    EXPECT_EQ(
        observation_receipt.observation.probe_result_id,
        probe_result_id);
    auto conflicting_replay = representative;
    conflicting_replay.seed_value = 1;
    conflicting_replay.endpoint = SeedProbeEndpoint::RandSeedCommitted;
    observation_receipt = {};
    EXPECT_FALSE(analysis_db->RecordSeedProbeObservation(
        conflicting_replay,
        &observation_receipt,
        &err));
    const auto unchanged_run = analysis_db->GetSeedProbeRun(probe_run_id);
    ASSERT_TRUE(unchanged_run.has_value());
    EXPECT_EQ(unchanged_run->status, SeedProbeRunStatus::Survey);
    EXPECT_EQ(
        unchanged_run->established_endpoint,
        SeedProbeEndpoint::AfterRandSeedSet);
    EXPECT_FALSE(unchanged_run->conflicting_endpoint.has_value());

    bool changed = false;
    ASSERT_TRUE(analysis_db->TransitionSeedProbeEvidence(
        {
            .probe_result_id = probe_result_id,
            .expected_state = SeedProbeEvidenceState::Observed,
            .new_state = SeedProbeEvidenceState::Provisional,
            .changed_at_utc = now,
            .correlation_id = "an-probe-evidence-input-set-owner",
            .causation_id = "an-probe-observation-input-set-owner",
        },
        &changed,
        &err)) << err;
    EXPECT_TRUE(changed);

    observation_receipt = {};
    ASSERT_TRUE(analysis_db->RecordSeedProbeObservation(
        {
            .probe_run_id = probe_run_id,
            .input_frame_id = input_frame_id,
            .source_job_id = 77011,
            .seed_value = 0,
            .origin_worker_id = 2,
            .origin_process_generation = 1,
            .origin_workset_epoch = 1,
            .terminal_sha256 = std::string(64, 'b'),
            .confirmation_of_probe_result_id = probe_result_id,
            .endpoint = SeedProbeEndpoint::AfterRandSeedSet,
            .recorded_at_utc = now,
            .correlation_id = "an-probe-confirmation-input-set-owner",
            .causation_id = "an-probe-observation-input-set-owner",
        },
        &observation_receipt,
        &err)) << err;
    EXPECT_TRUE(observation_receipt.inserted);
    const auto confirmation_result_id =
        observation_receipt.observation.probe_result_id;
    EXPECT_GT(confirmation_result_id, 0);
    const auto confirmation =
        analysis_db->GetSeedProbeResult(
            confirmation_result_id);
    ASSERT_TRUE(confirmation.has_value());
    EXPECT_EQ(confirmation->origin_worker_id, 2);
    EXPECT_EQ(confirmation->origin_process_generation, 1);
    EXPECT_EQ(confirmation->origin_workset_epoch, 1);

    changed = false;
    ASSERT_TRUE(analysis_db->TransitionSeedProbeEvidence(
        {
            .probe_result_id = probe_result_id,
            .expected_state = SeedProbeEvidenceState::Provisional,
            .new_state = SeedProbeEvidenceState::Confirmed,
            .changed_at_utc = now,
            .correlation_id = "an-probe-confirmed-input-set-owner",
            .causation_id = "an-probe-confirmation-input-set-owner",
        },
        &changed,
        &err)) << err;
    EXPECT_TRUE(changed);

    ASSERT_TRUE(analysis_db->ReplaceSeedProbeAcceptedInputFrames(
        {
            .probe_run_id = probe_run_id,
            .input_frame_ids = { input_frame_id },
            .replaced_at_utc = now,
            .correlation_id = "an-probe-accepted-input-set-owner",
            .causation_id = "an-probe-confirmed-input-set-owner",
        },
        &err)) << err;
    const auto frames = analysis_db->ListAnalysisInputSetFrames(probe_run->accepted_input_set_id);
    ASSERT_EQ(frames.size(), 1);
    EXPECT_EQ(frames[0].ordinal, 0);
    EXPECT_EQ(frames[0].input_frame_id, input_frame_id);

    const auto confirmed = analysis_db->GetSeedProbeResult(probe_result_id);
    ASSERT_TRUE(confirmed.has_value());
    EXPECT_EQ(confirmed->evidence_state, SeedProbeEvidenceState::Confirmed);
}

TEST_F(SqliteDbFixture, Stage5ConfirmedSeedProbeLookupIsScopedToAcceptedInputSetOwner) {
    using namespace savor::db;

    auto* analysis_db = db_service_->AnalysisDb();
    ASSERT_NE(analysis_db, nullptr);

    const auto now =
        types::UtcTimePoint(std::chrono::milliseconds(1712304000904));
    std::string err;
    const auto first = CreateConfirmedSeedProbeFixture(
        analysis_db,
        "accepted-input-owner-first",
        88001,
        99001,
        77100,
        0x11111111u,
        now,
        &err);
    ASSERT_TRUE(first.has_value()) << err;
    const auto second = CreateConfirmedSeedProbeFixture(
        analysis_db,
        "accepted-input-owner-second",
        88001,
        99001,
        77200,
        0x22222222u,
        now,
        &err);
    ASSERT_TRUE(second.has_value()) << err;
    ASSERT_EQ(first->input_frame_id, second->input_frame_id);

    const auto first_result =
        analysis_db->GetSeedProbeResult(first->probe_result_id);
    const auto second_result =
        analysis_db->GetSeedProbeResult(second->probe_result_id);
    ASSERT_TRUE(first_result.has_value());
    ASSERT_TRUE(second_result.has_value());
    ASSERT_NE(first_result->probe_run_id, second_result->probe_run_id);

    const auto first_run =
        analysis_db->GetSeedProbeRun(first_result->probe_run_id);
    const auto second_run =
        analysis_db->GetSeedProbeRun(second_result->probe_run_id);
    ASSERT_TRUE(first_run.has_value());
    ASSERT_TRUE(second_run.has_value());
    ASSERT_NE(
        first_run->accepted_input_set_id,
        second_run->accepted_input_set_id);

    ASSERT_TRUE(analysis_db->ReplaceSeedProbeAcceptedInputFrames(
        {
            .probe_run_id = first_result->probe_run_id,
            .input_frame_ids = {first->input_frame_id},
            .replaced_at_utc = now,
            .correlation_id = "accepted-input-owner-first",
            .causation_id = "accepted-input-owner-first",
        },
        &err)) << err;
    ASSERT_TRUE(analysis_db->ReplaceSeedProbeAcceptedInputFrames(
        {
            .probe_run_id = second_result->probe_run_id,
            .input_frame_ids = {second->input_frame_id},
            .replaced_at_utc = now,
            .correlation_id = "accepted-input-owner-second",
            .causation_id = "accepted-input-owner-second",
        },
        &err)) << err;

    const auto from_first =
        analysis_db->FindConfirmedSeedProbeResultForAcceptedInputSetFrame(
            first_run->accepted_input_set_id,
            first->input_frame_id);
    const auto from_second =
        analysis_db->FindConfirmedSeedProbeResultForAcceptedInputSetFrame(
            second_run->accepted_input_set_id,
            second->input_frame_id);
    ASSERT_TRUE(from_first.has_value());
    ASSERT_TRUE(from_second.has_value());
    EXPECT_EQ(from_first->probe_result_id, first->probe_result_id);
    EXPECT_EQ(from_first->seed_value, 0x11111111u);
    EXPECT_EQ(from_second->probe_result_id, second->probe_result_id);
    EXPECT_EQ(from_second->seed_value, 0x22222222u);
}

TEST_F(
    SqliteDbFixture,
    UiReadSeedProbeProjectionSeparatesSurveyGridFromConfirmedSearch) {
    auto* ui_read_db = db_service_->UiReadDb();
    ASSERT_NE(ui_read_db, nullptr);

    ASSERT_TRUE(ExecSql(
        db_,
        R"SQL(
INSERT INTO sp_probe_set(
    probe_set_id,name,probe_flavor,breakpoint_policy_name,
    segment_source_kind,created_at_utc)
VALUES(
    77100,'uiread-stage-classification','BATTLE_PRE','test',
    'fixture',1712304000950);
INSERT INTO an_input_set(
    input_set_id,source_ref_kind,source_ref_id,created_at_utc)
VALUES(77101,'sp_probe_run',77103,1712304000950);
INSERT INTO sp_probe_run(
    probe_run_id,materialization_key,probe_set_id,entry_savestate_id,
    seed_probe_spec_id,launch_samples_per_axis,codec_version,status,
    accepted_input_set_id,requested_at_utc)
VALUES(
    77103,'fixture.uiread-stage-classification',77100,77104,
    77105,1,1,'SURVEY',77101,1712304000950);
INSERT INTO sp_axis_xy(axis_xy_id,x,y) VALUES
    (77110,128,128),
    (77111,0,0),
    (77112,129,128),
    (77113,129,129),
    (77114,1,1);
INSERT INTO sp_input_frame(
    input_frame_id,main_axis_xy_id,cstick_axis_xy_id,trigger_axis_xy_id)
VALUES
    (77120,77110,77110,77111),
    (77121,77112,77110,77111),
    (77122,77113,77113,77114);

INSERT INTO exec_job_set(
    job_set_id,program_kind,purpose,created_by,created_at_utc,
    priority_boost,expected_total,domain_ref_kind,domain_ref_id)
VALUES(
    77200,100,'SeedProbe UIRead stage classification','test',
    1712304000950,0,6,'analysisseedprobe.probe_run',77103);
INSERT INTO exec_job(
    job_id,job_set_id,program_kind,program_version,program_ref_kind,
    program_ref_id,fingerprint,priority,state,attempts,max_attempts,
    queued_at_utc,input_ini)
VALUES
    (77201,77200,100,1,'analysisseedprobe.probe_run',77103,
     'uiread-neutral',1,'SUCCEEDED',1,1,1712304000950,
     '[SeedProbe.Request]
input_frame_id=77120
sample_ordinal=0
stage=SURVEY
version=1
'),
    (77202,77200,100,1,'analysisseedprobe.probe_run',77103,
     'uiread-grid',1,'SUCCEEDED',1,1,1712304000950,
     '[SeedProbe.Request]
input_frame_id=77121
sample_ordinal=1
stage=SURVEY
version=1
'),
    (77203,77200,100,1,'analysisseedprobe.probe_run',77103,
     'uiread-search',1,'SUCCEEDED',1,1,1712304000950,
     '[SeedProbe.Request]
input_frame_id=77122
sample_ordinal=2
stage=SEARCH
version=1
'),
    (77204,77200,100,1,'analysisseedprobe.probe_run',77103,
     'uiread-neutral-confirm',1,'SUCCEEDED',1,1,1712304000950,
     '[SeedProbe.Request]
input_frame_id=77120
sample_ordinal=0
stage=CONFIRM
version=1
'),
    (77205,77200,100,1,'analysisseedprobe.probe_run',77103,
     'uiread-grid-confirm',1,'SUCCEEDED',1,1,1712304000950,
     '[SeedProbe.Request]
input_frame_id=77121
sample_ordinal=1
stage=CONFIRM
version=1
'),
    (77206,77200,100,1,'analysisseedprobe.probe_run',77103,
     'uiread-search-confirm',1,'SUCCEEDED',1,1,1712304000950,
     '[SeedProbe.Request]
input_frame_id=77122
sample_ordinal=2
stage=CONFIRM
version=1
');

INSERT INTO sp_probe_result(
    probe_result_id,probe_run_id,input_frame_id,source_job_id,seed_value,
    origin_worker_id,origin_process_generation,origin_workset_epoch,
    terminal_sha256,confirmation_of_probe_result_id,
    evidence_state,recorded_at_utc)
VALUES
    (77301,77103,77120,77201,4294967295,1,1,1,
     '1111111111111111111111111111111111111111111111111111111111111111',
     NULL,'CONFIRMED',1712304000950),
    (77302,77103,77121,77202,0,1,1,2,
     '2222222222222222222222222222222222222222222222222222222222222222',
     NULL,'CONFIRMED',1712304000950),
    (77303,77103,77122,77203,4294967293,1,1,3,
     '3333333333333333333333333333333333333333333333333333333333333333',
     NULL,'CONFIRMED',1712304000950),
    (77304,77103,77120,77204,4294967295,1,1,4,
     '4444444444444444444444444444444444444444444444444444444444444444',
     77301,'OBSERVED',1712304000950),
    (77305,77103,77121,77205,0,1,1,5,
     '5555555555555555555555555555555555555555555555555555555555555555',
     77302,'OBSERVED',1712304000950),
    (77306,77103,77122,77206,4294967293,1,1,6,
     '6666666666666666666666666666666666666666666666666666666666666666',
     77303,'OBSERVED',1712304000950);
INSERT INTO sp_outbox_message(
    outbox_id,event_id,event_type,event_version,context_name,
    aggregate_kind,aggregate_id,occurred_at_utc,payload_ref_kind,
    payload_ref_id,attempt_count)
VALUES(
    77401,'uiread-stage-classification-event',
    'AnalysisSeedProbe.ObservationRecorded.v1',1,'AnalysisSeedProbe',
    'probe_run','77103',1712304000950,'probe_result',77303,0);
)SQL"))
        << sqlite3_errmsg(db_);

    RunUiReadProjectionUntilCaughtUp(
        *db_service_,
        "analysis-seedprobe");

    const auto summary =
        ui_read_db->GetSeedProbeRunSummary(77103);
    ASSERT_TRUE(summary.has_value());
    ASSERT_TRUE(summary->neutral_seed_value.has_value());
    EXPECT_EQ(*summary->neutral_seed_value, 0xFFFFFFFFu);
    EXPECT_EQ(summary->grid_count, 1);
    EXPECT_EQ(summary->unique_count, 1);

    const auto grid_points =
        ui_read_db->ListSeedProbeDeltaPoints(77103);
    ASSERT_EQ(grid_points.size(), 1);
    EXPECT_EQ(grid_points[0].source_family, "MAIN");
    EXPECT_EQ(grid_points[0].seed_value, 0u);
    EXPECT_EQ(grid_points[0].seed_delta, 1);

    const auto search_values =
        ui_read_db->ListSeedProbeUniqueValues(77103);
    ASSERT_EQ(search_values.size(), 1);
    EXPECT_EQ(search_values[0].seed_value, 0xFFFFFFFDu);
    EXPECT_EQ(search_values[0].seed_delta, -2);
    EXPECT_EQ(search_values[0].main_x, 129);
    EXPECT_EQ(search_values[0].main_y, 129);
    EXPECT_EQ(search_values[0].cstick_x, 129);
    EXPECT_EQ(search_values[0].cstick_y, 129);
    EXPECT_EQ(search_values[0].trigger_x, 1);
    EXPECT_EQ(search_values[0].trigger_y, 1);
}

TEST_F(SqliteDbFixture, Stage5AuthoringWorkflowGraphStoresDirectBattlePlanWithoutExternalInputs) {
    using namespace savor::db;
    std::string err;

    auto* authoring_db = db_service_->AuthoringDb();
    ASSERT_NE(authoring_db, nullptr);
    const auto now = types::UtcTimePoint(std::chrono::milliseconds(1712304000123));

    SaveWorkflowGraphResult saved{};
    ASSERT_TRUE(authoring_db->SaveWorkflowGraph(
        {
            .name = "canonical-start-to-battle",
            .description = "TAS -> seed probe -> battle",
            .graph_version = 1,
            .graph_hash = "graph-hash-canonical-start-to-battle",
            .nodes = {
                {
                    .node_key = "tas_1",
                    .unit_kind = "tas_movie",
                    .display_name = "TAS Movie",
                    .inputs = {
                        { .input_key = "dtm_artifact", .data_kind = "state_artifact.dtm_artifact_id", .ref_kind = "state_artifact", .display_name = "DTM artifact" },
                    },
                    .possible_outputs = {
                        { .output_key = "savestate", .data_kind = "state.savestate_id", .ref_kind = "state.savestate", .display_name = "Output savestate" },
                    },
                },
                {
                    .node_key = "probe_1",
                    .unit_kind = "seed_probe",
                    .display_name = "Seed Probe",
                    .inputs = {
                        { .input_key = "entry_savestate", .data_kind = "state.savestate_id", .ref_kind = "state.savestate", .display_name = "Entry savestate" },
                    },
                    .possible_outputs = {
                        { .output_key = "seed_probe_run", .data_kind = "analysis.seed_probe_run", .ref_kind = "sp_probe_run", .display_name = "SeedProbe run" },
                    },
                },
                {
                    .node_key = "battle_1",
                    .unit_kind = "battle",
                    .display_name = "Battle",
                    .authored_ref_kind = std::string("authoring.battle_plan"),
                    .authored_ref_id = 77,
                    .inputs = {
                        { .input_key = "seed_probe_run", .data_kind = "analysis.seed_probe_run", .ref_kind = "sp_probe_run", .display_name = "Confirmed SeedProbe run" },
                        { .input_key = "battle_context", .data_kind = "analysis_battle.battle_context_id", .ref_kind = "ab_battle_context", .display_name = "Battle context" },
                    },
                    .possible_outputs = {
                        { .output_key = "battle_set", .data_kind = "analysis_battle.battle_set", .ref_kind = "analysis_battle.battle_set", .display_name = "Battle set" },
                    },
                },
            },
            .edges = {
                { .from_node_key = "tas_1", .output_key = "savestate", .to_node_key = "probe_1", .input_key = "entry_savestate" },
                { .from_node_key = "probe_1", .output_key = "seed_probe_run", .to_node_key = "battle_1", .input_key = "seed_probe_run" },
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
    ASSERT_EQ(graph->edges.size(), 2u);
    EXPECT_EQ(graph->nodes[0].inputs[0].data_kind, "state_artifact.dtm_artifact_id");
    EXPECT_EQ(graph->nodes[1].possible_outputs[0].data_kind, "analysis.seed_probe_run");
    EXPECT_EQ(graph->nodes[2].authored_ref_kind.value_or(""), "authoring.battle_plan");
    EXPECT_EQ(graph->nodes[2].authored_ref_id.value_or(0), 77);
    EXPECT_EQ(graph->edges[1].from_node_key, "probe_1");

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

    SaveWorkflowGraphResult revised{};
    ASSERT_TRUE(authoring_db->SaveWorkflowGraph(
        {
            .workflow_graph_id = saved.workflow_graph_id,
            .parent_revision_id = saved.workflow_graph_revision_id,
            .name = "canonical-start-to-battle",
            .description = "TAS -> seed probe -> battle",
            .graph_version = 2,
            .graph_hash = "graph-hash-canonical-start-to-battle-v2",
            .nodes = {
                {
                    .node_key = "tas_1",
                    .unit_kind = "tas_movie",
                    .display_name = "TAS Movie",
                    .inputs = {
                        { .input_key = "dtm_artifact", .data_kind = "state_artifact.dtm_artifact_id", .ref_kind = "state_artifact", .display_name = "DTM artifact" },
                    },
                    .possible_outputs = {
                        { .output_key = "savestate", .data_kind = "state.savestate_id", .ref_kind = "state.savestate", .display_name = "Output savestate" },
                    },
                },
                {
                    .node_key = "probe_1",
                    .unit_kind = "seed_probe",
                    .display_name = "Seed Probe",
                    .inputs = {
                        { .input_key = "entry_savestate", .data_kind = "state.savestate_id", .ref_kind = "state.savestate", .display_name = "Entry savestate" },
                    },
                    .possible_outputs = {
                        { .output_key = "seed_probe_run", .data_kind = "analysis.seed_probe_run", .ref_kind = "sp_probe_run", .display_name = "SeedProbe run" },
                    },
                },
                {
                    .node_key = "battle_1",
                    .unit_kind = "battle",
                    .display_name = "Battle",
                    .authored_ref_kind = std::string("authoring.battle_plan"),
                    .authored_ref_id = 88,
                    .inputs = {
                        { .input_key = "seed_probe_run", .data_kind = "analysis.seed_probe_run", .ref_kind = "sp_probe_run", .display_name = "Confirmed SeedProbe run" },
                        { .input_key = "battle_context", .data_kind = "analysis_battle.battle_context_id", .ref_kind = "ab_battle_context", .display_name = "Battle context" },
                    },
                    .possible_outputs = {
                        { .output_key = "battle_set", .data_kind = "analysis_battle.battle_set", .ref_kind = "analysis_battle.battle_set", .display_name = "Battle set" },
                    },
                },
            },
            .edges = {
                { .from_node_key = "tas_1", .output_key = "savestate", .to_node_key = "probe_1", .input_key = "entry_savestate" },
                { .from_node_key = "probe_1", .output_key = "seed_probe_run", .to_node_key = "battle_1", .input_key = "seed_probe_run" },
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

    ASSERT_TRUE(authoring_db->SetWorkflowGraphHidden(saved.workflow_graph_id, true, &err)) << err;
    const auto active_graph_by_name = authoring_db->GetWorkflowGraphByName(active_graph->name);
    ASSERT_TRUE(active_graph_by_name.has_value());
    EXPECT_TRUE(active_graph_by_name->hidden);
    EXPECT_EQ(active_graph_by_name->workflow_graph_revision_id, revised.workflow_graph_revision_id);

    const auto first_revision = authoring_db->GetWorkflowGraphRevision(saved.workflow_graph_revision_id);
    ASSERT_TRUE(first_revision.has_value());
    EXPECT_EQ(first_revision->workflow_graph_id, saved.workflow_graph_id);
    EXPECT_EQ(first_revision->workflow_graph_revision_id, saved.workflow_graph_revision_id);
    EXPECT_EQ(first_revision->nodes[2].authored_ref_id.value_or(0), 77);

    sqlite3_stmt* status_column = nullptr;
    ASSERT_EQ(SQLITE_OK, sqlite3_prepare_v2(
        db_,
        "SELECT COUNT(1) FROM pragma_table_info('au_workflow_graph_revision') WHERE name='status';",
        -1,
        &status_column,
        nullptr));
    ASSERT_EQ(SQLITE_ROW, sqlite3_step(status_column));
    EXPECT_EQ(sqlite3_column_int64(status_column, 0), 0);
    sqlite3_finalize(status_column);

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
            .description = "Seed probe can feed a battle",
            .graph_version = 1,
            .graph_hash = "graph-hash-launchable-seedprobe-to-battle",
            .nodes = {
                {
                    .node_key = "probe_1",
                    .unit_kind = "seed_probe",
                    .display_name = "Seed Probe",
                    .inputs = {
                        { .input_key = "entry_savestate", .data_kind = "state.savestate_id", .ref_kind = "state.savestate", .display_name = "Entry savestate" },
                    },
                    .possible_outputs = {
                        { .output_key = "seed_probe_run", .data_kind = "analysis.seed_probe_run", .ref_kind = "sp_probe_run", .display_name = "SeedProbe run" },
                    },
                },
                {
                    .node_key = "battle_1",
                    .unit_kind = "battle",
                    .display_name = "Battle",
                    .authored_ref_kind = std::string("authoring.battle_plan"),
                    .authored_ref_id = 901,
                    .inputs = {
                        { .input_key = "seed_probe_run", .data_kind = "analysis.seed_probe_run", .ref_kind = "sp_probe_run", .display_name = "Confirmed SeedProbe run" },
                    },
                    .possible_outputs = {
                        { .output_key = "battle_set", .data_kind = "analysis_battle.battle_set", .ref_kind = "analysis_battle.battle_set", .display_name = "Battle set" },
                    },
                },
            },
            .edges = {
                { .from_node_key = "probe_1", .output_key = "seed_probe_run", .to_node_key = "battle_1", .input_key = "seed_probe_run" },
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
    command.unit_activations.push_back(TestUnitActivation("probe_1", "seed_probe", "Seed Probe", {}, 10, 1));
    command.unit_activations.push_back(TestUnitActivation("battle_1", "battle", "Battle", { "probe_1" }, 5, 1));
    command.input_bindings.push_back({
        .node_key = "probe_1",
        .input_key = "entry_savestate",
        .data_kind = "state.savestate_id",
        .ref_kind = "state.savestate",
        .ref_id = 44001,
        .source_kind = "external",
    });
    command.arguments.push_back({ .node_key = "probe_1", .argument_key = "samples_per_axis", .value_type = "integer", .integer_value = 4, .source_kind = "launcher" });
    command.arguments.push_back({ .node_key = "battle_1", .argument_key = "continuation_mode", .value_type = "choice", .text_value = std::string("automatic_best_per_ending_rng"), .source_kind = "launcher" });
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
    ASSERT_EQ(graph->arguments.size(), 4u);
    EXPECT_EQ(graph->arguments[0].workflow_instance_id, workflow_instance_id);
    EXPECT_EQ(graph->arguments[0].node_key, "probe_1");
    EXPECT_EQ(graph->arguments[0].argument_key, "samples_per_axis");
    EXPECT_EQ(graph->arguments[0].value_type, "integer");
    ASSERT_TRUE(graph->arguments[0].integer_value.has_value());
    EXPECT_EQ(*graph->arguments[0].integer_value, 4);
    EXPECT_FALSE(graph->arguments[0].text_value.has_value());
    EXPECT_EQ(graph->arguments[0].source_kind, "launcher");
    EXPECT_EQ(graph->arguments[1].workflow_instance_id, workflow_instance_id);
    EXPECT_EQ(graph->arguments[1].node_key, "battle_1");
    EXPECT_EQ(graph->arguments[1].argument_key, "continuation_mode");
    EXPECT_EQ(graph->arguments[1].value_type, "choice");
    EXPECT_EQ(graph->arguments[1].text_value.value_or(""), "automatic_best_per_ending_rng");
    EXPECT_EQ(graph->arguments[1].source_kind, "launcher");
    EXPECT_EQ(graph->arguments[2].workflow_instance_id, workflow_instance_id);
    EXPECT_EQ(graph->arguments[2].node_key, "battle_1");
    EXPECT_EQ(graph->arguments[2].argument_key, "fake_attack_min");
    EXPECT_EQ(graph->arguments[2].value_type, "integer");
    ASSERT_TRUE(graph->arguments[2].integer_value.has_value());
    EXPECT_EQ(*graph->arguments[2].integer_value, 22);
    EXPECT_EQ(graph->arguments[2].source_kind, "launcher");
    EXPECT_EQ(graph->arguments[3].argument_key, "fake_attack_max");
    EXPECT_EQ(graph->arguments[3].value_type, "integer");
    ASSERT_TRUE(graph->arguments[3].integer_value.has_value());
    EXPECT_EQ(*graph->arguments[3].integer_value, 25);
    EXPECT_EQ(graph->arguments[3].source_kind, "launcher");

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
    rejected.unit_activations.push_back(TestUnitActivation("probe_1", "seed_probe", "Seed Probe"));
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
    legacy.unit_activations.push_back(TestUnitActivation("probe_1", "seed_probe", "Seed Probe"));
    EXPECT_FALSE(execution_db->WorkflowCommandService()->CreateWorkflowInstance(legacy, &rejected_instance_id, &err));
    EXPECT_NE(err.find("workflow_kind must be workflow_graph"), std::string::npos);
}

TEST_F(SqliteDbFixture, Stage5GraphRoutingRoutesValidateRootToSterilizeAndClassifiesContractFailures) {
    using namespace savor::db;
    using namespace savor::db::execution::workflow;

    auto* authoring_db = db_service_->AuthoringDb();
    auto* execution_db = db_service_->ExecutionDb();
    ASSERT_NE(authoring_db, nullptr);
    ASSERT_NE(execution_db, nullptr);
    std::string err;
    savor::db::SaveWorkflowGraphResult saved{};
    const savor::db::types::UtcTimePoint now{
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())};
    ASSERT_TRUE(authoring_db->SaveWorkflowGraph(
        {
            .name = "route-validation-sterilize",
            .description = "Validated checkpoint feeds sterilization",
            .graph_version = 1,
            .graph_hash = "graph-hash-route-validation-sterilize",
            .nodes = {
                {
                    .node_key = "validate_1",
                    .unit_kind = "tas_movie_validate_root",
                    .display_name = "Validate Root",
                    .possible_outputs = {
                        { .output_key = "tas_movie_validation_attempt", .data_kind = "analysis.tas_movie_validation_attempt_id", .ref_kind = "tmv_validation_attempt", .display_name = "Validation attempt" },
                        { .output_key = "validated_checkpoint_savestate", .data_kind = "state.movie_paired_savestate_id", .ref_kind = "state.savestate", .display_name = "Validated checkpoint" },
                    },
                },
                {
                    .node_key = "sterilize_1",
                    .unit_kind = "tas_movie_checkpoint_sterilize",
                    .display_name = "Sterilize Checkpoint",
                    .inputs = {
                        { .input_key = "paired_checkpoint_savestate", .data_kind = "state.movie_paired_savestate_id", .ref_kind = "state.savestate", .display_name = "Paired checkpoint" },
                    },
                    .possible_outputs = {
                        { .output_key = "sterilized_checkpoint_savestate", .data_kind = "state.movie_inactive_savestate_id", .ref_kind = "state.savestate", .display_name = "Sterilized checkpoint" },
                    },
                },
            },
            .edges = {
                { .from_node_key = "validate_1", .output_key = "validated_checkpoint_savestate", .to_node_key = "sterilize_1", .input_key = "paired_checkpoint_savestate" },
            },
            .created_at_utc = now,
            .correlation_id = "route-validation-sterilize",
        },
        &saved,
        &err)) << err;

    auto validation = TestUnitActivation(
        "validate_1", "tas_movie_validate_root", "Validate Root", {}, 10, 1);
    validation.steps.front().step_kind = "tasmovie.validate_root";
    auto sterilize = TestUnitActivation(
        "sterilize_1", "tas_movie_checkpoint_sterilize", "Sterilize", {"validate_1"}, 5, 1);
    sterilize.steps.front().step_kind = "tasmovie.checkpoint_sterilize";
    WorkflowCreateInstanceCommand create{};
    create.workflow_kind = "workflow_graph";
    create.root_scope_kind = "manual";
    create.workflow_graph_revision_id = saved.workflow_graph_revision_id;
    create.created_by = "sqlite-fixture";
    create.created_at_utc = now.time_since_epoch().count();
    create.unit_activations = {validation, sterilize};
    std::int64_t workflow_instance_id = 0;
    ASSERT_TRUE(execution_db->WorkflowCommandService()->CreateWorkflowInstance(
        create, &workflow_instance_id, &err)) << err;

    auto graph = execution_db->WorkflowQueryService()->GetWorkflowGraph(workflow_instance_id);
    ASSERT_TRUE(graph.has_value());
    const auto validation_step = std::find_if(
        graph->steps.begin(), graph->steps.end(),
        [](const auto& step) { return step.step_key == "validate_1"; });
    const auto sterilize_step = std::find_if(
        graph->steps.begin(), graph->steps.end(),
        [](const auto& step) { return step.step_key == "sterilize_1"; });
    ASSERT_NE(validation_step, graph->steps.end());
    ASSERT_NE(sterilize_step, graph->steps.end());
    const auto validation_step_id = validation_step->workflow_step_id;
    const auto sterilize_step_id = sterilize_step->workflow_step_id;

    std::int64_t job_set_id = 0;
    ASSERT_TRUE(execution_db->CreateJobSet(
        {.program_kind = 1, .purpose = "validate", .created_by = std::string("test"), .expected_total = 1},
        &job_set_id, &err)) << err;
    ASSERT_TRUE(execution_db->WorkflowCommandService()->MarkStepMaterialized(
        {.workflow_step_id = validation_step_id, .job_set_id = job_set_id, .requested_by = "test"},
        &err)) << err;
    std::int64_t job_id = 0;
    ASSERT_TRUE(execution_db->EnqueueJob(
        {.job_set_id = job_set_id, .program_kind = 1, .program_version = 1,
         .program_ref_kind = "tmv_root_establishment_attempt", .program_ref_id = 1,
         .fingerprint = "validate-route", .priority = 0, .max_attempts = 1},
        &job_id, &err)) << err;
    ASSERT_TRUE(execution_db->JobCommandService()->AppendLifecycleEvent(
        {.kind = execution::jobs::JobLifecycleEventKind::JobCompleted,
         .job_id = job_id, .terminal_state = std::string("SUCCEEDED")},
        &err)) << err;
    ASSERT_TRUE(execution_db->RecordJobOutput(
        {.job_id = job_id, .output_key = "tas_movie_validation_attempt",
         .data_kind = "analysis.tas_movie_validation_attempt_id",
         .ref_kind = "tmv_validation_attempt", .ref_id = 71,
         .requested_by = "worker_result_drain"}, &err)) << err;
    ASSERT_TRUE(execution_db->RecordJobOutput(
        {.job_id = job_id, .output_key = "validated_checkpoint_savestate",
         .data_kind = "state.movie_paired_savestate_id",
         .ref_kind = "state.savestate", .ref_id = 72,
         .requested_by = "worker_result_drain"}, &err)) << err;
    ASSERT_TRUE(execution_db->WorkflowCommandService()->CompleteWorkflowStep(
        {.workflow_step_id = validation_step_id, .completion_state = "COMPLETED", .requested_by = "test"},
        &err)) << err;

    WorkflowGraphRoutingService router(
        execution_db, authoring_db, execution_db->WorkflowQueryService(),
        execution_db->WorkflowCommandService());
    WorkflowGraphRoutingResult route_result{};
    WorkflowStepSettlementSnapshot terminal{
        .workflow_instance_id = workflow_instance_id,
        .workflow_step_id = validation_step_id,
        .job_set_id = job_set_id,
        .workflow_kind = "workflow_graph",
        .workflow_graph_revision_id = saved.workflow_graph_revision_id,
        .step_key = "validate_1",
        .graph_node_key = "validate_1",
        .step_kind = "tasmovie.validate_root",
        .priority = 10,
        .expected_total = 1,
        .discovered_total = 1,
        .settled_total = 1,
        .succeeded_total = 1,
    };
    ASSERT_TRUE(router.RouteTerminalStep(terminal, &route_result, &err)) << err;
    EXPECT_TRUE(route_result.routed_input_binding);
    EXPECT_TRUE(route_result.advanced_ready_step);
    graph = execution_db->WorkflowQueryService()->GetWorkflowGraph(workflow_instance_id);
    ASSERT_TRUE(graph.has_value());
    const auto updated_sterilize = std::find_if(
        graph->steps.begin(), graph->steps.end(),
        [&](const auto& step) { return step.workflow_step_id == sterilize_step_id; });
    ASSERT_NE(updated_sterilize, graph->steps.end());
    EXPECT_EQ(updated_sterilize->state, WorkflowStepState::Ready);

    ASSERT_TRUE(execution_db->RecordJobOutput(
        {.job_id = job_id, .output_key = "undeclared_output",
         .data_kind = "analysis.invalid", .ref_kind = "invalid.ref",
         .ref_id = 73, .requested_by = "test"}, &err)) << err;
    route_result = {};
    EXPECT_FALSE(router.RouteTerminalStep(terminal, &route_result, &err));
    ASSERT_TRUE(route_result.failure.has_value());
    EXPECT_EQ(route_result.failure->failure_class,
        WorkflowGraphRoutingFailureClass::GraphConstruction);
    EXPECT_EQ(route_result.failure->code,
        "PROGRAM_OUTPUT_OUTSIDE_UNIT_CONTRACT");

    ASSERT_TRUE(execution_db->WorkflowCommandService()->FailWorkflowInstance(
        {.workflow_instance_id = workflow_instance_id,
         .workflow_step_id = std::nullopt,
         .failure_code = "WORKFLOW_GRAPH_CONSTRUCTION_FAILED",
         .failure_message = route_result.failure->message,
         .requested_by = "workflow_coordinator_graph_construction"},
        &err)) << err;
    graph = execution_db->WorkflowQueryService()->GetWorkflowGraph(workflow_instance_id);
    ASSERT_TRUE(graph.has_value());
    const auto completed_validation = std::find_if(
        graph->steps.begin(), graph->steps.end(),
        [&](const auto& step) { return step.workflow_step_id == validation_step_id; });
    ASSERT_NE(completed_validation, graph->steps.end());
    EXPECT_EQ(completed_validation->state, WorkflowStepState::Completed);
    const auto failed_workflows = execution_db->WorkflowQueryService()->ListWorkflowInstances(
        WorkflowInstanceState::Failed, 0, (std::numeric_limits<std::int64_t>::max)());
    EXPECT_TRUE(std::any_of(failed_workflows.begin(), failed_workflows.end(),
        [&](const auto& workflow) { return workflow.workflow_instance_id == workflow_instance_id; }));
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
                    .unit_kind = "seed_probe",
                    .display_name = "Seed Probe",
                    .inputs = {
                        { .input_key = "entry_savestate", .data_kind = "state.savestate_id", .display_name = "Entry savestate" },
                    },
                },
            },
            .edges = {
                {
                    .from_node_key = "tas_1",
                    .output_key = "savestate",
                    .to_node_key = "probe_1",
                    .input_key = "entry_savestate",
                    .guard_kind = std::string(
                        kWorkflowOutputPresentGuard),
                },
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
    create.unit_activations.push_back(TestUnitActivation("probe_1", "seed_probe", "Seed Probe", { "tas_1" }, 5, 1));
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
    ASSERT_TRUE(execution_db->WorkflowCommandService()->CompleteWorkflowStep(
        { .workflow_step_id = tas_step_it->workflow_step_id, .completion_state = "COMPLETED", .requested_by = "test" },
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
            .settled_total = 1,
            .succeeded_total = 1,
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

TEST_F(SqliteDbFixture, Stage5GraphRoutingOutputPresentWaitsForAlternateProvider) {
    using namespace savor::db;
    using namespace savor::db::execution::workflow;

    auto* authoring_db = db_service_->AuthoringDb();
    auto* execution_db = db_service_->ExecutionDb();
    ASSERT_NE(authoring_db, nullptr);
    ASSERT_NE(execution_db, nullptr);
    std::string err;
    const auto now = types::UtcTimePoint(
        std::chrono::milliseconds(1712304000791));
    SaveWorkflowGraphResult saved{};
    ASSERT_TRUE(authoring_db->SaveWorkflowGraph(
        {
            .name = "route-output-present-alternate",
            .description = "A missing guarded output waits for an active alternate provider",
            .graph_version = 1,
            .graph_hash = "graph-hash-route-output-present-alternate",
            .nodes = {
                {
                    .node_key = "left_1",
                    .unit_kind = "source",
                    .display_name = "Left",
                    .possible_outputs = {{
                        .output_key = "value",
                        .data_kind = "test.ref",
                        .display_name = "Value",
                    }},
                },
                {
                    .node_key = "right_1",
                    .unit_kind = "source",
                    .display_name = "Right",
                    .possible_outputs = {{
                        .output_key = "value",
                        .data_kind = "test.ref",
                        .display_name = "Value",
                    }},
                },
                {
                    .node_key = "target_1",
                    .unit_kind = "target",
                    .display_name = "Target",
                    .inputs = {{
                        .input_key = "input",
                        .data_kind = "test.ref",
                        .display_name = "Input",
                    }},
                },
            },
            .edges = {
                {
                    .from_node_key = "left_1",
                    .output_key = "value",
                    .to_node_key = "target_1",
                    .input_key = "input",
                    .guard_kind = std::string(
                        kWorkflowOutputPresentGuard),
                },
                {
                    .from_node_key = "right_1",
                    .output_key = "value",
                    .to_node_key = "target_1",
                    .input_key = "input",
                    .guard_kind = std::string(
                        kWorkflowOutputPresentGuard),
                },
            },
            .created_at_utc = now,
            .correlation_id = "au-output-present-alternate",
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
    create.unit_activations.push_back(
        TestUnitActivation("left_1", "source", "Left", {}, 10, 1));
    create.unit_activations.push_back(
        TestUnitActivation("right_1", "source", "Right", {}, 10, 1));
    create.unit_activations.push_back(TestUnitActivation(
        "target_1", "target", "Target",
        {"left_1", "right_1"}, 1, 1));
    ASSERT_TRUE(execution_db->WorkflowCommandService()
        ->CreateWorkflowInstance(
            create, &workflow_instance_id, &err)) << err;

    auto find_step = [&](std::string_view node_key) {
        const auto graph = execution_db->WorkflowQueryService()
            ->GetWorkflowGraph(workflow_instance_id);
        EXPECT_TRUE(graph.has_value());
        if (!graph) return WorkflowStepRecord{};
        const auto step = std::find_if(
            graph->steps.begin(), graph->steps.end(),
            [&](const auto& candidate) {
                return candidate.graph_node_key == node_key;
            });
        EXPECT_NE(step, graph->steps.end());
        return step == graph->steps.end()
            ? WorkflowStepRecord{}
            : *step;
    };
    auto finish_source = [&](std::string_view node_key,
                             std::optional<std::int64_t> output_ref) {
        const auto step = find_step(node_key);
        std::int64_t job_set_id = 0;
        EXPECT_TRUE(execution_db->CreateJobSet(
            {
                .program_kind = 10,
                .purpose = "Guard source",
                .created_by = std::string("test"),
                .expected_total = 1,
            },
            &job_set_id,
            &err)) << err;
        EXPECT_TRUE(execution_db->WorkflowCommandService()
            ->MarkStepMaterialized(
                {
                    .workflow_step_id = step.workflow_step_id,
                    .job_set_id = job_set_id,
                    .requested_by = "test",
                },
                &err)) << err;
        std::int64_t job_id = 0;
        EXPECT_TRUE(execution_db->EnqueueJob(
            {
                .job_set_id = job_set_id,
                .program_kind = 10,
                .program_version = 1,
                .program_ref_kind = "test",
                .program_ref_id = step.workflow_step_id,
                .fingerprint = std::string(node_key) + "-job",
                .priority = 0,
                .max_attempts = 1,
            },
            &job_id,
            &err)) << err;
        EXPECT_TRUE(execution_db->JobCommandService()
            ->AppendLifecycleEvent(
                {
                    .kind = execution::jobs::JobLifecycleEventKind::JobCompleted,
                    .job_id = job_id,
                    .terminal_state = std::string("SUCCEEDED"),
                },
                &err)) << err;
        if (output_ref) {
            EXPECT_TRUE(execution_db->RecordJobOutput(
                {
                    .job_id = job_id,
                    .output_key = "value",
                    .data_kind = "test.ref",
                    .ref_kind = "test.ref",
                    .ref_id = *output_ref,
                    .requested_by = "test",
                },
                &err)) << err;
        }
        EXPECT_TRUE(execution_db->WorkflowCommandService()->CompleteWorkflowStep(
            {
                .workflow_step_id = step.workflow_step_id,
                .completion_state = "COMPLETED",
                .requested_by = "test",
            },
            &err)) << err;
        return WorkflowStepSettlementSnapshot{
            .workflow_instance_id = workflow_instance_id,
            .workflow_step_id = step.workflow_step_id,
            .job_set_id = job_set_id,
            .workflow_kind = "workflow_graph",
            .workflow_graph_revision_id = saved.workflow_graph_revision_id,
            .step_key = std::string(node_key),
            .graph_node_key = std::string(node_key),
            .step_kind = "source",
            .priority = 10,
            .expected_total = 1,
            .discovered_total = 1,
            .settled_total = 1,
            .succeeded_total = 1,
            .failed_total = 0,
        };
    };

    WorkflowGraphRoutingService router(
        execution_db, authoring_db,
        execution_db->WorkflowQueryService(),
        execution_db->WorkflowCommandService());
    auto terminal = finish_source("left_1", std::nullopt);
    WorkflowGraphRoutingResult result{};
    ASSERT_TRUE(router.RouteTerminalStep(terminal, &result, &err)) << err;
    EXPECT_FALSE(result.routed_input_binding);
    EXPECT_EQ(result.skipped_step_count, 0);
    EXPECT_EQ(find_step("target_1").state, WorkflowStepState::Waiting);

    terminal = finish_source("right_1", 88001);
    result = {};
    ASSERT_TRUE(router.RouteTerminalStep(terminal, &result, &err)) << err;
    EXPECT_TRUE(result.routed_input_binding);
    EXPECT_TRUE(result.advanced_ready_step);
    EXPECT_EQ(find_step("target_1").state, WorkflowStepState::Ready);
}

TEST_F(SqliteDbFixture, Stage5GraphRoutingOutputPresentSkipsImpossibleDescendants) {
    using namespace savor::db;
    using namespace savor::db::execution::workflow;

    auto* authoring_db = db_service_->AuthoringDb();
    auto* execution_db = db_service_->ExecutionDb();
    ASSERT_NE(authoring_db, nullptr);
    ASSERT_NE(execution_db, nullptr);
    std::string err;
    const auto now = types::UtcTimePoint(
        std::chrono::milliseconds(1712304000791));
    SaveWorkflowGraphResult saved{};
    ASSERT_TRUE(authoring_db->SaveWorkflowGraph(
        {
            .name = "route-output-present-skip",
            .description = "Missing guarded output skips the impossible branch",
            .graph_version = 1,
            .graph_hash = "graph-hash-route-output-present-skip",
            .nodes = {
                {
                    .node_key = "root_1",
                    .unit_kind = "root",
                    .display_name = "Root",
                    .possible_outputs = {{
                        .output_key = "next",
                        .data_kind = "test.ref",
                        .display_name = "Next",
                    }},
                },
                {
                    .node_key = "middle_1",
                    .unit_kind = "middle",
                    .display_name = "Middle",
                    .inputs = {{
                        .input_key = "input",
                        .data_kind = "test.ref",
                        .display_name = "Input",
                    }},
                    .possible_outputs = {{
                        .output_key = "next",
                        .data_kind = "test.ref",
                        .display_name = "Next",
                    }},
                },
                {
                    .node_key = "leaf_1",
                    .unit_kind = "leaf",
                    .display_name = "Leaf",
                    .inputs = {{
                        .input_key = "input",
                        .data_kind = "test.ref",
                        .display_name = "Input",
                    }},
                },
            },
            .edges = {
                {
                    .from_node_key = "root_1",
                    .output_key = "next",
                    .to_node_key = "middle_1",
                    .input_key = "input",
                    .guard_kind = std::string(
                        kWorkflowOutputPresentGuard),
                },
                {
                    .from_node_key = "middle_1",
                    .output_key = "next",
                    .to_node_key = "leaf_1",
                    .input_key = "input",
                    .guard_kind = std::string(
                        kWorkflowOutputPresentGuard),
                },
            },
            .created_at_utc = now,
            .correlation_id = "au-output-present-skip",
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
    create.unit_activations.push_back(
        TestUnitActivation("root_1", "root", "Root", {}, 10, 1));
    create.unit_activations.push_back(
        TestUnitActivation(
            "middle_1", "middle", "Middle", {"root_1"}, 5, 1));
    create.unit_activations.push_back(
        TestUnitActivation(
            "leaf_1", "leaf", "Leaf", {"middle_1"}, 1, 1));
    ASSERT_TRUE(execution_db->WorkflowCommandService()
        ->CreateWorkflowInstance(
            create, &workflow_instance_id, &err)) << err;

    auto graph = execution_db->WorkflowQueryService()->GetWorkflowGraph(
        workflow_instance_id);
    ASSERT_TRUE(graph.has_value());
    const auto root_step = std::find_if(
        graph->steps.begin(), graph->steps.end(),
        [](const auto& step) { return step.graph_node_key == "root_1"; });
    ASSERT_NE(root_step, graph->steps.end());
    std::int64_t root_job_set_id = 0;
    ASSERT_TRUE(execution_db->CreateJobSet(
        {
            .program_kind = 10,
            .purpose = "Guard root",
            .created_by = std::string("test"),
            .expected_total = 1,
        },
        &root_job_set_id,
        &err)) << err;
    ASSERT_TRUE(execution_db->WorkflowCommandService()
        ->MarkStepMaterialized(
            {
                .workflow_step_id = root_step->workflow_step_id,
                .job_set_id = root_job_set_id,
                .requested_by = "test",
            },
            &err)) << err;
    ASSERT_TRUE(execution_db->WorkflowCommandService()->CompleteWorkflowStep(
        {
            .workflow_step_id = root_step->workflow_step_id,
            .completion_state = "COMPLETED",
            .requested_by = "test",
        },
        &err)) << err;

    WorkflowGraphRoutingService router(
        execution_db, authoring_db,
        execution_db->WorkflowQueryService(),
        execution_db->WorkflowCommandService());
    const WorkflowStepSettlementSnapshot terminal{
        .workflow_instance_id = workflow_instance_id,
        .workflow_step_id = root_step->workflow_step_id,
        .job_set_id = root_job_set_id,
        .workflow_kind = "workflow_graph",
        .workflow_graph_revision_id = saved.workflow_graph_revision_id,
        .step_key = "root_1",
        .graph_node_key = "root_1",
        .step_kind = "root",
        .priority = 10,
        .expected_total = 1,
        .discovered_total = 1,
        .settled_total = 1,
        .succeeded_total = 1,
        .failed_total = 0,
    };
    WorkflowGraphRoutingResult result{};
    ASSERT_TRUE(router.RouteTerminalStep(terminal, &result, &err)) << err;
    EXPECT_EQ(result.skipped_step_count, 2);
    EXPECT_TRUE(result.workflow_completed);

    graph = execution_db->WorkflowQueryService()->GetWorkflowGraph(
        workflow_instance_id);
    ASSERT_TRUE(graph.has_value());
    EXPECT_EQ(graph->instance.state, WorkflowInstanceState::Completed);
    const auto expect_skipped = [&](std::string_view node_key,
                                    std::string_view reason) {
        const auto step = std::find_if(
            graph->steps.begin(), graph->steps.end(),
            [&](const auto& candidate) {
                return candidate.graph_node_key == node_key;
            });
        ASSERT_NE(step, graph->steps.end());
        EXPECT_EQ(step->state, WorkflowStepState::Skipped);
        EXPECT_EQ(step->blocked_reason, std::optional<std::string>(reason));
        const auto activation = std::find_if(
            graph->unit_activations.begin(),
            graph->unit_activations.end(),
            [&](const auto& candidate) {
                return candidate.graph_node_key == node_key;
            });
        ASSERT_NE(activation, graph->unit_activations.end());
        EXPECT_EQ(
            activation->state,
            WorkflowUnitActivationState::Skipped);
    };
    expect_skipped(
        "middle_1", "guard_not_satisfied:root_1.next");
    expect_skipped(
        "leaf_1", "guard_not_satisfied:middle_1.next");

    result = {};
    ASSERT_TRUE(router.RouteTerminalStep(terminal, &result, &err)) << err;
    EXPECT_EQ(result.skipped_step_count, 0);
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
            .source_job_set_id = 9001,
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
        .source_job_set_id = 100,
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
        .source_job_set_id = 100,
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
    const auto dtm_path = temp_root / "entry-full.dtm";
    const auto itinerary_path = temp_root / "entry-full.tmi";
    {
        std::ofstream out(sav_path, std::ios::binary | std::ios::trunc);
        out << "full-entry-savestate";
    }
    {
        std::ofstream out(dtm_path, std::ios::binary | std::ios::trunc);
        out << "full-entry-dtm";
    }
    {
        std::ofstream out(itinerary_path, std::ios::binary | std::ios::trunc);
        out << "full-entry-itinerary";
    }

    const auto sav_sql = std::string("INSERT INTO state_artifact(artifact_id,sha256,size_bytes,compression_kind,filename,file_ext,artifact_kind,created_at_utc) VALUES(60,'fullsha',20,'NONE','")
        + sav_path.generic_string() + "','.sav','SAV',1000);"
        + "INSERT INTO state_artifact(artifact_id,sha256,size_bytes,compression_kind,filename,file_ext,artifact_kind,created_at_utc) VALUES(61,'fulldtmsha',14,'NONE','"
        + dtm_path.generic_string() + "','.dtm','DTM',1000);"
        + "INSERT INTO state_artifact(artifact_id,sha256,size_bytes,compression_kind,filename,file_ext,artifact_kind,created_at_utc) VALUES(62,'fulltmisha',20,'NONE','"
        + itinerary_path.generic_string() + "','.tmi','TAS_MOVIE_ITINERARY',1000);"
        + "INSERT INTO state_savestate(savestate_id,artifact_id,savestate_type,note,is_complete,created_at_utc) VALUES(601,60,'ENTRY','full',1,1000);";
    ASSERT_TRUE(ExecSql(db_, sav_sql.c_str()));
    savor::runtime::battlerecord::BattleReplaySourceBindingV1 archived_binding{
        .source_savestate_id = 601,
        .source_dtm_artifact_id = 61,
        .source_itinerary_artifact_id = 62,
        .source_savestate_sha256 = std::string(64, '1'),
        .source_dtm_sha256 = std::string(64, '2'),
        .source_itinerary_sha256 = std::string(64, '3'),
    };
    archived_binding.canonical_sha256 =
        savor::runtime::battlerecord::ComputeBattleReplaySourceBindingHashV1(
            archived_binding);
    const auto archived_binding_bytes =
        savor::runtime::battlerecord::EncodeBattleReplaySourceBindingV1(
            archived_binding);
    ASSERT_FALSE(archived_binding_bytes.empty());
    const auto sql_hex = [](std::span<const std::uint8_t> bytes) {
        constexpr char digits[] = "0123456789abcdef";
        std::string output;
        output.reserve(bytes.size() * 2);
        for (const auto byte : bytes) {
            output.push_back(digits[byte >> 4]);
            output.push_back(digits[byte & 0x0f]);
        }
        return output;
    };
    ASSERT_TRUE(ExecSql(db_, R"SQL(
INSERT INTO exec_job_set(
    job_set_id,program_kind,purpose,created_by,created_at_utc,priority_boost,
    expected_total,domain_ref_kind,domain_ref_id,meta_note,
    materialization_key,materialization_state,population_sealed_at_utc,
    workset_publication_completed_at_utc)
VALUES(
    7100,7,'rehydrate full','test',1000,0,
    1,'analysis_battle.turn_job',8300,'full',
    'fixture.archive.battle-turn','WORKSET_PUBLICATION_COMPLETE',1000,1000);
INSERT INTO exec_job(job_id,job_set_id,program_kind,program_version,program_ref_kind,program_ref_id,fingerprint,priority,state,attempts,max_attempts,queued_at_utc,ended_at_utc)
VALUES(7101,7100,7,1,'analysis_battle.turn_job',8300,'rehydrate-full-job',0,'SUCCEEDED',1,1,1000,2000);
INSERT INTO exec_workflow_instance(workflow_instance_id,workflow_kind,state,root_scope_kind,root_scope_id,created_by,created_at_utc,completed_at_utc)
VALUES(7001,'BATTLE_RUN','COMPLETED','job_set',7100,'test',1000,2000);
INSERT INTO exec_workflow_step(workflow_step_id,workflow_instance_id,workflow_unit_activation_id,step_key,graph_node_key,step_kind,state,priority,attempts,max_attempts,job_set_id,input_ref_kind,input_ref_id,output_ref_kind,output_ref_id,created_at_utc,completed_at_utc)
VALUES(7201,7001,NULL,'battle','battle','battle.single_turn','COMPLETED',0,1,1,7100,'state.savestate_id',601,'state.savestate_id',601,1000,2000);
INSERT INTO exec_workset(
    workset_id,job_set_id,workflow_step_id,root_job_set_id,
    workset_key,program_kind,program_version,
    contract_key,module_canonical_id,module_version,module_sha256,
    entrypoint,verified_dependency_sha256,runtime_profile_sha256,
    program_package_sha256,execution_affinity_key,
    estimated_payload_bytes,priority,item_count,published_at_utc,
    capture_binding_payload,capture_binding_sha256,
    progress_plan_payload,progress_plan_sha256)
VALUES(
    7110,7100,7201,7100,
    'fixture.archive.battle-turn.workset',7,1,
    'fixture-contract','soa.battle.single_turn',1,
    'aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa',
    'execute',
    'bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb',
    'cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc',
    'dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd',
    NULL,128,0,1,1000,
    X'010203',
    '5555555555555555555555555555555555555555555555555555555555555555',
    X'040506',
    '6666666666666666666666666666666666666666666666666666666666666666');
UPDATE exec_job
SET workset_id=7110,workset_item_ordinal=0
WHERE job_id=7101;
INSERT INTO exec_workflow_step_output(workflow_step_output_id,workflow_instance_id,workflow_step_id,graph_node_key,output_key,data_kind,ref_kind,ref_id,created_at_utc)
VALUES(7202,7001,7201,'battle','output_savestate','state.savestate_id','state.savestate_id',601,2000);
INSERT INTO exec_workflow_instance_input_binding(workflow_instance_input_binding_id,workflow_instance_id,workflow_graph_revision_id,node_key,input_key,data_kind,ref_kind,ref_id,source_kind,created_at_utc)
VALUES(7203,7001,1,'battle','entry','state.savestate_id','state.savestate_id',601,'manual',1000);
INSERT INTO exec_workflow_instance_argument(workflow_instance_argument_id,workflow_instance_id,node_key,argument_key,value_type,integer_value,text_value,source_kind,created_at_utc)
VALUES(7204,7001,'battle','turn_index','integer',1,NULL,'manual',1000);
INSERT INTO ui_workflow_instance(workflow_instance_id,workflow_kind,state,display_state,root_scope_kind,created_by,created_at_utc,completed_at_utc,battle_final_victory_count)
VALUES(7001,'BATTLE_RUN','COMPLETED','COMPLETED','job_set','test',1000,2000,0);
INSERT INTO ab_battle_set(
    battle_set_id,name,entry_savestate_id,battle_plan_id,
    battle_plan_fingerprint,continuation_mode,
    launch_fake_attack_min,launch_fake_attack_max,
    status,created_at_utc,completed_at_utc)
VALUES(8000,'rehydrate battle',601,1,
       'rehydrate-battle-plan-v1','automatic_best_per_ending_rng',
       0,0,'COMPLETED',1000,2000);
INSERT INTO ab_seed_candidate(seed_candidate_id,battle_set_id,seed_value,source_kind,candidate_status,created_at_utc)
VALUES(8100,8000,123,'MANUAL','SELECTED',1000);
INSERT INTO ab_battle_advancement_pool(battle_advancement_pool_id,battle_set_id,turn_index,pool_name,criterion_kind,created_at_utc)
VALUES(8200,8000,1,'pool','best',1000);
INSERT INTO ab_turn_wave(wave_id,battle_set_id,turn_index,context_probe_id,parent_wave_id,parent_turn_job_id,seed_candidate_id,battle_advancement_pool_id,status,created_at_utc,completed_at_utc)
VALUES(8250,8000,1,NULL,NULL,NULL,8100,8200,'COMPLETED',1000,2000);
INSERT INTO ab_predicate_execution_package_v1(
    predicate_execution_package_id,wave_id,predicate_group_revision_id,
    predicate_group_sha256,execution_package_sha256,execution_package_blob,
    phase_program_kind,phase_program_version,phase_canonical_id,phase_revision,
    phase_sha256,hook_contract_canonical_id,hook_contract_revision,
    hook_contract_sha256,created_at_utc)
VALUES(
    8260,8250,777,
    '1111111111111111111111111111111111111111111111111111111111111111',
    '2222222222222222222222222222222222222222222222222222222222222222',
    X'50504531',7,1,'soa.battle.single_turn',1,
    '3333333333333333333333333333333333333333333333333333333333333333',
    'soa.battle.predicate_hooks',2,
    '4444444444444444444444444444444444444444444444444444444444444444',1000);
INSERT INTO ab_battle_context_probe(context_probe_id,wave_id,source_savestate_id,exec_job_id,probe_status,context_blob,context_version,recorded_at_utc,created_at_utc)
VALUES(8275,8250,601,7101,'COMPLETED','{}',1,1500,1000);
UPDATE ab_turn_wave SET context_probe_id=8275 WHERE wave_id=8250;
INSERT INTO ab_turn_job(turn_job_id,wave_id,exec_job_id,plan_id,fake_attacks_this_turn,fake_attacks_used_before,job_state,started_at_utc,ended_at_utc,has_results,output_savestate_id,recorded_at_utc,source_savestate_id,seed_candidate_id,authored_plan_id,authored_turn_index)
VALUES(8300,8250,7101,1,0,0,'SUCCEEDED',1000,2000,1,601,2000,601,8100,1,1);
INSERT INTO ab_battle_advancement_decision(battle_advancement_decision_id,battle_advancement_pool_id,turn_job_id,decision_kind,decision_reason,created_at_utc)
VALUES(8400,8200,8300,'SELECTED','best',2000);
INSERT INTO ab_manual_followup(manual_followup_id,turn_job_id,manual_followup_status,recorded_sav_artifact_id,note,updated_at_utc)
VALUES(8500,8300,'UNREVIEWED',NULL,'restore me',2000);
INSERT INTO ab_battle_completion(
    battle_completion_id,workflow_instance_id,workflow_step_id,exec_job_id,
    battle_set_id,wave_id,selected_turn_job_id,selected_execution_job_id,
    entry_savestate_id,completion_savestate_id,manifest_version,manifest_blob,
    manifest_sha256,route_kind,transition_filename,worker_terminal_sha256,
    status,created_at_utc,completed_at_utc)
VALUES(
    8600,7001,7201,7101,8000,8250,8300,7101,601,601,1,X'42434D31',
    'aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa',
    'FIELD_NAVIGATION','me001a.sct',
    'bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb',
    'COMPLETED',1000,2000);
)SQL"));
    const std::string replay_sql = std::string(R"SQL(
INSERT INTO ab_battle_replay(
    battle_replay_id,battle_completion_id,workflow_instance_id,
    workflow_step_id,exec_job_id,source_savestate_id,source_dtm_artifact_id,
    source_itinerary_artifact_id,source_binding_version,source_binding_blob,
    source_binding_sha256,replay_plan_version,replay_plan_blob,
    replay_plan_sha256,outcome,mismatch_turn,expected_rng,observed_rng,
    observed_completion_blob,observed_completion_sha256,
    observed_transition_blob,observed_transition_sha256,
    worker_terminal_sha256,status,created_at_utc,completed_at_utc)
VALUES(
    8700,8600,7001,7201,7101,601,61,62,1,X')SQL") +
        sql_hex(archived_binding_bytes) + "','" +
        archived_binding.canonical_sha256 + R"SQL(',1,X'42525031',
    'cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc',
    'MATCHED',0,0,0,X'42434D31',
    'aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa',
    X'46544331',
    'dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd',
    'eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee',
    'MATCHED',1000,2000);
)SQL";
    ASSERT_TRUE(ExecSql(db_, replay_sql.c_str()));

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
    EXPECT_EQ(ReadInt64(db_, ("SELECT item_count FROM ar_archive_item WHERE archive_package_id="
        + std::to_string(package.archive_package_id)
        + " AND item_kind='analysis_battle_replays';").c_str()), 1);
    EXPECT_EQ(ReadInt64(db_, ("SELECT item_count FROM ar_archive_item WHERE archive_package_id="
        + std::to_string(package.archive_package_id)
        + " AND item_kind='analysis_predicate_execution_packages';").c_str()), 1);

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
    EXPECT_EQ(preview.savestate_zip_entry_count, 3);

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
    const auto new_predicate_package_id = ReadInt64(db_, "SELECT CAST(new_id AS INTEGER) FROM ar_rehydrate_map WHERE entity_kind='analysis_predicate_execution_package' AND old_id='8260' ORDER BY rehydrate_map_id DESC LIMIT 1;");
    const auto new_context_id = ReadInt64(db_, "SELECT CAST(new_id AS INTEGER) FROM ar_rehydrate_map WHERE entity_kind='analysis_battle_context_probe' AND old_id='8275' ORDER BY rehydrate_map_id DESC LIMIT 1;");
    const auto new_turn_job_id = ReadInt64(db_, "SELECT CAST(new_id AS INTEGER) FROM ar_rehydrate_map WHERE entity_kind='analysis_battle_turn_job' AND old_id='8300' ORDER BY rehydrate_map_id DESC LIMIT 1;");
    const auto new_completion_id = ReadInt64(db_, "SELECT CAST(new_id AS INTEGER) FROM ar_rehydrate_map WHERE entity_kind='analysis_battle_completion' AND old_id='8600' ORDER BY rehydrate_map_id DESC LIMIT 1;");
    const auto new_replay_id = ReadInt64(db_, "SELECT CAST(new_id AS INTEGER) FROM ar_rehydrate_map WHERE entity_kind='analysis_battle_replay' AND old_id='8700' ORDER BY rehydrate_map_id DESC LIMIT 1;");
    ASSERT_GT(new_workflow_id, 0);
    ASSERT_GT(new_job_id, 0);
    ASSERT_GT(new_battle_set_id, 0);
    ASSERT_GT(new_wave_id, 0);
    ASSERT_GT(new_predicate_package_id, 0);
    ASSERT_GT(new_context_id, 0);
    ASSERT_GT(new_turn_job_id, 0);
    ASSERT_GT(new_completion_id, 0);
    ASSERT_GT(new_replay_id, 0);

    EXPECT_EQ(ReadInt64(db_, ("SELECT COUNT(1) FROM exec_workflow_step_output WHERE workflow_instance_id=" + std::to_string(new_workflow_id) + " AND ref_id=601;").c_str()), 1);
    EXPECT_EQ(ReadInt64(db_, ("SELECT COUNT(1) FROM exec_workflow_instance_input_binding WHERE workflow_instance_id=" + std::to_string(new_workflow_id) + " AND ref_id=601;").c_str()), 1);
    EXPECT_EQ(ReadInt64(db_, ("SELECT COUNT(1) FROM exec_workflow_instance_argument WHERE workflow_instance_id=" + std::to_string(new_workflow_id) + " AND argument_key='turn_index' AND integer_value=1;").c_str()), 1);
    EXPECT_EQ(ReadInt64(db_, ("SELECT entry_savestate_id FROM ab_battle_set WHERE battle_set_id=" + std::to_string(new_battle_set_id) + ";").c_str()), 601);
    EXPECT_EQ(ReadInt64(db_, ("SELECT exec_job_id FROM ab_turn_job WHERE turn_job_id=" + std::to_string(new_turn_job_id) + ";").c_str()), new_job_id);
    EXPECT_EQ(ReadInt64(db_, ("SELECT context_probe_id FROM ab_turn_wave WHERE wave_id=" + std::to_string(new_wave_id) + ";").c_str()), new_context_id);
    EXPECT_EQ(ReadInt64(db_, ("SELECT wave_id FROM ab_predicate_execution_package_v1 WHERE predicate_execution_package_id=" + std::to_string(new_predicate_package_id) + ";").c_str()), new_wave_id);
    EXPECT_EQ(ReadInt64(db_, ("SELECT predicate_group_revision_id FROM ab_predicate_execution_package_v1 WHERE predicate_execution_package_id=" + std::to_string(new_predicate_package_id) + ";").c_str()), 777);
    EXPECT_EQ(ReadText(db_, ("SELECT lower(hex(execution_package_blob)) FROM ab_predicate_execution_package_v1 WHERE predicate_execution_package_id=" + std::to_string(new_predicate_package_id) + ";").c_str()), "50504531");
    EXPECT_EQ(ReadText(db_, ("SELECT note FROM ab_manual_followup WHERE turn_job_id=" + std::to_string(new_turn_job_id) + ";").c_str()), "restore me");
    EXPECT_EQ(ReadInt64(db_, ("SELECT battle_completion_id FROM ab_battle_replay WHERE battle_replay_id=" + std::to_string(new_replay_id) + ";").c_str()), new_completion_id);
    EXPECT_EQ(ReadInt64(db_, ("SELECT workflow_instance_id FROM ab_battle_replay WHERE battle_replay_id=" + std::to_string(new_replay_id) + ";").c_str()), new_workflow_id);
    EXPECT_EQ(ReadInt64(db_, ("SELECT exec_job_id FROM ab_battle_replay WHERE battle_replay_id=" + std::to_string(new_replay_id) + ";").c_str()), new_job_id);
    EXPECT_EQ(ReadText(db_, ("SELECT lower(hex(replay_plan_blob)) FROM ab_battle_replay WHERE battle_replay_id=" + std::to_string(new_replay_id) + ";").c_str()), "42525031");
    EXPECT_EQ(ReadText(db_, ("SELECT lower(hex(observed_completion_blob)) FROM ab_battle_replay WHERE battle_replay_id=" + std::to_string(new_replay_id) + ";").c_str()), "42434d31");
    EXPECT_EQ(ReadText(db_, ("SELECT lower(hex(observed_transition_blob)) FROM ab_battle_replay WHERE battle_replay_id=" + std::to_string(new_replay_id) + ";").c_str()), "46544331");
    EXPECT_EQ(
        ReadText(
            db_,
            ("SELECT w.derived_state_binding_sha256 "
             "FROM exec_workset w JOIN exec_job j ON j.workset_id=w.workset_id "
             "WHERE j.job_id=" + std::to_string(new_job_id) + ";").c_str()),
        "4bf4d7c8b3d9d28238f46029b93c4649ec76fdfdd387a49565b630560ab8240f");
    EXPECT_EQ(
        ReadText(
            db_,
            ("SELECT lower(hex(w.derived_state_binding_payload)) "
             "FROM exec_workset w JOIN exec_job j ON j.workset_id=w.workset_id "
             "WHERE j.job_id=" + std::to_string(new_job_id) + ";").c_str()),
        "0100000001000000000000004000000034626634643763386233643964323832333866343630323962393363343634396563373666646664643338376134393536356236333035363061623832343066");
    EXPECT_EQ(
        ReadText(
            db_,
            ("SELECT lower(hex(w.capture_binding_payload)) "
             "FROM exec_workset w JOIN exec_job j ON j.workset_id=w.workset_id "
             "WHERE j.job_id=" + std::to_string(new_job_id) + ";").c_str()),
        "010203");
    EXPECT_EQ(
        ReadText(
            db_,
            ("SELECT lower(hex(w.progress_plan_payload)) "
             "FROM exec_workset w JOIN exec_job j ON j.workset_id=w.workset_id "
             "WHERE j.job_id=" + std::to_string(new_job_id) + ";").c_str()),
        "040506");

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
VALUES(9001,'seed-input-hash','sp_probe_run',9010,1000);
INSERT INTO sp_axis_xy(axis_xy_id,x,y) VALUES(9002,128,128),(9003,0,0);
INSERT INTO sp_input_frame(input_frame_id,main_axis_xy_id,cstick_axis_xy_id,trigger_axis_xy_id)
VALUES(9005,9002,9002,9003);
INSERT INTO an_input_set_frame(input_set_id,ordinal,input_frame_id,added_at_utc)
VALUES(9001,0,9005,1000);
INSERT INTO sp_probe_run(
    probe_run_id,materialization_key,probe_set_id,entry_savestate_id,
    seed_probe_spec_id,launch_samples_per_axis,codec_version,status,
    accepted_input_set_id,requested_at_utc,completed_at_utc,
    established_endpoint,established_endpoint_source_job_id)
VALUES(
    9010,'fixture.archive.seedprobe',9000,701,
    1,1,2,'COMPLETED',9001,1000,2000,
    'AFTER_RAND_SEED_SET',9102);
INSERT INTO sp_probe_result(
    probe_result_id,probe_run_id,input_frame_id,source_job_id,seed_value,
    origin_worker_id,origin_process_generation,origin_workset_epoch,
    terminal_sha256,confirmation_of_probe_result_id,
    evidence_state,recorded_at_utc)
VALUES
    (9020,9010,9005,9102,1000,1,1,1,
     '1111111111111111111111111111111111111111111111111111111111111111',
     NULL,'CONFIRMED',2000),
    (9021,9010,9005,9103,1000,1,1,2,
     '2222222222222222222222222222222222222222222222222222222222222222',
     9020,'OBSERVED',2000);
INSERT INTO sp_encounter_projection(encounter_projection_id,probe_run_id,seed_value,option_ordinal,encounter_id,encounter_frame,movement_required,recorded_at_utc)
VALUES(9060,9010,1000,0,'battle',10,1,2000);
INSERT INTO exec_job_set(
    job_set_id,program_kind,purpose,created_by,created_at_utc,priority_boost,
    expected_total,domain_ref_kind,domain_ref_id,meta_note,materialization_key,
    materialization_state,population_sealed_at_utc,
    workset_publication_completed_at_utc)
VALUES(
    9100,1,'SeedProbe Survey','test',1000,0,
    3,'sp_probe_run',9010,'stage=SURVEY','fixture.archive.seedprobe.survey',
    'WORKSET_PUBLICATION_COMPLETE',1000,1000);
INSERT INTO exec_workflow_instance(workflow_instance_id,workflow_kind,state,root_scope_kind,root_scope_id,created_by,created_at_utc,completed_at_utc)
VALUES(9102,'SEED_PROBE','COMPLETED','job_set',9100,'test',1000,2000);
INSERT INTO exec_workflow_step(workflow_step_id,workflow_instance_id,step_key,graph_node_key,step_kind,state,priority,attempts,max_attempts,job_set_id,input_ref_kind,input_ref_id,output_ref_kind,output_ref_id,created_at_utc,completed_at_utc)
VALUES(9103,9102,'probe','probe','seed_probe_chain','COMPLETED',0,1,1,9100,'sp_probe_run',9010,'sp_probe_run',9010,1000,2000);
INSERT INTO exec_workset(
    workset_id,job_set_id,workflow_step_id,root_job_set_id,
    workset_key,program_kind,program_version,
    contract_key,module_canonical_id,module_version,module_sha256,
    entrypoint,verified_dependency_sha256,runtime_profile_sha256,
    program_package_sha256,execution_affinity_key,
    estimated_payload_bytes,priority,item_count,published_at_utc)
VALUES(
    9110,9100,9103,9100,'fixture.archive.seedprobe.workset',1,2,
    'fixture-compatibility','soa.seed_probe',2,
    'aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa',
    'probe',
    'bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb',
    'cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc',
    'dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd',
    NULL,128,0,3,1000);
INSERT INTO exec_workset_dispatch_attempt(
    dispatch_attempt_id,workset_id,dispatch_sequence,state,claim_token,
    lease_expires_at_utc,claimed_at_utc,dispatched_at_utc,closed_at_utc,
    close_reason_code,close_reason_text)
VALUES(
    9120,9110,1,'CLOSED','fixture-seedprobe-dispatch',
    NULL,1100,1200,2000,'ALL_TERMINAL','fixture complete');
INSERT INTO exec_job(
    job_id,job_set_id,program_kind,program_version,program_ref_kind,
    program_ref_id,fingerprint,priority,state,attempts,max_attempts,
    queued_at_utc,ended_at_utc,input_ini,workset_id,workset_item_ordinal,
    dispatch_attempt_id,reserved_attempt_id,
    cancellation_group_key,cancellation_state,cancellation_request_key,
    cancellation_reason_code,cancellation_reason_text,
    cancellation_requested_by,cancellation_caused_by_job_id,
    cancellation_requested_at_utc,cancellation_delivered_at_utc,
    cancellation_resolved_at_utc,cancellation_resolution_code)
VALUES
    (9101,9100,1,2,'sp_probe_run',9010,'rehydrate-seed-canceled',0,
     'CANCELED',1,1,1000,2000,
     '[SeedProbe.Request]
 desired_delta=2
 input_frame_id=9005
 sample_ordinal=1
 stage=SEARCH
 version=1
 ',9110,0,NULL,NULL,
     'seedprobe.run.9010.search.delta.2','RESOLVED',
     'cancel-search-delta-2','SEED_PROBE_SEARCH_WINNER',
     'confirmed candidate already won','seedprobe-result-processor',9103,
     1500,1600,1700,'CANCELED'),
    (9102,9100,1,2,'sp_probe_run',9010,'rehydrate-seed-observed',0,
     'SUCCEEDED',1,1,1000,2000,
     '[SeedProbe.Request]
input_frame_id=9005
sample_ordinal=0
stage=SURVEY
version=1
',9110,1,9120,1,
     NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL),
    (9103,9100,1,2,'sp_probe_run',9010,'rehydrate-seed-confirm',0,
     'SUCCEEDED',1,1,1000,2000,
     '[SeedProbe.Request]
confirmation_of_probe_result_id=9020
input_frame_id=9005
sample_ordinal=0
stage=CONFIRM
version=1
     ',9110,2,9120,1,
     NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL);
INSERT INTO exec_job_progress(
    job_id,attempt_id,ordinal,dispatch_attempt_id,workset_item_ordinal,
    workset_id,item_id,invocation_id,library_id,library_revision,
    progress_point_id,has_routed_provenance,routed_sequence,
    sample_snapshot_id,trigger_epoch,schema_id,schema_revision,
    schema_sha256,typed_payload,display_text,recorded_at_utc)
VALUES(
    9102,1,1,9120,0,9110,1,7001,'soa.progress.runtime.vi/1',1,
    'vi.current',1,21,22,23,'soa.progress.schema.vi/1',1,
    'eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee',
    X'5649503109','VI 9',1500);
INSERT INTO exec_job_cancellation_request(
    cancellation_request_id,job_id,request_key,reason_code,reason_text,
    requested_by,caused_by_job_id,requested_at_utc,state,delivery_attempts,
    delivered_at_utc,resolved_at_utc,resolution_code)
VALUES(
    9150,9101,'cancel-search-delta-2','SEED_PROBE_SEARCH_WINNER',
    'confirmed candidate already won','seedprobe-result-processor',9103,
    1500,'RESOLVED',1,1600,1700,'CANCELED');
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
    const auto new_canceled_job_id = ReadInt64(db_, "SELECT CAST(new_id AS INTEGER) FROM ar_rehydrate_map WHERE entity_kind='job' AND old_id='9101' ORDER BY rehydrate_map_id DESC LIMIT 1;");
    const auto new_observed_job_id = ReadInt64(db_, "SELECT CAST(new_id AS INTEGER) FROM ar_rehydrate_map WHERE entity_kind='job' AND old_id='9102' ORDER BY rehydrate_map_id DESC LIMIT 1;");
    const auto new_confirm_job_id = ReadInt64(db_, "SELECT CAST(new_id AS INTEGER) FROM ar_rehydrate_map WHERE entity_kind='job' AND old_id='9103' ORDER BY rehydrate_map_id DESC LIMIT 1;");
    const auto new_input_frame_id = ReadInt64(db_, "SELECT CAST(new_id AS INTEGER) FROM ar_rehydrate_map WHERE entity_kind='analysis_seed_probe_input_frame' AND old_id='9005' ORDER BY rehydrate_map_id DESC LIMIT 1;");
    const auto new_probe_result_id = ReadInt64(db_, "SELECT CAST(new_id AS INTEGER) FROM ar_rehydrate_map WHERE entity_kind='analysis_seed_probe_result' AND old_id='9020' ORDER BY rehydrate_map_id DESC LIMIT 1;");
    const auto new_confirmation_result_id = ReadInt64(db_, "SELECT CAST(new_id AS INTEGER) FROM ar_rehydrate_map WHERE entity_kind='analysis_seed_probe_result' AND old_id='9021' ORDER BY rehydrate_map_id DESC LIMIT 1;");
    const auto new_cancellation_request_id = ReadInt64(db_, "SELECT CAST(new_id AS INTEGER) FROM ar_rehydrate_map WHERE entity_kind='job_cancellation_request' AND old_id='9150' ORDER BY rehydrate_map_id DESC LIMIT 1;");
    const auto new_workset_id = ReadInt64(db_, "SELECT CAST(new_id AS INTEGER) FROM ar_rehydrate_map WHERE entity_kind='workset' AND old_id='9110' ORDER BY rehydrate_map_id DESC LIMIT 1;");
    const auto new_dispatch_attempt_id = ReadInt64(db_, "SELECT CAST(new_id AS INTEGER) FROM ar_rehydrate_map WHERE entity_kind='workset_dispatch_attempt' AND old_id='9120' ORDER BY rehydrate_map_id DESC LIMIT 1;");
    const auto new_step_id = ReadInt64(db_, "SELECT CAST(new_id AS INTEGER) FROM ar_rehydrate_map WHERE entity_kind='workflow_step' AND old_id='9103' ORDER BY rehydrate_map_id DESC LIMIT 1;");
    ASSERT_GT(new_probe_run_id, 0);
    ASSERT_GT(new_canceled_job_id, 0);
    ASSERT_GT(new_observed_job_id, 0);
    ASSERT_GT(new_confirm_job_id, 0);
    ASSERT_GT(new_input_frame_id, 0);
    ASSERT_GT(new_probe_result_id, 0);
    ASSERT_GT(new_confirmation_result_id, 0);
    ASSERT_GT(new_cancellation_request_id, 0);
    ASSERT_GT(new_workset_id, 0);
    ASSERT_GT(new_dispatch_attempt_id, 0);
    ASSERT_GT(new_step_id, 0);
    EXPECT_EQ(ReadInt64(db_, ("SELECT program_ref_id FROM exec_job WHERE job_id=" + std::to_string(new_observed_job_id) + ";").c_str()), new_probe_run_id);
    EXPECT_EQ(ReadInt64(db_, ("SELECT input_ref_id FROM exec_workflow_step WHERE workflow_step_id=" + std::to_string(new_step_id) + ";").c_str()), new_probe_run_id);
    EXPECT_EQ(ReadInt64(db_, ("SELECT workflow_step_id FROM exec_workset WHERE workset_id=" + std::to_string(new_workset_id) + ";").c_str()), new_step_id);
    EXPECT_EQ(
        ReadInt64(db_, ("SELECT root_job_set_id FROM exec_workset WHERE workset_id=" + std::to_string(new_workset_id) + ";").c_str()),
        ReadInt64(db_, "SELECT CAST(new_id AS INTEGER) FROM ar_rehydrate_map WHERE entity_kind='job_set' AND old_id='9100' ORDER BY rehydrate_map_id DESC LIMIT 1;"));
    EXPECT_EQ(ReadInt64(db_, ("SELECT COUNT(1) FROM sp_probe_result WHERE probe_run_id=" + std::to_string(new_probe_run_id) + ";").c_str()), 2);
    EXPECT_EQ(ReadInt64(db_, ("SELECT entry_savestate_id FROM sp_probe_run WHERE probe_run_id=" + std::to_string(new_probe_run_id) + ";").c_str()), 701);
    EXPECT_EQ(ReadInt64(db_, ("SELECT accepted_input_set_id FROM sp_probe_run WHERE probe_run_id=" + std::to_string(new_probe_run_id) + ";").c_str()),
        ReadInt64(db_, "SELECT CAST(new_id AS INTEGER) FROM ar_rehydrate_map WHERE entity_kind='analysis_input_set' AND old_id='9001' ORDER BY rehydrate_map_id DESC LIMIT 1;"));
    EXPECT_EQ(ReadText(db_, ("SELECT established_endpoint FROM sp_probe_run WHERE probe_run_id=" + std::to_string(new_probe_run_id) + ";").c_str()), "AFTER_RAND_SEED_SET");
    EXPECT_EQ(ReadInt64(db_, ("SELECT established_endpoint_source_job_id FROM sp_probe_run WHERE probe_run_id=" + std::to_string(new_probe_run_id) + ";").c_str()), new_observed_job_id);
    EXPECT_NE(ReadText(db_, ("SELECT materialization_key FROM sp_probe_run WHERE probe_run_id=" + std::to_string(new_probe_run_id) + ";").c_str()), "fixture.archive.seedprobe");
    EXPECT_EQ(ReadInt64(db_, ("SELECT source_job_id FROM sp_probe_result WHERE probe_result_id=" + std::to_string(new_probe_result_id) + ";").c_str()), new_observed_job_id);
    EXPECT_EQ(ReadText(db_, ("SELECT evidence_state FROM sp_probe_result WHERE probe_result_id=" + std::to_string(new_probe_result_id) + ";").c_str()), "CONFIRMED");
    EXPECT_EQ(ReadInt64(db_, ("SELECT confirmation_of_probe_result_id FROM sp_probe_result WHERE probe_result_id=" + std::to_string(new_confirmation_result_id) + ";").c_str()), new_probe_result_id);
    EXPECT_EQ(ReadInt64(db_, ("SELECT source_job_id FROM sp_probe_result WHERE probe_result_id=" + std::to_string(new_confirmation_result_id) + ";").c_str()), new_confirm_job_id);
    EXPECT_EQ(ReadInt64(db_, ("SELECT dispatch_attempt_id FROM exec_job WHERE job_id=" + std::to_string(new_observed_job_id) + ";").c_str()), new_dispatch_attempt_id);
    EXPECT_EQ(ReadInt64(db_, ("SELECT workset_item_ordinal FROM exec_job WHERE job_id=" + std::to_string(new_observed_job_id) + ";").c_str()), 0);
    EXPECT_EQ(ReadInt64(db_, ("SELECT reserved_attempt_id FROM exec_job WHERE job_id=" + std::to_string(new_observed_job_id) + ";").c_str()), 1);
    EXPECT_EQ(ReadInt64(db_, ("SELECT dispatch_attempt_id FROM exec_job WHERE job_id=" + std::to_string(new_confirm_job_id) + ";").c_str()), new_dispatch_attempt_id);
    EXPECT_EQ(ReadInt64(db_, ("SELECT workset_item_ordinal FROM exec_job WHERE job_id=" + std::to_string(new_confirm_job_id) + ";").c_str()), 1);
    EXPECT_EQ(ReadInt64(db_, ("SELECT reserved_attempt_id FROM exec_job WHERE job_id=" + std::to_string(new_confirm_job_id) + ";").c_str()), 1);
    EXPECT_EQ(
        ReadInt64(
            db_,
            ("SELECT COUNT(1) FROM exec_job_progress WHERE job_id=" +
                std::to_string(new_observed_job_id) + ";").c_str()),
        1);
    EXPECT_EQ(
        ReadInt64(
            db_,
            ("SELECT dispatch_attempt_id FROM exec_job_progress WHERE job_id=" +
                std::to_string(new_observed_job_id) + ";").c_str()),
        new_dispatch_attempt_id);
    EXPECT_EQ(
        ReadInt64(
            db_,
            ("SELECT workset_id FROM exec_job_progress WHERE job_id=" +
                std::to_string(new_observed_job_id) + ";").c_str()),
        new_workset_id);
    EXPECT_EQ(
        ReadText(
            db_,
            ("SELECT display_text FROM exec_job_progress WHERE job_id=" +
                std::to_string(new_observed_job_id) + ";").c_str()),
        "VI 9");
    const auto observed_spec =
        execution::programdb::seedprobe::DecodeSeedProbeJobSpec(
            ReadText(
                db_,
                ("SELECT input_ini FROM exec_job WHERE job_id="
                    + std::to_string(new_observed_job_id) + ";")
                    .c_str()));
    ASSERT_TRUE(observed_spec.has_value());
    EXPECT_EQ(
        observed_spec->stage,
        execution::programdb::seedprobe::SeedProbeJobStage::Survey);
    EXPECT_EQ(observed_spec->input_frame_id, new_input_frame_id);
    EXPECT_EQ(observed_spec->sample_ordinal, 0);
    const auto confirm_spec =
        execution::programdb::seedprobe::DecodeSeedProbeJobSpec(
            ReadText(
                db_,
                ("SELECT input_ini FROM exec_job WHERE job_id="
                    + std::to_string(new_confirm_job_id) + ";")
                    .c_str()));
    ASSERT_TRUE(confirm_spec.has_value());
    EXPECT_EQ(
        confirm_spec->stage,
        execution::programdb::seedprobe::SeedProbeJobStage::Confirm);
    EXPECT_EQ(confirm_spec->input_frame_id, new_input_frame_id);
    EXPECT_EQ(
        confirm_spec->confirmation_of_probe_result_id,
        std::optional<std::int64_t>(new_probe_result_id));
    const auto search_spec =
        execution::programdb::seedprobe::DecodeSeedProbeJobSpec(
            ReadText(
                db_,
                ("SELECT input_ini FROM exec_job WHERE job_id="
                    + std::to_string(new_canceled_job_id) + ";")
                    .c_str()));
    ASSERT_TRUE(search_spec.has_value());
    EXPECT_EQ(
        search_spec->stage,
        execution::programdb::seedprobe::SeedProbeJobStage::Search);
    EXPECT_EQ(search_spec->input_frame_id, new_input_frame_id);
    EXPECT_EQ(
        search_spec->desired_delta,
        std::optional<std::int32_t>(2));
    EXPECT_EQ(
        ReadText(
            db_,
            ("SELECT cancellation_group_key FROM exec_job WHERE job_id="
                + std::to_string(new_canceled_job_id) + ";")
                .c_str()),
        "seedprobe.run." + std::to_string(new_probe_run_id)
            + ".search.delta.2");
    EXPECT_NE(
        ReadText(
            db_,
            ("SELECT fingerprint FROM exec_job WHERE job_id="
                + std::to_string(new_observed_job_id) + ";")
                .c_str()),
        "rehydrate-seed-observed");
    EXPECT_EQ(ReadInt64(db_, ("SELECT cancellation_caused_by_job_id FROM exec_job WHERE job_id=" + std::to_string(new_canceled_job_id) + ";").c_str()), new_confirm_job_id);
    EXPECT_EQ(ReadInt64(db_, ("SELECT caused_by_job_id FROM exec_job_cancellation_request WHERE cancellation_request_id=" + std::to_string(new_cancellation_request_id) + ";").c_str()), new_confirm_job_id);

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
            .source_job_set_id = 0,
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
    archive_package_id,source_context,source_job_set_id,source_scope_kind,source_workflow_count,
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

TEST_F(SqliteDbFixture, Stage3Phase1DbContracts_FailedStepParksWorkflowRecoverably) {
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
    ASSERT_TRUE(commands->FailWorkflowInstance(
        {
            .workflow_instance_id = 1401,
            .workflow_step_id = 1402,
            .failure_code = "TEST_FAILURE",
            .failure_message = "test failure",
            .requested_by = "SavorTests-failure",
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
    ASSERT_TRUE(state_db->ImportExternalArtifact(
        {
            .absolute_source_path = source_path,
            .artifact = {
                .sha256 = hash::sha256_of_file(source_path.string()),
                .size_bytes = static_cast<std::int64_t>(std::filesystem::file_size(source_path)),
                .compression_kind = 0,
                .display_filename = source_path.filename().string(),
                .file_ext = ".sav",
                .artifact_kind = "SAV",
                .created_at_utc = savor::db::types::UtcNow(),
                .correlation_id = "test.state.materialize_savestate",
                .causation_id = "test",
            },
        },
        &artifact_id,
        &err))
        << err;
    ASSERT_TRUE(std::filesystem::remove(source_path));

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
    ASSERT_TRUE(state_db->ImportExternalArtifact(
        {
            .absolute_source_path = first_path,
            .artifact = {
                .sha256 = hash::sha256_of_file(first_path.string()),
                .size_bytes = static_cast<std::int64_t>(std::filesystem::file_size(first_path)),
                .compression_kind = 0,
                .display_filename = first_path.filename().string(),
                .file_ext = ".sav",
                .artifact_kind = "SAV",
                .created_at_utc = savor::db::types::UtcNow(),
                .correlation_id = "test.state.artifact.dedupe",
                .causation_id = "test",
            },
        },
        &first_artifact_id,
        &err))
        << err;

    std::int64_t second_artifact_id = 0;
    ASSERT_TRUE(state_db->ImportExternalArtifact(
        {
            .absolute_source_path = second_path,
            .artifact = {
                .sha256 = hash::sha256_of_file(second_path.string()),
                .size_bytes = static_cast<std::int64_t>(std::filesystem::file_size(second_path)),
                .compression_kind = 0,
                .display_filename = second_path.filename().string(),
                .file_ext = ".sav",
                .artifact_kind = "SAV",
                .created_at_utc = savor::db::types::UtcNow(),
                .correlation_id = "test.state.artifact.dedupe",
                .causation_id = "test",
            },
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

TEST_F(SqliteDbFixture, StateDbAcceptsFirstClassBattleCompletionArtifact) {
    auto* state_db = db_service_->StateDb();
    ASSERT_NE(state_db, nullptr);

    const std::string manifest_bytes("BCM1\0fixture", 12);
    const auto manifest_path = temp_root_ / "completion.bcmb";
    {
        std::ofstream out(manifest_path, std::ios::binary);
        out.write(manifest_bytes.data(),
                  static_cast<std::streamsize>(manifest_bytes.size()));
    }

    std::string error;
    std::int64_t artifact_id = 0;
    ASSERT_TRUE(state_db->ImportExternalArtifact(
        {
            .absolute_source_path = manifest_path,
            .artifact = {
                .sha256 = hash::sha256(
                    manifest_bytes.data(), manifest_bytes.size()),
                .size_bytes = static_cast<std::int64_t>(manifest_bytes.size()),
                .compression_kind = 0,
                .display_filename = manifest_path.filename().string(),
                .file_ext = ".bcmb",
                .artifact_kind = "BATTLE_COMPLETION",
                .created_at_utc = savor::db::types::UtcNow(),
                .correlation_id = "test.state.battle-completion",
                .causation_id = "test",
            },
        },
        &artifact_id,
        &error)) << error;

    const auto artifact = state_db->GetArtifact(artifact_id);
    ASSERT_TRUE(artifact.has_value());
    EXPECT_EQ(artifact->artifact_kind, "BATTLE_COMPLETION");
    EXPECT_EQ(artifact->file_ext, ".bcmb");
    EXPECT_EQ(artifact->display_filename, manifest_path.filename().string());
    EXPECT_FALSE(std::filesystem::path(artifact->object_relpath).is_absolute());
    EXPECT_EQ(
        std::filesystem::path(artifact->object_path).parent_path().parent_path().parent_path().parent_path(),
        temp_root_ / "object_store");
    EXPECT_TRUE(std::filesystem::is_regular_file(artifact->object_path));

    std::int64_t rejected_id = 0;
    EXPECT_FALSE(state_db->ImportExternalArtifact(
        {
            .absolute_source_path = temp_root_ / "unknown.bin",
            .artifact = {
                .sha256 = std::string(64, 'f'),
                .size_bytes = 1,
                .compression_kind = 0,
                .display_filename = "unknown.bin",
                .file_ext = ".bin",
                .artifact_kind = "UNKNOWN_COMPLETION_KIND",
                .created_at_utc = savor::db::types::UtcNow(),
                .correlation_id = "test.state.battle-completion",
                .causation_id = "test",
            },
        },
        &rejected_id,
        &error));
}

TEST_F(SqliteDbFixture, StateDbStoresOnlyContainedWorkspaceArtifacts) {
    auto* state_db = db_service_->StateDb();
    ASSERT_NE(state_db, nullptr);

    const std::string bytes = "workspace-artifact";
    const auto workspace_source =
        state_db->ArtifactWorkspaceRoot() / "published" / "contained.sav";
    ASSERT_TRUE(std::filesystem::create_directories(
        workspace_source.parent_path()));
    {
        std::ofstream out(workspace_source, std::ios::binary);
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }

    const savor::db::ArtifactStoreMetadata metadata{
        .sha256 = hash::sha256(bytes.data(), bytes.size()),
        .size_bytes = static_cast<std::int64_t>(bytes.size()),
        .compression_kind = 0,
        .display_filename = "contained.sav",
        .file_ext = ".sav",
        .artifact_kind = "SAV",
        .created_at_utc = savor::db::types::UtcNow(),
        .correlation_id = "test.state.workspace-ingestion",
        .causation_id = "test",
    };
    std::string error;
    std::int64_t artifact_id = 0;
    ASSERT_TRUE(state_db->StoreWorkspaceArtifact({
        .workspace_relative_path =
            std::filesystem::path("published") / "contained.sav",
        .artifact = metadata,
    }, &artifact_id, &error)) << error;
    const auto artifact = state_db->GetArtifact(artifact_id);
    ASSERT_TRUE(artifact.has_value());
    EXPECT_TRUE(std::filesystem::is_regular_file(artifact->object_path));

    std::int64_t rejected_id = 0;
    error.clear();
    EXPECT_FALSE(state_db->StoreWorkspaceArtifact({
        .workspace_relative_path =
            std::filesystem::path("..") / "outside.sav",
        .artifact = metadata,
    }, &rejected_id, &error));
    EXPECT_FALSE(error.empty());

    error.clear();
    EXPECT_FALSE(state_db->ImportExternalArtifact({
        .absolute_source_path = "relative.sav",
        .artifact = metadata,
    }, &rejected_id, &error));
    EXPECT_FALSE(error.empty());
}

TEST_F(SqliteDbFixture, StateDbRejectsCorruptedExistingObject) {
    auto* state_db = db_service_->StateDb();
    ASSERT_NE(state_db, nullptr);

    const auto source = temp_root_ / "object-integrity.sav";
    const std::string bytes = "verified-object";
    {
        std::ofstream out(source, std::ios::binary);
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }
    const savor::db::ImportExternalArtifactCommand command{
        .absolute_source_path = source,
        .artifact = {
            .sha256 = hash::sha256(bytes.data(), bytes.size()),
            .size_bytes = static_cast<std::int64_t>(bytes.size()),
            .compression_kind = 0,
            .display_filename = source.filename().string(),
            .file_ext = ".sav",
            .artifact_kind = "SAV",
            .created_at_utc = savor::db::types::UtcNow(),
            .correlation_id = "test.state.object-integrity",
            .causation_id = "test",
        },
    };
    std::string error;
    std::int64_t artifact_id = 0;
    ASSERT_TRUE(state_db->ImportExternalArtifact(
        command, &artifact_id, &error)) << error;
    const auto artifact = state_db->GetArtifact(artifact_id);
    ASSERT_TRUE(artifact.has_value());
    {
        std::ofstream out(artifact->object_path, std::ios::binary | std::ios::trunc);
        out << "corrupt";
    }

    std::int64_t repeated_id = 0;
    error.clear();
    EXPECT_FALSE(state_db->ImportExternalArtifact(
        command, &repeated_id, &error));
    EXPECT_FALSE(error.empty());
}

TEST(SavorDbArtifactObjectStore, RelocatedDatabaseRootResolvesCanonicalObjectLocator) {
    namespace migrations = savor::db::migrations;

    const auto container = MakeTempPhase4Dir("state-artifact-relocation");
    const auto source_root = container / "source";
    const auto target_root = container / "target";
    const auto source_paths = MakePhase4DbPaths(source_root);
    ASSERT_TRUE(std::filesystem::create_directories(source_root));

    std::int64_t artifact_id = 0;
    std::string artifact_sha;
    std::string error;
    {
        savor::db::core::DBService service(
            source_paths,
            migrations::MigrationSourceOptions{
                .source_kind = migrations::MigrationSourceKind::Embedded});
        ASSERT_TRUE(service.Start(&error)) << error;
        const auto input = source_root / "portable-input.dtm";
        const std::string bytes = "portable-artifact-bytes";
        {
            std::ofstream out(input, std::ios::binary);
            out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        }
        artifact_sha = hash::sha256(bytes.data(), bytes.size());
        ASSERT_TRUE(service.StateDb()->ImportExternalArtifact({
            .absolute_source_path = input,
            .artifact = {
                .sha256 = artifact_sha,
                .size_bytes = static_cast<std::int64_t>(bytes.size()),
                .compression_kind = 0,
                .display_filename = "portable-input.dtm",
                .file_ext = ".dtm",
                .artifact_kind = "DTM",
                .created_at_utc = savor::db::types::UtcNow(),
                .correlation_id = "test.state.relative-locator",
                .causation_id = "test",
            },
        }, &artifact_id, &error)) << error;

        const auto artifact = service.StateDb()->GetArtifact(artifact_id);
        ASSERT_TRUE(artifact.has_value());
        ASSERT_FALSE(artifact->object_relpath.empty());
        EXPECT_FALSE(std::filesystem::path(artifact->object_relpath).is_absolute());

        service.Stop();
    }

    std::error_code ec;
    std::filesystem::copy(
        source_root, target_root,
        std::filesystem::copy_options::recursive |
            std::filesystem::copy_options::overwrite_existing,
        ec);
    ASSERT_FALSE(ec) << ec.message();
    std::filesystem::remove_all(source_root, ec);
    ASSERT_FALSE(ec) << ec.message();

    auto target_paths = MakePhase4DbPaths(target_root);
    savor::db::core::DBService relocated(
        target_paths,
        migrations::MigrationSourceOptions{
            .source_kind = migrations::MigrationSourceKind::Embedded});
    ASSERT_TRUE(relocated.Start(&error)) << error;
    const auto artifact = relocated.StateDb()->GetArtifact(artifact_id);
    ASSERT_TRUE(artifact.has_value());
    EXPECT_EQ(artifact->sha256, artifact_sha);
    EXPECT_EQ(artifact->display_filename, "portable-input.dtm");
    EXPECT_FALSE(std::filesystem::path(artifact->object_relpath).is_absolute());
    EXPECT_TRUE(std::filesystem::is_regular_file(artifact->object_path));
    EXPECT_EQ(
        std::filesystem::path(artifact->object_path)
            .lexically_relative(target_paths.object_store_root)
            .generic_string(),
        artifact->object_relpath);
    EXPECT_TRUE(ReadText(
        relocated.RawStateSqlite(),
        "SELECT object_relpath FROM state_artifact WHERE artifact_id=1;")
        .starts_with("state_artifacts/"));
    relocated.Stop();
    std::filesystem::remove_all(container, ec);
}

TEST(SavorDbArtifactObjectStore, RejectsAbsoluteAndTraversalLocators) {
    namespace state = savor::db::state;

    std::string error;
    EXPECT_FALSE(state::IsValidArtifactObjectRelativePath(
        std::filesystem::path("C:/outside/artifact.sav"), &error));
    EXPECT_FALSE(error.empty());

    error.clear();
    EXPECT_FALSE(state::IsValidArtifactObjectRelativePath(
        std::filesystem::path("state_artifacts") / ".." / "outside.sav",
        &error));
    EXPECT_FALSE(error.empty());

    error.clear();
    const auto canonical = state::MakeArtifactObjectRelativePath(
        std::string(64, 'A'), ".sav", &error);
    ASSERT_TRUE(canonical.has_value()) << error;
    EXPECT_EQ(
        canonical->generic_string(),
        "state_artifacts/aa/aa/" + std::string(64, 'a') + ".sav");
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
    ASSERT_TRUE(state_db->ImportExternalArtifact(
        {
            .absolute_source_path = source_path,
            .artifact = {
                .sha256 = hash::sha256_of_file(source_path.string()),
                .size_bytes = static_cast<std::int64_t>(std::filesystem::file_size(source_path)),
                .compression_kind = 0,
                .display_filename = source_path.filename().string(),
                .file_ext = ".sav",
                .artifact_kind = "SAV",
                .created_at_utc = savor::db::types::UtcNow(),
                .correlation_id = "test.state.artifact.attached_projection",
                .causation_id = "test",
            },
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
            .battle_plan_id = 202,
            .battle_plan_fingerprint = "projection-plan",
            .continuation_mode = BattleContinuationMode::AutomaticBestPerEndingRng,
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
            .battle_plan_id = 222,
            .battle_plan_fingerprint = "projection-turn-two-plan",
            .continuation_mode = BattleContinuationMode::AutomaticBestPerEndingRng,
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
            .battle_plan_id = 202,
            .battle_plan_fingerprint = "projection-incremental-plan",
            .continuation_mode = BattleContinuationMode::AutomaticBestPerEndingRng,
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
            .battle_plan_id = 202,
            .battle_plan_fingerprint = "query-battle-plan",
            .continuation_mode = BattleContinuationMode::AutomaticBestPerEndingRng,
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
    savor::db::state::SqliteStateDb state_db(
        state_handle, separate_root / "workflow-runtime", paths.object_store_root);

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

    const auto diagnostic_path = separate_root / "diagnostic.sav";
    const std::string diagnostic_bytes = "diagnostic-12";
    {
        std::ofstream out(diagnostic_path, std::ios::binary);
        out.write(diagnostic_bytes.data(), static_cast<std::streamsize>(diagnostic_bytes.size()));
    }
    std::int64_t artifact_id = 0;
    ASSERT_TRUE(state_db.ImportExternalArtifact(
        {
            .absolute_source_path = diagnostic_path,
            .artifact = {
                .sha256 = hash::sha256(
                    diagnostic_bytes.data(), diagnostic_bytes.size()),
                .size_bytes = static_cast<std::int64_t>(diagnostic_bytes.size()),
                .compression_kind = 0,
                .display_filename = diagnostic_path.filename().string(),
                .file_ext = ".sav",
                .artifact_kind = "SAV",
                .created_at_utc = savor::db::types::UtcNow(),
                .correlation_id = "projection-diagnostic",
                .causation_id = "test",
            },
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
    savor::db::state::SqliteStateDb state_db(
        state_handle, separate_root / "workflow-runtime", paths.object_store_root);

    std::string err;
    for (int i = 0; i < 3; ++i) {
        const auto source_path = separate_root / ("batch-" + std::to_string(i) + ".sav");
        const std::string bytes(static_cast<std::size_t>(12 + i), static_cast<char>('a' + i));
        {
            std::ofstream out(source_path, std::ios::binary);
            out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        }
        std::int64_t artifact_id = 0;
        ASSERT_TRUE(state_db.ImportExternalArtifact(
            {
                .absolute_source_path = source_path,
                .artifact = {
                    .sha256 = hash::sha256(bytes.data(), bytes.size()),
                    .size_bytes = static_cast<std::int64_t>(bytes.size()),
                    .compression_kind = 0,
                    .display_filename = source_path.filename().string(),
                    .file_ext = ".sav",
                    .artifact_kind = "SAV",
                    .created_at_utc = savor::db::types::UtcNow(),
                    .correlation_id = "projection-batch",
                    .causation_id = "test",
                },
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
        "INSERT INTO exec_job_set(job_set_id,program_kind,purpose,created_by,created_at_utc,priority_boost,expected_total,domain_ref_kind,domain_ref_id,meta_note) "
        "VALUES(1,42,'progress-flood','test',1000,0,1,NULL,NULL,NULL);"));
    ASSERT_TRUE(ExecSql(exec_handle,
        "INSERT INTO exec_job(job_id,job_set_id,parent_job_id,program_kind,program_version,program_ref_kind,program_ref_id,fingerprint,priority,state,attempts,max_attempts,claimed_by_token,lease_expires_at_utc,queued_at_utc,started_at_utc,ended_at_utc,error_code,error_text) "
        "VALUES(17401,1,NULL,42,1,'test',1,'progress-flood-job',1,'RUNNING',1,1,'worker-1',NULL,1000,1100,NULL,NULL,NULL);"));
    ASSERT_TRUE(ExecSql(exec_handle,
        "INSERT INTO exec_job_progress("
        "job_id,attempt_id,ordinal,dispatch_attempt_id,workset_item_ordinal,"
        "workset_id,item_id,invocation_id,library_id,library_revision,"
        "progress_point_id,has_routed_provenance,routed_sequence,"
        "sample_snapshot_id,trigger_epoch,schema_id,schema_revision,"
        "schema_sha256,typed_payload,display_text,recorded_at_utc) "
        "VALUES(17401,1,1,99,0,99,1,7,'soa.progress.runtime.vi/1',1,"
        "'vi.current',1,3,4,5,'soa.progress.schema.vi/1',1,"
        "printf('%064d',0),X'5649503109','VI 9',1200);"));
    for (int i = 1; i <= 1000; ++i) {
        ASSERT_TRUE(ExecSql(exec_handle,
            ("INSERT INTO exec_outbox_message(outbox_id,event_id,event_type,event_version,context_name,aggregate_kind,aggregate_id,correlation_id,causation_id,occurred_at_utc,payload_ref_kind,payload_ref_id,published_at_utc,attempt_count,last_error) "
             "VALUES(" + std::to_string(i) + ",'progress-" + std::to_string(i) + "','Execution.JobProgressed.v2',2,'Execution','job','17401','test','test'," + std::to_string(1000 + i) + ",'job',17401,NULL,0,NULL);")
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
    EXPECT_EQ(ReadInt64(verify_handle, "SELECT COUNT(1) FROM ui_job_progress WHERE job_id=17401;"), 1);
    EXPECT_EQ(ReadText(verify_handle, "SELECT display_text FROM ui_job_progress WHERE job_id=17401;"), "VI 9");
    EXPECT_EQ(ReadText(verify_handle, "SELECT last_progress_text FROM ui_job_summary WHERE job_id=17401;"), "VI 9");
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
        "INSERT INTO exec_job_set(job_set_id,program_kind,purpose,created_by,created_at_utc,priority_boost,expected_total,domain_ref_kind,domain_ref_id,meta_note) "
        "VALUES(25002,42,'terminal-job-test','test',1000,0,2,NULL,NULL,NULL);"));
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
    EXPECT_EQ(ReadInt64(verify_handle, "SELECT job_settled_count FROM ui_workflow_step WHERE workflow_step_id=25003;"), 1);
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
        "INSERT INTO exec_job_set(job_set_id,program_kind,purpose,created_by,created_at_utc,priority_boost,expected_total,domain_ref_kind,domain_ref_id,meta_note) "
        "VALUES(26050,42,'display-active-job','test',1000,0,1,NULL,NULL,NULL);"));
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

    savor::db::UiWorkflowInstanceListQuery open_query{};
    open_query.exclude_final = true;
    open_query.limit = 10;
    const auto open_page = ui_read.ListWorkflowInstances(open_query);
    EXPECT_NE(std::find_if(open_page.items.begin(), open_page.items.end(), [](const auto& row) {
        return row.workflow_instance_id == 26003;
    }), open_page.items.end());
    EXPECT_EQ(std::find_if(open_page.items.begin(), open_page.items.end(), [](const auto& row) {
        return row.workflow_instance_id == 26004;
    }), open_page.items.end());

    const auto counts = ui_read.CountWorkflowDisplayStates();
    EXPECT_EQ(counts.running, 2);
    EXPECT_EQ(counts.queued, 1);
    EXPECT_EQ(counts.waiting, 1);
    EXPECT_EQ(counts.completed, 1);
    EXPECT_EQ(counts.finalized, 1);
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
INSERT INTO exec_job_set(job_set_id,program_kind,purpose,created_by,created_at_utc,priority_boost,expected_total,domain_ref_kind,domain_ref_id,meta_note)
VALUES(27010,7,'supersede-projection','test',1000,0,3,NULL,NULL,NULL);
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
    EXPECT_EQ(ReadInt64(verify_handle, "SELECT COUNT(1) FROM ui_job_summary WHERE job_set_id=27010 AND state='QUEUED';"), 1);
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
        "INSERT INTO exec_job_set(job_set_id,program_kind,purpose,created_by,created_at_utc,priority_boost,expected_total,domain_ref_kind,domain_ref_id,meta_note) "
        "VALUES(32001,42,'priority-old-jobs','test',1000,0,5,NULL,NULL,NULL);"));
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
        "INSERT INTO exec_job_set(job_set_id,program_kind,purpose,created_by,created_at_utc,priority_boost,expected_total,domain_ref_kind,domain_ref_id,meta_note) "
        "VALUES(33110,42,'priority-job-set','test',1000,0,1,NULL,NULL,NULL);"));
    ASSERT_TRUE(ExecSql(exec_handle,
        "INSERT INTO exec_job_set(job_set_id,program_kind,purpose,created_by,created_at_utc,priority_boost,expected_total,domain_ref_kind,domain_ref_id,meta_note) "
        "VALUES(33120,42,'priority-plain-job','test',1000,0,1,NULL,NULL,NULL);"));
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
        "INSERT INTO exec_job_set(job_set_id,program_kind,purpose,created_by,created_at_utc,priority_boost,expected_total,domain_ref_kind,domain_ref_id,meta_note) "
        "VALUES(34110,42,'rollup-job-set','test',1000,0,24,NULL,NULL,NULL);"));
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

TEST_F(SqliteDbFixture, MarkWorksetJobStartedClassifiesAuthorityRejectionsAndPreservesIdempotency) {
    using savor::db::ExecutionDbOperationDisposition;
    using savor::db::MarkWorksetJobStartedCommand;
    using savor::db::WorksetJobStartReceipt;
    using savor::db::execution::workflow::SqliteExecutionDb;

    ASSERT_TRUE(ExecSql(db_, R"SQL(
INSERT INTO exec_job_set(
    job_set_id,program_kind,purpose,created_by,created_at_utc)
VALUES(35010,42,'start-authority','test',1000);

INSERT INTO exec_workflow_instance(
    workflow_instance_id,workflow_kind,state,root_scope_kind,
    created_by,created_at_utc)
VALUES(35011,'START_AUTHORITY','RUNNING','manual','test',1000);

INSERT INTO exec_workflow_step(
    workflow_step_id,workflow_instance_id,step_key,step_kind,state,
    job_set_id,priority,attempts,max_attempts,created_at_utc)
VALUES(
    35012,35011,'Start','start.authority','MATERIALIZED',
    35010,0,0,1,1000);

INSERT INTO exec_workset(
    workset_id,job_set_id,workflow_step_id,root_job_set_id,
    workset_key,program_kind,program_version,
    contract_key,module_canonical_id,module_version,module_sha256,
    entrypoint,verified_dependency_sha256,runtime_profile_sha256,
    program_package_sha256,estimated_payload_bytes,priority,item_count,
    published_at_utc)
VALUES(
    35020,35010,35012,35010,'start-authority-workset',42,1,
    'start-authority-compatibility','test.module',1,printf('%064d',0),
    'test-entrypoint',printf('%064d',0),printf('%064d',0),
    printf('%064d',0),1,0,2,1000);

INSERT INTO exec_workset_dispatch_attempt(
    dispatch_attempt_id,workset_id,dispatch_sequence,state,claim_token,
    lease_expires_at_utc,claimed_at_utc,dispatched_at_utc)
VALUES(
    35030,35020,1,'ACTIVE','start-authority-token',
    1,1000,1000);

INSERT INTO exec_job(
    job_id,job_set_id,program_kind,program_version,program_ref_kind,
    program_ref_id,fingerprint,priority,state,attempts,max_attempts,
    claimed_by_token,queued_at_utc,workset_id,workset_item_ordinal,
    dispatch_attempt_id,reserved_attempt_id)
VALUES(
    35040,35010,42,1,'test',1,'start-authority-job',0,
    'CLAIMED',0,2,'start-authority-token',1000,35020,0,35030,1);
)SQL"));

    SqliteExecutionDb execution_db(db_);
    const MarkWorksetJobStartedCommand valid_command{
        .dispatch_attempt_id = 35030,
        .claim_token = "start-authority-token",
        .job_id = 35040,
        .workset_item_ordinal = 0,
        .reserved_attempt_id = 1,
        .worker_invocation_id = "start-authority-invocation",
        .requested_by = "test",
    };
    const auto expect_disposition =
        [&](const MarkWorksetJobStartedCommand& command,
            ExecutionDbOperationDisposition expected) {
            WorksetJobStartReceipt receipt{};
            std::string error;
            savor::db::PersistWorkerExecutionEventsBatchReceipt batch;
            ASSERT_TRUE(execution_db.PersistWorkerExecutionEventsBatch(
                {.events = {command}},
                &batch,
                &error))
                << error;
            ASSERT_EQ(batch.events.size(), 1u);
            receipt = std::get<WorksetJobStartReceipt>(batch.events.front());
            EXPECT_EQ(receipt.disposition, expected);
        };

    auto command = valid_command;
    command.job_id = 35999;
    expect_disposition(
        command,
        ExecutionDbOperationDisposition::Missing);

    command = valid_command;
    command.claim_token = "wrong-token";
    expect_disposition(
        command,
        ExecutionDbOperationDisposition::TokenMismatch);

    command = valid_command;
    command.reserved_attempt_id = 2;
    expect_disposition(
        command,
        ExecutionDbOperationDisposition::AttemptMismatch);

    command = valid_command;
    command.workset_item_ordinal = 1;
    expect_disposition(
        command,
        ExecutionDbOperationDisposition::WrongState);

    expect_disposition(
        valid_command,
        ExecutionDbOperationDisposition::Applied);
    expect_disposition(
        valid_command,
        ExecutionDbOperationDisposition::AlreadyApplied);
    EXPECT_EQ(
        ReadText(
            db_,
            "SELECT state FROM exec_job WHERE job_id=35040;"),
        "RUNNING");
    EXPECT_EQ(
        ReadInt64(
            db_,
            "SELECT attempts FROM exec_job WHERE job_id=35040;"),
        1);
}

TEST_F(SqliteDbFixture, CanonicalJobProgressIsStructuredOrderedAndIdempotent) {
    using namespace savor::db;
    using savor::db::execution::workflow::SqliteExecutionDb;

    ASSERT_TRUE(ExecSql(db_, R"SQL(
INSERT INTO exec_job_set(
    job_set_id,program_kind,purpose,created_by,created_at_utc)
VALUES(35510,42,'canonical-progress','test',1000);

INSERT INTO exec_workflow_instance(
    workflow_instance_id,workflow_kind,state,root_scope_kind,
    created_by,created_at_utc)
VALUES(35511,'CANONICAL_PROGRESS','RUNNING','manual','test',1000);

INSERT INTO exec_workflow_step(
    workflow_step_id,workflow_instance_id,step_key,step_kind,state,
    job_set_id,priority,attempts,max_attempts,created_at_utc)
VALUES(
    35512,35511,'Progress','progress.test','MATERIALIZED',
    35510,0,0,1,1000);

INSERT INTO exec_workset(
    workset_id,job_set_id,workflow_step_id,root_job_set_id,
    workset_key,program_kind,program_version,
    contract_key,module_canonical_id,module_version,module_sha256,
    entrypoint,verified_dependency_sha256,runtime_profile_sha256,
    program_package_sha256,estimated_payload_bytes,priority,item_count,
    published_at_utc)
VALUES(
    35520,35510,35512,35510,'canonical-progress-workset',42,1,
    'canonical-progress-contract','test.module',1,printf('%064d',0),
    'test-entrypoint',printf('%064d',0),printf('%064d',0),
    printf('%064d',0),1,0,1,1000);

INSERT INTO exec_workset_dispatch_attempt(
    dispatch_attempt_id,workset_id,dispatch_sequence,state,claim_token,
    lease_expires_at_utc,claimed_at_utc,dispatched_at_utc)
VALUES(
    35530,35520,1,'ACTIVE','canonical-progress-token',
    999999,1000,1000);

INSERT INTO exec_job(
    job_id,job_set_id,program_kind,program_version,program_ref_kind,
    program_ref_id,fingerprint,priority,state,attempts,max_attempts,
    claimed_by_token,queued_at_utc,workset_id,workset_item_ordinal,
    dispatch_attempt_id,reserved_attempt_id)
VALUES(
    35540,35510,42,1,'test',1,'canonical-progress-job',0,
    'RUNNING',1,2,'canonical-progress-token',1000,35520,0,35530,1);
)SQL"));

    SqliteExecutionDb execution_db(db_);
    const RecordCanonicalJobProgressCommand valid{
        .dispatch_attempt_id = 35530,
        .claim_token = "canonical-progress-token",
        .job_id = 35540,
        .workset_item_ordinal = 0,
        .reserved_attempt_id = 1,
        .workset_id = 35530,
        .item_id = 1,
        .invocation_id = 77,
        .ordinal = 1,
        .library_id = "soa.progress.runtime.vi/1",
        .library_revision = 1,
        .progress_point_id = "vi.current",
        .has_routed_provenance = true,
        .routed_sequence = 11,
        .sample_snapshot_id = 12,
        .trigger_epoch = 13,
        .schema_id = "soa.progress.schema.vi/1",
        .schema_revision = 1,
        .schema_sha256 = std::string(64, 'a'),
        .typed_payload = {'V', 'I', 'P', '1', 9},
        .display_text = "VI 9",
        .requested_by = "test",
    };
    const auto persist = [&](const RecordCanonicalJobProgressCommand& command) {
        PersistWorkerExecutionEventsBatchReceipt batch;
        std::string error;
        EXPECT_TRUE(execution_db.PersistWorkerExecutionEventsBatch(
            {.events = {command}},
            &batch,
            &error)) << error;
        EXPECT_EQ(batch.events.size(), 1u);
        return std::get<CanonicalJobProgressReceipt>(batch.events.front());
    };

    EXPECT_EQ(
        persist(valid).disposition,
        ExecutionDbOperationDisposition::Applied);
    EXPECT_EQ(
        persist(valid).disposition,
        ExecutionDbOperationDisposition::AlreadyApplied);
    EXPECT_EQ(
        ReadInt64(db_, "SELECT COUNT(1) FROM exec_job_progress WHERE job_id=35540;"),
        1);
    EXPECT_EQ(
        ReadText(db_, "SELECT display_text FROM exec_job_progress WHERE job_id=35540;"),
        "VI 9");
    EXPECT_EQ(
        ReadText(db_, "SELECT event_type FROM exec_outbox_message WHERE aggregate_id='35540' ORDER BY outbox_id DESC LIMIT 1;"),
        "Execution.JobProgressed.v2");

    auto conflict = valid;
    conflict.display_text = "VI 10";
    EXPECT_EQ(
        persist(conflict).disposition,
        ExecutionDbOperationDisposition::AttemptMismatch);

    ASSERT_TRUE(ExecSql(
        db_,
        "UPDATE exec_job SET state='SUCCEEDED' WHERE job_id=35540;"));
    EXPECT_EQ(
        persist(valid).disposition,
        ExecutionDbOperationDisposition::AlreadyApplied);
    auto after_terminal = valid;
    after_terminal.ordinal = 2;
    EXPECT_EQ(
        persist(after_terminal).disposition,
        ExecutionDbOperationDisposition::WrongState);
    EXPECT_EQ(
        ReadText(db_, "SELECT state FROM exec_job WHERE job_id=35540;"),
        "SUCCEEDED");
}

TEST_F(
    SqliteDbFixture,
    BatchedWorkerPersistenceAndResultClaimsPreserveOrderAndThirtyTwoItemBoundary) {
    using namespace savor::db;
    using savor::db::execution::workflow::SqliteExecutionDb;

    SqliteExecutionDb execution_db(db_);
    std::string error;
    EnsureMaterializingJobSetReceipt job_set{};
    ASSERT_TRUE(execution_db.EnsureMaterializingJobSet(
        {
            .materialization_key = "batch-persistence-job-set",
            .program_kind = 42,
            .purpose = "batch-persistence",
            .created_by = "test",
            .created_at_utc = 1,
            .expected_total = 33,
        },
        &job_set,
        &error)) << error;
    ASSERT_GT(job_set.job_set_id, 0);
    const auto workflow_sql =
        "INSERT INTO exec_workflow_instance("
        "workflow_instance_id,workflow_kind,state,root_scope_kind,"
        "created_by,created_at_utc) VALUES(36000,'BATCH_TEST','RUNNING',"
        "'manual','test',1);"
        "INSERT INTO exec_workflow_step("
        "workflow_step_id,workflow_instance_id,step_key,step_kind,state,"
        "job_set_id,priority,attempts,max_attempts,created_at_utc) VALUES("
        "36001,36000,'Batch','batch.persistence','MATERIALIZED',"
        + std::to_string(job_set.job_set_id)
        + ",0,0,1,1);";
    ASSERT_TRUE(ExecSql(db_, workflow_sql.c_str()));

    std::vector<std::int64_t> job_ids;
    for (int index = 0; index < 33; ++index) {
        CreatePendingJobReceipt job{};
        ASSERT_TRUE(execution_db.CreatePendingJob(
            {
                .job_set_id = job_set.job_set_id,
                .program_kind = 42,
                .program_version = 1,
                .program_ref_kind = "batch",
                .program_ref_id = index + 1,
                .fingerprint = "batch-job-" + std::to_string(index),
                .priority = 0,
                .max_attempts = 2,
                .input_ini = "[batch]",
            },
            &job,
            &error)) << error;
        ASSERT_EQ(job.disposition, ExecutionDbOperationDisposition::Applied);
        job_ids.push_back(job.job_id);
    }
    SealJobPopulationReceipt seal{};
    ASSERT_TRUE(execution_db.SealJobPopulation(
        {
            .job_set_id = job_set.job_set_id,
            .expected_job_count = 33,
            .requested_by = "test",
        },
        &seal,
        &error)) << error;

    const auto availability_before =
        execution_db.GetExecutionWorkAvailability(&error);
    ASSERT_TRUE(availability_before.has_value()) << error;
    savor::db::ExecutionWorksetObservationBindingV1 observation;
    std::optional<savor::runtime::WorksetCaptureBindingV1> capture;
    savor::runtime::progress::ProgressPlanV1 progress;
    ASSERT_TRUE(savor::runtime::EncodeWorksetCaptureBindingV1(
        capture, observation.capture_binding_payload));
    ASSERT_TRUE(savor::runtime::EncodeProgressPlanV1(
        progress, observation.progress_plan_payload));
    observation.capture_binding_sha256 =
        savor::runtime::EmptyWorksetCaptureBindingHashV1();
    observation.progress_plan_sha256 = progress.content_sha256;
    savor::db::ExecutionWorksetDerivedStateBindingV1 derived_state;
    const savor::runtime::derived::WorksetDerivedStateBindingV1
        empty_derived_state;
    ASSERT_TRUE(savor::runtime::EncodeWorksetDerivedStateBindingV1(
        empty_derived_state, derived_state.binding_payload));
    derived_state.binding_sha256 = empty_derived_state.content_sha256;
    PublishWorksetCommand workset{
        .job_set_id = job_set.job_set_id,
        .workflow_step_id = 36001,
        .workset_key = "batch-persistence-workset",
        .program_kind = 42,
        .program_version = 1,
        .contract = {
            .contract_key = "batch-persistence-compatibility",
            .module_canonical_id = "batch.persistence.module",
            .module_version = 1,
            .module_sha256 = std::string(64, '1'),
            .entrypoint = "execute",
            .verified_dependency_sha256 = std::string(64, '2'),
            .runtime_profile_sha256 = std::string(64, '3'),
            .program_package_sha256 = std::string(64, '4'),
            .estimated_payload_bytes = 33,
        },
        .derived_state = derived_state,
        .observation = observation,
        .priority = 0,
        .ordered_job_ids = job_ids,
        .requested_by = "test",
    };
    PublishWorksetWaveReceipt wave{};
    ASSERT_TRUE(execution_db.PublishWorksetWave(
        {
            .job_set_id = job_set.job_set_id,
            .expected_job_count = 33,
            .worksets = {workset},
            .requested_by = "test",
        },
        &wave,
        &error)) << error;
    ASSERT_EQ(wave.disposition, ExecutionDbOperationDisposition::Applied);
    ASSERT_EQ(wave.worksets.size(), 1u);
    EXPECT_TRUE(wave.ready_workset_availability_changed);
    const auto availability_after =
        execution_db.GetExecutionWorkAvailability(&error);
    ASSERT_TRUE(availability_after.has_value()) << error;
    EXPECT_EQ(
        availability_after->generation,
        availability_before->generation + 1);

    PublishWorksetWaveReceipt replay{};
    ASSERT_TRUE(execution_db.PublishWorksetWave(
        {
            .job_set_id = job_set.job_set_id,
            .expected_job_count = 33,
            .worksets = {workset},
            .requested_by = "test",
        },
        &replay,
        &error)) << error;
    EXPECT_EQ(
        replay.disposition,
        ExecutionDbOperationDisposition::AlreadyApplied);
    EXPECT_FALSE(replay.ready_workset_availability_changed);

    auto conflicting_workset = workset;
    conflicting_workset.observation.progress_plan_sha256 =
        std::string(64, 'f');
    PublishWorksetWaveReceipt conflict{};
    ASSERT_TRUE(execution_db.PublishWorksetWave(
        {
            .job_set_id = job_set.job_set_id,
            .expected_job_count = 33,
            .worksets = {conflicting_workset},
            .requested_by = "test",
        },
        &conflict,
        &error)) << error;
    EXPECT_EQ(
        conflict.disposition,
        ExecutionDbOperationDisposition::Conflict);

    const auto claimed_worksets = execution_db.ClaimPublishedWorksetBatch(
        {
            .batch_nonce = "batch-persistence-claim",
            .requested_workset_count = 1,
        },
        &error);
    ASSERT_TRUE(error.empty()) << error;
    ASSERT_EQ(claimed_worksets.size(), 1u);
    ASSERT_EQ(claimed_worksets.front().items.size(), 33u);
    EXPECT_EQ(
        claimed_worksets.front().observation.capture_binding_payload,
        observation.capture_binding_payload);
    EXPECT_EQ(
        claimed_worksets.front().observation.capture_binding_sha256,
        observation.capture_binding_sha256);
    EXPECT_EQ(
        claimed_worksets.front().observation.progress_plan_payload,
        observation.progress_plan_payload);
    EXPECT_EQ(
        claimed_worksets.front().observation.progress_plan_sha256,
        observation.progress_plan_sha256);
    WorksetDispatchMutationReceipt dispatched{};
    ASSERT_TRUE(execution_db.MarkWorksetActive(
        {
            .dispatch_attempt_id =
                claimed_worksets.front().dispatch_attempt_id,
            .claim_token = claimed_worksets.front().claim_token,
            .lease_duration_ms = 180000,
            .requested_by = "test",
        },
        &dispatched,
        &error)) << error;

    const auto terminal_command = [&](std::size_t index, bool unstarted) {
        const auto& item = claimed_worksets.front().items[index];
        const auto sha = std::string(63, 'a')
            + "0123456789abcdef"[index % 16];
        return StageWorkerTerminalCommand{
            .dispatch_attempt_id =
                claimed_worksets.front().dispatch_attempt_id,
            .claim_token = claimed_worksets.front().claim_token,
            .job_id = item.job_id,
            .workset_item_ordinal = item.workset_item_ordinal,
            .reserved_attempt_id = item.reserved_attempt_id,
            .terminal_status = "SUCCEEDED",
            .terminal_fingerprint = sha,
            .terminal_id = std::to_string(index + 1),
            .unstarted = unstarted,
            .result_blob = {
                .relative_path = "worker_results/batch-"
                    + std::to_string(index) + ".bin",
                .sha256 = sha,
                .size_bytes = 1,
                .format = "test.batch.v1",
            },
            .requested_by = "test",
        };
    };

    PersistWorkerExecutionEventsBatchCommand first_batch;
    const auto& first_item = claimed_worksets.front().items.front();
    const MarkWorksetJobStartedCommand valid_start{
        .dispatch_attempt_id = claimed_worksets.front().dispatch_attempt_id,
        .claim_token = claimed_worksets.front().claim_token,
        .job_id = first_item.job_id,
        .workset_item_ordinal = first_item.workset_item_ordinal,
        .reserved_attempt_id = first_item.reserved_attempt_id,
        .worker_invocation_id = "batch-first-invocation",
        .requested_by = "test",
    };
    auto invalid_start = valid_start;
    invalid_start.job_id = 0;
    PersistWorkerExecutionEventsBatchReceipt rolled_back;
    EXPECT_FALSE(execution_db.PersistWorkerExecutionEventsBatch(
        {.events = {valid_start, invalid_start}},
        &rolled_back,
        &error));
    const auto rolled_back_state_sql =
        "SELECT state FROM exec_job WHERE job_id="
        + std::to_string(first_item.job_id) + ";";
    EXPECT_EQ(ReadText(db_, rolled_back_state_sql.c_str()),
        "CLAIMED");
    error.clear();

    auto stale_start = valid_start;
    stale_start.claim_token = "stale-token";
    PersistWorkerExecutionEventsBatchReceipt isolated;
    ASSERT_TRUE(execution_db.PersistWorkerExecutionEventsBatch(
        {.events = {stale_start, valid_start}},
        &isolated,
        &error)) << error;
    ASSERT_EQ(isolated.events.size(), 2u);
    EXPECT_EQ(
        std::get<WorksetJobStartReceipt>(isolated.events[0]).disposition,
        ExecutionDbOperationDisposition::TokenMismatch);
    EXPECT_EQ(
        std::get<WorksetJobStartReceipt>(isolated.events[1]).disposition,
        ExecutionDbOperationDisposition::Applied);

    first_batch.events.emplace_back(valid_start);
    first_batch.events.emplace_back(terminal_command(0, false));
    for (std::size_t index = 1; index < 31; ++index) {
        first_batch.events.emplace_back(terminal_command(index, true));
    }
    ASSERT_EQ(first_batch.events.size(), 32u);
    PersistWorkerExecutionEventsBatchReceipt first_receipt;
    ASSERT_TRUE(execution_db.PersistWorkerExecutionEventsBatch(
        first_batch,
        &first_receipt,
        &error)) << error;
    ASSERT_EQ(first_receipt.events.size(), 32u);
    EXPECT_TRUE(std::holds_alternative<WorksetJobStartReceipt>(
        first_receipt.events.front()));
    EXPECT_TRUE(std::holds_alternative<StageWorkerTerminalReceipt>(
        first_receipt.events[1]));

    WorksetDispatchMutationReceipt draining{};
    ASSERT_TRUE(execution_db.MarkWorksetDraining(
        {
            .dispatch_attempt_id =
                claimed_worksets.front().dispatch_attempt_id,
            .claim_token = claimed_worksets.front().claim_token,
            .requested_by = "test-worker-terminal-state",
        },
        &draining,
        &error)) << error;
    EXPECT_EQ(
        draining.disposition,
        ExecutionDbOperationDisposition::Applied);
    EXPECT_FALSE(draining.dispatch_closed);
    EXPECT_EQ(
        ReadText(
            db_,
            ("SELECT state FROM exec_workset_dispatch_attempt WHERE dispatch_attempt_id="
                + std::to_string(
                    claimed_worksets.front().dispatch_attempt_id)
                + ";").c_str()),
        "DRAINING");
    EXPECT_EQ(
        ReadInt64(
            db_,
            ("SELECT COUNT(1) FROM exec_workset_dispatch_attempt WHERE dispatch_attempt_id="
                + std::to_string(
                    claimed_worksets.front().dispatch_attempt_id)
                + " AND lease_expires_at_utc IS NULL AND draining_at_utc IS NOT NULL;").c_str()),
        1);

    PersistWorkerExecutionEventsBatchReceipt second_receipt;
    ASSERT_TRUE(execution_db.PersistWorkerExecutionEventsBatch(
        {.events = {terminal_command(31, true), terminal_command(32, true)}},
        &second_receipt,
        &error)) << error;
    ASSERT_EQ(second_receipt.events.size(), 2u);
    EXPECT_EQ(
        ReadText(
            db_,
            ("SELECT state FROM exec_workset_dispatch_attempt WHERE dispatch_attempt_id="
                + std::to_string(
                    claimed_worksets.front().dispatch_attempt_id)
                + ";").c_str()),
        "CLOSED");
    const auto finished_count_sql =
        "SELECT COUNT(1) FROM exec_job "
        "WHERE job_set_id=" + std::to_string(job_set.job_set_id)
        + " AND state='EXECUTION_FINISHED';";
    EXPECT_EQ(ReadInt64(db_, finished_count_sql.c_str()),
        33);
    const auto terminal_availability =
        execution_db.GetExecutionWorkAvailability(&error);
    ASSERT_TRUE(terminal_availability.has_value()) << error;
    EXPECT_TRUE(terminal_availability->has_execution_finished_results);

    const auto result_claims =
        execution_db.ClaimExecutionFinishedJobsBatch(
            {
                .requested_job_count = 32,
            },
            &error);
    ASSERT_TRUE(error.empty()) << error;
    ASSERT_EQ(result_claims.size(), 32u);
    const auto one_result_remaining =
        execution_db.GetExecutionWorkAvailability(&error);
    ASSERT_TRUE(one_result_remaining.has_value()) << error;
    EXPECT_TRUE(one_result_remaining->has_execution_finished_results);
    for (std::size_t index = 1; index < result_claims.size(); ++index) {
        EXPECT_LT(
            result_claims[index - 1].job.job_id,
            result_claims[index].job.job_id);
    }
    CommitResultFinalizationsBatchCommand finalizations;
    for (std::size_t index = 0; index < 2; ++index) {
        CommitResultFinalizationCommand finalization{
            .job_id = result_claims[index].job.job_id,
            .disposition =
                ExecutionResultFinalizationDisposition::Final,
            .final_state = "SUCCEEDED",
            .requested_by = "test",
        };
        if (index == 0) {
            finalization.cancellation_requests.push_back({
                .job_id = result_claims[2].job.job_id,
                .request_key = "batch-finalization-cancel",
                .reason_code = "TEST_WINNER",
                .reason_text = "committed with result finalization",
                .requested_by = "test",
                .caused_by_job_id = result_claims[0].job.job_id,
                .terminal_disposition = "AUTOMATIC_SUPERSESSION",
            });
        }
        finalizations.finalizations.push_back(std::move(finalization));
    }
    std::vector<ResultProcessingReceipt> finalization_receipts;
    ASSERT_TRUE(execution_db.CommitResultFinalizationsBatch(
        finalizations,
        &finalization_receipts,
        &error)) << error;
    ASSERT_EQ(finalization_receipts.size(), 2u);
    EXPECT_EQ(
        finalization_receipts[0].disposition,
        ExecutionDbOperationDisposition::Applied);
    EXPECT_EQ(
        finalization_receipts[1].disposition,
        ExecutionDbOperationDisposition::Applied);
    ASSERT_EQ(
        finalization_receipts[0].committed_cancellations.size(),
        1u);
    const auto& committed_cancellation =
        finalization_receipts[0].committed_cancellations.front();
    EXPECT_EQ(
        committed_cancellation.job_id,
        result_claims[2].job.job_id);
    EXPECT_EQ(committed_cancellation.reason_code, "TEST_WINNER");
    EXPECT_EQ(
        committed_cancellation.durable_job_state,
        "EXECUTION_FINISHED");
    EXPECT_EQ(
        committed_cancellation.dispatch_attempt_id,
        claimed_worksets.front().dispatch_attempt_id);
    EXPECT_EQ(
        committed_cancellation.claim_token,
        claimed_worksets.front().claim_token);
    EXPECT_EQ(
        committed_cancellation.disposition,
        ExecutionDbOperationDisposition::Applied);

    std::vector<ResultProcessingReceipt> replay_receipts;
    ASSERT_TRUE(execution_db.CommitResultFinalizationsBatch(
        {.finalizations = {finalizations.finalizations.front()}},
        &replay_receipts,
        &error)) << error;
    ASSERT_EQ(replay_receipts.size(), 1u);
    EXPECT_EQ(
        replay_receipts.front().disposition,
        ExecutionDbOperationDisposition::AlreadyApplied);
    ASSERT_EQ(
        replay_receipts.front().committed_cancellations.size(),
        1u);
    EXPECT_EQ(
        replay_receipts.front().committed_cancellations.front()
            .disposition,
        ExecutionDbOperationDisposition::AlreadyApplied);
    const auto final_claim =
        execution_db.ClaimExecutionFinishedJobsBatch(
            {
                .requested_job_count = 32,
            },
            &error);
    ASSERT_TRUE(error.empty()) << error;
    ASSERT_EQ(final_claim.size(), 1u);
    const auto no_results_remaining =
        execution_db.GetExecutionWorkAvailability(&error);
    ASSERT_TRUE(no_results_remaining.has_value()) << error;
    EXPECT_FALSE(no_results_remaining->has_execution_finished_results);

    std::vector<ResultProcessingReceipt> failure_receipts;
    ASSERT_TRUE(execution_db.CommitResultFinalizationsBatch(
        {.finalizations = {{
            .job_id = result_claims[2].job.job_id,
            .disposition = ExecutionResultFinalizationDisposition::Final,
            .final_state = "FAILED",
            .error_code = "TEST_RESULT_CANARY",
            .error_text = "visible generic result failure",
            .requested_by = "test",
        }}},
        &failure_receipts,
        &error)) << error;
    ASSERT_EQ(failure_receipts.size(), 1u);
    EXPECT_EQ(failure_receipts.front().processing_state, "PROCESSED");
    EXPECT_EQ(ReadText(
        db_,
        ("SELECT state || ':' || error_code "
            "FROM exec_job WHERE job_id="
            + std::to_string(result_claims[2].job.job_id) + ";").c_str()),
        "FAILED:TEST_RESULT_CANARY");

}

TEST_F(
    SqliteDbFixture,
    DelayedWorksetDrainingClosesAfterAllJobsAreAlreadyTerminal) {
    using namespace savor::db;
    using savor::db::execution::workflow::SqliteExecutionDb;

    ASSERT_TRUE(ExecSql(
        db_,
        "INSERT INTO exec_job_set("
        "job_set_id,program_kind,purpose,created_by,created_at_utc,"
        "expected_total) VALUES(37000,42,'delayed-draining','test',1,1);"
        "INSERT INTO exec_workflow_instance("
        "workflow_instance_id,workflow_kind,state,root_scope_kind,"
        "created_by,created_at_utc) VALUES(37001,'DRAINING_TEST',"
        "'COMPLETED','manual','test',1);"
        "INSERT INTO exec_workflow_step("
        "workflow_step_id,workflow_instance_id,step_key,step_kind,state,"
        "job_set_id,priority,attempts,max_attempts,created_at_utc) VALUES("
        "37002,37001,'Drain','draining.test','COMPLETED',37000,0,1,1,1);"
        "INSERT INTO exec_workset("
        "workset_id,job_set_id,workflow_step_id,root_job_set_id,workset_key,"
        "program_kind,program_version,contract_key,module_canonical_id,"
        "module_version,module_sha256,entrypoint,verified_dependency_sha256,"
        "runtime_profile_sha256,program_package_sha256,"
        "estimated_payload_bytes,priority,item_count,published_at_utc) VALUES("
        "37003,37000,37002,37000,'delayed-draining-workset',42,1,"
        "'delayed-draining-compatibility','delayed.draining.module',1,"
        "'1111111111111111111111111111111111111111111111111111111111111111',"
        "'execute',"
        "'2222222222222222222222222222222222222222222222222222222222222222',"
        "'3333333333333333333333333333333333333333333333333333333333333333',"
        "printf('%064d',0),1,0,1,1);"
        "INSERT INTO exec_workset_dispatch_attempt("
        "dispatch_attempt_id,workset_id,dispatch_sequence,state,claim_token,"
        "lease_expires_at_utc,claimed_at_utc,dispatched_at_utc) VALUES("
        "37004,37003,1,'ACTIVE','delayed-draining-token',1000,1,2);"
        "INSERT INTO exec_job("
        "job_id,job_set_id,program_kind,program_version,program_ref_kind,"
        "program_ref_id,fingerprint,priority,state,attempts,max_attempts,"
        "queued_at_utc,workset_id,workset_item_ordinal,dispatch_attempt_id,"
        "reserved_attempt_id) VALUES("
        "37005,37000,42,1,'test',1,'delayed-draining-job',0,'SUCCEEDED',"
        "1,1,1,37003,0,37004,1);"));

    SqliteExecutionDb execution_db(db_);
    WorksetDispatchMutationReceipt draining{};
    std::string error;
    ASSERT_TRUE(execution_db.MarkWorksetDraining(
        {
            .dispatch_attempt_id = 37004,
            .claim_token = "delayed-draining-token",
            .requested_by = "test-terminal-state-arrived-last",
        },
        &draining,
        &error)) << error;
    EXPECT_EQ(
        draining.disposition,
        ExecutionDbOperationDisposition::Applied);
    EXPECT_TRUE(draining.dispatch_closed);
    EXPECT_EQ(
        ReadText(
            db_,
            "SELECT state FROM exec_workset_dispatch_attempt "
            "WHERE dispatch_attempt_id=37004;"),
        "CLOSED");
    EXPECT_EQ(
        ReadText(
            db_,
            "SELECT close_reason_code FROM exec_workset_dispatch_attempt "
            "WHERE dispatch_attempt_id=37004;"),
        "WORKER_TERMINALS_STAGED");
    EXPECT_EQ(
        ReadInt64(
            db_,
            "SELECT COUNT(1) FROM exec_workset_dispatch_attempt "
            "WHERE dispatch_attempt_id=37004 "
            "AND lease_expires_at_utc IS NULL "
            "AND draining_at_utc IS NOT NULL "
            "AND closed_at_utc IS NOT NULL;"),
        1);
}

TEST_F(
    SqliteDbFixture,
    CancellationStartupLoadAndOutcomeMutationsAreLeaseFree) {
    using namespace savor::db;
    using savor::db::execution::workflow::SqliteExecutionDb;
    ASSERT_TRUE(ExecSql(db_, R"SQL(
INSERT INTO exec_job_set(
    job_set_id,program_kind,purpose,created_by,created_at_utc)
VALUES(36100,42,'cancellation-batch','test',1);
)SQL"));
    SqliteExecutionDb execution_db(db_);
    std::string error;
    for (int index = 0; index < 33; ++index) {
        const auto job_id = 36101 + index;
        const auto request_id = 36201 + index;
        const auto request_key =
            "cancel-batch-" + std::to_string(index);
        const auto insert_job_sql =
            "INSERT INTO exec_job("
            "job_id,job_set_id,program_kind,program_version,program_ref_kind,"
            "program_ref_id,fingerprint,priority,state,attempts,max_attempts,"
            "queued_at_utc,cancellation_request_key,cancellation_state,"
            "cancellation_requested_at_utc) VALUES("
            + std::to_string(job_id)
            + ",36100,42,1,'batch',"
            + std::to_string(index + 1)
            + ",'cancel-batch-" + std::to_string(index)
            + "',0,'QUEUED',0,1,1,'" + request_key
            + "','REQUESTED',1);"
              "INSERT INTO exec_job_cancellation_request("
              "cancellation_request_id,job_id,request_key,reason_code,"
              "requested_by,requested_at_utc,state) VALUES("
            + std::to_string(request_id) + ","
            + std::to_string(job_id) + ",'" + request_key
            + "','TEST','test',1,'REQUESTED');";
        ASSERT_TRUE(ExecSql(db_, insert_job_sql.c_str()));
    }

    const auto claimed =
        execution_db.ListUnresolvedJobCancellations(&error);
    ASSERT_TRUE(error.empty()) << error;
    ASSERT_EQ(claimed.size(), 33u);
    MutateJobCancellationsBatchCommand mutations;
    for (std::size_t index = 0; index < 32; ++index) {
        const auto& cancellation = claimed[index];
        mutations.mutations.push_back({
            .kind = JobCancellationOutcomeKind::ResolveWithoutWorker,
            .cancellation_request_id =
                cancellation.cancellation_request_id,
            .job_id = cancellation.job_id,
            .resolution_code = "WORKFLOW_CANCELED_BEFORE_WORKER",
            .requested_by = "test",
        });
    }
    std::vector<JobCancellationReceipt> receipts;
    ASSERT_TRUE(execution_db.MutateJobCancellationsBatch(
        mutations,
        &receipts,
        &error)) << error;
    ASSERT_EQ(receipts.size(), 32u);
    for (const auto& receipt : receipts) {
        EXPECT_EQ(receipt.disposition, ExecutionDbOperationDisposition::Applied);
        EXPECT_EQ(receipt.state, "RESOLVED");
    }
    EXPECT_EQ(ReadInt64(
        db_,
        "SELECT COUNT(1) FROM exec_job WHERE job_set_id=36100 "
        "AND state='CANCELED';"),
        32);
    EXPECT_EQ(ReadInt64(
        db_,
        "SELECT COUNT(1) FROM exec_job_cancellation_request "
        "WHERE state='REQUESTED';"),
        1);

    const auto last_claim =
        execution_db.ListUnresolvedJobCancellations(&error);
    ASSERT_TRUE(error.empty()) << error;
    ASSERT_EQ(last_claim.size(), 1u);

    std::vector<JobCancellationReceipt> failure_receipts;
    ASSERT_TRUE(execution_db.MutateJobCancellationsBatch(
        {.mutations = {{
            .kind = JobCancellationOutcomeKind::DeliveryFailed,
            .cancellation_request_id =
                last_claim.front().cancellation_request_id,
            .job_id = last_claim.front().job_id,
            .error_code = "TEST_DELIVERY_FAILURE",
            .error_text = "visible cancellation canary",
            .requested_by = "test",
        }}},
        &failure_receipts,
        &error)) << error;
    ASSERT_EQ(failure_receipts.size(), 1u);
    EXPECT_EQ(failure_receipts.front().state, "REQUESTED");
    EXPECT_EQ(ReadText(
        db_,
        "SELECT last_delivery_error_code "
        "FROM exec_job_cancellation_request "
        "WHERE state='REQUESTED';"),
        "TEST_DELIVERY_FAILURE");
    EXPECT_EQ(ReadInt64(
        db_,
        "SELECT delivery_attempts FROM exec_job_cancellation_request "
        "WHERE last_delivery_error_code='TEST_DELIVERY_FAILURE';"),
        1);

    ASSERT_TRUE(ExecSql(
        db_,
        ("UPDATE exec_job SET state='EXECUTION_FINISHED' WHERE job_id="
            + std::to_string(last_claim.front().job_id) + ";").c_str()));
    std::vector<JobCancellationReceipt> terminal_receipts;
    ASSERT_TRUE(execution_db.MutateJobCancellationsBatch(
        {.mutations = {{
            .kind = JobCancellationOutcomeKind::WorkerTerminalResolved,
            .cancellation_request_id =
                last_claim.front().cancellation_request_id,
            .job_id = last_claim.front().job_id,
            .resolution_code = "WORKER_TERMINAL",
            .requested_by = "test",
        }}},
        &terminal_receipts,
        &error)) << error;
    ASSERT_EQ(terminal_receipts.size(), 1u);
    EXPECT_EQ(terminal_receipts.front().state, "RESOLVED");
    EXPECT_EQ(ReadText(
        db_,
        ("SELECT cancellation_state FROM exec_job WHERE job_id="
            + std::to_string(last_claim.front().job_id) + ";").c_str()),
        "RESOLVED");
    EXPECT_EQ(ReadText(
        db_,
        ("SELECT cancellation_resolution_code FROM exec_job WHERE job_id="
            + std::to_string(last_claim.front().job_id) + ";").c_str()),
        "WORKER_TERMINAL");
}

TEST_F(
    SqliteDbFixture,
    PartialCancellationNeverReleasesClaimedWorksetsAndResolvesAllSixtyFour) {
    using namespace savor::db;
    using savor::db::execution::workflow::SqliteExecutionDb;

    ASSERT_TRUE(ExecSql(db_, R"SQL(
INSERT INTO exec_job_set(
    job_set_id,program_kind,purpose,created_by,created_at_utc)
VALUES(36300,42,'four-canceled-worksets','test',1);
INSERT INTO exec_workflow_instance(
    workflow_instance_id,workflow_kind,state,root_scope_kind,
    root_scope_id,created_by,created_at_utc)
VALUES(36301,'TEST','RUNNING','job_set',36300,'test',1);
INSERT INTO exec_workflow_step(
    workflow_step_id,workflow_instance_id,step_key,graph_node_key,
    step_kind,state,priority,attempts,max_attempts,job_set_id,
    created_at_utc)
VALUES(36302,36301,'probe','probe','test','MATERIALIZED',
    0,1,1,36300,1);
)SQL"));

    std::vector<JobCancellationOutcomeCommand> first_per_workset;
    std::vector<JobCancellationOutcomeCommand> remaining;
    for (int workset_index = 0; workset_index < 4; ++workset_index) {
        const auto workset_id = 36310 + workset_index;
        const auto dispatch_id = 36320 + workset_index;
        const auto workset_sql =
            "INSERT INTO exec_workset("
            "workset_id,job_set_id,workflow_step_id,root_job_set_id,"
            "workset_key,program_kind,program_version,contract_key,"
            "module_canonical_id,module_version,module_sha256,entrypoint,"
            "verified_dependency_sha256,runtime_profile_sha256,"
            "program_package_sha256,estimated_payload_bytes,priority,"
            "item_count,published_at_utc) VALUES("
            + std::to_string(workset_id)
            + ",36300,36302,36300,'cancel-workset-"
            + std::to_string(workset_index)
            + "',42,1,'generic','test.module',1,printf('%064d',0),"
              "'test-entrypoint',printf('%064d',0),printf('%064d',0),"
              "printf('%064d',0),1,0,16,1);"
              "INSERT INTO exec_workset_dispatch_attempt("
              "dispatch_attempt_id,workset_id,dispatch_sequence,state,"
              "claim_token,claimed_at_utc) VALUES("
            + std::to_string(dispatch_id) + ","
            + std::to_string(workset_id)
            + ",1,'CLAIMED','cancel-token-"
            + std::to_string(workset_index) + "',1);";
        ASSERT_TRUE(ExecSql(db_, workset_sql.c_str()));

        for (int item_index = 0; item_index < 16; ++item_index) {
            const auto flat_index = workset_index * 16 + item_index;
            const auto job_id = 36400 + flat_index;
            const auto request_id = 36500 + flat_index;
            const auto request_key =
                "four-workset-cancel-" + std::to_string(flat_index);
            const auto job_sql =
                "INSERT INTO exec_job("
                "job_id,job_set_id,program_kind,program_version,"
                "program_ref_kind,program_ref_id,fingerprint,priority,"
                "state,attempts,max_attempts,queued_at_utc,workset_id,"
                "workset_item_ordinal,dispatch_attempt_id,"
                "reserved_attempt_id,"
                "claimed_by_token,cancellation_request_key,"
                "cancellation_state,cancellation_requested_at_utc) VALUES("
                + std::to_string(job_id)
                + ",36300,42,1,'test',"
                + std::to_string(flat_index + 1)
                + ",'four-workset-job-" + std::to_string(flat_index)
                + "',0,'CLAIMED',1,1,1,"
                + std::to_string(workset_id) + ","
                + std::to_string(item_index) + ","
                + std::to_string(dispatch_id) + ",1,'cancel-token-"
                + std::to_string(workset_index) + "','" + request_key
                + "','REQUESTED',1);"
                  "INSERT INTO exec_job_cancellation_request("
                  "cancellation_request_id,job_id,request_key,reason_code,"
                  "requested_by,requested_at_utc,state) VALUES("
                + std::to_string(request_id) + ","
                + std::to_string(job_id) + ",'" + request_key
                + "','TEST','test',1,'REQUESTED');";
            ASSERT_TRUE(ExecSql(db_, job_sql.c_str()));
            JobCancellationOutcomeCommand mutation{
                .kind = JobCancellationOutcomeKind::InitialSidecarApplied,
                .cancellation_request_id = request_id,
                .job_id = job_id,
                .dispatch_attempt_id = dispatch_id,
                .claim_token = "cancel-token-"
                    + std::to_string(workset_index),
                .resolution_code = "INITIAL_SIDECAR_APPLIED",
                .requested_by = "test",
            };
            if (item_index == 0)
                first_per_workset.push_back(std::move(mutation));
            else
                remaining.push_back(std::move(mutation));
        }
    }

    SqliteExecutionDb execution_db(db_);
    std::string error;
    std::vector<JobCancellationReceipt> receipts;
    ASSERT_TRUE(execution_db.MutateJobCancellationsBatch(
        {.mutations = first_per_workset},
        &receipts,
        &error)) << error;
    ASSERT_EQ(receipts.size(), 4u);
    EXPECT_EQ(ReadInt64(
        db_,
        "SELECT COUNT(1) FROM exec_workset_dispatch_attempt "
        "WHERE state='CLAIMED';"),
        4);
    EXPECT_EQ(ReadInt64(
        db_,
        "SELECT COUNT(1) FROM exec_job_cancellation_request "
        "WHERE state='REQUESTED';"),
        60);

    receipts.clear();
    ASSERT_TRUE(execution_db.MutateJobCancellationsBatch(
        {.mutations = remaining},
        &receipts,
        &error)) << error;
    ASSERT_EQ(receipts.size(), 60u);
    EXPECT_EQ(ReadInt64(
        db_,
        "SELECT COUNT(1) FROM exec_job_cancellation_request "
        "WHERE state='RESOLVED';"),
        64);
    EXPECT_EQ(ReadInt64(
        db_,
        "SELECT COUNT(1) FROM exec_job WHERE state='CANCELED';"),
        64);
    EXPECT_EQ(ReadInt64(
        db_,
        "SELECT COUNT(1) FROM exec_workset_dispatch_attempt "
        "WHERE state='CLOSED' AND close_reason_code='ALL_ITEMS_CANCELED';"),
        4);
}

TEST_F(
    SqliteDbFixture,
    InterruptedWorksetRecoveryRequeuesAndMakesWorksetClaimableAgain) {
    using savor::db::ClaimPublishedWorksetBatchCommand;
    using savor::db::execution::workflow::SqliteExecutionDb;

    ASSERT_TRUE(ExecSql(db_, R"SQL(
INSERT INTO exec_job_set(
    job_set_id,program_kind,purpose,created_by,created_at_utc)
VALUES(35110,42,'expired-recovery','test',1000);

INSERT INTO exec_workflow_instance(
    workflow_instance_id,workflow_kind,state,root_scope_kind,
    root_scope_id,created_by,created_at_utc)
VALUES(
    35111,'TEST','RUNNING','job_set',35110,'test',1000);

INSERT INTO exec_workflow_step(
    workflow_step_id,workflow_instance_id,step_key,graph_node_key,
    step_kind,state,priority,attempts,max_attempts,job_set_id,
    created_at_utc)
VALUES(
    35112,35111,'probe','probe','test','MATERIALIZED',
    0,1,1,35110,1000);

INSERT INTO exec_workset(
    workset_id,job_set_id,workflow_step_id,root_job_set_id,
    workset_key,program_kind,program_version,
    contract_key,module_canonical_id,module_version,module_sha256,
    entrypoint,verified_dependency_sha256,runtime_profile_sha256,
    program_package_sha256,estimated_payload_bytes,priority,item_count,
    published_at_utc)
VALUES(
    35120,35110,35112,35110,'expired-recovery-workset',42,1,
    'expired-recovery-compatibility','test.module',1,printf('%064d',0),
    'test-entrypoint',printf('%064d',0),printf('%064d',0),
    printf('%064d',0),1,0,1,1000);

INSERT INTO exec_workset_dispatch_attempt(
    dispatch_attempt_id,workset_id,dispatch_sequence,state,claim_token,
    lease_expires_at_utc,claimed_at_utc,dispatched_at_utc)
VALUES(
    35130,35120,1,'ACTIVE','expired-recovery-token',
    1,1000,1000);

INSERT INTO exec_job(
    job_id,job_set_id,program_kind,program_version,program_ref_kind,
    program_ref_id,fingerprint,priority,state,attempts,max_attempts,
    claimed_by_token,lease_expires_at_utc,queued_at_utc,started_at_utc,
    workset_id,workset_item_ordinal,dispatch_attempt_id,
    reserved_attempt_id)
VALUES
(
    35140,35110,42,1,'test',1,'expired-recovery-job',0,
    'RUNNING',1,1,'expired-recovery-token',1,1000,1000,
    35120,0,35130,1),
(
    35141,35110,42,1,'test',2,'expired-recovery-unstarted',0,
    'CLAIMED',0,1,'expired-recovery-token',1,1000,NULL,
    35120,1,35130,1);
)SQL"));

    SqliteExecutionDb execution_db(db_);
    std::string error;
    const auto before_recovery_signal =
        execution_db.GetExecutionWorkAvailability(&error);
    ASSERT_TRUE(before_recovery_signal.has_value()) << error;
    EXPECT_FALSE(before_recovery_signal->has_ready_worksets);
    savor::db::RecoverInterruptedWorksetDispatchesReceipt recovery{};
    ASSERT_TRUE(execution_db.RecoverInterruptedWorksetDispatches(
        &recovery,
        &error))
        << error;
    ASSERT_EQ(recovery.dispatches_closed, 1);
    ASSERT_EQ(recovery.jobs_interrupted, 1);
    ASSERT_EQ(recovery.jobs_requeued, 1);
    ASSERT_EQ(recovery.created_worksets, 1);
    const auto after_recovery_signal =
        execution_db.GetExecutionWorkAvailability(&error);
    ASSERT_TRUE(after_recovery_signal.has_value()) << error;
    EXPECT_TRUE(after_recovery_signal->has_ready_worksets);
    EXPECT_GT(
        after_recovery_signal->generation,
        before_recovery_signal->generation);
    EXPECT_EQ(
        ReadText(
            db_,
            "SELECT state FROM exec_job WHERE job_id=35140;"),
        "INTERRUPTED");
    EXPECT_EQ(
        ReadText(
            db_,
            "SELECT state FROM exec_workset_dispatch_attempt "
            "WHERE dispatch_attempt_id=35130;"),
        "CLOSED");
    EXPECT_EQ(
        ReadInt64(
            db_,
            "SELECT COUNT(1) FROM exec_job WHERE job_id=35140 "
            "AND claimed_by_token IS NULL "
            "AND dispatch_attempt_id IS NULL "
            "AND workset_id=35120;"),
        1);
    const auto replacement_workset_id = ReadInt64(
        db_, "SELECT workset_id FROM exec_job WHERE job_id=35141;");
    EXPECT_NE(replacement_workset_id, 35120);
    EXPECT_EQ(
        ReadInt64(
            db_,
            "SELECT COUNT(1) FROM exec_job WHERE job_id=35141 "
            "AND state='QUEUED' AND attempts=0 "
            "AND claimed_by_token IS NULL "
            "AND dispatch_attempt_id IS NULL "
            "AND workset_item_ordinal=0;"),
        1);
    const auto claimed = execution_db.ClaimPublishedWorksetBatch(
        {
            .batch_nonce = "restart-replacement-claim",
            .requested_workset_count = 1,
        },
        &error);
    ASSERT_TRUE(error.empty()) << error;
    ASSERT_EQ(claimed.size(), 1u);
    ASSERT_EQ(claimed.front().workset_id, replacement_workset_id);
    ASSERT_EQ(claimed.front().items.size(), 1u);
    EXPECT_EQ(claimed.front().items.front().job_id, 35141);
    EXPECT_EQ(claimed.front().items.front().workset_item_ordinal, 0u);
}

TEST_F(
    SqliteDbFixture,
    PublishedWorksetBatchClaimUsesGlobalPriorityOrderAndRollsBackAsAUnit) {
    using savor::db::ClaimPublishedWorksetBatchCommand;
    using savor::db::execution::workflow::SqliteExecutionDb;

    ASSERT_TRUE(ExecSql(db_, R"SQL(
INSERT INTO exec_job_set(
    job_set_id,program_kind,purpose,created_by,created_at_utc)
VALUES
    (35210,42,'batch-claim','test',1000),
    (35310,42,'batch-rollback','test',1000);
INSERT INTO exec_workflow_instance(
    workflow_instance_id,workflow_kind,state,root_scope_kind,
    root_scope_id,created_by,created_at_utc)
VALUES
    (35211,'TEST','RUNNING','job_set',35210,'test',1000),
    (35311,'TEST','RUNNING','job_set',35310,'test',1000);
INSERT INTO exec_workflow_step(
    workflow_step_id,workflow_instance_id,step_key,graph_node_key,
    step_kind,state,priority,attempts,max_attempts,job_set_id,
    created_at_utc)
VALUES
    (35212,35211,'probe','probe','test','MATERIALIZED',
     0,1,1,35210,1000),
    (35312,35311,'probe','probe','test','MATERIALIZED',
     0,1,1,35310,1000);
INSERT INTO exec_workset(
    workset_id,job_set_id,workflow_step_id,root_job_set_id,
    workset_key,program_kind,program_version,contract_key,
    module_canonical_id,module_version,module_sha256,entrypoint,
    verified_dependency_sha256,runtime_profile_sha256,
    program_package_sha256,execution_affinity_key,
    estimated_payload_bytes,priority,item_count,published_at_utc)
VALUES
    (35220,35210,35212,35210,'cold',42,1,'compat',
     'test.module',1,printf('%064d',0),'test-entrypoint',
     printf('%064d',0),printf('%064d',0),printf('%064d',0),'cold',1,10,1,1000),
    (35221,35210,35212,35210,'warm',42,1,'compat',
     'test.module',1,printf('%064d',0),'test-entrypoint',
     printf('%064d',0),printf('%064d',0),printf('%064d',0),'warm',1,10,1,1001),
    (35222,35210,35212,35210,'incompatible',42,1,'compat',
     'other.module',1,printf('%064d',0),'test-entrypoint',
     printf('%064d',0),printf('%064d',0),printf('%064d',0),'warm',1,10,1,900),
    (35223,35210,35212,35210,'lower',42,1,'compat',
     'test.module',1,printf('%064d',0),'test-entrypoint',
     printf('%064d',0),printf('%064d',0),printf('%064d',0),'warm',1,5,1,800),
    (35320,35310,35312,35310,'rollback-a',42,1,'compat',
     'test.module',1,printf('%064d',0),'test-entrypoint',
     printf('%064d',0),printf('%064d',0),printf('%064d',0),'warm',1,20,1,1000),
    (35321,35310,35312,35310,'rollback-b',42,1,'compat',
     'test.module',1,printf('%064d',0),'test-entrypoint',
     printf('%064d',0),printf('%064d',0),printf('%064d',0),'warm',1,20,1,1001);
INSERT INTO exec_job(
    job_id,job_set_id,program_kind,program_version,program_ref_kind,
    program_ref_id,fingerprint,priority,state,attempts,max_attempts,
    queued_at_utc,workset_id,workset_item_ordinal)
VALUES
    (35240,35210,42,1,'test',1,'cold-job',10,'QUEUED',0,2,1000,35220,0),
    (35241,35210,42,1,'test',1,'warm-job',10,'QUEUED',0,2,1000,35221,0),
    (35242,35210,42,1,'test',1,'other-job',10,'QUEUED',0,2,1000,35222,0),
    (35243,35210,42,1,'test',1,'lower-job',5,'QUEUED',0,2,1000,35223,0),
    (35340,35310,42,1,'test',1,'rollback-a-job',20,'QUEUED',0,2,1000,35320,0),
    (35341,35310,42,1,'test',1,'rollback-b-job',20,'QUEUED',0,2,1000,35321,0);
)SQL"));

    SqliteExecutionDb execution_db(db_);
    std::string error;

    // Priority 20 is currently claimable, so exercise whole-batch rollback
    // before testing the lower priority selection.
    ASSERT_TRUE(ExecSql(db_, R"SQL(
CREATE TRIGGER fail_second_batch_claim
BEFORE UPDATE OF dispatch_attempt_id ON exec_job
WHEN NEW.job_id=35341 AND NEW.dispatch_attempt_id IS NOT NULL
BEGIN
    SELECT RAISE(ABORT,'batch rollback probe');
END;
)SQL"));
    const auto failed = execution_db.ClaimPublishedWorksetBatch(
        {
            .batch_nonce = "rollback-batch",
            .requested_workset_count = 2,
        },
        &error);
    EXPECT_TRUE(failed.empty());
    EXPECT_NE(error.find("batch rollback probe"), std::string::npos)
        << error;
    EXPECT_EQ(
        ReadInt64(
            db_,
            "SELECT COUNT(1) FROM exec_workset_dispatch_attempt "
            "WHERE workset_id IN (35320,35321);"),
        0);
    EXPECT_EQ(
        ReadInt64(
            db_,
            "SELECT COUNT(1) FROM exec_job "
            "WHERE job_id IN (35340,35341) AND state='QUEUED';"),
        2);
    ASSERT_TRUE(ExecSql(db_, R"SQL(
DROP TRIGGER fail_second_batch_claim;
UPDATE exec_job SET attempts=max_attempts WHERE job_id IN (35340,35341);
)SQL"));

    error.clear();
    const auto claimed = execution_db.ClaimPublishedWorksetBatch(
        {
            .batch_nonce = "ordered-batch",
            .requested_workset_count = 2,
        },
        &error);
    ASSERT_EQ(claimed.size(), 2u) << error;
    EXPECT_EQ(claimed[0].workset_id, 35222);
    EXPECT_EQ(claimed[1].workset_id, 35220);
    EXPECT_EQ(claimed[0].claim_token, "ordered-batch-1");
    EXPECT_EQ(claimed[1].claim_token, "ordered-batch-2");
    ASSERT_EQ(claimed[0].items.size(), 1u);
    ASSERT_EQ(claimed[1].items.size(), 1u);
    EXPECT_EQ(claimed[0].items[0].workset_item_ordinal, 0u);
    EXPECT_EQ(claimed[1].items[0].workset_item_ordinal, 0u);
    EXPECT_EQ(
        ReadInt64(
            db_,
            "SELECT COUNT(1) FROM exec_job "
            "WHERE job_id IN (35240,35242) AND state='CLAIMED';"),
        2);
    EXPECT_EQ(
        ReadText(
            db_,
            "SELECT state FROM exec_job WHERE job_id=35243;"),
        "QUEUED");
}

TEST_F(
    SqliteDbFixture,
    WorksetJobReorganizationReusesFailedJobIdsInFreshWorkset) {
    using savor::db::ClaimPublishedWorksetBatchCommand;
    using savor::db::ReorganizedWorksetPlanEntry;
    using savor::db::WorksetJobReorganizationPlan;
    using savor::db::WorksetJobReorganizationReceipt;
    using savor::db::execution::workflow::SqliteExecutionDb;

    ASSERT_TRUE(ExecSql(db_, R"SQL(
INSERT INTO exec_job_set(
    job_set_id,program_kind,purpose,created_by,created_at_utc)
VALUES(39010,42,'manual-retry','test',1);
INSERT INTO exec_workflow_instance(
    workflow_instance_id,workflow_kind,state,root_scope_kind,
    root_scope_id,created_by,created_at_utc)
VALUES(39011,'TEST','FAILED','job_set',39010,'test',1);
INSERT INTO exec_workflow_step(
    workflow_step_id,workflow_instance_id,step_key,graph_node_key,
    step_kind,state,priority,attempts,max_attempts,job_set_id,
    created_at_utc)
VALUES(39012,39011,'probe','probe','test','FAILED',0,1,1,39010,1);
INSERT INTO exec_workset(
    workset_id,job_set_id,workflow_step_id,root_job_set_id,
    workset_key,program_kind,program_version,contract_key,
    module_canonical_id,module_version,module_sha256,entrypoint,
    verified_dependency_sha256,runtime_profile_sha256,
    program_package_sha256,estimated_payload_bytes,priority,item_count,
    published_at_utc)
VALUES
    (39020,39010,39012,39010,'source-a',42,1,'retry-contract',
     'test.module',1,printf('%064d',0),'test-entrypoint',
     printf('%064d',0),printf('%064d',0),printf('%064d',0),2,3,1,1),
    (39021,39010,39012,39010,'source-b',42,1,'retry-contract',
     'test.module',1,printf('%064d',0),'test-entrypoint',
     printf('%064d',0),printf('%064d',0),printf('%064d',0),2,5,1,1);
INSERT INTO exec_job(
    job_id,job_set_id,program_kind,program_version,program_ref_kind,
    program_ref_id,fingerprint,priority,state,attempts,max_attempts,
    queued_at_utc,workset_id,workset_item_ordinal,result_processing_state)
VALUES
    (39030,39010,42,1,'test',1,'retry-a',3,'FAILED',1,1,1,39020,0,'PROCESSED'),
    (39031,39010,42,1,'test',2,'retry-b',5,'FAILED',1,1,1,39021,0,'PROCESSED');
)SQL"));

    ReorganizedWorksetPlanEntry entry{};
    entry.workflow_step_id = 39012;
    entry.job_set_id = 39010;
    entry.job_set_id = 39010;
    entry.program_kind = 42;
    entry.program_version = 1;
    entry.contract = {
        .contract_key = "retry-contract",
        .module_canonical_id = "test.module",
        .module_version = 1,
        .module_sha256 = std::string(64, '0'),
        .entrypoint = "test-entrypoint",
        .verified_dependency_sha256 = std::string(64, '0'),
        .runtime_profile_sha256 = std::string(64, '0'),
        .program_package_sha256 = std::string(64, '0'),
    };
    entry.derived_state.binding_payload = {1};
    entry.derived_state.binding_sha256 = std::string(64, '1');
    entry.priority = 5;
    entry.estimated_payload_bytes = 4;
    entry.ordered_job_ids = {39030, 39031};

    SqliteExecutionDb execution_db(db_);
    WorksetJobReorganizationReceipt receipt{};
    std::string error;
    ASSERT_TRUE(execution_db.ApplyWorksetJobReorganization(
        WorksetJobReorganizationPlan{
            .workflow_instance_id = 39011,
            .worksets = {entry},
            .requested_by = "test",
        },
        &receipt,
        &error)) << error;
    ASSERT_EQ(receipt.requeued_job_count, 2);
    ASSERT_EQ(receipt.created_workset_count, 1);
    ASSERT_EQ(receipt.workset_ids.size(), 1u);
    EXPECT_EQ(ReadText(db_, "SELECT state FROM exec_job WHERE job_id=39030;"), "QUEUED");
    EXPECT_EQ(ReadInt64(db_, "SELECT max_attempts FROM exec_job WHERE job_id=39030;"), 2);
    EXPECT_EQ(ReadInt64(db_, "SELECT COUNT(1) FROM exec_job WHERE job_id IN (39030,39031);"), 2);
    EXPECT_EQ(ReadInt64(db_, "SELECT COUNT(1) FROM exec_job WHERE workset_id IN (39020,39021);"), 0);
    EXPECT_EQ(ReadText(db_, "SELECT state FROM exec_workflow_instance WHERE workflow_instance_id=39011;"), "RUNNING");
    EXPECT_EQ(ReadText(db_, "SELECT state FROM exec_workflow_step WHERE workflow_step_id=39012;"), "MATERIALIZED");

    const auto claimed = execution_db.ClaimPublishedWorksetBatch(
        ClaimPublishedWorksetBatchCommand{
            .batch_nonce = "manual-retry",
            .requested_workset_count = 2,
        },
        &error);
    ASSERT_EQ(claimed.size(), 1u) << error;
    EXPECT_EQ(claimed.front().workset_id, receipt.workset_ids.front());
    ASSERT_EQ(claimed.front().items.size(), 2u);
    EXPECT_EQ(claimed.front().items[0].job_id, 39030);
    EXPECT_EQ(claimed.front().items[1].job_id, 39031);
}

}

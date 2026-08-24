#include "Analysis/IAnalysisDb.h"
#include "Archive/ArchivePackageService.h"
#include "Archive/RehydrateExecutor.h"
#include "Authoring/IAuthoringDb.h"
#include "Execution/ProgramDB/ProductionProgramKindRegistry.h"
#include "Execution/ProgramDB/TasMovieValidation/PreparedSterilizedCheckpointEvidence.h"
#include "Execution/ProgramDB/TasMovieValidation/TasMovieValidationProgram.h"
#include "Execution/Workflow/WorkflowOrchestration.h"
#include "Phases/Programs/TasMovieValidation/TasMovieValidationModule.h"
#include "Runner/IPC/DurableWorkerTerminalEnvelope.h"
#include "Runner/Runtime/ProgramRuntime/Codec/ProgramCodecV1.h"
#include "State/IStateDb.h"
#include "UIRead/Projectors/UiReadProjectionService.h"
#include "Utils/Hash.h"
#include "common/SqliteDbFixture.h"
#include "Runner/Runtime/ProgramKind.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace savor::db;

constexpr std::uint32_t kRootPc = 0x80101E48u;

std::string Sha(char digit)
{
    return std::string(64, digit);
}

std::int64_t StoreDtmFile(
    IStateDb* state,
    const std::filesystem::path& path,
    std::uint64_t input_count)
{
    std::vector<std::uint8_t> bytes(
        0x100 + static_cast<std::size_t>(input_count) * 8,
        0);
    bytes[0] = 'D';
    bytes[1] = 'T';
    bytes[2] = 'M';
    bytes[3] = 0x1A;
    constexpr char game_id[] = "GEAE8P";
    std::copy_n(game_id, 6, bytes.begin() + 4);
    bytes[0x00B] = 0x01;
    for (unsigned shift = 0; shift != 64; shift += 8)
        bytes[0x015 + shift / 8] =
            static_cast<std::uint8_t>(input_count >> shift);
    for (std::size_t offset = 0x100; offset < bytes.size(); offset += 8) {
        bytes[offset + 4] = 0x80;
        bytes[offset + 5] = 0x80;
        bytes[offset + 6] = 0x80;
        bytes[offset + 7] = 0x80;
    }
    {
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        stream.write(
            reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
        if (!stream)
            throw std::runtime_error("failed writing TAS Movie test DTM");
    }
    std::int64_t artifact_id = 0;
    std::string error;
    const auto sha256 = hash::sha256_of_file(path.string());
    if (!state->StoreArtifact(
            {
                .sha256 = sha256,
                .size_bytes = static_cast<std::int64_t>(bytes.size()),
                .compression_kind = 0,
                .filename = path.string(),
                .file_ext = ".dtm",
                .artifact_kind = "DTM",
                .created_at_utc = types::UtcTimePoint(
                    std::chrono::milliseconds(1000)),
                .correlation_id = "tas-movie-descriptor-test",
                .causation_id = "test",
            },
            &artifact_id,
            &error))
        throw std::runtime_error(error);
    return artifact_id;
}

savor::runtime::program::ProgramValueGraph TrueGraph()
{
    using namespace savor::runtime::program;
    ProgramValue root{
        ProgramValueId(1),
        TypeRef::Builtin(BuiltinType::Bool),
        true,
    };
    return {root.id, {std::move(root)}};
}

std::vector<std::uint8_t> TasMovieProgramResult(
    std::span<const std::uint8_t> input,
    const savor::runtime::tasmovie::TasMovieValidationResultV1& outcome,
    std::uint64_t invocation_id,
    std::uint64_t attempt_id,
    std::vector<savor::runtime::program::ProgramArtifact> artifacts = {})
{
    using namespace savor::runtime;
    using namespace savor::runtime::program;
    const auto phase =
        savor::runtime::tasmovie::TasMovieValidationFullPhaseDefinitionV1();
    std::string diagnostic;
    const auto invocation = phase->BuildResolvedExecution(
        input,
        ProgramExecutionId(invocation_id),
        AttemptId(attempt_id),
        &diagnostic);
    if (!invocation)
        throw std::logic_error(diagnostic);
    ProgramResult result{
        .invocation_id = ProgramExecutionId(invocation_id),
        .attempt_id = AttemptId(attempt_id),
        .module = invocation->module,
        .entrypoint = invocation->entrypoint,
        .resolved_dependencies = invocation->dependencies,
        .infrastructure = ProgramInfrastructureStatus::Completed,
        .domain_outcome = TrueGraph(),
        .cleanup = ProgramCleanupStatus::Clean,
        .session_disposition = SessionDisposition::Clean,
        .output = savor::runtime::tasmovie::EncodeTasMovieValidationResultV1(
            outcome),
        .artifacts = std::move(artifacts),
        .provenance = {.requesting_component = "test.tas_movie_validation"},
    };
    const auto encoded = EncodeProgramResultV1(result);
    if (!encoded)
        throw std::logic_error(encoded.status.message);
    return encoded.bytes;
}

std::int64_t StoreArtifact(
    IStateDb* state,
    std::string sha,
    std::string extension,
    std::string kind,
    std::int64_t ordinal)
{
    const auto display_filename =
        "tas-movie-artifact-" + std::to_string(ordinal) + extension;
    const auto source_root = std::filesystem::temp_directory_path() /
        "savor-tests-tas-movie-artifacts";
    std::filesystem::create_directories(source_root);
    const auto source_path = source_root /
        (sha + "-" + display_filename);
    const auto size = 100 + ordinal;
    {
        std::ofstream out(source_path, std::ios::binary | std::ios::trunc);
        const std::string bytes(static_cast<std::size_t>(size), 'x');
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }
    std::int64_t id = 0;
    std::string error;
    const bool stored = state->StoreArtifact(
        {
            .sha256 = std::move(sha),
            .size_bytes = size,
            .compression_kind = 0,
            .filename = source_path.string(),
            .display_filename = display_filename,
            .file_ext = std::move(extension),
            .artifact_kind = std::move(kind),
            .created_at_utc = types::UtcTimePoint(std::chrono::milliseconds(ordinal)),
            .correlation_id = "tas-movie-persistence-test",
            .causation_id = "test",
        },
        &id,
        &error);
    EXPECT_TRUE(stored) << error;
    return id;
}

std::int64_t StorePhysicalArtifact(
    IStateDb* state,
    const std::filesystem::path& path,
    std::string_view bytes,
    std::string extension,
    std::string kind,
    std::int64_t ordinal)
{
    {
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        if (!stream) throw std::runtime_error("failed writing physical artifact");
    }
    std::int64_t id = 0;
    std::string error;
    const auto sha = hash::sha256_of_file(path.string());
    if (!state->StoreArtifact({
            .sha256 = sha,
            .size_bytes = static_cast<std::int64_t>(bytes.size()),
            .compression_kind = 0,
            .filename = path.string(),
            .file_ext = std::move(extension),
            .artifact_kind = std::move(kind),
            .created_at_utc = types::UtcTimePoint(
                std::chrono::milliseconds(ordinal)),
            .correlation_id = "prepared-evidence-test",
            .causation_id = "test",
        }, &id, &error)) {
        throw std::runtime_error(error);
    }
    return id;
}

CreateTasMovieValidationRequestCommand RootValidationRequest(
    std::int64_t workflow_instance_id,
    std::int64_t workflow_step_id,
    std::string effective_sha)
{
    return {
        .materialization_key = "tas-movie-root-" + std::to_string(workflow_step_id),
        .workflow_instance_id = workflow_instance_id,
        .workflow_step_id = workflow_step_id,
        .step_kind = "tasmovie.validate_root",
        .operation = TasMovieValidationOperation::Validate,
        .source_kind = TasMovieValidationSourceKind::RootEstablishment,
        .source_ref_id = 40,
        .source_dtm_artifact_id = 41,
        .source_dtm_sha256 = Sha('1'),
        .rtc_value = 7,
        .effective_dtm_sha256 = std::move(effective_sha),
        .itinerary_artifact_id = 42,
        .itinerary_sha256 = Sha('2'),
        .required_final_breakpoint_pc = kRootPc,
        .capture_root_checkpoint = true,
        .full_phase_program_kind = static_cast<std::int64_t>(savor::PK_TasMovie),
        .full_phase_program_version = 1,
        .full_phase_canonical_id = "savor.full_phase.tas_movie_validation",
        .full_phase_contract_revision = 1,
        .full_phase_sha256 = Sha('3'),
        .module_canonical_id = "soa.tas_movie_validation",
        .module_revision = 1,
        .module_sha256 = Sha('4'),
        .created_at_utc = types::UtcTimePoint(std::chrono::milliseconds(workflow_step_id)),
    };
}

TEST_F(SqliteDbFixture, TasMoviePersistenceStateRootTreeIdentityAndLineageAreImmutable)
{
    auto* state = db_service_->StateDb();
    ASSERT_NE(state, nullptr);

    const auto source_dtm = StoreArtifact(state, Sha('1'), ".dtm", "DTM", 1);
    const auto root_dtm = StoreArtifact(state, Sha('2'), ".dtm", "DTM", 2);
    const auto itinerary = StoreArtifact(state, Sha('3'), ".tmi", "TAS_MOVIE_ITINERARY", 3);
    const auto sav_artifact = StoreArtifact(state, Sha('4'), ".sav", "SAV", 4);
    const auto child_dtm = StoreArtifact(state, Sha('5'), ".dtm", "DTM", 5);
    const auto child_itinerary = StoreArtifact(state, Sha('6'), ".tmi", "TAS_MOVIE_ITINERARY", 6);
    const auto grandchild_dtm = StoreArtifact(state, Sha('7'), ".dtm", "DTM", 7);
    const auto child_sav_artifact = StoreArtifact(state, Sha('8'), ".sav", "SAV", 8);
    const auto grandchild_sav_artifact = StoreArtifact(state, Sha('9'), ".sav", "SAV", 9);

    std::string error;
    std::int64_t savestate_id = 0;
    const CreateSavestateCommand savestate{
        .artifact_id = sav_artifact,
        .playback_state = SavestatePlaybackState::MoviePaired,
        .dtm_artifact_id = root_dtm,
        .savestate_type = "TAS_MOVIE_ROOT_CHECKPOINT",
        .note = "canonical root checkpoint",
        .is_complete = true,
        .created_at_utc = types::UtcTimePoint(std::chrono::milliseconds(10)),
        .correlation_id = "tas-movie-persistence-test",
        .causation_id = "test",
    };
    ASSERT_TRUE(state->CreateSavestate(savestate, &savestate_id, &error)) << error;
    std::int64_t repeated_savestate_id = 0;
    ASSERT_TRUE(state->CreateSavestate(savestate, &repeated_savestate_id, &error)) << error;
    EXPECT_EQ(repeated_savestate_id, savestate_id);
    ASSERT_TRUE(state->FindSavestateByArtifactId(sav_artifact).has_value());
    std::int64_t child_savestate_id = 0;
    ASSERT_TRUE(state->CreateSavestate({
        .artifact_id = child_sav_artifact,
        .playback_state = SavestatePlaybackState::MoviePaired,
        .dtm_artifact_id = child_dtm,
        .savestate_type = "TAS_MOVIE_TREE_CHECKPOINT",
        .note = "child checkpoint",
        .is_complete = true,
        .created_at_utc = types::UtcTimePoint(std::chrono::milliseconds(11)),
        .correlation_id = "tas-movie-persistence-test",
        .causation_id = "test",
    }, &child_savestate_id, &error)) << error;
    std::int64_t grandchild_savestate_id = 0;
    ASSERT_TRUE(state->CreateSavestate({
        .artifact_id = grandchild_sav_artifact,
        .playback_state = SavestatePlaybackState::MoviePaired,
        .dtm_artifact_id = grandchild_dtm,
        .savestate_type = "TAS_MOVIE_TREE_CHECKPOINT",
        .note = "grandchild checkpoint",
        .is_complete = true,
        .created_at_utc = types::UtcTimePoint(std::chrono::milliseconds(12)),
        .correlation_id = "tas-movie-persistence-test",
        .causation_id = "test",
    }, &grandchild_savestate_id, &error)) << error;

    const CreateTasMovieRootCommand root_command{
        .source_dtm_artifact_id = source_dtm,
        .dtm_artifact_id = root_dtm,
        .rtc_value = 7,
        .itinerary_artifact_id = itinerary,
        .required_final_breakpoint_pc = kRootPc,
        .checkpoint_savestate_id = savestate_id,
        .source_context_kind = "tmv_validation_request",
        .source_context_id = 50,
        .created_at_utc = types::UtcTimePoint(std::chrono::milliseconds(11)),
        .correlation_id = "tas-movie-persistence-test",
        .causation_id = "test",
    };
    std::int64_t root_id = 0;
    ASSERT_TRUE(state->CreateTasMovieRoot(root_command, &root_id, &error)) << error;
    std::int64_t repeated_root_id = 0;
    ASSERT_TRUE(state->CreateTasMovieRoot(root_command, &repeated_root_id, &error)) << error;
    EXPECT_EQ(repeated_root_id, root_id);
    auto converged_root = root_command;
    converged_root.source_context_id = 51;
    converged_root.created_at_utc =
        types::UtcTimePoint(std::chrono::milliseconds(13));
    std::int64_t converged_root_id = 0;
    ASSERT_TRUE(state->CreateTasMovieRoot(
        converged_root,
        &converged_root_id,
        &error)) << error;
    EXPECT_EQ(converged_root_id, root_id);
    EXPECT_EQ(
        state->GetTasMovieRoot(root_id)->source_context_id,
        root_command.source_context_id);
    ASSERT_EQ(state->FindTasMovieRootBySourceRtc(source_dtm, 7)->tas_movie_root_id, root_id);
    ASSERT_EQ(state->FindTasMovieRootByDtmArtifactId(root_dtm)->tas_movie_root_id, root_id);

    CreateTasMovieTreeCommand child{
        .tas_movie_root_id = root_id,
        .dtm_artifact_id = child_dtm,
        .itinerary_artifact_id = child_itinerary,
        .required_final_breakpoint_pc = kRootPc,
        .checkpoint_savestate_id = child_savestate_id,
        .source_context_kind = "tas_movie_round",
        .source_context_id = 60,
        .created_at_utc = types::UtcTimePoint(std::chrono::milliseconds(12)),
        .correlation_id = "tas-movie-persistence-test",
        .causation_id = "test",
    };
    std::int64_t child_id = 0;
    ASSERT_TRUE(state->CreateTasMovieTree(child, &child_id, &error)) << error;
    child.parent_tas_movie_tree_id = child_id;
    child.dtm_artifact_id = grandchild_dtm;
    child.checkpoint_savestate_id = grandchild_savestate_id;
    child.source_context_id = 61;
    std::int64_t grandchild_id = 0;
    ASSERT_TRUE(state->CreateTasMovieTree(child, &grandchild_id, &error)) << error;

    ASSERT_EQ(state->FindTasMovieTreeByDtmArtifactId(grandchild_dtm)->tas_movie_tree_id, grandchild_id);
    const auto lineage = state->ListTasMovieTreeLineage(grandchild_id);
    ASSERT_EQ(lineage.size(), 2u);
    EXPECT_EQ(lineage[0].tas_movie_tree_id, child_id);
    EXPECT_EQ(lineage[1].tas_movie_tree_id, grandchild_id);

    const auto events = state->ReadUnpublishedOutboxBatch(0, 100);
    const auto root_events = std::count_if(events.begin(), events.end(), [](const auto& event) {
        return event.event_type == "State.TasMovieRootCreated.v1";
    });
    const auto tree_events = std::count_if(events.begin(), events.end(), [](const auto& event) {
        return event.event_type == "State.TasMovieTreeCreated.v1";
    });
    EXPECT_EQ(root_events, 1);
    EXPECT_EQ(tree_events, 2);

    const auto ui_path = temp_root_ / "tas-movie-state-projection.sqlite";
    DbConfigPaths ui_paths{};
    ui_paths.execution_db_path = ui_path;
    ui_paths.state_db_path = ui_path;
    ui_paths.analysis_db_path = ui_path;
    ui_paths.authoring_db_path = ui_path;
    ui_paths.ui_read_db_path = ui_path;
    ui_paths.archive_db_path = ui_path;
    core::DBService ui_initializer(
        ui_paths,
        migrations::MigrationSourceOptions{
            .source_kind = migrations::MigrationSourceKind::Embedded});
    ASSERT_TRUE(ui_initializer.Start(&error)) << error;
    ui_initializer.Stop();

    const auto source_path = temp_root_ / "savordb_test.sqlite";
    uiread::projectors::UiReadProjectionService projection({
        .ui_read_db_path = ui_path,
        .execution_db_path = source_path,
        .state_db_path = source_path,
        .analysis_db_path = source_path,
        .archive_db_path = source_path,
        .poll_interval = std::chrono::hours(24),
        .enabled_stream_ids = {"state"},
    });
    ASSERT_TRUE(projection.Start(&error)) << error;
    ASSERT_TRUE(projection.RunOnce(&error)) << error;
    projection.Stop();

    sqlite3* ui_db = nullptr;
    ASSERT_EQ(SQLITE_OK, sqlite3_open(ui_path.string().c_str(), &ui_db));
    const auto projected_count = [&](const char* table) {
        sqlite3_stmt* statement = nullptr;
        const std::string sql = "SELECT COUNT(1) FROM " + std::string(table) + ";";
        EXPECT_EQ(SQLITE_OK, sqlite3_prepare_v2(
            ui_db, sql.c_str(), -1, &statement, nullptr));
        EXPECT_EQ(SQLITE_ROW, sqlite3_step(statement));
        const auto count = sqlite3_column_int64(statement, 0);
        sqlite3_finalize(statement);
        return count;
    };
    EXPECT_EQ(projected_count("ui_state_savestate_summary"), 3);
    EXPECT_EQ(projected_count("ui_tas_movie_root_summary"), 1);
    EXPECT_EQ(projected_count("ui_tas_movie_tree_summary"), 2);
    EXPECT_EQ(sqlite3_close(ui_db), SQLITE_OK);
}

TEST_F(SqliteDbFixture, TasMovieCheckpointSterilizationIsTypedCanonicalAndRecoverable)
{
    auto* state = db_service_->StateDb();
    auto* analysis = db_service_->AnalysisDb();
    ASSERT_NE(state, nullptr);
    ASSERT_NE(analysis, nullptr);
    const auto dtm = StoreArtifact(state, Sha('a'), ".dtm", "DTM", 101);
    const auto paired_sav = StoreArtifact(state, Sha('b'), ".sav", "SAV", 102);
    const auto inactive_sav = StoreArtifact(state, Sha('c'), ".sav", "SAV", 103);
    std::string error;

    EXPECT_FALSE(state->CreateSavestate({
        .artifact_id = paired_sav,
        .playback_state = SavestatePlaybackState::MoviePaired,
        .savestate_type = "BAD_PAIRED",
        .is_complete = true,
        .created_at_utc = types::UtcTimePoint(std::chrono::milliseconds(101)),
    }, nullptr, &error));
    EXPECT_FALSE(state->CreateSavestate({
        .artifact_id = inactive_sav,
        .playback_state = SavestatePlaybackState::MovieInactive,
        .dtm_artifact_id = dtm,
        .savestate_type = "BAD_INACTIVE",
        .is_complete = true,
        .created_at_utc = types::UtcTimePoint(std::chrono::milliseconds(102)),
    }, nullptr, &error));

    std::int64_t source_id = 0;
    ASSERT_TRUE(state->CreateSavestate({
        .artifact_id = paired_sav,
        .playback_state = SavestatePlaybackState::MoviePaired,
        .dtm_artifact_id = dtm,
        .savestate_type = "TAS_MOVIE_ROOT_CHECKPOINT",
        .note = "source",
        .is_complete = true,
        .created_at_utc = types::UtcTimePoint(std::chrono::milliseconds(103)),
        .correlation_id = "sterilization-test",
        .causation_id = "test",
    }, &source_id, &error)) << error;

    const auto inactive_artifact = state->GetArtifact(inactive_sav);
    ASSERT_TRUE(inactive_artifact.has_value());
    CreateOrGetSterilizedCheckpointCommand create{
        .from_savestate_id = source_id,
        .artifact = {
            .sha256 = Sha('c'),
            .size_bytes = inactive_artifact->size_bytes,
            .compression_kind = 0,
            .filename = inactive_artifact->filename,
            .display_filename = inactive_artifact->display_filename,
            .file_ext = ".sav",
            .artifact_kind = "SAV",
            .created_at_utc = types::UtcTimePoint(std::chrono::milliseconds(104)),
            .correlation_id = "sterilization-test",
            .causation_id = "test",
        },
        .savestate_type = "TAS_MOVIE_STERILIZED_CHECKPOINT",
        .note = "inactive",
        .method_kind = "tasmovie.checkpoint_sterilize.v1",
        .source_context_kind = "tmv_checkpoint_sterilization_request",
        .source_context_id = 200,
        .created_at_utc = types::UtcTimePoint(std::chrono::milliseconds(104)),
        .correlation_id = "sterilization-test",
        .causation_id = "test",
    };
    CreateOrGetSterilizedCheckpointReceipt first{};
    ASSERT_TRUE(state->CreateOrGetSterilizedCheckpoint(
        create, &first, &error)) << error;
    EXPECT_TRUE(first.created);
    CreateOrGetSterilizedCheckpointReceipt repeated{};
    ASSERT_TRUE(state->CreateOrGetSterilizedCheckpoint(
        create, &repeated, &error)) << error;
    EXPECT_FALSE(repeated.created);
    EXPECT_EQ(repeated.savestate_id, first.savestate_id);
    EXPECT_EQ(repeated.derivation_id, first.derivation_id);
    const auto result = state->GetSavestate(first.savestate_id);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->playback_state, SavestatePlaybackState::MovieInactive);
    EXPECT_FALSE(result->dtm_artifact_id.has_value());
    const auto derivation = state->FindSavestateDerivationBySourceAndMethod(
        source_id, "tasmovie.checkpoint_sterilize.v1");
    ASSERT_TRUE(derivation.has_value());
    EXPECT_EQ(derivation->to_savestate_id, first.savestate_id);

    const auto phase = savor::runtime::tasmovie::
        TasMovieCheckpointSterilizationFullPhaseDefinitionV1();
    const CreateTasMovieCheckpointSterilizationRequestCommand request{
        .materialization_key = "sterilization-request-200",
        .workflow_instance_id = 200,
        .workflow_step_id = 201,
        .source_savestate_id = source_id,
        .source_savestate_artifact_id = paired_sav,
        .source_savestate_sha256 = Sha('b'),
        .source_dtm_artifact_id = dtm,
        .source_dtm_sha256 = Sha('a'),
        .full_phase_program_kind = phase->identity().program_kind,
        .full_phase_program_version = phase->identity().program_version,
        .full_phase_canonical_id = phase->identity().canonical_id,
        .full_phase_contract_revision = phase->identity().contract_revision,
        .full_phase_sha256 = phase->identity().canonical_sha256,
        .module_canonical_id = phase->runtime_contract().module.canonical_id,
        .module_revision = phase->runtime_contract().module.revision,
        .module_sha256 = phase->runtime_contract().module.canonical_hash,
        .created_at_utc = types::UtcTimePoint(std::chrono::milliseconds(105)),
    };
    std::int64_t request_id = 0;
    ASSERT_TRUE(analysis->CreateTasMovieCheckpointSterilizationRequest(
        request, &request_id, &error)) << error;
    std::int64_t repeated_request = 0;
    ASSERT_TRUE(analysis->CreateTasMovieCheckpointSterilizationRequest(
        request, &repeated_request, &error)) << error;
    EXPECT_EQ(repeated_request, request_id);
    ASSERT_EQ(analysis->GetTasMovieCheckpointSterilizationRequestForWorkflowStep(
        201)->sterilization_request_id, request_id);

    RecordTasMovieCheckpointSterilizationAttemptCommand attempt{
        .sterilization_request_id = request_id,
        .source_job_id = 300,
        .worker_terminal_sha256 = Sha('d'),
        .candidate_savestate_sha256 = Sha('c'),
        .produced_savestate_id = first.savestate_id,
        .worker_id = "worker-sterilize",
        .worker_process_generation = 4,
        .workset_epoch = 5,
        .recorded_at_utc = types::UtcTimePoint(std::chrono::milliseconds(106)),
    };
    std::int64_t attempt_id = 0;
    ASSERT_TRUE(analysis->RecordTasMovieCheckpointSterilizationAttempt(
        attempt, &attempt_id, &error)) << error;
    std::int64_t repeated_attempt = 0;
    ASSERT_TRUE(analysis->RecordTasMovieCheckpointSterilizationAttempt(
        attempt, &repeated_attempt, &error)) << error;
    EXPECT_EQ(repeated_attempt, attempt_id);
    ASSERT_EQ(analysis->FindTasMovieCheckpointSterilizationAttempt(
        300, Sha('d'))->produced_savestate_id, first.savestate_id);
    const auto attempts =
        analysis->ListTasMovieCheckpointSterilizationAttemptsForRequest(
            request_id);
    ASSERT_EQ(attempts.size(), 1u);
    EXPECT_EQ(attempts.front().sterilization_attempt_id, attempt_id);

    sqlite3_stmt* outbox = nullptr;
    ASSERT_EQ(SQLITE_OK, sqlite3_prepare_v2(
        db_,
        "SELECT event_type,COUNT(1) FROM tmv_outbox_message "
        "WHERE event_type LIKE 'AnalysisTasMovie.Sterilization%' "
        "GROUP BY event_type ORDER BY event_type;",
        -1, &outbox, nullptr));
    ASSERT_EQ(SQLITE_ROW, sqlite3_step(outbox));
    EXPECT_STREQ(reinterpret_cast<const char*>(sqlite3_column_text(outbox, 0)),
        "AnalysisTasMovie.SterilizationAttemptRecorded.v1");
    EXPECT_EQ(sqlite3_column_int64(outbox, 1), 1);
    ASSERT_EQ(SQLITE_ROW, sqlite3_step(outbox));
    EXPECT_STREQ(reinterpret_cast<const char*>(sqlite3_column_text(outbox, 0)),
        "AnalysisTasMovie.SterilizationRequestCreated.v1");
    EXPECT_EQ(sqlite3_column_int64(outbox, 1), 1);
    EXPECT_EQ(SQLITE_DONE, sqlite3_step(outbox));
    sqlite3_finalize(outbox);
}

TEST_F(SqliteDbFixture, TasMoviePersistenceAnalysisLedgerQuarantinesAndRestoresExactBytes)
{
    auto* analysis = db_service_->AnalysisDb();
    ASSERT_NE(analysis, nullptr);
    std::string error;

    auto establishment = RootValidationRequest(100, 101, Sha('5'));
    establishment.materialization_key = "tas-movie-establish";
    establishment.step_kind = "tasmovie.establish_root_cursor";
    establishment.operation = TasMovieValidationOperation::EstablishRootCursor;
    establishment.source_kind = TasMovieValidationSourceKind::DtmArtifact;
    establishment.source_ref_id = establishment.source_dtm_artifact_id;
    establishment.rtc_value.reset();
    establishment.itinerary_artifact_id.reset();
    establishment.itinerary_sha256.reset();
    establishment.capture_root_checkpoint = false;
    std::int64_t establishment_request_id = 0;
    ASSERT_TRUE(analysis->CreateTasMovieValidationRequest(
        establishment, &establishment_request_id, &error)) << error;

    EXPECT_FALSE(analysis->RecordTasMovieValidationAttempt(
        {
            .validation_request_id = establishment_request_id,
            .source_job_id = 199,
            .worker_terminal_sha256 = Sha('5'),
            .outcome = TasMovieValidationOutcome::Valid,
            .failure_reason = TasMovieValidationFailureReason::None,
            .actual_pc = kRootPc,
            .actual_input_count = 123,
            .worker_id = "worker-invalid-shape",
            .worker_process_generation = 1,
            .workset_epoch = 1,
            .recorded_at_utc = types::UtcTimePoint(
                std::chrono::milliseconds(101)),
        },
        nullptr,
        &error));

    std::int64_t establishment_attempt_id = 0;
    ASSERT_TRUE(analysis->RecordTasMovieValidationAttempt(
        {
            .validation_request_id = establishment_request_id,
            .source_job_id = 200,
            .worker_terminal_sha256 = Sha('6'),
            .outcome = TasMovieValidationOutcome::RootCursorEstablished,
            .failure_reason = TasMovieValidationFailureReason::None,
            .actual_pc = kRootPc,
            .actual_input_count = 123,
            .candidate_itinerary_artifact_id = 42,
            .candidate_itinerary_sha256 = Sha('2'),
            .worker_id = "worker-a",
            .worker_process_generation = 1,
            .workset_epoch = 2,
            .recorded_at_utc = types::UtcTimePoint(std::chrono::milliseconds(102)),
        },
        &establishment_attempt_id,
        &error)) << error;
    EXPECT_FALSE(analysis->GetTasMovieValidationStatus(establishment.effective_dtm_sha256).has_value());

    auto validation = RootValidationRequest(110, 111, Sha('7'));
    std::int64_t invalid_request_id = 0;
    ASSERT_TRUE(analysis->CreateTasMovieValidationRequest(validation, &invalid_request_id, &error)) << error;
    EXPECT_FALSE(analysis->RecordTasMovieValidationAttempt(
        {
            .validation_request_id = invalid_request_id,
            .source_job_id = 209,
            .worker_terminal_sha256 = Sha('7'),
            .outcome = TasMovieValidationOutcome::Valid,
            .failure_reason = TasMovieValidationFailureReason::None,
            .actual_pc = kRootPc,
            .actual_input_count = 123,
            .worker_id = "worker-missing-root",
            .worker_process_generation = 1,
            .workset_epoch = 1,
            .recorded_at_utc = types::UtcTimePoint(
                std::chrono::milliseconds(111)),
        },
        nullptr,
        &error));
    std::int64_t invalid_attempt_id = 0;
    RecordTasMovieValidationAttemptCommand invalid_attempt{
        .validation_request_id = invalid_request_id,
        .source_job_id = 210,
        .worker_terminal_sha256 = Sha('8'),
        .outcome = TasMovieValidationOutcome::Invalid,
        .failure_reason = TasMovieValidationFailureReason::MovieDesynchronized,
        .expected_pc = kRootPc,
        .expected_input_count = 123,
        .actual_pc = kRootPc,
        .actual_input_count = 124,
        .last_verified_itinerary_index = 0,
        .worker_id = "worker-b",
        .worker_process_generation = 2,
        .workset_epoch = 3,
        .recorded_at_utc = types::UtcTimePoint(std::chrono::milliseconds(112)),
    };
    ASSERT_TRUE(analysis->RecordTasMovieValidationAttempt(
        invalid_attempt, &invalid_attempt_id, &error)) << error;
    std::int64_t repeated_attempt_id = 0;
    invalid_attempt.recorded_at_utc = types::UtcTimePoint(std::chrono::milliseconds(999));
    ASSERT_TRUE(analysis->RecordTasMovieValidationAttempt(
        invalid_attempt, &repeated_attempt_id, &error)) << error;
    EXPECT_EQ(repeated_attempt_id, invalid_attempt_id);
    invalid_attempt.actual_input_count = 125;
    EXPECT_FALSE(analysis->RecordTasMovieValidationAttempt(invalid_attempt, nullptr, &error));
    auto status = analysis->GetTasMovieValidationStatus(validation.effective_dtm_sha256);
    ASSERT_TRUE(status.has_value());
    EXPECT_EQ(status->status, TasMovieValidationStatus::Quarantined);
    EXPECT_EQ(status->validation_attempt_id, invalid_attempt_id);

    validation.materialization_key = "tas-movie-root-revalidation";
    validation.workflow_instance_id = 120;
    validation.workflow_step_id = 121;
    validation.capture_root_checkpoint = false;
    std::int64_t valid_request_id = 0;
    ASSERT_TRUE(analysis->CreateTasMovieValidationRequest(validation, &valid_request_id, &error)) << error;
    std::int64_t valid_attempt_id = 0;
    ASSERT_TRUE(analysis->RecordTasMovieValidationAttempt(
        {
            .validation_request_id = valid_request_id,
            .source_job_id = 220,
            .worker_terminal_sha256 = Sha('9'),
            .outcome = TasMovieValidationOutcome::Valid,
            .failure_reason = TasMovieValidationFailureReason::None,
            .actual_pc = kRootPc,
            .actual_input_count = 123,
            .last_verified_itinerary_index = 0,
            .worker_id = "worker-c",
            .worker_process_generation = 3,
            .workset_epoch = 4,
            .recorded_at_utc = types::UtcTimePoint(std::chrono::milliseconds(122)),
        },
        &valid_attempt_id,
        &error)) << error;
    status = analysis->GetTasMovieValidationStatus(validation.effective_dtm_sha256);
    ASSERT_TRUE(status.has_value());
    EXPECT_EQ(status->status, TasMovieValidationStatus::Valid);
    EXPECT_EQ(status->validation_attempt_id, valid_attempt_id);
    EXPECT_TRUE(analysis->GetTasMovieValidationAttempt(invalid_attempt_id).has_value());
}

TEST_F(SqliteDbFixture, TasMoviePersistenceOutboxProjectsValidationHistoryIdempotently)
{
    auto* analysis = db_service_->AnalysisDb();
    ASSERT_NE(analysis, nullptr);
    std::string error;

    auto request = RootValidationRequest(410, 411, Sha('e'));
    std::int64_t request_id = 0;
    ASSERT_TRUE(analysis->CreateTasMovieValidationRequest(
        request, &request_id, &error)) << error;
    std::int64_t repeated_request_id = 0;
    ASSERT_TRUE(analysis->CreateTasMovieValidationRequest(
        request, &repeated_request_id, &error)) << error;
    EXPECT_EQ(repeated_request_id, request_id);

    RecordTasMovieValidationAttemptCommand attempt{
        .validation_request_id = request_id,
        .source_job_id = 412,
        .worker_terminal_sha256 = Sha('f'),
        .outcome = TasMovieValidationOutcome::Invalid,
        .failure_reason = TasMovieValidationFailureReason::MovieDesynchronized,
        .expected_pc = kRootPc,
        .expected_input_count = 19,
        .actual_pc = kRootPc,
        .actual_input_count = 20,
        .worker_id = "projection-worker",
        .worker_process_generation = 1,
        .workset_epoch = 2,
        .recorded_at_utc = types::UtcTimePoint(std::chrono::milliseconds(412)),
    };
    std::int64_t attempt_id = 0;
    ASSERT_TRUE(analysis->RecordTasMovieValidationAttempt(
        attempt, &attempt_id, &error)) << error;
    std::int64_t repeated_attempt_id = 0;
    ASSERT_TRUE(analysis->RecordTasMovieValidationAttempt(
        attempt, &repeated_attempt_id, &error)) << error;
    EXPECT_EQ(repeated_attempt_id, attempt_id);

    sqlite3_stmt* outbox_count = nullptr;
    ASSERT_EQ(SQLITE_OK, sqlite3_prepare_v2(
        db_, "SELECT COUNT(1) FROM tmv_outbox_message;", -1,
        &outbox_count, nullptr));
    ASSERT_EQ(SQLITE_ROW, sqlite3_step(outbox_count));
    EXPECT_EQ(sqlite3_column_int64(outbox_count, 0), 2);
    sqlite3_finalize(outbox_count);

    const auto ui_path = temp_root_ / "tas-movie-projection.sqlite";
    DbConfigPaths ui_paths{};
    ui_paths.execution_db_path = ui_path;
    ui_paths.state_db_path = ui_path;
    ui_paths.analysis_db_path = ui_path;
    ui_paths.authoring_db_path = ui_path;
    ui_paths.ui_read_db_path = ui_path;
    ui_paths.archive_db_path = ui_path;
    core::DBService ui_initializer(
        ui_paths,
        migrations::MigrationSourceOptions{
            .source_kind = migrations::MigrationSourceKind::Embedded});
    ASSERT_TRUE(ui_initializer.Start(&error)) << error;
    ui_initializer.Stop();

    const auto source_path = temp_root_ / "savordb_test.sqlite";
    uiread::projectors::UiReadProjectionService projection({
        .ui_read_db_path = ui_path,
        .execution_db_path = source_path,
        .state_db_path = source_path,
        .analysis_db_path = source_path,
        .archive_db_path = source_path,
        .poll_interval = std::chrono::hours(24),
        .enabled_stream_ids = {"analysis-tasmovie"},
    });
    ASSERT_TRUE(projection.Start(&error)) << error;
    ASSERT_TRUE(projection.RunOnce(&error)) << error;
    projection.Stop();

    sqlite3* ui_db = nullptr;
    ASSERT_EQ(SQLITE_OK, sqlite3_open(ui_path.string().c_str(), &ui_db));
    sqlite3_stmt* projected = nullptr;
    ASSERT_EQ(SQLITE_OK, sqlite3_prepare_v2(
        ui_db,
        "SELECT latest_validation_attempt_id,latest_outcome,latest_failure_reason,latest_actual_input_count "
        "FROM ui_tas_movie_validation_request_summary WHERE validation_request_id=?1;",
        -1, &projected, nullptr));
    sqlite3_bind_int64(projected, 1, request_id);
    ASSERT_EQ(SQLITE_ROW, sqlite3_step(projected));
    EXPECT_EQ(sqlite3_column_int64(projected, 0), attempt_id);
    EXPECT_STREQ(reinterpret_cast<const char*>(sqlite3_column_text(projected, 1)), "INVALID");
    EXPECT_STREQ(reinterpret_cast<const char*>(sqlite3_column_text(projected, 2)), "MOVIE_DESYNCHRONIZED");
    EXPECT_EQ(sqlite3_column_int64(projected, 3), 20);
    sqlite3_finalize(projected);
    EXPECT_EQ(sqlite3_close(ui_db), SQLITE_OK);
}

TEST_F(
    SqliteDbFixture,
    TasMoviePersistenceDescriptorMaterializesReconstructsAndPersistsRootCursor)
{
    using namespace savor::db::execution::programdb;
    using namespace savor::db::execution::programdb::tasmovievalidation;
    using namespace savor::db::execution::workflow;
    using namespace savor::runtime;
    using namespace savor::runtime::tasmovie;

    auto* execution = db_service_->ExecutionDb();
    auto* state = db_service_->StateDb();
    auto* analysis = db_service_->AnalysisDb();
    auto* authoring = db_service_->AuthoringDb();
    ASSERT_NE(execution, nullptr);
    ASSERT_NE(state, nullptr);
    ASSERT_NE(analysis, nullptr);
    ASSERT_NE(authoring, nullptr);

    const auto source_path = temp_root_ / "handcrafted-root.dtm";
    const auto source_artifact_id = StoreDtmFile(state, source_path, 4);

    SaveWorkflowGraphResult saved_graph{};
    std::string error;
    ASSERT_TRUE(authoring->SaveWorkflowGraph(
        {
            .name = "tas-movie-root-cursor-test",
            .description = "Focused root cursor descriptor test",
            .graph_version = 1,
            .graph_hash = "tas-movie-root-cursor-test-v1",
            .nodes = {{
                .node_key = "root-cursor",
                .unit_kind = "tas_movie_establish_root_cursor",
                .display_name = "TAS Movie: Establish Root Cursor",
                .inputs = {{
                    .input_key = "root_dtm",
                    .data_kind = "state_artifact.dtm_artifact_id",
                    .ref_kind = "state_artifact",
                    .display_name = "Handcrafted root DTM",
                }},
                .possible_outputs = {{
                    .output_key = "tas_movie_validation_attempt",
                    .data_kind = "analysis.tas_movie_validation_attempt_id",
                    .ref_kind = "tmv_validation_attempt",
                    .display_name = "Validation attempt",
                }},
            }},
            .created_at_utc = types::UtcTimePoint(
                std::chrono::milliseconds(1000)),
            .correlation_id = "tas-movie-descriptor-test",
        },
        &saved_graph,
        &error)) << error;

    WorkflowCreateInstanceCommand create{};
    create.workflow_kind = "workflow_graph";
    create.root_scope_kind = "manual";
    create.workflow_graph_revision_id =
        saved_graph.workflow_graph_revision_id;
    create.created_by = "tas-movie-descriptor-test";
    create.created_at_utc = 1000;
    create.unit_activations.push_back({
        .activation_key = "root-cursor",
        .graph_node_key = "root-cursor",
        .unit_kind = "tas_movie_establish_root_cursor",
        .display_name = "TAS Movie: Establish Root Cursor",
        .activation_params_json = "{}",
        .steps = {{
            .step_key = "root-cursor",
            .step_kind = "tasmovie.establish_root_cursor",
            .priority = 1,
            .max_attempts = 1,
        }},
    });
    create.input_bindings.push_back({
        .node_key = "root-cursor",
        .input_key = "root_dtm",
        .data_kind = "state_artifact.dtm_artifact_id",
        .ref_kind = "state_artifact",
        .ref_id = source_artifact_id,
        .source_kind = "test",
    });
    std::int64_t workflow_instance_id = 0;
    ASSERT_TRUE(execution->WorkflowCommandService()->CreateWorkflowInstance(
        create,
        &workflow_instance_id,
        &error)) << error;
    const auto workflow =
        execution->WorkflowQueryService()->GetWorkflowGraph(
            workflow_instance_id);
    ASSERT_TRUE(workflow.has_value());
    ASSERT_EQ(workflow->steps.size(), 1u);
    ASSERT_TRUE(workflow->edges.empty());
    EXPECT_EQ(workflow->steps.front().max_attempts, 1);
    const auto workflow_step_id = workflow->steps.front().workflow_step_id;

    ProgramJobMaterializationContext materialization{
        .step = {
            .workflow_instance_id = workflow_instance_id,
            .workflow_step_id = workflow_step_id,
            .step_key = "root-cursor",
            .step_kind = "tasmovie.establish_root_cursor",
            .step_priority = 1,
        },
        .graph = WorkflowGraphStepScheduleContext{
            .workflow_instance_id = workflow_instance_id,
            .workflow_step_id = workflow_step_id,
            .step_key = "root-cursor",
            .step_kind = "tasmovie.establish_root_cursor",
            .activation_key = "root-cursor",
            .activation_graph_node_key = "root-cursor",
            .unit_kind = "tas_movie_establish_root_cursor",
            .activation_params_json = "{}",
            .step_priority = 1,
            .input_bindings = {{
                .node_key = "root-cursor",
                .input_key = "root_dtm",
                .data_kind = "state_artifact.dtm_artifact_id",
                .ref_kind = "state_artifact",
                .ref_id = source_artifact_id,
                .source_kind = "test",
            }},
        },
    };

    const auto descriptor = BuildTasMovieValidationProgramDescriptor(
        execution,
        state,
        analysis,
        {.working_dir_root = temp_root_ / "tas-movie-runtime"});
    ASSERT_NE(descriptor.job_materializer, nullptr);
    ASSERT_NE(descriptor.workset_reconstruction, nullptr);
    ASSERT_NE(descriptor.result_handler, nullptr);

    WorkflowStepScheduleResult scheduled{};
    ASSERT_TRUE(descriptor.job_materializer->Materialize(
        materialization,
        &scheduled,
        &error)) << error;
    WorkflowStepScheduleResult repeated{};
    ASSERT_TRUE(descriptor.job_materializer->Materialize(
        materialization,
        &repeated,
        &error)) << error;
    EXPECT_EQ(repeated.job_set_id, scheduled.job_set_id);
    EXPECT_EQ(
        repeated.persistence.program_ref_id,
        scheduled.persistence.program_ref_id);

    const auto request = analysis->GetTasMovieValidationRequest(
        scheduled.persistence.program_ref_id);
    ASSERT_TRUE(request.has_value());
    EXPECT_EQ(
        request->operation,
        TasMovieValidationOperation::EstablishRootCursor);
    EXPECT_EQ(request->source_dtm_artifact_id, source_artifact_id);
    EXPECT_FALSE(request->rtc_value.has_value());
    EXPECT_FALSE(request->itinerary_artifact_id.has_value());
    EXPECT_FALSE(request->capture_root_checkpoint);

    const auto job_rows =
        execution->ListJobsInJobSet(scheduled.job_set_id);
    ASSERT_EQ(job_rows.size(), 1u);
    const auto job = execution->GetExecutionJob(job_rows.front().job_id);
    ASSERT_TRUE(job.has_value());
    EXPECT_EQ(job->max_attempts, 1);

    constexpr std::uint64_t reserved_attempt_id = 1;
    constexpr std::int64_t dispatch_attempt_id = 7001;
    const auto reconstructed =
        descriptor.workset_reconstruction->Reconstruct(
            {
                .workset_id = 6001,
                .dispatch_attempt_id = dispatch_attempt_id,
                .workflow_step_id = workflow_step_id,
                .job_set_id = scheduled.job_set_id,
                .dispatch_token = "tas-movie-descriptor-test-dispatch",
                .contract_key = "tas-movie-validation-test",
                .state_compatibility = {
                    .game_id = "GEAE8P",
                    .iso_sha256 = Sha('a'),
                    .emulator_build = "test",
                    .runtime_revision = "test",
                },
                .items = {{
                    .job_id = job->job_id,
                    .workset_item_ordinal = 0,
                    .reserved_attempt_id = reserved_attempt_id,
                    .claim_token = "tas-movie-descriptor-test-claim",
                    .program_kind = job->program_kind,
                    .program_version = job->program_version,
                    .program_ref_kind = job->program_ref_kind,
                    .program_ref_id = job->program_ref_id,
                    .savestate_id = job->savestate_id,
                    .fingerprint = job->fingerprint,
                    .input_ini = job->input_ini,
                }},
            },
            &error);
    ASSERT_TRUE(reconstructed.has_value()) << error;
    ASSERT_EQ(reconstructed->workset.items.size(), 1u);
    EXPECT_EQ(
        reconstructed->workset.baseline.artifact.kind,
        ProgramBaselineArtifactKind::ReadOnlyMovie);
    TasMovieValidationRequestV1 native_request{};
    ASSERT_TRUE(DecodeTasMovieValidationExecutionInputV1(
        reconstructed->workset.items.front().execution.input_payload,
        native_request,
        &error)) << error;
    EXPECT_EQ(
        native_request.operation,
        TasMovieValidationOperationV1::EstablishRootCursor);
    EXPECT_TRUE(native_request.itinerary.checkpoints.empty());
    EXPECT_FALSE(native_request.final_checkpoint_path.has_value());
    EXPECT_EQ(
        hash::sha256_of_file(native_request.dtm_path),
        request->effective_dtm_sha256);

    const TasMovieValidationResultV1 outcome{
        .outcome = TasMovieValidationOutcomeV1::RootCursorEstablished,
        .candidate_checkpoint = TasMovieCheckpointV1{
            .pc = kRootPc,
            .input_count = DtmInputCount{3},
        },
    };
    const auto program_result = TasMovieProgramResult(
        reconstructed->workset.items.front().execution.input_payload,
        outcome,
        static_cast<std::uint64_t>(job->job_id),
        reserved_attempt_id);
    DurableWorkerTerminalEnvelope terminal{
        .envelope_version = kDurableWorkerTerminalEnvelopeVersion,
        .wrms_protocol_version = savor::wrms::ProtocolVersion,
        .worker_id = 41,
        .process_generation = 2,
        .terminal = {
            .outbound_sequence = 1,
            .workset_id = static_cast<std::uint64_t>(dispatch_attempt_id),
            .item_id = static_cast<std::uint64_t>(job->job_id),
            .item_ordinal = 0,
            .invocation_id = static_cast<std::uint64_t>(job->job_id),
            .attempt_id = reserved_attempt_id,
            .terminal_id = 1,
            .terminal_order = 1,
            .status = savor::wrms::InvocationTerminalStatus::Succeeded,
            .session_disposition = savor::wrms::SessionDispositionCode::Clean,
            .workset_epoch = 3,
            .unstarted = false,
            .rejection_code = savor::wrms::RejectionCode::None,
            .result = program_result,
        },
    };
    std::vector<std::uint8_t> envelope;
    ASSERT_TRUE(EncodeDurableWorkerTerminalEnvelope(
        terminal,
        &envelope,
        &error)) << error;
    const auto terminal_sha = hash::sha256(
        envelope.data(),
        envelope.size());
    const auto decision = descriptor.result_handler->Process({
        .job_id = job->job_id,
        .job_set_id = job->job_set_id,
        .program_kind = job->program_kind,
        .program_version = job->program_version,
        .program_ref_kind = job->program_ref_kind,
        .program_ref_id = job->program_ref_id,
        .fingerprint = job->fingerprint,
        .input_ini = job->input_ini,
        .terminal = {
            .job_id = job->job_id,
            .workset_id = 6001,
            .dispatch_attempt_id = dispatch_attempt_id,
            .reserved_attempt_id = reserved_attempt_id,
            .format = "savor.worker-terminal-envelope.v1",
            .sha256 = terminal_sha,
            .envelope = envelope,
        },
    });
    EXPECT_EQ(decision.final_job_state, "SUCCEEDED");
    ASSERT_EQ(decision.outputs.size(), 2u);
    const auto attempt_output = std::find_if(
        decision.outputs.begin(), decision.outputs.end(),
        [](const auto& output) {
            return output.output_key == "tas_movie_validation_attempt";
        });
    const auto established_output = std::find_if(
        decision.outputs.begin(), decision.outputs.end(),
        [](const auto& output) {
            return output.output_key
                == "established_root_cursor_attempt";
        });
    ASSERT_NE(attempt_output, decision.outputs.end());
    ASSERT_NE(established_output, decision.outputs.end());
    EXPECT_EQ(established_output->ref_id, attempt_output->ref_id);
    const auto attempt = analysis->GetTasMovieValidationAttempt(
        attempt_output->ref_id);
    ASSERT_TRUE(attempt.has_value());
    EXPECT_EQ(
        attempt->outcome,
        TasMovieValidationOutcome::RootCursorEstablished);
    ASSERT_TRUE(attempt->candidate_itinerary_artifact_id.has_value());
    const auto itinerary_artifact = state->GetArtifact(
        *attempt->candidate_itinerary_artifact_id);
    ASSERT_TRUE(itinerary_artifact.has_value());
    EXPECT_EQ(itinerary_artifact->artifact_kind, "TAS_MOVIE_ITINERARY");

    SaveWorkflowGraphResult root_graph{};
    ASSERT_TRUE(authoring->SaveWorkflowGraph(
        {
            .name = "tas-movie-root-validation-test",
            .description = "Focused RTC root validation descriptor test",
            .graph_version = 1,
            .graph_hash = "tas-movie-root-validation-test-v1",
            .nodes = {{
                .node_key = "validate-root",
                .unit_kind = "tas_movie_validate_root",
                .display_name = "TAS Movie: Validate Root",
                .inputs = {{
                    .input_key = "root_establishment",
                    .data_kind = "analysis.tas_movie_validation_attempt_id",
                    .ref_kind = "tmv_validation_attempt",
                    .display_name = "Root cursor establishment",
                }},
                .possible_outputs = {{
                    .output_key = "tas_movie_validation_attempt",
                    .data_kind = "analysis.tas_movie_validation_attempt_id",
                    .ref_kind = "tmv_validation_attempt",
                    .display_name = "Validation attempt",
                }},
            }},
            .created_at_utc = types::UtcTimePoint(
                std::chrono::milliseconds(2000)),
            .correlation_id = "tas-movie-root-validation-test",
        },
        &root_graph,
        &error)) << error;
    WorkflowCreateInstanceCommand create_root{};
    create_root.workflow_kind = "workflow_graph";
    create_root.root_scope_kind = "manual";
    create_root.workflow_graph_revision_id =
        root_graph.workflow_graph_revision_id;
    create_root.created_by = "tas-movie-root-validation-test";
    create_root.created_at_utc = 2000;
    create_root.unit_activations.push_back({
        .activation_key = "validate-root",
        .graph_node_key = "validate-root",
        .unit_kind = "tas_movie_validate_root",
        .display_name = "TAS Movie: Validate Root",
        .activation_params_json = "{}",
        .steps = {{
            .step_key = "validate-root",
            .step_kind = "tasmovie.validate_root",
            .priority = 1,
            .max_attempts = 1,
        }},
    });
    create_root.input_bindings.push_back({
        .node_key = "validate-root",
        .input_key = "root_establishment",
        .data_kind = "analysis.tas_movie_validation_attempt_id",
        .ref_kind = "tmv_validation_attempt",
        .ref_id = attempt->validation_attempt_id,
        .source_kind = "test",
    });
    create_root.arguments.push_back({
        .node_key = "validate-root",
        .argument_key = "rtc",
        .value_type = "integer",
        .integer_value = 7,
        .source_kind = "test",
    });
    std::int64_t root_workflow_id = 0;
    ASSERT_TRUE(execution->WorkflowCommandService()->CreateWorkflowInstance(
        create_root,
        &root_workflow_id,
        &error)) << error;
    const auto root_workflow =
        execution->WorkflowQueryService()->GetWorkflowGraph(root_workflow_id);
    ASSERT_TRUE(root_workflow.has_value());
    ASSERT_EQ(root_workflow->steps.size(), 1u);
    ASSERT_TRUE(root_workflow->edges.empty());
    const auto root_step_id = root_workflow->steps.front().workflow_step_id;
    ProgramJobMaterializationContext root_materialization{
        .step = {
            .workflow_instance_id = root_workflow_id,
            .workflow_step_id = root_step_id,
            .step_key = "validate-root",
            .step_kind = "tasmovie.validate_root",
            .step_priority = 1,
        },
        .graph = WorkflowGraphStepScheduleContext{
            .workflow_instance_id = root_workflow_id,
            .workflow_step_id = root_step_id,
            .workflow_graph_revision_id =
                root_graph.workflow_graph_revision_id,
            .step_key = "validate-root",
            .step_kind = "tasmovie.validate_root",
            .activation_key = "validate-root",
            .activation_graph_node_key = "validate-root",
            .unit_kind = "tas_movie_validate_root",
            .activation_params_json = "{}",
            .step_priority = 1,
            .input_bindings = {{
                .node_key = "validate-root",
                .input_key = "root_establishment",
                .data_kind = "analysis.tas_movie_validation_attempt_id",
                .ref_kind = "tmv_validation_attempt",
                .ref_id = attempt->validation_attempt_id,
                .source_kind = "test",
            }},
            .arguments = {{
                .node_key = "validate-root",
                .argument_key = "rtc",
                .value_type = "integer",
                .integer_value = 7,
                .source_kind = "test",
            }},
        },
    };
    WorkflowStepScheduleResult root_scheduled{};
    ASSERT_TRUE(descriptor.job_materializer->Materialize(
        root_materialization,
        &root_scheduled,
        &error)) << error;
    sqlite3_stmt* attach_root_job_set = nullptr;
    ASSERT_EQ(SQLITE_OK, sqlite3_prepare_v2(
        db_,
        "UPDATE exec_workflow_step SET job_set_id=?1 WHERE workflow_step_id=?2;",
        -1,
        &attach_root_job_set,
        nullptr));
    sqlite3_bind_int64(
        attach_root_job_set, 1, root_scheduled.job_set_id);
    sqlite3_bind_int64(attach_root_job_set, 2, root_step_id);
    ASSERT_EQ(SQLITE_DONE, sqlite3_step(attach_root_job_set));
    sqlite3_finalize(attach_root_job_set);
    const auto root_request = analysis->GetTasMovieValidationRequest(
        root_scheduled.persistence.program_ref_id);
    ASSERT_TRUE(root_request.has_value());
    EXPECT_EQ(root_request->operation, TasMovieValidationOperation::Validate);
    EXPECT_EQ(root_request->rtc_value, 7);
    EXPECT_TRUE(root_request->capture_root_checkpoint);
    EXPECT_NE(
        root_request->effective_dtm_sha256,
        root_request->source_dtm_sha256);

    const auto root_jobs =
        execution->ListJobsInJobSet(root_scheduled.job_set_id);
    ASSERT_EQ(root_jobs.size(), 1u);
    const auto root_job = execution->GetExecutionJob(root_jobs.front().job_id);
    ASSERT_TRUE(root_job.has_value());
    constexpr std::uint64_t root_attempt_id = 1;
    constexpr std::int64_t root_dispatch_id = 8001;
    const WorksetReconstructionContext root_reconstruction_context{
                .workset_id = 8000,
                .dispatch_attempt_id = root_dispatch_id,
                .workflow_step_id = root_step_id,
                .job_set_id = root_scheduled.job_set_id,
                .dispatch_token = "tas-movie-root-validation-dispatch",
                .contract_key = "tas-movie-root-validation",
                .state_compatibility = {
                    .game_id = "GEAE8P",
                    .iso_sha256 = Sha('b'),
                    .emulator_build = "test",
                    .runtime_revision = "test",
                },
                .items = {{
                    .job_id = root_job->job_id,
                    .workset_item_ordinal = 0,
                    .reserved_attempt_id = root_attempt_id,
                    .claim_token = "tas-movie-root-validation-claim",
                    .program_kind = root_job->program_kind,
                    .program_version = root_job->program_version,
                    .program_ref_kind = root_job->program_ref_kind,
                    .program_ref_id = root_job->program_ref_id,
                    .savestate_id = root_job->savestate_id,
                    .fingerprint = root_job->fingerprint,
                    .input_ini = root_job->input_ini,
                }},
            };
    const auto root_reconstructed =
        descriptor.workset_reconstruction->Reconstruct(
            root_reconstruction_context,
            &error);
    ASSERT_TRUE(root_reconstructed.has_value()) << error;

    sqlite3_stmt* corrupt_rtc = nullptr;
    ASSERT_EQ(SQLITE_OK, sqlite3_prepare_v2(
        db_,
        "UPDATE tmv_validation_request SET rtc_value=?1 "
        "WHERE validation_request_id=?2;",
        -1,
        &corrupt_rtc,
        nullptr));
    sqlite3_bind_int64(
        corrupt_rtc,
        1,
        static_cast<sqlite3_int64>(
            std::numeric_limits<std::uint32_t>::max()) + 1);
    sqlite3_bind_int64(
        corrupt_rtc, 2, root_request->validation_request_id);
    ASSERT_EQ(SQLITE_DONE, sqlite3_step(corrupt_rtc));
    sqlite3_finalize(corrupt_rtc);
    error.clear();
    EXPECT_FALSE(descriptor.workset_reconstruction->Reconstruct(
        root_reconstruction_context,
        &error).has_value());
    EXPECT_NE(error.find("0..UINT32_MAX"), std::string::npos);

    ASSERT_EQ(SQLITE_OK, sqlite3_prepare_v2(
        db_,
        "UPDATE tmv_validation_request SET rtc_value=7 "
        "WHERE validation_request_id=?1;",
        -1,
        &corrupt_rtc,
        nullptr));
    sqlite3_bind_int64(
        corrupt_rtc, 1, root_request->validation_request_id);
    ASSERT_EQ(SQLITE_DONE, sqlite3_step(corrupt_rtc));
    sqlite3_finalize(corrupt_rtc);
    const auto& root_input =
        root_reconstructed->workset.items.front().execution.input_payload;
    TasMovieValidationRequestV1 root_native{};
    ASSERT_TRUE(DecodeTasMovieValidationExecutionInputV1(
        root_input,
        root_native,
        &error)) << error;
    EXPECT_EQ(root_native.operation, TasMovieValidationOperationV1::Validate);
    ASSERT_EQ(root_native.itinerary.checkpoints.size(), 1u);
    ASSERT_TRUE(root_native.final_checkpoint_path.has_value());
    EXPECT_EQ(
        hash::sha256_of_file(root_native.dtm_path),
        root_request->effective_dtm_sha256);

    const std::filesystem::path captured_state(
        *root_native.final_checkpoint_path);
    ASSERT_TRUE(std::filesystem::create_directories(
        captured_state.parent_path()));
    {
        std::ofstream stream(
            captured_state,
            std::ios::binary | std::ios::trunc);
        stream.put('S');
        ASSERT_TRUE(stream.good());
    }
    const std::filesystem::path captured_sidecar(
        captured_state.string() + ".dtm");
    ASSERT_TRUE(std::filesystem::copy_file(
        root_native.dtm_path,
        captured_sidecar,
        std::filesystem::copy_options::overwrite_existing));
    const auto capture_sha =
        hash::sha256_of_file(captured_state.string());
    const auto capture_hash =
        savor::runtime::program::ContentHash256::FromHex(capture_sha);
    ASSERT_TRUE(capture_hash.has_value());
    const auto valid_result = TasMovieProgramResult(
        root_input,
        TasMovieValidationResultV1{
            .outcome = TasMovieValidationOutcomeV1::Valid,
        },
        static_cast<std::uint64_t>(root_job->job_id),
        root_attempt_id,
        {{
            .sequence = savor::runtime::program::ProgramArtifactSequence(1),
            .artifact = {
                .artifact_id = "root-checkpoint",
                .schema = {
                    .canonical_id = "test.tas_movie.root_checkpoint",
                    .version = 1,
                    .schema_hash = *capture_hash,
                },
                .content_hash = *capture_hash,
                .storage_reference = captured_state.string(),
                .complete = true,
            },
        }});
    DurableWorkerTerminalEnvelope root_terminal{
        .envelope_version = kDurableWorkerTerminalEnvelopeVersion,
        .wrms_protocol_version = savor::wrms::ProtocolVersion,
        .worker_id = 42,
        .process_generation = 3,
        .terminal = {
            .outbound_sequence = 1,
            .workset_id = static_cast<std::uint64_t>(root_dispatch_id),
            .item_id = static_cast<std::uint64_t>(root_job->job_id),
            .item_ordinal = 0,
            .invocation_id = static_cast<std::uint64_t>(root_job->job_id),
            .attempt_id = root_attempt_id,
            .terminal_id = 1,
            .terminal_order = 1,
            .status = savor::wrms::InvocationTerminalStatus::Succeeded,
            .session_disposition = savor::wrms::SessionDispositionCode::Clean,
            .workset_epoch = 4,
            .unstarted = false,
            .rejection_code = savor::wrms::RejectionCode::None,
            .result = valid_result,
        },
    };
    std::vector<std::uint8_t> root_envelope;
    ASSERT_TRUE(EncodeDurableWorkerTerminalEnvelope(
        root_terminal,
        &root_envelope,
        &error)) << error;
    const auto root_terminal_sha = hash::sha256(
        root_envelope.data(),
        root_envelope.size());
    const auto valid_decision = descriptor.result_handler->Process({
        .job_id = root_job->job_id,
        .job_set_id = root_job->job_set_id,
        .program_kind = root_job->program_kind,
        .program_version = root_job->program_version,
        .program_ref_kind = root_job->program_ref_kind,
        .program_ref_id = root_job->program_ref_id,
        .fingerprint = root_job->fingerprint,
        .input_ini = root_job->input_ini,
        .terminal = {
            .job_id = root_job->job_id,
            .workset_id = 8000,
            .dispatch_attempt_id = root_dispatch_id,
            .reserved_attempt_id = root_attempt_id,
            .format = "savor.worker-terminal-envelope.v1",
            .sha256 = root_terminal_sha,
            .envelope = root_envelope,
        },
    });
    EXPECT_EQ(valid_decision.final_job_state, "SUCCEEDED");
    ASSERT_EQ(valid_decision.outputs.size(), 2u);
    const auto valid_attempt_output = std::find_if(
        valid_decision.outputs.begin(), valid_decision.outputs.end(),
        [](const auto& output) {
            return output.output_key == "tas_movie_validation_attempt";
        });
    const auto checkpoint_output = std::find_if(
        valid_decision.outputs.begin(), valid_decision.outputs.end(),
        [](const auto& output) {
            return output.output_key
                == "validated_checkpoint_savestate";
        });
    ASSERT_NE(valid_attempt_output, valid_decision.outputs.end());
    ASSERT_NE(checkpoint_output, valid_decision.outputs.end());
    const auto valid_attempt = analysis->GetTasMovieValidationAttempt(
        valid_attempt_output->ref_id);
    ASSERT_TRUE(valid_attempt.has_value());
    ASSERT_TRUE(valid_attempt->produced_tas_movie_root_id.has_value());
    const auto root = state->GetTasMovieRoot(
        *valid_attempt->produced_tas_movie_root_id);
    ASSERT_TRUE(root.has_value());
    EXPECT_EQ(
        checkpoint_output->ref_id,
        root->checkpoint_savestate_id);
    EXPECT_EQ(root->rtc_value, 7);
    EXPECT_EQ(root->source_dtm_artifact_id, source_artifact_id);
    EXPECT_EQ(
        state->GetArtifact(root->dtm_artifact_id)->sha256,
        root_request->effective_dtm_sha256);
    const auto valid_status = analysis->GetTasMovieValidationStatus(
        root_request->effective_dtm_sha256);
    ASSERT_TRUE(valid_status.has_value());
    EXPECT_EQ(valid_status->status, TasMovieValidationStatus::Valid);
    const auto archive_root = temp_root_ / "tas-movie-archive";
    archive::SqliteArchivePackageService package_service(
        db_,
        execution,
        db_service_->UiReadDb(),
        db_service_->ArchiveDb(),
        DbConfigPaths{.archive_store_root = archive_root},
        db_,
        db_,
        db_);
    const auto package = package_service.CreateWorkflowPackage({
        .selection = {.workflow_instance_ids = {root_workflow_id}},
        .created_at_utc = types::UtcTimePoint(std::chrono::milliseconds(3000)),
        .archive_name = "TAS Movie root validation",
        .correlation_id = "tas-movie-archive-test",
        .causation_id = "test",
    });
    ASSERT_TRUE(package.success) << package.error.value_or("unknown archive error");
    const auto has_item = [&](std::string_view kind) {
        return std::ranges::any_of(package.files, [&](const auto& file) {
            return file.item_kind == kind && file.row_count > 0;
        });
    };
    EXPECT_TRUE(has_item("analysis_tas_movie_validation_requests"));
    EXPECT_TRUE(has_item("analysis_tas_movie_validation_attempts"));
    EXPECT_TRUE(has_item("analysis_tas_movie_validation_statuses"));
    EXPECT_TRUE(has_item("state_tas_movie_roots"));
    EXPECT_TRUE(has_item("state_artifacts"));
    EXPECT_TRUE(has_item("state_savestates"));

    std::int64_t rehydrate_request_id = 0;
    ASSERT_TRUE(db_service_->ArchiveDb()->RequestRehydrate(
        {
            .archive_package_id = package.archive_package_id,
            .status = "REQUESTED",
            .requested_at_utc = types::UtcTimePoint(std::chrono::milliseconds(4000)),
            .target_namespace = "tas-movie-target",
            .correlation_id = "tas-movie-rehydrate-test",
            .causation_id = "test",
        },
        &rehydrate_request_id,
        &error)) << error;

    const auto target_path = temp_root_ / "tas-movie-rehydrate.sqlite";
    DbConfigPaths target_paths{};
    target_paths.execution_db_path = target_path;
    target_paths.state_db_path = target_path;
    target_paths.analysis_db_path = target_path;
    target_paths.authoring_db_path = target_path;
    target_paths.ui_read_db_path = target_path;
    target_paths.archive_db_path = target_path;
    core::DBService target_service(
        target_paths,
        migrations::MigrationSourceOptions{
            .source_kind = migrations::MigrationSourceKind::Embedded});
    ASSERT_TRUE(target_service.Start(&error)) << error;
    sqlite3* target_db = nullptr;
    ASSERT_EQ(SQLITE_OK, sqlite3_open(target_path.string().c_str(), &target_db));
    ASSERT_NE(target_db, nullptr);
    ASSERT_EQ(SQLITE_OK, sqlite3_busy_timeout(target_db, 5000));

    archive::SqliteRehydrateExecutor rehydrate(
        target_db,
        db_,
        db_service_->ArchiveDb(),
        archive_root,
        target_db,
        target_db);
    const auto rehydrated = rehydrate.Execute({
        .rehydrate_request_id = rehydrate_request_id,
        .now_utc = types::UtcTimePoint(std::chrono::milliseconds(5000)),
        .correlation_id = "tas-movie-rehydrate-test",
        .causation_id = "test",
    });
    ASSERT_TRUE(rehydrated.success)
        << rehydrated.error.value_or("unknown rehydrate error");

    const auto mapped_id = [&](std::string_view kind, std::int64_t old_id) {
        sqlite3_stmt* statement = nullptr;
        const std::string sql =
            "SELECT CAST(new_id AS INTEGER) FROM ar_rehydrate_map "
            "WHERE rehydrate_request_id=?1 AND entity_kind=?2 AND old_id=?3 "
            "ORDER BY rehydrate_map_id DESC LIMIT 1;";
        EXPECT_EQ(SQLITE_OK, sqlite3_prepare_v2(
            db_, sql.c_str(), -1, &statement, nullptr));
        if (statement == nullptr)
            return std::int64_t{0};
        sqlite3_bind_int64(statement, 1, rehydrate_request_id);
        sqlite3_bind_text(
            statement,
            2,
            kind.data(),
            static_cast<int>(kind.size()),
            SQLITE_TRANSIENT);
        const auto old_text = std::to_string(old_id);
        sqlite3_bind_text(
            statement, 3, old_text.c_str(), -1, SQLITE_TRANSIENT);
        const auto step = sqlite3_step(statement);
        EXPECT_EQ(SQLITE_ROW, step);
        const auto value = step == SQLITE_ROW
            ? sqlite3_column_int64(statement, 0)
            : std::int64_t{0};
        sqlite3_finalize(statement);
        return value;
    };
    const auto mapped_root_id = mapped_id(
        "state_tas_movie_root", *valid_attempt->produced_tas_movie_root_id);
    const auto mapped_request_id = mapped_id(
        "analysis_tas_movie_validation_request",
        root_request->validation_request_id);
    const auto mapped_attempt_id = mapped_id(
        "analysis_tas_movie_validation_attempt",
        valid_attempt->validation_attempt_id);
    ASSERT_GT(mapped_root_id, 0);
    ASSERT_GT(mapped_request_id, 0);
    ASSERT_GT(mapped_attempt_id, 0);
    const auto restored_root = target_service.StateDb()->GetTasMovieRoot(
        mapped_root_id);
    ASSERT_TRUE(restored_root.has_value());
    EXPECT_EQ(restored_root->source_context_kind, "tmv_validation_request");
    EXPECT_EQ(restored_root->source_context_id, mapped_request_id);
    const auto restored_attempt =
        target_service.AnalysisDb()->GetTasMovieValidationAttempt(
            mapped_attempt_id);
    ASSERT_TRUE(restored_attempt.has_value());
    EXPECT_EQ(restored_attempt->produced_tas_movie_root_id, mapped_root_id);
    const auto restored_status =
        target_service.AnalysisDb()->GetTasMovieValidationStatus(
            root_request->effective_dtm_sha256);
    ASSERT_TRUE(restored_status.has_value());
    EXPECT_EQ(restored_status->status, TasMovieValidationStatus::Valid);
    EXPECT_EQ(restored_status->validation_attempt_id, mapped_attempt_id);

    sqlite3_close(target_db);
    target_db = nullptr;
    target_service.Stop();

    const auto invalid_result = TasMovieProgramResult(
        root_input,
        TasMovieValidationResultV1{
            .outcome = TasMovieValidationOutcomeV1::Invalid,
            .failure = TasMovieValidationFailureV1{
                .reason =
                    TasMovieValidationFailureReasonV1::MovieDesynchronized,
                .diagnostics = {
                    .expected_pc = kRootPc,
                    .expected_input_count = DtmInputCount{3},
                    .actual_pc = kRootPc,
                    .actual_input_count = DtmInputCount{4},
                },
            },
        },
        static_cast<std::uint64_t>(root_job->job_id),
        2);
    root_terminal.terminal.attempt_id = 2;
    root_terminal.terminal.terminal_id = 2;
    root_terminal.terminal.terminal_order = 2;
    root_terminal.terminal.result = invalid_result;
    root_envelope.clear();
    ASSERT_TRUE(EncodeDurableWorkerTerminalEnvelope(
        root_terminal,
        &root_envelope,
        &error)) << error;
    const auto invalid_terminal_sha = hash::sha256(
        root_envelope.data(),
        root_envelope.size());
    const auto invalid_decision = descriptor.result_handler->Process({
        .job_id = root_job->job_id,
        .job_set_id = root_job->job_set_id,
        .program_kind = root_job->program_kind,
        .program_version = root_job->program_version,
        .program_ref_kind = root_job->program_ref_kind,
        .program_ref_id = root_job->program_ref_id,
        .fingerprint = root_job->fingerprint,
        .input_ini = root_job->input_ini,
        .terminal = {
            .job_id = root_job->job_id,
            .workset_id = 8000,
            .dispatch_attempt_id = root_dispatch_id,
            .reserved_attempt_id = 2,
            .format = "savor.worker-terminal-envelope.v1",
            .sha256 = invalid_terminal_sha,
            .envelope = root_envelope,
        },
    });
    EXPECT_EQ(invalid_decision.final_job_state, "SUCCEEDED");
    EXPECT_EQ(invalid_decision.error_code, "TAS_MOVIE_INVALID");
    ASSERT_EQ(invalid_decision.outputs.size(), 1u);
    const auto invalid_attempt = analysis->GetTasMovieValidationAttempt(
        invalid_decision.outputs.front().ref_id);
    ASSERT_TRUE(invalid_attempt.has_value());
    EXPECT_EQ(invalid_attempt->outcome, TasMovieValidationOutcome::Invalid);
    EXPECT_FALSE(invalid_attempt->produced_tas_movie_root_id.has_value());
    const auto quarantined_status = analysis->GetTasMovieValidationStatus(
        root_request->effective_dtm_sha256);
    ASSERT_TRUE(quarantined_status.has_value());
    EXPECT_EQ(
        quarantined_status->status,
        TasMovieValidationStatus::Quarantined);
    const auto ui_path = temp_root_ / "tas-movie-alerts.sqlite";
    DbConfigPaths ui_paths{};
    ui_paths.execution_db_path = ui_path;
    ui_paths.state_db_path = ui_path;
    ui_paths.analysis_db_path = ui_path;
    ui_paths.authoring_db_path = ui_path;
    ui_paths.ui_read_db_path = ui_path;
    ui_paths.archive_db_path = ui_path;
    core::DBService ui_initializer(
        ui_paths,
        migrations::MigrationSourceOptions{
            .source_kind = migrations::MigrationSourceKind::Embedded});
    ASSERT_TRUE(ui_initializer.Start(&error)) << error;
    ui_initializer.Stop();

    const auto db_source_path = temp_root_ / "savordb_test.sqlite";
    uiread::projectors::UiReadProjectionService projection({
        .ui_read_db_path = ui_path,
        .execution_db_path = db_source_path,
        .state_db_path = db_source_path,
        .analysis_db_path = db_source_path,
        .archive_db_path = db_source_path,
        .poll_interval = std::chrono::hours(24),
        .enabled_stream_ids = {"execution"},
    });
    ASSERT_TRUE(projection.Start(&error)) << error;
    ASSERT_TRUE(projection.RunOnce(&error)) << error;
    projection.Stop();
    sqlite3* ui_db = nullptr;
    ASSERT_EQ(SQLITE_OK, sqlite3_open(ui_path.string().c_str(), &ui_db));
    ASSERT_NE(ui_db, nullptr);
    sqlite3_stmt* alert = nullptr;
    ASSERT_EQ(SQLITE_OK, sqlite3_prepare_v2(
        ui_db,
        "SELECT COUNT(1) FROM ui_workflow_alert WHERE workflow_instance_id=?1 "
        "AND alert_kind='TAS_MOVIE_INVALID' AND alert_code=?2 "
        "AND is_active=1;",
        -1,
        &alert,
        nullptr));
    sqlite3_bind_int64(alert, 1, root_workflow_id);
    const auto alert_code =
        "TAS_MOVIE_INVALID:" + root_request->effective_dtm_sha256;
    sqlite3_bind_text(
        alert, 2, alert_code.c_str(), -1, SQLITE_TRANSIENT);
    ASSERT_EQ(SQLITE_ROW, sqlite3_step(alert));
    EXPECT_EQ(sqlite3_column_int64(alert, 0), 1);
    sqlite3_finalize(alert);
    sqlite3_close(ui_db);
}

TEST_F(SqliteDbFixture, TasMoviePersistenceProductionRegistryUsesOnlyClosedValidationSteps)
{
    using namespace savor::db::execution::programdb;
    ProgramKindRegistry registry;
    std::string error;
    ASSERT_TRUE(BuildProductionProgramKindRegistry(
        {
            .execution_db = db_service_->ExecutionDb(),
            .state_db = db_service_->StateDb(),
            .analysis_db = db_service_->AnalysisDb(),
            .authoring_db = db_service_->AuthoringDb(),
        },
        MakeProductionProgramKindRegistryConfig(temp_root_ / "tas-movie-runtime"),
        &registry,
        &error)) << error;

    EXPECT_NE(registry.FindForStepKind("tasmovie.establish_root_cursor"), nullptr);
    EXPECT_NE(registry.FindForStepKind("tasmovie.validate_root"), nullptr);
    EXPECT_NE(registry.FindForStepKind("tasmovie.validate_tree"), nullptr);
    const auto* sterilize = registry.FindForStepKind(
        "tasmovie.checkpoint_sterilize");
    ASSERT_NE(sterilize, nullptr);
    EXPECT_EQ(
        sterilize->program_kind,
        static_cast<std::int32_t>(savor::PK_TasMovieCheckpointSterilize));
    EXPECT_EQ(registry.FindForStepKind("tas_movie"), nullptr);
    EXPECT_EQ(registry.FindForStepKind("tasmovie.play"), nullptr);
}

} // namespace

#include "BattleRecordProgram.h"
#include "BattleReplayProgram.h"

#include "../TasMovieValidation/PreparedSterilizedCheckpointEvidence.h"
#include "../WorksetDerivedStateBinding.h"
#include "../WorksetObservationBinding.h"
#include "../../../Analysis/IAnalysisDb.h"
#include "../../../Common/Types/UtcTimestamp.h"
#include "../../../Execution/IExecutionDb.h"
#include "../../../State/IStateDb.h"
#include "../../../../SavorCore/Core/Input/SoaBattle/BattleCommandCodec.h"
#include "../../../../SavorCore/Phases/Programs/BattleRecord/BattleRecordModule.h"
#include "../../../../SavorCore/Phases/Programs/BattleRecord/BattleReplayModule.h"
#include "../../../../SavorCore/Phases/Programs/TasMovieValidation/TasMovieValidationModule.h"
#include "../../../../SavorCore/Runner/IPC/DurableWorkerTerminalEnvelope.h"
#include "../../../../SavorCore/Runner/Runtime/ProgramKind.h"
#include "../../../../SavorCore/Runner/Runtime/ProgramRuntime/Registry/CanonicalActionCatalog.h"
#include "../../../../SavorCore/Runner/Runtime/Worksets/WorksetWireCodec.h"
#include "../../../../SavorCore/Tas/DtmFile.h"
#include "../../../../SavorCore/Utils/Hash.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace savor::db::execution::programdb::battlerecord {
namespace {

namespace phase = savor::runtime::battlerecord;
namespace replayphase = savor::runtime::battlereplay;
namespace completion = savor::runtime::battlecompletion;
namespace tasmovie = savor::runtime::tasmovie;
namespace evidence = tasmovieevidence;
using savor::runtime::program::CanonicalAction;

constexpr std::string_view kStepKind = "battle.record";
constexpr std::string_view kProgramRefKind =
    "analysis_battle.battle_recording";
constexpr std::string_view kCompletionRefKind =
    "analysis_battle.battle_completion";
constexpr std::string_view kInputKey = "completion";
constexpr std::string_view kInputDataKind =
    "analysis_battle.battle_completion";
constexpr std::string_view kOutputKey = "recording";
constexpr std::string_view kOutputDataKind =
    "analysis_battle.battle_recording";
constexpr std::string_view kPurpose = "BATTLE_RECORD";
constexpr std::string_view kCreatedBy = "battle_record_program_kind";
constexpr std::string_view kTreeRefKind = "state_tas_movie_tree";
constexpr std::uint32_t kMaximumLineageTurns = 256;
constexpr std::size_t kDeclaredTerminalBytes = 4ull * 1024ull * 1024ull;

constexpr std::string_view kReplayStepKind = "battle.replay";
constexpr std::string_view kReplayProgramRefKind =
    "analysis_battle.battle_replay";
constexpr std::string_view kReplayOutputKey = "replay";
constexpr std::string_view kReplayOutputDataKind =
    "analysis_battle.battle_replay";
constexpr std::string_view kReplayPurpose = "BATTLE_REPLAY";
constexpr std::string_view kReplayCreatedBy = "battle_replay_program_kind";

const WorksetObservationDefaultsV1& ObservationDefaults()
{
    static const WorksetObservationDefaultsV1 defaults{
        .progress_library_ids = {"soa.progress.battle.events/1"},
    };
    return defaults;
}

std::int64_t NowMs()
{
    return types::UtcNow().time_since_epoch().count();
}

bool Fail(std::string message, std::string* error_out)
{
    if (error_out)
        *error_out = std::move(message);
    return false;
}

std::filesystem::path Root(const std::filesystem::path& configured)
{
    return configured.empty()
        ? std::filesystem::temp_directory_path() / "savor-battle-record"
        : configured;
}

std::optional<std::string> HashFile(const std::filesystem::path& path)
{
    try
    {
        return hash::sha256_of_file(path.string());
    }
    catch (...)
    {
        return std::nullopt;
    }
}

bool IsExactFile(const std::filesystem::path& path,
                 std::string_view expected_sha,
                 std::int64_t expected_size = 0)
{
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error) || error)
        return false;
    const auto size = std::filesystem::file_size(path, error);
    return !error && size > 0 &&
        (expected_size <= 0 ||
         size == static_cast<std::uint64_t>(expected_size)) &&
        HashFile(path) == std::optional<std::string>(expected_sha);
}

bool EnsureParent(const std::filesystem::path& path, std::string* error_out)
{
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    return !error || Fail(
        "could not create Battle Recording artifact directory: " +
            error.message(),
        error_out);
}

std::optional<std::filesystem::path> MaterializeArtifact(
    IStateDb* state_db,
    const ArtifactRecord& artifact,
    const std::filesystem::path& destination,
    std::string* error_out)
{
    if (!state_db || artifact.artifact_id <= 0 || artifact.sha256.size() != 64 ||
        artifact.size_bytes <= 0 || !EnsureParent(destination, error_out))
        return std::nullopt;
    if (!IsExactFile(destination, artifact.sha256, artifact.size_bytes))
    {
        if (!state_db->MaterializeArtifactToPath(
                artifact.artifact_id, destination.string(), error_out) ||
            !IsExactFile(destination, artifact.sha256, artifact.size_bytes))
        {
            if (error_out && error_out->empty())
                *error_out = "State DB did not materialize the exact artifact";
            return std::nullopt;
        }
    }
    return destination;
}

std::optional<std::filesystem::path> MaterializeSavestate(
    IStateDb* state_db,
    const SavestateRecord& state,
    const std::filesystem::path& destination,
    std::string* error_out)
{
    if (!state_db || state.savestate_id <= 0 ||
        state.artifact_sha256.size() != 64 || state.artifact_size_bytes <= 0 ||
        !EnsureParent(destination, error_out))
        return std::nullopt;
    if (!IsExactFile(destination, state.artifact_sha256,
                     state.artifact_size_bytes))
    {
        if (!state_db->MaterializeSavestateToPath(
                state.savestate_id, destination.string(), error_out) ||
            !IsExactFile(destination, state.artifact_sha256,
                         state.artifact_size_bytes))
        {
            if (error_out && error_out->empty())
                *error_out = "State DB did not materialize the exact savestate";
            return std::nullopt;
        }
    }
    return destination;
}

std::optional<std::vector<std::uint8_t>> ReadFile(
    const std::filesystem::path& path,
    std::string* error_out)
{
    std::ifstream stream(path, std::ios::binary);
    if (!stream)
    {
        Fail("could not open immutable Battle Recording artifact", error_out);
        return std::nullopt;
    }
    stream.seekg(0, std::ios::end);
    const auto end = stream.tellg();
    if (end < 0)
    {
        Fail("could not determine Battle Recording artifact size", error_out);
        return std::nullopt;
    }
    stream.seekg(0, std::ios::beg);
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(end));
    if (!bytes.empty() &&
        !stream.read(reinterpret_cast<char*>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size())))
    {
        Fail("could not read immutable Battle Recording artifact", error_out);
        return std::nullopt;
    }
    return bytes;
}

bool WriteFileAtomically(const std::filesystem::path& path,
                         std::span<const std::uint8_t> bytes,
                         std::string* error_out)
{
    if (!EnsureParent(path, error_out))
        return false;
    const auto expected = hash::sha256(bytes.data(), bytes.size());
    if (IsExactFile(path, expected, static_cast<std::int64_t>(bytes.size())))
        return true;
    const auto temporary = std::filesystem::path(path.string() + ".publishing");
    std::error_code error;
    std::filesystem::remove(temporary, error);
    {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        if (!stream || (!bytes.empty() &&
            !stream.write(reinterpret_cast<const char*>(bytes.data()),
                          static_cast<std::streamsize>(bytes.size()))))
            return Fail("could not write Battle Recording artifact", error_out);
    }
    std::filesystem::rename(temporary, path, error);
    if (!error)
        return true;
    if (IsExactFile(path, expected, static_cast<std::int64_t>(bytes.size())))
    {
        std::filesystem::remove(temporary, error);
        return true;
    }
    return Fail("could not publish Battle Recording artifact: " +
                    error.message(),
                error_out);
}

std::optional<std::int64_t> CompletionId(
    const ProgramJobMaterializationContext& context)
{
    if (context.graph)
    {
        for (const auto& binding : context.graph->inputs)
        {
            if (binding.input_key == kInputKey &&
                binding.data_kind == kInputDataKind &&
                binding.ref_kind == kCompletionRefKind && binding.ref_id > 0)
                return binding.ref_id;
        }
    }
    if (context.step.domain_ref_kind == kCompletionRefKind &&
        context.step.domain_ref_id > 0)
        return context.step.domain_ref_id;
    return std::nullopt;
}

GCInputFrame SeedFrame(const AnalysisInputSetFrameRow& row)
{
    const auto byte = [](std::int32_t value) {
        return static_cast<std::uint8_t>(std::clamp(value, 0, 255));
    };
    GCInputFrame frame{};
    frame.buttons = 0;
    frame.main_x = byte(row.main_x);
    frame.main_y = byte(row.main_y);
    frame.c_x = byte(row.cstick_x);
    frame.c_y = byte(row.cstick_y);
    frame.trig_l = byte(row.trigger_x);
    frame.trig_r = byte(row.trigger_y);
    return frame;
}

std::optional<phase::BattleReplayPlanV1> BuildReplayPlan(
    IStateDb* state_db,
    IAnalysisDb* analysis_db,
    const BattleCompletionRecord& completion_row,
    std::string* error_out)
{
    if (!state_db || !analysis_db || completion_row.status != "COMPLETED" ||
        !completion_row.manifest_blob || !completion_row.manifest_sha256 ||
        completion_row.manifest_sha256->size() != 64)
    {
        Fail("battle.record requires one completed Battle Completion", error_out);
        return std::nullopt;
    }

    const auto manifest_bytes = std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(
            completion_row.manifest_blob->data()),
        completion_row.manifest_blob->size());
    completion::BattleCompletionManifestV1 manifest{};
    if (hash::sha256(manifest_bytes.data(), manifest_bytes.size()) !=
            *completion_row.manifest_sha256 ||
        !completion::DecodeBattleCompletionManifestV1(
            manifest_bytes, manifest))
    {
        Fail("Battle Completion manifest bytes are malformed or changed",
             error_out);
        return std::nullopt;
    }

    // The relational manifest blob is the durable Battle Completion result.
    // Its first-class artifact is deliberately optional so deleting a user-
    // managed .bcmb file cannot invalidate that result or prevent a replay.
    if (completion_row.manifest_artifact_id)
    {
        const auto artifact = state_db->GetArtifact(
            *completion_row.manifest_artifact_id);
        if (artifact)
        {
            if (artifact->artifact_kind != completion::ManifestArtifactKind ||
                artifact->sha256 != *completion_row.manifest_sha256 ||
                artifact->size_bytes !=
                    static_cast<std::int64_t>(manifest_bytes.size()))
            {
                Fail("Battle Completion manifest artifact metadata drifted",
                     error_out);
                return std::nullopt;
            }
            std::error_code exists_error;
            const bool artifact_exists = std::filesystem::exists(
                artifact->object_path, exists_error);
            if ((!exists_error && artifact_exists &&
                 !IsExactFile(artifact->object_path,
                              artifact->sha256,
                              artifact->size_bytes)) ||
                exists_error)
            {
                Fail("Battle Completion manifest artifact changed",
                     error_out);
                return std::nullopt;
            }
        }
    }

    const auto selected = analysis_db->GetBattleTurnJob(
        completion_row.selected_turn_job_id);
    const auto selected_result = selected && selected->exec_job_id
        ? analysis_db->GetBattleSingleTurnResultForExecJob(
              *selected->exec_job_id)
        : std::nullopt;
    if (!selected || !selected->exec_job_id ||
        *selected->exec_job_id != completion_row.selected_execution_job_id ||
        selected->wave_id != completion_row.wave_id ||
        selected->job_state != BattleTurnJobState::Succeeded ||
        selected->battle_outcome != BattleTurnOutcome::Victory ||
        !selected_result || selected_result->terminal_kind != "SUCCEEDED" ||
        selected_result->domain_outcome !=
            std::optional<std::string>("Victory") ||
        !selected_result->ending_rng)
    {
        Fail("selected Battle Completion Victory lineage is no longer exact",
             error_out);
        return std::nullopt;
    }
    const completion::BattleCompletionLineageV1 expected_lineage{
        static_cast<std::uint64_t>(completion_row.battle_set_id),
        static_cast<std::uint64_t>(completion_row.wave_id),
        static_cast<std::uint64_t>(completion_row.selected_turn_job_id),
        static_cast<std::uint64_t>(completion_row.selected_execution_job_id)};
    if (manifest.lineage != expected_lineage)
    {
        Fail("Battle Completion manifest lineage drifted", error_out);
        return std::nullopt;
    }

    const auto battle_set = analysis_db->GetBattleSet(
        completion_row.battle_set_id);
    if (!battle_set)
    {
        Fail("BattleSet for selected Battle Completion is missing", error_out);
        return std::nullopt;
    }
    std::vector<phase::BattleReplayTurnV1> reverse_turns;
    reverse_turns.reserve(8);
    BattleTurnJobSnapshot cursor_job = *selected;
    BattleTurnWaveSnapshot cursor_wave{};
    for (;;)
    {
        if (reverse_turns.size() >= kMaximumLineageTurns)
        {
            Fail("selected Battle lineage exceeds the supported turn count",
                 error_out);
            return std::nullopt;
        }
        const auto wave = analysis_db->GetBattleTurnWave(cursor_job.wave_id);
        const auto result = cursor_job.exec_job_id
            ? analysis_db->GetBattleSingleTurnResultForExecJob(
                  *cursor_job.exec_job_id)
            : std::nullopt;
        const auto commands = cursor_job.resolved_turn_commands_blob
            ? soa::battle::actions::decode_battle_turn_commands_hex(
                  *cursor_job.resolved_turn_commands_blob)
            : std::nullopt;
        if (!wave || wave->battle_set_id != battle_set->battle_set_id ||
            wave->turn_index <= 0 || !cursor_job.exec_job_id ||
            cursor_job.job_state != BattleTurnJobState::Succeeded || !result ||
            result->terminal_kind != "SUCCEEDED" || !result->domain_outcome ||
            !result->ending_rng || *result->ending_rng < 0 ||
            static_cast<std::uint64_t>(*result->ending_rng) >
                std::numeric_limits<std::uint32_t>::max() ||
            !commands || commands->empty() || commands->size() > 4 ||
            cursor_job.fake_attacks_this_turn < 0)
        {
            Fail("selected Battle replay lineage contains an incomplete turn",
                 error_out);
            return std::nullopt;
        }

        const bool final_turn = reverse_turns.empty();
        const auto expected_outcome = final_turn
            ? savor::runtime::battlesingleturn::
                  BattleSingleTurnOutcomeV1::Victory
            : savor::runtime::battlesingleturn::
                  BattleSingleTurnOutcomeV1::ReachedNextTurn;
        const auto expected_name = final_turn ? "Victory" : "ReachedNextTurn";
        const auto expected_db_outcome = final_turn
            ? BattleTurnOutcome::Victory
            : BattleTurnOutcome::ReachedNextTurn;
        if (*result->domain_outcome != expected_name ||
            cursor_job.battle_outcome != expected_db_outcome ||
            (final_turn && result->successor_savestate_id !=
                               cursor_job.output_savestate_id) ||
            (!final_turn && (!cursor_job.output_savestate_id ||
                result->successor_savestate_id !=
                    cursor_job.output_savestate_id)))
        {
            Fail("selected Battle replay outcomes are not a contiguous Victory path",
                 error_out);
            return std::nullopt;
        }
        reverse_turns.push_back({
            .turn_index = static_cast<std::uint32_t>(wave->turn_index),
            .plan = {
                .fake_attack_count = static_cast<std::uint32_t>(
                    cursor_job.fake_attacks_this_turn),
                .commands = *commands,
            },
            .expected_outcome = expected_outcome,
            .expected_ending_rng = static_cast<std::uint32_t>(
                *result->ending_rng),
        });
        cursor_wave = *wave;

        if (wave->turn_index == 1)
        {
            if (wave->parent_wave_id || wave->parent_turn_job_id ||
                cursor_job.source_savestate_id !=
                    std::optional<std::int64_t>(
                        battle_set->entry_savestate_id))
            {
                Fail("turn-one Battle replay lineage has an unexpected parent",
                     error_out);
                return std::nullopt;
            }
            break;
        }
        if (!wave->parent_wave_id || !wave->parent_turn_job_id)
        {
            Fail("later Battle replay turn has no exact parent", error_out);
            return std::nullopt;
        }
        const auto parent = analysis_db->GetBattleTurnJob(
            *wave->parent_turn_job_id);
        if (!parent || parent->wave_id != *wave->parent_wave_id ||
            !parent->output_savestate_id ||
            cursor_job.source_savestate_id != parent->output_savestate_id)
        {
            Fail("Battle replay parent linkage is ambiguous or changed",
                 error_out);
            return std::nullopt;
        }
        cursor_job = *parent;
    }

    std::ranges::reverse(reverse_turns);
    for (std::size_t index = 0; index < reverse_turns.size(); ++index)
    {
        if (reverse_turns[index].turn_index != index + 1)
        {
            Fail("Battle replay turns are not one-based and contiguous",
                 error_out);
            return std::nullopt;
        }
    }

    const auto seed_candidate = analysis_db->GetBattleSeedCandidate(
        cursor_wave.seed_candidate_id);
    const auto probe = seed_candidate && seed_candidate->source_probe_result_id
        ? analysis_db->GetSeedProbeResult(
              *seed_candidate->source_probe_result_id)
        : std::nullopt;
    const auto input_frame = seed_candidate &&
            seed_candidate->source_input_frame_id
        ? analysis_db->GetAnalysisInputFrame(
              *seed_candidate->source_input_frame_id)
        : std::nullopt;
    if (!seed_candidate ||
        seed_candidate->battle_set_id != battle_set->battle_set_id ||
        seed_candidate->source_kind !=
            BattleSeedCandidateSourceKind::SeedProbeConfirmedResult ||
        !seed_candidate->source_probe_result_id ||
        !seed_candidate->source_input_frame_id || !probe ||
        probe->evidence_state != SeedProbeEvidenceState::Confirmed ||
        probe->input_frame_id != *seed_candidate->source_input_frame_id ||
        static_cast<std::int64_t>(probe->seed_value) !=
            seed_candidate->seed_value || !input_frame)
    {
        Fail("Battle replay has no exact confirmed turn-one SeedProbe frame",
             error_out);
        return std::nullopt;
    }

    phase::BattleReplayPlanV1 plan{};
    plan.selected_lineage = expected_lineage;
    plan.battle_completion_id = static_cast<std::uint64_t>(
        completion_row.battle_completion_id);
    plan.confirmed_seed_frame = SeedFrame(*input_frame);
    plan.turns = std::move(reverse_turns);
    plan.expected_completion = std::move(manifest);
    plan.canonical_sha256 = phase::ComputeBattleReplayPlanHashV1(plan);
    std::string diagnostic;
    if (!phase::ValidateBattleReplayPlanV1(plan, &diagnostic))
    {
        Fail("Battle replay plan is invalid: " + diagnostic, error_out);
        return std::nullopt;
    }
    return plan;
}

phase::BattleReplaySourceBindingV1 SourceBinding(
    const SavestateRecord& state,
    const std::optional<ArtifactRecord>& dtm,
    const std::optional<ArtifactRecord>& itinerary)
{
    phase::BattleReplaySourceBindingV1 binding{};
    binding.source_savestate_id = static_cast<std::uint64_t>(
        state.savestate_id);
    binding.source_savestate_sha256 = state.artifact_sha256;
    if (dtm)
    {
        binding.source_dtm_artifact_id = static_cast<std::uint64_t>(
            dtm->artifact_id);
        binding.source_dtm_sha256 = dtm->sha256;
    }
    if (itinerary)
    {
        binding.source_itinerary_artifact_id = static_cast<std::uint64_t>(
            itinerary->artifact_id);
        binding.source_itinerary_sha256 = itinerary->sha256;
    }
    binding.canonical_sha256 =
        phase::ComputeBattleReplaySourceBindingHashV1(binding);
    return binding;
}

std::optional<phase::BattleReplaySourceBindingV1>
ResolveRecordingSourceBinding(
    IStateDb* state_db,
    IAnalysisDb* analysis_db,
    std::int64_t battle_set_entry_savestate_id,
    std::string* error_out)
{
    evidence::PreparedSterilizedCheckpointEvidence prepared{};
    if (!evidence::ResolvePreparedSterilizedCheckpointEvidence(
            state_db, analysis_db, battle_set_entry_savestate_id,
            &prepared, error_out))
        return std::nullopt;
    auto binding = SourceBinding(
        prepared.paired_source_checkpoint,
        prepared.effective_dtm_artifact,
        prepared.itinerary_artifact);
    std::string diagnostic;
    if (!phase::ValidateBattleReplaySourceBindingV1(binding, &diagnostic))
    {
        Fail("Battle Recording source binding is invalid: " + diagnostic,
             error_out);
        return std::nullopt;
    }
    return binding;
}

bool HasAdjacentDtm(const std::filesystem::path& savestate)
{
    std::error_code error;
    if (std::filesystem::is_regular_file(savestate.string() + ".dtm", error))
        return true;
    error.clear();
    auto replaced = savestate;
    replaced.replace_extension(".dtm");
    return replaced != savestate &&
        std::filesystem::is_regular_file(replaced, error);
}

std::optional<phase::BattleReplaySourceBindingV1>
ResolveReplaySourceBinding(
    IStateDb* state_db,
    IAnalysisDb* analysis_db,
    std::int64_t battle_set_entry_savestate_id,
    std::string* error_out)
{
    if (!state_db || !analysis_db || battle_set_entry_savestate_id <= 0)
    {
        Fail("Battle Replay source dependencies are unavailable", error_out);
        return std::nullopt;
    }
    const auto state = state_db->GetSavestate(battle_set_entry_savestate_id);
    if (!state || !state->is_complete || state->artifact_kind != "SAV" ||
        state->artifact_file_ext != ".sav" ||
        !IsExactFile(state->artifact_filename, state->artifact_sha256,
                     state->artifact_size_bytes))
    {
        Fail("BattleSet entry savestate is incomplete or physically changed",
             error_out);
        return std::nullopt;
    }

    if (state->playback_state == SavestatePlaybackState::MovieInactive)
    {
        if (state->dtm_artifact_id || state->dtm_sha256 ||
            state->dtm_filename || HasAdjacentDtm(state->artifact_filename))
        {
            Fail("movie-inactive BattleSet entry has contradictory DTM evidence",
                 error_out);
            return std::nullopt;
        }
        return SourceBinding(*state, std::nullopt, std::nullopt);
    }

    if (state->playback_state != SavestatePlaybackState::MoviePaired ||
        !state->dtm_artifact_id || !state->dtm_sha256 ||
        !state->dtm_filename)
    {
        Fail("BattleSet entry has an unsupported playback state", error_out);
        return std::nullopt;
    }
    const auto dtm = state_db->GetArtifact(*state->dtm_artifact_id);
    const auto root = dtm
        ? state_db->FindTasMovieRootByDtmArtifactId(dtm->artifact_id)
        : std::nullopt;
    const auto validation_request = root && root->source_context_id > 0
        ? analysis_db->GetTasMovieValidationRequest(root->source_context_id)
        : std::nullopt;
    const auto validation_status = validation_request
        ? analysis_db->GetTasMovieValidationStatus(
              validation_request->effective_dtm_sha256)
        : std::nullopt;
    const auto validation_attempt = validation_status
        ? analysis_db->GetTasMovieValidationAttempt(
              validation_status->validation_attempt_id)
        : std::nullopt;
    const auto itinerary = root
        ? state_db->GetArtifact(root->itinerary_artifact_id)
        : std::nullopt;
    if (!dtm || dtm->artifact_kind != "DTM" || dtm->file_ext != ".dtm" ||
        dtm->sha256 != *state->dtm_sha256 ||
        dtm->object_path != *state->dtm_filename ||
        !IsExactFile(dtm->object_path, dtm->sha256, dtm->size_bytes) ||
        !root || root->checkpoint_savestate_id != state->savestate_id ||
        root->dtm_artifact_id != dtm->artifact_id ||
        root->source_context_kind != "tmv_validation_request" ||
        !validation_request ||
        validation_request->operation != TasMovieValidationOperation::Validate ||
        validation_request->effective_dtm_sha256 != dtm->sha256 ||
        !validation_status ||
        validation_status->status != TasMovieValidationStatus::Valid ||
        !validation_attempt ||
        validation_attempt->validation_request_id !=
            validation_request->validation_request_id ||
        validation_attempt->outcome != TasMovieValidationOutcome::Valid ||
        validation_attempt->produced_tas_movie_root_id !=
            root->tas_movie_root_id ||
        !itinerary || itinerary->artifact_kind != "TAS_MOVIE_ITINERARY" ||
        !IsExactFile(itinerary->object_path, itinerary->sha256,
                     itinerary->size_bytes))
    {
        Fail("movie-paired BattleSet entry lacks exact validated TAS evidence",
             error_out);
        return std::nullopt;
    }
    auto binding = SourceBinding(*state, *dtm, *itinerary);
    std::string diagnostic;
    if (!phase::ValidateBattleReplaySourceBindingV1(binding, &diagnostic))
    {
        Fail("Battle Replay source binding is invalid: " + diagnostic,
             error_out);
        return std::nullopt;
    }
    return binding;
}

std::string JobInput(std::int64_t recording_id,
                     std::string_view plan_hash,
                     std::string_view source_binding_hash,
                     std::string_view phase_hash)
{
    return "BREC1:" + std::to_string(recording_id) + ":" +
        std::string(plan_hash) + ":" + std::string(source_binding_hash) +
        ":" + std::string(phase_hash);
}

std::string Fingerprint(const BattleRecordingRecord& row,
                        std::string_view phase_hash)
{
    const std::string canonical = std::to_string(row.battle_recording_id) +
        "\n" + row.replay_plan_sha256 + "\n" + row.source_binding_sha256 +
        "\n" + std::string(phase_hash);
    return "PK=10;PV=1;brec=" +
        hash::sha256(canonical.data(), canonical.size());
}

ProgramResultDecision Decision(
    std::string state,
    std::optional<std::string> code = std::nullopt,
    std::optional<std::string> text = std::nullopt)
{
    ProgramResultDecision result{};
    result.final_job_state = std::move(state);
    result.error_code = std::move(code);
    result.error_text = std::move(text);
    return result;
}

ProgramResultOutput Output(std::int64_t id)
{
    return {.output_key = std::string(kOutputKey),
            .data_kind = std::string(kOutputDataKind),
            .ref_kind = std::string(kProgramRefKind),
            .ref_id = id};
}

ProgramJobContinuationOutput ContinuationOutput(std::int64_t id)
{
    return {.output_key = std::string(kOutputKey),
            .data_kind = std::string(kOutputDataKind),
            .ref_kind = std::string(kProgramRefKind),
            .ref_id = id};
}

std::string ReplayJobInput(std::int64_t replay_id,
                           std::string_view plan_hash,
                           std::string_view source_binding_hash,
                           std::string_view phase_hash)
{
    return "BRPL1:" + std::to_string(replay_id) + ":" +
        std::string(plan_hash) + ":" + std::string(source_binding_hash) +
        ":" + std::string(phase_hash);
}

std::string ReplayFingerprint(const BattleReplayRecord& row,
                              std::string_view phase_hash)
{
    const std::string canonical = std::to_string(row.battle_replay_id) +
        "\n" + row.replay_plan_sha256 + "\n" + row.source_binding_sha256 +
        "\n" + std::string(phase_hash);
    return "PK=12;PV=1;brep=" +
        hash::sha256(canonical.data(), canonical.size());
}

ProgramResultOutput ReplayOutput(std::int64_t id)
{
    return {.output_key = std::string(kReplayOutputKey),
            .data_kind = std::string(kReplayOutputDataKind),
            .ref_kind = std::string(kReplayProgramRefKind),
            .ref_id = id};
}

ProgramJobContinuationOutput ReplayContinuationOutput(std::int64_t id)
{
    return {.output_key = std::string(kReplayOutputKey),
            .data_kind = std::string(kReplayOutputDataKind),
            .ref_kind = std::string(kReplayProgramRefKind),
            .ref_id = id};
}

class Materializer final : public IProgramJobMaterializer
{
public:
    Materializer(IExecutionDb* execution, IStateDb* state,
                 IAnalysisDb* analysis, BattleRecordProgramConfig config)
        : execution_(execution), state_(state), analysis_(analysis),
          config_(std::move(config))
    {
    }

    bool MaterializeJobs(const ProgramJobMaterializationContext& context,
                     WorkflowStepScheduleResult* result_out,
                     std::string* error_out) const override
    {
        if (!result_out || !execution_ || !state_ || !analysis_ ||
            context.step.step_kind != kStepKind)
            return Fail("Battle Recording materialization is incomplete",
                        error_out);
        *result_out = {};
        const auto completion_id = CompletionId(context);
        const auto completion_row = completion_id
            ? analysis_->GetBattleCompletion(*completion_id)
            : std::nullopt;
        if (!completion_row)
            return Fail("battle.record requires an explicit completed Battle Completion",
                        error_out);
        auto plan = BuildCanonicalBattleReplayPlan(
            state_, analysis_, *completion_row, error_out);
        if (!plan)
            return false;
        const auto battle_set = analysis_->GetBattleSet(
            static_cast<std::int64_t>(plan->selected_lineage.battle_set_id));
        auto source_binding = battle_set
            ? ResolveRecordingSourceBinding(
                  state_, analysis_, battle_set->entry_savestate_id, error_out)
            : std::nullopt;
        if (!battle_set || !source_binding)
            return battle_set ? false : Fail(
                "Battle Recording BattleSet is unavailable", error_out);
        std::string diagnostic;
        const auto plan_bytes = phase::EncodeBattleReplayPlanV1(*plan, &diagnostic);
        const auto source_binding_bytes =
            phase::EncodeBattleReplaySourceBindingV1(
                *source_binding, &diagnostic);
        auto definition = phase::PrepareBattleRecordFullPhaseV1(
            *plan, &diagnostic);
        if (plan_bytes.empty() || source_binding_bytes.empty() || !definition)
            return Fail("Battle Recording phase preparation failed: " + diagnostic,
                        error_out);

        const auto package = savor::runtime::fullphase::
            BuildFullPhaseProgramPackage(*definition);
        ResolvedWorksetObservationBindingV1 observation;
        if (!ResolveWorksetObservationBindingV1(
                context, ObservationDefaults(),
                Root(config_.working_dir_root) / "captures", &observation,
                error_out))
            return false;
        ResolvedWorksetDerivedStateBindingV1 derived;
        if (!ResolveWorksetDerivedStateBindingV1(
                std::span<const std::string>{}, package, &derived, error_out))
            return false;

        const auto materialization_key = "battle.record.step." +
            std::to_string(context.step.workflow_step_id);
        std::int64_t recording_id = 0;
        if (!analysis_->CreateBattleRecording({
                .battle_completion_id = completion_row->battle_completion_id,
                .workflow_instance_id = context.step.workflow_instance_id,
                .workflow_step_id = context.step.workflow_step_id,
                .source_savestate_id =
                    static_cast<std::int64_t>(
                        source_binding->source_savestate_id),
                .source_dtm_artifact_id =
                    static_cast<std::int64_t>(
                        *source_binding->source_dtm_artifact_id),
                .source_itinerary_artifact_id =
                    static_cast<std::int64_t>(
                        *source_binding->source_itinerary_artifact_id),
                .source_binding_version = 1,
                .source_binding_blob = std::string(
                    reinterpret_cast<const char*>(source_binding_bytes.data()),
                    source_binding_bytes.size()),
                .source_binding_sha256 = source_binding->canonical_sha256,
                .replay_plan_version = phase::ProgramVersion,
                .replay_plan_blob = std::string(
                    reinterpret_cast<const char*>(plan_bytes.data()),
                    plan_bytes.size()),
                .replay_plan_sha256 = plan->canonical_sha256,
                .status = "QUEUED",
                .created_at_utc = types::UtcNow(),
                .correlation_id = materialization_key,
                .causation_id = "explicit-battle-record-request",
            }, &recording_id, error_out))
            return false;
        const auto row = analysis_->GetBattleRecording(recording_id);
        if (!row)
            return Fail("Battle Recording request could not be reloaded",
                        error_out);

        EnsureMaterializingJobSetReceipt ensured{};
        if (!execution_->EnsureMaterializingJobSet({
                .materialization_key = materialization_key,
                .program_kind = static_cast<std::int32_t>(savor::PK_BattleRecord),
                .purpose = std::string(kPurpose),
                .created_by = std::string(kCreatedBy),
                .created_at_utc = NowMs(),
                .priority_boost = context.step.step_priority,
                .expected_total = 1,
                .domain_ref_kind = std::string(kProgramRefKind),
                .domain_ref_id = recording_id,
                .meta_note = "completion=" +
                    std::to_string(completion_row->battle_completion_id),
            }, &ensured, error_out))
            return false;
        if (ensured.materialization_state == "MATERIALIZING")
        {
            CreatePendingJobReceipt created{};
            if (!execution_->CreatePendingJob({
                    .job_set_id = ensured.job_set_id,
                    .program_kind =
                        static_cast<std::int32_t>(savor::PK_BattleRecord),
                    .program_version = phase::ProgramVersion,
                    .program_ref_kind = std::string(kProgramRefKind),
                    .program_ref_id = recording_id,
                    .savestate_id = static_cast<std::int64_t>(
                        source_binding->source_savestate_id),
                    .fingerprint = Fingerprint(
                        *row, definition->identity().canonical_sha256),
                    .priority = context.step.step_priority,
                    .max_attempts = 1,
                    .input_ini = JobInput(
                        recording_id, plan->canonical_sha256,
                        source_binding->canonical_sha256,
                        definition->identity().canonical_sha256),
                }, &created, error_out))
                return false;
        }
        const auto jobs = execution_->ListJobsInJobSet(ensured.job_set_id);
        if (jobs.size() != 1 || jobs.front().input_ini != JobInput(
                recording_id, plan->canonical_sha256,
                source_binding->canonical_sha256,
                definition->identity().canonical_sha256))
            return Fail("Battle Recording singleton job shape drifted",
                        error_out);
        if (!analysis_->BindBattleRecordingExecutionJob({
                .battle_recording_id = recording_id,
                .workflow_instance_id = context.step.workflow_instance_id,
                .workflow_step_id = context.step.workflow_step_id,
                .exec_job_id = jobs.front().job_id,
            }, error_out))
            return false;
        SealJobPopulationReceipt sealed{};
        if (!execution_->SealJobPopulation({
                .job_set_id = ensured.job_set_id,
                .expected_job_count = 1,
                .requested_by = std::string(kCreatedBy),
            }, &sealed, error_out))
            return false;
        const auto& identity = definition->identity();
        const auto& runtime = definition->runtime_contract();
        PublishWorksetWaveReceipt published{};
        if (!execution_->PublishWorksetWave({
                .job_set_id = ensured.job_set_id,
                .expected_job_count = 1,
                .worksets = {{
                    .job_set_id = ensured.job_set_id,
                    .workflow_step_id = context.step.workflow_step_id,
                    .workset_key = materialization_key + ".workset.0",
                    .program_kind =
                        static_cast<std::int32_t>(savor::PK_BattleRecord),
                    .program_version = phase::ProgramVersion,
                    .contract = {
                        .contract_key = "battle-record:v1:" +
                            std::to_string(recording_id) + ":plan:" +
                            plan->canonical_sha256 + ":source:" +
                            source_binding->canonical_sha256 + ":phase:" +
                            identity.canonical_sha256,
                        .module_canonical_id = runtime.module.canonical_id,
                        .module_version =
                            static_cast<std::int32_t>(runtime.module.revision),
                        .module_sha256 = runtime.module.canonical_hash,
                        .entrypoint = runtime.entrypoint,
                        .verified_dependency_sha256 =
                            runtime.verified_dependency_sha256,
                        .runtime_profile_sha256 = runtime.runtime_profile_sha256,
                        .program_package_sha256 = package.canonical_sha256,
                        .estimated_payload_bytes = kDeclaredTerminalBytes,
                    },
                    .derived_state = {
                        .binding_payload = derived.encoded_binding,
                        .binding_sha256 = derived.binding_sha256,
                    },
                    .observation = {
                        .capture_binding_payload =
                            observation.encoded_capture_binding,
                        .capture_binding_sha256 =
                            observation.capture_binding_sha256,
                        .progress_plan_payload =
                            observation.encoded_progress_plan,
                        .progress_plan_sha256 =
                            observation.progress_plan_sha256,
                    },
                    .priority = context.step.step_priority,
                    .ordered_job_ids = {jobs.front().job_id},
                    .requested_by = std::string(kCreatedBy),
                }},
                .requested_by = std::string(kCreatedBy),
            }, &published, error_out))
            return false;
        result_out->job_set_id = ensured.job_set_id;
        result_out->persistence = {
            .program_ref_kind = std::string(kProgramRefKind),
            .program_ref_id = recording_id,
            .fingerprint = Fingerprint(*row, identity.canonical_sha256),
            .program_version = phase::ProgramVersion,
        };
        result_out->event_lines.push_back(
            "[battle-record-materialized] recording=" +
            std::to_string(recording_id) + " completion=" +
            std::to_string(completion_row->battle_completion_id) + " turns=" +
            std::to_string(plan->turns.size()) + " plan=" +
            plan->canonical_sha256);
        return true;
    }

    bool Continue(const ProgramJobContinuationContext& context,
                  ProgramJobContinuationResult* result_out,
                  std::string* error_out) const override
    {
        if (!result_out || !execution_ || !analysis_)
            return Fail("Battle Recording continuation is incomplete",
                        error_out);
        const auto jobs = execution_->ListJobsInJobSet(context.job_set_id);
        if (jobs.size() != 1)
            return Fail("Battle Recording lost its singleton job shape",
                        error_out);
        const auto job = execution_->GetExecutionJob(jobs.front().job_id);
        const auto row = job
            ? analysis_->GetBattleRecording(job->program_ref_id)
            : std::nullopt;
        if (!job || !row ||
            (row->status != "COMPLETED" &&
             row->status != "REPLAY_MISMATCH"))
            return Fail("Battle Recording result is not durable", error_out);
        result_out->disposition = ProgramJobContinuationDisposition::Complete;
        result_out->output = ContinuationOutput(row->battle_recording_id);
        return true;
    }

private:
    IExecutionDb* execution_{};
    IStateDb* state_{};
    IAnalysisDb* analysis_{};
    BattleRecordProgramConfig config_;
};

class Reconstruction final : public IWorksetReconstructionAdapter
{
public:
    Reconstruction(IStateDb* state, IAnalysisDb* analysis,
                   std::filesystem::path root)
        : state_(state), analysis_(analysis), root_(Root(root))
    {
    }

    std::optional<WorksetReconstructionResult> Reconstruct(
        const WorksetReconstructionContext& context,
        std::string* error_out) const override
    {
        const auto fail = [&](std::string message) {
            Fail(std::move(message), error_out);
            return std::optional<WorksetReconstructionResult>{};
        };
        if (!state_ || !analysis_ || context.items.size() != 1 ||
            context.workset_id <= 0 || context.dispatch_attempt_id <= 0 ||
            context.workflow_step_id <= 0 || context.job_set_id <= 0 ||
            context.dispatch_token.empty() ||
            !context.state_compatibility.Complete())
            return fail("Battle Recording reconstruction requires one exact item");
        const auto& item = context.items.front();
        const auto row = analysis_->GetBattleRecording(item.program_ref_id);
        const auto completion_row = row
            ? analysis_->GetBattleCompletion(row->battle_completion_id)
            : std::nullopt;
        if (!row || !completion_row || row->status != "QUEUED" ||
            row->exec_job_id != item.job_id ||
            item.program_kind !=
                static_cast<std::int32_t>(savor::PK_BattleRecord) ||
            item.program_version != phase::ProgramVersion ||
            item.program_ref_kind != kProgramRefKind ||
            item.savestate_id != row->source_savestate_id)
            return fail("Battle Recording immutable request drifted");
        auto live_plan = BuildCanonicalBattleReplayPlan(
            state_, analysis_, *completion_row, error_out);
        if (!live_plan)
            return std::nullopt;
        const auto stored_bytes = std::span<const std::uint8_t>(
            reinterpret_cast<const std::uint8_t*>(row->replay_plan_blob.data()),
            row->replay_plan_blob.size());
        phase::BattleReplayPlanV1 stored_plan{};
        std::string diagnostic;
        if (!phase::DecodeBattleReplayPlanV1(
                stored_bytes, stored_plan, &diagnostic) ||
            stored_plan.battle_completion_id !=
                static_cast<std::uint64_t>(row->battle_completion_id) ||
            stored_plan.canonical_sha256 != row->replay_plan_sha256 ||
            stored_plan.canonical_sha256 != live_plan->canonical_sha256 ||
            phase::EncodeBattleReplayPlanV1(stored_plan) !=
                phase::EncodeBattleReplayPlanV1(*live_plan))
            return fail("Battle Recording replay plan changed after admission: " +
                        diagnostic);
        const auto binding_bytes = std::span<const std::uint8_t>(
            reinterpret_cast<const std::uint8_t*>(
                row->source_binding_blob.data()),
            row->source_binding_blob.size());
        phase::BattleReplaySourceBindingV1 source_binding{};
        const auto battle_set = analysis_->GetBattleSet(
            static_cast<std::int64_t>(stored_plan.selected_lineage.battle_set_id));
        auto live_binding = battle_set
            ? ResolveRecordingSourceBinding(
                  state_, analysis_, battle_set->entry_savestate_id, error_out)
            : std::nullopt;
        if (!battle_set || !live_binding ||
            !phase::DecodeBattleReplaySourceBindingV1(
                binding_bytes, source_binding, &diagnostic) ||
            row->source_binding_version != 1 ||
            source_binding.canonical_sha256 != row->source_binding_sha256 ||
            source_binding.canonical_sha256 !=
                live_binding->canonical_sha256 ||
            phase::EncodeBattleReplaySourceBindingV1(source_binding) !=
                phase::EncodeBattleReplaySourceBindingV1(*live_binding))
            return fail("Battle Recording source binding changed after admission: " +
                        diagnostic);
        auto definition = phase::PrepareBattleRecordFullPhaseV1(
            stored_plan, &diagnostic);
        if (!definition)
            return fail("Battle Recording phase could not be reconstructed: " +
                        diagnostic);
        if (item.input_ini != JobInput(
                row->battle_recording_id, row->replay_plan_sha256,
                row->source_binding_sha256,
                definition->identity().canonical_sha256))
            return fail("Battle Recording item binding drifted");

        const auto source_state = state_->GetSavestate(
            static_cast<std::int64_t>(source_binding.source_savestate_id));
        const auto source_dtm = source_binding.source_dtm_artifact_id
            ? state_->GetArtifact(static_cast<std::int64_t>(
                  *source_binding.source_dtm_artifact_id))
            : std::nullopt;
        if (!source_state || !source_dtm || !source_state->is_complete ||
            source_state->playback_state != SavestatePlaybackState::MoviePaired ||
            source_state->dtm_artifact_id != source_dtm->artifact_id ||
            source_state->artifact_sha256 !=
                source_binding.source_savestate_sha256 ||
            source_dtm->sha256 != *source_binding.source_dtm_sha256)
            return fail("Battle Recording movie-paired source drifted");

        // TAS phases may explicitly choose a ReadOnlyMovie baseline, but a
        // movie-paired checkpoint is restored as a Savestate baseline so its
        // embedded playback cursor remains the source of truth.
        const auto baseline_state = root_ / "baselines" /
            (source_state->artifact_sha256 + ".sav");
        const auto baseline_dtm = std::filesystem::path(
            baseline_state.string() + ".dtm");
        const auto state_path = MaterializeSavestate(
            state_, *source_state, baseline_state, error_out);
        const auto dtm_path = MaterializeArtifact(
            state_, *source_dtm, baseline_dtm, error_out);
        if (!dtm_path || !state_path)
            return std::nullopt;

        const auto package = savor::runtime::fullphase::
            BuildFullPhaseProgramPackage(*definition);
        const auto common_bytes = phase::EncodeBattleReplayPlanV1(stored_plan);
        const auto& runtime = definition->runtime_contract();
        savor::runtime::WorkerWorksetDefinition workset{};
        workset.workset_id = savor::runtime::WorkerWorksetId(
            static_cast<std::uint64_t>(context.dispatch_attempt_id));
        workset.phase_invocation = {
            .invocation_id = {
                .workflow_step_id =
                    static_cast<std::uint64_t>(context.workflow_step_id),
                .job_set_id =
                    static_cast<std::uint64_t>(context.job_set_id),
            },
            .program_package = package,
            .common_input = savor::runtime::fullphase::MakeFullPhaseCommonInput(
                "soa.battle.record.CommonInput", 1, common_bytes),
        };
        workset.baseline = {
            .artifact = {
                .kind =
                    savor::runtime::ProgramBaselineArtifactKind::Savestate,
                .state_path = *state_path,
                .state_sha256 = source_state->artifact_sha256,
                .movie_path = *dtm_path,
                .movie_sha256 = source_dtm->sha256,
                .compatibility = context.state_compatibility,
                .lineage = {
                    .edge = runtime.baseline_lineage,
                    .producer = "SavorDb.PK_BattleRecord",
                },
            },
            .lineage = runtime.baseline_lineage,
        };
        workset.derived_state = context.derived_state;
        workset.capture = context.capture;
        workset.progress_plan = context.progress_plan;
        workset.execution_key = {
            .module = runtime.module,
            .entrypoint = runtime.entrypoint,
            .verified_dependency_sha256 = runtime.verified_dependency_sha256,
            .runtime_profile_sha256 = runtime.runtime_profile_sha256,
            .baseline = savor::runtime::ComputeProgramBaselineKey(
                workset.baseline),
            .movie_policy_sha256 = runtime.movie_policy_sha256,
            .service_policy_sha256 = runtime.service_policy_sha256,
            .program_package_sha256 = package.canonical_sha256,
            .common_input_sha256 =
                workset.phase_invocation.common_input.content_sha256,
            .derived_state_binding_sha256 =
                workset.derived_state.content_sha256,
            .capture_binding_sha256 = workset.capture
                ? workset.capture->content_sha256
                : savor::runtime::EmptyWorksetCaptureBindingHashV1(),
            .progress_plan_sha256 = workset.progress_plan.content_sha256,
        };
        workset.execution_key.canonical_sha256 =
            savor::runtime::ComputeWorkerWorksetExecutionKeyHash(
                workset.execution_key);

        const auto artifact_root = root_ / "artifacts";
        std::error_code directory_error;
        std::filesystem::create_directories(artifact_root, directory_error);
        if (directory_error)
            return fail("could not create Battle Recording output directory: " +
                        directory_error.message());
        phase::BattleRecordRequestV1 request{
            .output_dtm_path = (artifact_root /
                ("recording-" + std::to_string(row->battle_recording_id) +
                 "-attempt-" + std::to_string(item.reserved_attempt_id) +
                 ".dtm")).string(),
            .output_preseed_savestate_path = (artifact_root /
                ("recording-" + std::to_string(row->battle_recording_id) +
                 "-attempt-" + std::to_string(item.reserved_attempt_id) +
                 "-preseed.sav")).string(),
        };
        workset.items.push_back({
            .item_id = savor::runtime::WorkerWorksetItemId(
                static_cast<std::uint64_t>(item.job_id)),
            .ordinal = 0,
            .execution = {
                .execution_id = savor::runtime::ProgramExecutionId(
                    static_cast<std::uint64_t>(item.job_id)),
                .attempt_id =
                    savor::runtime::AttemptId(item.reserved_attempt_id),
                .input_payload =
                    phase::EncodeBattleRecordExecutionInputV1(request),
            },
            .declared_terminal_bytes = kDeclaredTerminalBytes,
            .correlation = {
                .durable_job_id = std::to_string(item.job_id),
                .claim_token = item.claim_token,
                .parent_correlation = context.contract_key,
            },
        });
        std::vector<std::uint8_t> encoded;
        const auto status = savor::runtime::EncodeWorkerWorksetV5(
            workset, encoded);
        if (!status)
            return fail("Battle Recording workset encoding failed: " +
                        status.message);
        workset.encoded_size_bytes = encoded.size();
        return WorksetReconstructionResult{
            .workset = std::move(workset),
            .ordered_job_ids = {item.job_id},
        };
    }

private:
    IStateDb* state_{};
    IAnalysisDb* analysis_{};
    std::filesystem::path root_;
};

struct PublishedRecordingArtifacts
{
    std::int64_t dtm_artifact_id = 0;
    std::int64_t itinerary_artifact_id = 0;
    std::int64_t checkpoint_savestate_id = 0;
    std::int64_t tree_id = 0;
    std::string timing_anchor_blob;
    std::filesystem::path itinerary_staging_path;
    std::string itinerary_staging_sha256;
    std::uint64_t itinerary_staging_size = 0;
};

class ResultHandler final : public IProgramResultHandler
{
public:
    ResultHandler(IStateDb* state, IAnalysisDb* analysis,
                  std::filesystem::path root)
        : state_(state), analysis_(analysis), root_(Root(root))
    {
    }

    ProgramResultDecision Process(
        const ProgramResultProcessingContext& context) const override
    {
        if (!state_ || !analysis_ ||
            context.program_kind !=
                static_cast<std::int32_t>(savor::PK_BattleRecord) ||
            context.program_version != phase::ProgramVersion ||
            context.program_ref_kind != kProgramRefKind)
            return Decision("FAILED", "BATTLE_RECORD_IDENTITY_INVALID",
                "job is not an exact battle.record request");
        const auto row = analysis_->GetBattleRecording(context.program_ref_id);
        if (!row || row->exec_job_id != context.job_id)
            return Decision("FAILED", "BATTLE_RECORD_REQUEST_DRIFT",
                "durable Battle Recording identity drifted");
        if (row->status == "COMPLETED" || row->status == "REPLAY_MISMATCH") {
            if (row->worker_terminal_sha256 != context.terminal.sha256)
                return Decision("FAILED", "BATTLE_RECORD_RESULT_DRIFT",
                    "completed Battle Recording belongs to another worker terminal");
            return SuccessfulDecision(*row);
        }

        const auto plan_bytes = std::span<const std::uint8_t>(
            reinterpret_cast<const std::uint8_t*>(row->replay_plan_blob.data()),
            row->replay_plan_blob.size());
        phase::BattleReplayPlanV1 plan{};
        std::string error;
        if (!phase::DecodeBattleReplayPlanV1(plan_bytes, plan, &error) ||
            plan.battle_completion_id !=
                static_cast<std::uint64_t>(row->battle_completion_id) ||
            plan.canonical_sha256 != row->replay_plan_sha256)
            return PersistFailure(*row, context, "BATTLE_RECORD_PLAN_INVALID",
                error.empty() ? "durable replay plan is malformed" : error);
        auto definition = phase::PrepareBattleRecordFullPhaseV1(plan, &error);
        if (!definition || context.input_ini != JobInput(
                row->battle_recording_id, row->replay_plan_sha256,
                row->source_binding_sha256,
                definition->identity().canonical_sha256))
            return PersistFailure(*row, context, "BATTLE_RECORD_PLAN_DRIFT",
                error.empty() ? "prepared replay identity drifted" : error);

        savor::runtime::DurableWorkerTerminalEnvelope terminal{};
        if (!savor::runtime::DecodeDurableWorkerTerminalEnvelope(
                context.terminal.envelope, &terminal, &error))
            return PersistFailure(*row, context,
                "BATTLE_RECORD_TERMINAL_INVALID", error);
        using Status = savor::wrms::InvocationTerminalStatus;
        if (terminal.terminal.status != Status::Succeeded ||
            terminal.terminal.unstarted ||
            terminal.terminal.workset_id !=
                static_cast<std::uint64_t>(
                    context.terminal.dispatch_attempt_id) ||
            terminal.terminal.item_id !=
                static_cast<std::uint64_t>(context.job_id) ||
            terminal.terminal.invocation_id !=
                static_cast<std::uint64_t>(context.job_id) ||
            terminal.terminal.attempt_id !=
                context.terminal.reserved_attempt_id ||
            terminal.process_generation == 0 ||
            terminal.terminal.workset_epoch == 0 ||
            terminal.terminal.session_disposition !=
                savor::wrms::SessionDispositionCode::Clean)
            return PersistFailure(*row, context,
                terminal.terminal.error_code.empty()
                    ? "BATTLE_RECORD_EXECUTION_FAILED"
                    : terminal.terminal.error_code,
                terminal.terminal.message.empty()
                    ? "battle.record did not complete cleanly"
                    : terminal.terminal.message,
                "FAILED");

        phase::BattleRecordResultV1 result{};
        if (!definition->DecodeProgramResult(
                terminal.terminal.result, result, &error))
            return PersistFailure(*row, context,
                "BATTLE_RECORD_RESULT_INVALID",
                error.empty() ? "Battle Recording result is malformed" : error);

        if (result.outcome == completion::BattleRecordOutcomeV1::ReplayMismatch)
        {
            if (!analysis_->CompleteBattleRecording({
                    .battle_recording_id = row->battle_recording_id,
                    .outcome = "REPLAY_MISMATCH",
                    .worker_terminal_sha256 = context.terminal.sha256,
                    .status = "REPLAY_MISMATCH",
                    .completed_at_utc = types::UtcNow(),
                    .correlation_id = "battle-recording-" +
                        std::to_string(row->battle_recording_id),
                    .causation_id = "execution-job-" +
                        std::to_string(context.job_id),
                }, &error))
                throw std::runtime_error(error);
            auto decision = Decision("SUCCEEDED");
            decision.outputs.push_back(Output(row->battle_recording_id));
            decision.event_lines.push_back(
                "[battle-record-replay-mismatch] recording=" +
                std::to_string(row->battle_recording_id) +
                " turn=" + std::to_string(result.mismatch_turn) +
                " expected_rng=" + std::to_string(result.expected_rng) +
                " observed_rng=" + std::to_string(result.observed_rng) +
                " completion_observed=" +
                (result.observed_completion ? "1" : "0"));
            return decision;
        }

        if (!result.observed_completion || !result.transition ||
            !completion::SemanticallyEqualBattleCompletionManifestV1(
                *result.observed_completion, plan.expected_completion) ||
            result.observed_completion->transition != *result.transition ||
            result.timing_anchor.turn_index != plan.turns.back().turn_index ||
            result.timing_anchor.actor_slot !=
                plan.turns.back().plan.commands.back().actor_slot ||
            result.timing_anchor.command_ordinal !=
                plan.turns.back().plan.commands.size() ||
            result.timing_anchor.semantic_role !=
                "final_player_command_commitment" ||
            result.timing_anchor.dtm_input_index == 0 ||
            result.checkpoint_input_count <
                result.timing_anchor.dtm_input_index ||
            result.final_input_count <= result.checkpoint_input_count)
            return PersistFailure(*row, context,
                "BATTLE_RECORD_SEMANTIC_DRIFT",
                "Recorded replay does not match its immutable completion plan");

        const auto published = PublishRecordedArtifacts(
            *row, plan, result, context, &error);
        if (!published)
            throw std::runtime_error(error.empty()
                ? "Battle Recording artifact publication failed"
                : error);
        if (!analysis_->CompleteBattleRecording({
                .battle_recording_id = row->battle_recording_id,
                .outcome = "RECORDED",
                .recorded_dtm_artifact_id = published->dtm_artifact_id,
                .recorded_itinerary_artifact_id =
                    published->itinerary_artifact_id,
                .paired_checkpoint_savestate_id =
                    published->checkpoint_savestate_id,
                .timing_anchor_version = completion::TimingAnchorVersion,
                .timing_anchor_blob = published->timing_anchor_blob,
                .tas_movie_tree_id = published->tree_id,
                .worker_terminal_sha256 = context.terminal.sha256,
                .status = "COMPLETED",
                .completed_at_utc = types::UtcNow(),
                .correlation_id = "battle-recording-" +
                    std::to_string(row->battle_recording_id),
                .causation_id = "execution-job-" +
                    std::to_string(context.job_id),
            }, &error))
            throw std::runtime_error(error);
        auto decision = Decision("SUCCEEDED");
        decision.cleanup_worker_staging = true;
        decision.staging_files.push_back({
            .relative_path = published->itinerary_staging_path
                .lexically_relative(root_).generic_string(),
            .sha256 = published->itinerary_staging_sha256,
            .size_bytes = published->itinerary_staging_size,
        });
        decision.outputs.push_back(Output(row->battle_recording_id));
        decision.event_lines.push_back(
            "[battle-recorded] recording=" +
            std::to_string(row->battle_recording_id) + " tree=" +
            std::to_string(published->tree_id) + " dtm=" +
            std::to_string(published->dtm_artifact_id) + " checkpoint=" +
            std::to_string(published->checkpoint_savestate_id) +
            " checkpoint_input_count=" +
            std::to_string(result.checkpoint_input_count) +
            " final_input_count=" +
            std::to_string(result.final_input_count) +
            " neutral_tail=" +
            std::to_string(
                result.final_input_count - result.checkpoint_input_count));
        return decision;
    }

    private:
    ProgramResultDecision SuccessfulDecision(
        const BattleRecordingRecord& row) const
    {
        auto decision = Decision("SUCCEEDED");
        decision.outputs.push_back(Output(row.battle_recording_id));
        return decision;
    }

    ProgramResultDecision PersistFailure(
        const BattleRecordingRecord& row,
        const ProgramResultProcessingContext& context,
        std::string code,
        std::string text,
        std::string job_state = "FAILED") const
    {
        std::string error;
        if (!analysis_->FailBattleRecording({
                .battle_recording_id = row.battle_recording_id,
                .error_code = code,
                .error_text = text,
                .worker_terminal_sha256 = context.terminal.sha256,
                .completed_at_utc = types::UtcNow(),
                .correlation_id = "battle-recording-" +
                    std::to_string(row.battle_recording_id),
                .causation_id = "execution-job-" +
                    std::to_string(context.job_id),
            }, &error))
            throw std::runtime_error(error);
        auto decision = Decision(
            std::move(job_state), std::move(code), std::move(text));
        decision.cleanup_worker_staging = true;
        return decision;
    }

    std::optional<PublishedRecordingArtifacts> PublishRecordedArtifacts(
        const BattleRecordingRecord& row,
        const phase::BattleReplayPlanV1& plan,
        const phase::BattleRecordResultV1& result,
        const ProgramResultProcessingContext& context,
        std::string* error_out) const
    {
        if (!result.observed_completion || !result.transition)
        {
            Fail("Battle Recording omitted its observed completion evidence",
                 error_out);
            return std::nullopt;
        }
        phase::BattleReplaySourceBindingV1 source_binding{};
        std::string binding_diagnostic;
        if (!phase::DecodeBattleReplaySourceBindingV1(
                std::span<const std::uint8_t>(
                    reinterpret_cast<const std::uint8_t*>(
                        row.source_binding_blob.data()),
                    row.source_binding_blob.size()),
                source_binding, &binding_diagnostic) ||
            !source_binding.source_dtm_artifact_id ||
            !source_binding.source_itinerary_artifact_id)
        {
            Fail("Battle Recording source binding is malformed: " +
                     binding_diagnostic,
                 error_out);
            return std::nullopt;
        }
        const auto& transition = *result.transition;
        const auto sav_schema = savor::runtime::program::
            CanonicalActionArtifactPayloadSchemaIdentity(
                CanonicalAction::SavestateSaveImmutableArtifact);
        const auto dtm_schema = savor::runtime::program::
            CanonicalActionArtifactPayloadSchemaIdentity(
                CanonicalAction::MovieStopRecording);
        if (!sav_schema || !dtm_schema || result.artifacts.size() != 2)
        {
            Fail("Battle Recording returned an invalid artifact contract",
                 error_out);
            return std::nullopt;
        }
        const savor::runtime::program::ArtifactReferenceValue* sav = nullptr;
        const savor::runtime::program::ArtifactReferenceValue* dtm = nullptr;
        for (const auto& artifact : result.artifacts)
        {
            if (artifact.artifact.schema == *sav_schema)
                sav = &artifact.artifact;
            else if (artifact.artifact.schema == *dtm_schema)
                dtm = &artifact.artifact;
            else
            {
                Fail("Battle Recording returned an unknown artifact schema",
                     error_out);
                return std::nullopt;
            }
        }
        if (!sav || !dtm || !sav->complete || !dtm->complete)
        {
            Fail("Battle Recording did not finalize both declared artifacts",
                 error_out);
            return std::nullopt;
        }
        const std::filesystem::path sav_path(sav->storage_reference);
        const std::filesystem::path dtm_path(dtm->storage_reference);
        const std::filesystem::path sidecar(sav_path.string() + ".dtm");
        const auto sav_sha = HashFile(sav_path);
        const auto dtm_sha = HashFile(dtm_path);
        std::error_code size_error;
        const auto sav_size = std::filesystem::file_size(sav_path, size_error);
        if (size_error || sav_size == 0 || sav_path.extension() != ".sav" ||
            !sav_sha || *sav_sha != sav->content_hash.ToHex())
        {
            Fail("Battle Recording paired checkpoint does not match worker evidence",
                 error_out);
            return std::nullopt;
        }
        size_error.clear();
        const auto dtm_size = std::filesystem::file_size(dtm_path, size_error);
        if (size_error || dtm_size == 0 || dtm_path.extension() != ".dtm" ||
            !dtm_sha || *dtm_sha != dtm->content_hash.ToHex())
        {
            Fail("Battle Recording finalized DTM does not match worker evidence",
                 error_out);
            return std::nullopt;
        }
        std::error_code sidecar_error;
        if (std::filesystem::exists(sidecar, sidecar_error) ||
            sidecar_error)
        {
            Fail("Battle Recording returned an unexpected checkpoint-time DTM sidecar",
                 error_out);
            return std::nullopt;
        }

        savor::tas::DtmFile recorded;
        std::string reason;
        if (!recorded.load(dtm_path.string()) ||
            !recorded.supports_gc_poll_editing(&reason) ||
            recorded.info().starts_from_savestate ||
            recorded.info().input_count != recorded.gc_poll_count() ||
            result.final_input_count != recorded.info().input_count ||
            result.checkpoint_input_count >= result.final_input_count)
        {
            Fail("Battle Recording finalized DTM is not a valid paired GC movie: " +
                     reason,
                 error_out);
            return std::nullopt;
        }

        const auto source_itinerary = state_->GetArtifact(
            static_cast<std::int64_t>(
                *source_binding.source_itinerary_artifact_id));
        const auto source_dtm = state_->GetArtifact(
            static_cast<std::int64_t>(
                *source_binding.source_dtm_artifact_id));
        if (!source_itinerary || !source_dtm ||
            source_itinerary->sha256 !=
                *source_binding.source_itinerary_sha256 ||
            source_dtm->sha256 != *source_binding.source_dtm_sha256)
        {
            Fail("Battle Recording source DTM itinerary evidence changed",
                 error_out);
            return std::nullopt;
        }
        const auto itinerary_path = MaterializeArtifact(
            state_, *source_itinerary,
            root_ / "itineraries" /
                (source_itinerary->sha256 + ".tmi"),
            error_out);
        const auto source_dtm_path = MaterializeArtifact(
            state_, *source_dtm,
            root_ / "baselines" / (source_dtm->sha256 + ".dtm"),
            error_out);
        if (!itinerary_path || !source_dtm_path)
            return std::nullopt;
        const auto itinerary_bytes = ReadFile(*itinerary_path, error_out);
        tasmovie::TasMovieItineraryV1 itinerary{};
        savor::tas::DtmFile original;
        if (!itinerary_bytes ||
            !tasmovie::DecodeTasMovieItineraryArtifactV1(
                *itinerary_bytes, itinerary, error_out) ||
            itinerary.checkpoints.empty() ||
            !original.load(source_dtm_path->string()) ||
            !tasmovie::ValidateTasMovieItineraryArtifactV1(
                itinerary, original.info().input_count,
                itinerary.checkpoints.back().pc, error_out))
            return std::nullopt;
        itinerary.checkpoints.push_back(tasmovie::MakeTasMovieCheckpointV1(
            transition.provenance.pc,
            result.checkpoint_input_count,
            transition.provenance.vi_count,
            transition.sct_filename,
            transition.area,
            transition.effective_suffix));
        if (!tasmovie::ValidateTasMovieItineraryArtifactV1(
                itinerary, recorded.info().input_count,
                transition.provenance.pc, error_out))
            return std::nullopt;
        const auto new_itinerary =
            tasmovie::EncodeTasMovieItineraryArtifactV1(itinerary, error_out);
        if (new_itinerary.empty())
            return std::nullopt;
        const auto itinerary_sha = hash::sha256(
            new_itinerary.data(), new_itinerary.size());
        const auto published_itinerary = root_ / "artifacts" /
            (itinerary_sha + ".tmi");
        if (!WriteFileAtomically(
                published_itinerary, new_itinerary, error_out))
            return std::nullopt;

        const auto correlation = "battle-recording-" +
            std::to_string(row.battle_recording_id);
        const auto causation = "execution-job-" +
            std::to_string(context.job_id);
        std::int64_t dtm_artifact_id = 0;
        if (!StoreWorkspaceArtifactFile(state_, dtm_path, {
                .sha256 = *dtm_sha,
                .size_bytes = static_cast<std::int64_t>(dtm_size),
                .file_ext = ".dtm",
                .artifact_kind = "DTM",
                .created_at_utc = types::UtcNow(),
                .correlation_id = correlation,
                .causation_id = causation,
            }, &dtm_artifact_id, error_out))
            return std::nullopt;
        std::int64_t sav_artifact_id = 0;
        if (!StoreWorkspaceArtifactFile(state_, sav_path, {
                .sha256 = *sav_sha,
                .size_bytes = static_cast<std::int64_t>(sav_size),
                .file_ext = ".sav",
                .artifact_kind = "SAV",
                .created_at_utc = types::UtcNow(),
                .correlation_id = correlation,
                .causation_id = causation,
            }, &sav_artifact_id, error_out))
            return std::nullopt;
        std::int64_t itinerary_artifact_id = 0;
        if (!StoreWorkspaceArtifactFile(state_, published_itinerary, {
                .sha256 = itinerary_sha,
                .size_bytes =
                    static_cast<std::int64_t>(new_itinerary.size()),
                .file_ext = ".tmi",
                .artifact_kind = "TAS_MOVIE_ITINERARY",
                .created_at_utc = types::UtcNow(),
                .correlation_id = correlation,
                .causation_id = causation,
            }, &itinerary_artifact_id, error_out))
            return std::nullopt;
        std::int64_t checkpoint_id = 0;
        if (!state_->CreateSavestate({
                .artifact_id = sav_artifact_id,
                .playback_state = SavestatePlaybackState::MoviePaired,
                .dtm_artifact_id = dtm_artifact_id,
                .savestate_type = "BATTLE_RECORD_PRESEED_CHECKPOINT",
                .note = "Movie-paired accepted field preseed after recorded Battle",
                .is_complete = true,
                .created_at_utc = types::UtcNow(),
                .correlation_id = correlation,
                .causation_id = causation,
            }, &checkpoint_id, error_out))
            return std::nullopt;
        if (!state_->DeriveSavestate({
                .from_savestate_id = static_cast<std::int64_t>(
                    source_binding.source_savestate_id),
                .to_savestate_id = checkpoint_id,
                .method_kind = "battle.record.v1",
                .source_context_kind = std::string(kProgramRefKind),
                .source_context_id = row.battle_recording_id,
                .created_at_utc = types::UtcNow(),
                .correlation_id = correlation,
                .causation_id = causation,
            }, nullptr, error_out))
            return std::nullopt;

        const auto source_tree = state_->FindTasMovieTreeByDtmArtifactId(
            static_cast<std::int64_t>(*source_binding.source_dtm_artifact_id));
        const auto source_root = source_tree
            ? state_->GetTasMovieRoot(source_tree->tas_movie_root_id)
            : state_->FindTasMovieRootByDtmArtifactId(
                static_cast<std::int64_t>(*source_binding.source_dtm_artifact_id));
        if (!source_root)
        {
            Fail("Battle Recording source TAS Movie root is missing",
                 error_out);
            return std::nullopt;
        }
        std::int64_t tree_id = 0;
        if (!state_->CreateTasMovieTree({
                .tas_movie_root_id = source_root->tas_movie_root_id,
                .parent_tas_movie_tree_id = source_tree
                    ? std::optional<std::int64_t>(source_tree->tas_movie_tree_id)
                    : std::nullopt,
                .dtm_artifact_id = dtm_artifact_id,
                .itinerary_artifact_id = itinerary_artifact_id,
                .required_final_breakpoint_pc =
                    transition.provenance.pc,
                .checkpoint_savestate_id = checkpoint_id,
                .source_context_kind = std::string(kProgramRefKind),
                .source_context_id = row.battle_recording_id,
                .created_at_utc = types::UtcNow(),
                .correlation_id = correlation,
                .causation_id = causation,
            }, &tree_id, error_out))
            return std::nullopt;
        std::vector<std::uint8_t> anchor;
        if (!completion::EncodeBattleTimingAdjustmentAnchorV1(
                result.timing_anchor, anchor))
        {
            Fail("Battle Recording timing anchor could not be encoded",
                 error_out);
            return std::nullopt;
        }
        return PublishedRecordingArtifacts{
            .dtm_artifact_id = dtm_artifact_id,
            .itinerary_artifact_id = itinerary_artifact_id,
            .checkpoint_savestate_id = checkpoint_id,
            .tree_id = tree_id,
            .timing_anchor_blob = std::string(
                reinterpret_cast<const char*>(anchor.data()), anchor.size()),
            .itinerary_staging_path = published_itinerary,
            .itinerary_staging_sha256 = itinerary_sha,
            .itinerary_staging_size =
                static_cast<std::uint64_t>(new_itinerary.size()),
        };
    }

    IStateDb* state_{};
    IAnalysisDb* analysis_{};
    std::filesystem::path root_;
};

class ReplayMaterializer final : public IProgramJobMaterializer
{
public:
    ReplayMaterializer(IExecutionDb* execution, IStateDb* state,
                       IAnalysisDb* analysis,
                       battlereplay::BattleReplayProgramConfig config)
        : execution_(execution), state_(state), analysis_(analysis),
          config_(std::move(config)) {}

    bool MaterializeJobs(const ProgramJobMaterializationContext& context,
                     WorkflowStepScheduleResult* result_out,
                     std::string* error_out) const override
    {
        if (!result_out || !execution_ || !state_ || !analysis_ ||
            context.step.step_kind != kReplayStepKind)
            return Fail("Battle Replay materialization is incomplete", error_out);
        *result_out = {};
        const auto completion_id = CompletionId(context);
        const auto completion_row = completion_id
            ? analysis_->GetBattleCompletion(*completion_id) : std::nullopt;
        if (!completion_row)
            return Fail("battle.replay requires an explicit completed Battle Completion",
                        error_out);
        auto plan = BuildCanonicalBattleReplayPlan(
            state_, analysis_, *completion_row, error_out);
        if (!plan) return false;
        const auto battle_set = analysis_->GetBattleSet(
            static_cast<std::int64_t>(plan->selected_lineage.battle_set_id));
        auto source_binding = battle_set
            ? ResolveReplaySourceBinding(
                  state_, analysis_, battle_set->entry_savestate_id, error_out)
            : std::nullopt;
        if (!battle_set || !source_binding)
            return battle_set ? false : Fail(
                "Battle Replay BattleSet is unavailable", error_out);
        std::string diagnostic;
        const auto plan_bytes = phase::EncodeBattleReplayPlanV1(*plan, &diagnostic);
        const auto source_binding_bytes =
            phase::EncodeBattleReplaySourceBindingV1(
                *source_binding, &diagnostic);
        const auto definition = replayphase::PrepareBattleReplayFullPhaseV1(
            *plan, &diagnostic);
        if (plan_bytes.empty() || source_binding_bytes.empty() || !definition)
            return Fail("Battle Replay phase preparation failed: " + diagnostic,
                        error_out);
        const auto package = savor::runtime::fullphase::
            BuildFullPhaseProgramPackage(*definition);
        ResolvedWorksetObservationBindingV1 observation;
        if (!ResolveWorksetObservationBindingV1(
                context, ObservationDefaults(),
                Root(config_.working_dir_root) / "captures", &observation,
                error_out)) return false;
        ResolvedWorksetDerivedStateBindingV1 derived;
        if (!ResolveWorksetDerivedStateBindingV1(
                std::span<const std::string>{}, package, &derived, error_out))
            return false;

        const auto key = "battle.replay.step." +
            std::to_string(context.step.workflow_step_id);
        std::int64_t replay_id = 0;
        if (!analysis_->CreateBattleReplay({
                .battle_completion_id = completion_row->battle_completion_id,
                .workflow_instance_id = context.step.workflow_instance_id,
                .workflow_step_id = context.step.workflow_step_id,
                .source_savestate_id = static_cast<std::int64_t>(
                    source_binding->source_savestate_id),
                .source_dtm_artifact_id = source_binding->source_dtm_artifact_id
                    ? std::optional<std::int64_t>(static_cast<std::int64_t>(
                          *source_binding->source_dtm_artifact_id))
                    : std::nullopt,
                .source_itinerary_artifact_id =
                    source_binding->source_itinerary_artifact_id
                    ? std::optional<std::int64_t>(static_cast<std::int64_t>(
                          *source_binding->source_itinerary_artifact_id))
                    : std::nullopt,
                .source_binding_version = 1,
                .source_binding_blob = std::string(
                    reinterpret_cast<const char*>(source_binding_bytes.data()),
                    source_binding_bytes.size()),
                .source_binding_sha256 = source_binding->canonical_sha256,
                .replay_plan_version = replayphase::ProgramVersion,
                .replay_plan_blob = std::string(
                    reinterpret_cast<const char*>(plan_bytes.data()),
                    plan_bytes.size()),
                .replay_plan_sha256 = plan->canonical_sha256,
                .status = "QUEUED", .created_at_utc = types::UtcNow(),
                .correlation_id = key,
                .causation_id = "explicit-battle-replay-request",
            }, &replay_id, error_out)) return false;
        const auto row = analysis_->GetBattleReplay(replay_id);
        if (!row) return Fail("Battle Replay request could not be reloaded",
                              error_out);

        EnsureMaterializingJobSetReceipt ensured{};
        if (!execution_->EnsureMaterializingJobSet({
                .materialization_key = key,
                .program_kind = static_cast<std::int32_t>(savor::PK_BattleReplay),
                .purpose = std::string(kReplayPurpose),
                .created_by = std::string(kReplayCreatedBy),
                .created_at_utc = NowMs(),
                .priority_boost = context.step.step_priority,
                .expected_total = 1,
                .domain_ref_kind = std::string(kReplayProgramRefKind),
                .domain_ref_id = replay_id,
                .meta_note = "completion=" +
                    std::to_string(completion_row->battle_completion_id),
            }, &ensured, error_out)) return false;
        if (ensured.materialization_state == "MATERIALIZING")
        {
            CreatePendingJobReceipt created{};
            if (!execution_->CreatePendingJob({
                    .job_set_id = ensured.job_set_id,
                    .program_kind = static_cast<std::int32_t>(savor::PK_BattleReplay),
                    .program_version = replayphase::ProgramVersion,
                    .program_ref_kind = std::string(kReplayProgramRefKind),
                    .program_ref_id = replay_id,
                    .savestate_id = static_cast<std::int64_t>(
                        source_binding->source_savestate_id),
                    .fingerprint = ReplayFingerprint(
                        *row, definition->identity().canonical_sha256),
                    .priority = context.step.step_priority,
                    .max_attempts = 1,
                    .input_ini = ReplayJobInput(replay_id,
                        plan->canonical_sha256,
                        source_binding->canonical_sha256,
                        definition->identity().canonical_sha256),
                }, &created, error_out)) return false;
        }
        const auto jobs = execution_->ListJobsInJobSet(ensured.job_set_id);
        if (jobs.size() != 1 || jobs.front().input_ini != ReplayJobInput(
                replay_id, plan->canonical_sha256,
                source_binding->canonical_sha256,
                definition->identity().canonical_sha256))
            return Fail("Battle Replay singleton job shape drifted", error_out);
        if (!analysis_->BindBattleReplayExecutionJob({
                .battle_replay_id = replay_id,
                .workflow_instance_id = context.step.workflow_instance_id,
                .workflow_step_id = context.step.workflow_step_id,
                .exec_job_id = jobs.front().job_id,
            }, error_out)) return false;
        SealJobPopulationReceipt sealed{};
        if (!execution_->SealJobPopulation({
                .job_set_id = ensured.job_set_id, .expected_job_count = 1,
                .requested_by = std::string(kReplayCreatedBy),
            }, &sealed, error_out)) return false;
        const auto& identity = definition->identity();
        const auto& runtime = definition->runtime_contract();
        PublishWorksetWaveReceipt published{};
        if (!execution_->PublishWorksetWave({
                .job_set_id = ensured.job_set_id, .expected_job_count = 1,
                .worksets = {{
                    .job_set_id = ensured.job_set_id,
                    .workflow_step_id = context.step.workflow_step_id,
                    .workset_key = key + ".workset.0",
                    .program_kind = static_cast<std::int32_t>(savor::PK_BattleReplay),
                    .program_version = replayphase::ProgramVersion,
                    .contract = {
                        .contract_key = "battle-replay:v1:" +
                            std::to_string(replay_id) + ":plan:" +
                            plan->canonical_sha256 + ":source:" +
                            source_binding->canonical_sha256 + ":phase:" +
                            identity.canonical_sha256,
                        .module_canonical_id = runtime.module.canonical_id,
                        .module_version = static_cast<std::int32_t>(
                            runtime.module.revision),
                        .module_sha256 = runtime.module.canonical_hash,
                        .entrypoint = runtime.entrypoint,
                        .verified_dependency_sha256 =
                            runtime.verified_dependency_sha256,
                        .runtime_profile_sha256 = runtime.runtime_profile_sha256,
                        .program_package_sha256 = package.canonical_sha256,
                        .estimated_payload_bytes = kDeclaredTerminalBytes,
                    },
                    .derived_state = {.binding_payload = derived.encoded_binding,
                        .binding_sha256 = derived.binding_sha256},
                    .observation = {
                        .capture_binding_payload = observation.encoded_capture_binding,
                        .capture_binding_sha256 = observation.capture_binding_sha256,
                        .progress_plan_payload = observation.encoded_progress_plan,
                        .progress_plan_sha256 = observation.progress_plan_sha256,
                    },
                    .priority = context.step.step_priority,
                    .ordered_job_ids = {jobs.front().job_id},
                    .requested_by = std::string(kReplayCreatedBy),
                }}, .requested_by = std::string(kReplayCreatedBy),
            }, &published, error_out)) return false;
        result_out->job_set_id = ensured.job_set_id;
        result_out->persistence = {
            .program_ref_kind = std::string(kReplayProgramRefKind),
            .program_ref_id = replay_id,
            .fingerprint = ReplayFingerprint(*row, identity.canonical_sha256),
            .program_version = replayphase::ProgramVersion,
        };
        result_out->event_lines.push_back(
            "[battle-replay-materialized] replay=" + std::to_string(replay_id) +
            " completion=" + std::to_string(completion_row->battle_completion_id) +
            " turns=" + std::to_string(plan->turns.size()) +
            " plan=" + plan->canonical_sha256);
        return true;
    }

    bool Continue(const ProgramJobContinuationContext& context,
                  ProgramJobContinuationResult* result_out,
                  std::string* error_out) const override
    {
        if (!result_out || !execution_ || !analysis_)
            return Fail("Battle Replay continuation is incomplete", error_out);
        const auto jobs = execution_->ListJobsInJobSet(context.job_set_id);
        if (jobs.size() != 1)
            return Fail("Battle Replay lost its singleton job shape", error_out);
        const auto job = execution_->GetExecutionJob(jobs.front().job_id);
        const auto row = job ? analysis_->GetBattleReplay(job->program_ref_id)
                             : std::nullopt;
        if (!job || !row || (row->status != "MATCHED" &&
                             row->status != "REPLAY_MISMATCH"))
            return Fail("Battle Replay result is not durable", error_out);
        result_out->disposition = ProgramJobContinuationDisposition::Complete;
        result_out->output = ReplayContinuationOutput(row->battle_replay_id);
        return true;
    }

private:
    IExecutionDb* execution_{};
    IStateDb* state_{};
    IAnalysisDb* analysis_{};
    battlereplay::BattleReplayProgramConfig config_;
};

class ReplayReconstruction final : public IWorksetReconstructionAdapter
{
public:
    ReplayReconstruction(IStateDb* state, IAnalysisDb* analysis,
                         std::filesystem::path root)
        : state_(state), analysis_(analysis), root_(Root(root)) {}

    std::optional<WorksetReconstructionResult> Reconstruct(
        const WorksetReconstructionContext& context,
        std::string* error_out) const override
    {
        const auto fail = [&](std::string message) {
            Fail(std::move(message), error_out);
            return std::optional<WorksetReconstructionResult>{};
        };
        if (!state_ || !analysis_ || context.items.size() != 1 ||
            context.workset_id <= 0 || context.dispatch_attempt_id <= 0 ||
            context.workflow_step_id <= 0 || context.job_set_id <= 0 ||
            context.dispatch_token.empty() ||
            !context.state_compatibility.Complete())
            return fail("Battle Replay reconstruction requires one exact item");
        const auto& item = context.items.front();
        const auto row = analysis_->GetBattleReplay(item.program_ref_id);
        const auto completion_row = row
            ? analysis_->GetBattleCompletion(row->battle_completion_id)
            : std::nullopt;
        if (!row || !completion_row || row->status != "QUEUED" ||
            row->exec_job_id != item.job_id ||
            item.program_kind != static_cast<std::int32_t>(savor::PK_BattleReplay) ||
            item.program_version != replayphase::ProgramVersion ||
            item.program_ref_kind != kReplayProgramRefKind ||
            item.savestate_id != row->source_savestate_id)
            return fail("Battle Replay immutable request drifted");
        auto live_plan = BuildCanonicalBattleReplayPlan(
            state_, analysis_, *completion_row, error_out);
        if (!live_plan) return std::nullopt;
        const auto stored_bytes = std::span<const std::uint8_t>(
            reinterpret_cast<const std::uint8_t*>(row->replay_plan_blob.data()),
            row->replay_plan_blob.size());
        phase::BattleReplayPlanV1 stored_plan{};
        std::string diagnostic;
        if (!phase::DecodeBattleReplayPlanV1(stored_bytes, stored_plan, &diagnostic) ||
            stored_plan.canonical_sha256 != row->replay_plan_sha256 ||
            stored_plan.canonical_sha256 != live_plan->canonical_sha256 ||
            phase::EncodeBattleReplayPlanV1(stored_plan) !=
                phase::EncodeBattleReplayPlanV1(*live_plan))
            return fail("Battle Replay plan changed after admission: " + diagnostic);
        const auto binding_bytes = std::span<const std::uint8_t>(
            reinterpret_cast<const std::uint8_t*>(
                row->source_binding_blob.data()),
            row->source_binding_blob.size());
        phase::BattleReplaySourceBindingV1 source_binding{};
        const auto battle_set = analysis_->GetBattleSet(
            static_cast<std::int64_t>(stored_plan.selected_lineage.battle_set_id));
        auto live_binding = battle_set
            ? ResolveReplaySourceBinding(
                  state_, analysis_, battle_set->entry_savestate_id, error_out)
            : std::nullopt;
        if (!battle_set || !live_binding ||
            !phase::DecodeBattleReplaySourceBindingV1(
                binding_bytes, source_binding, &diagnostic) ||
            row->source_binding_version != 1 ||
            source_binding.canonical_sha256 != row->source_binding_sha256 ||
            source_binding.canonical_sha256 !=
                live_binding->canonical_sha256 ||
            phase::EncodeBattleReplaySourceBindingV1(source_binding) !=
                phase::EncodeBattleReplaySourceBindingV1(*live_binding))
            return fail("Battle Replay source binding changed after admission: " +
                        diagnostic);
        const auto definition = replayphase::PrepareBattleReplayFullPhaseV1(
            stored_plan, &diagnostic);
        if (!definition)
            return fail("Battle Replay phase could not be reconstructed: " + diagnostic);
        if (item.input_ini != ReplayJobInput(row->battle_replay_id,
                row->replay_plan_sha256, row->source_binding_sha256,
                definition->identity().canonical_sha256))
            return fail("Battle Replay item binding drifted");
        const auto source_state = state_->GetSavestate(
            static_cast<std::int64_t>(source_binding.source_savestate_id));
        const auto source_dtm = source_binding.source_dtm_artifact_id
            ? state_->GetArtifact(static_cast<std::int64_t>(
                  *source_binding.source_dtm_artifact_id))
            : std::nullopt;
        if (!source_state || !source_state->is_complete ||
            source_state->artifact_sha256 !=
                source_binding.source_savestate_sha256 ||
            source_state->dtm_artifact_id.has_value() != source_dtm.has_value() ||
            (source_dtm &&
                (source_state->dtm_artifact_id != source_dtm->artifact_id ||
                 source_dtm->sha256 != *source_binding.source_dtm_sha256)))
            return fail("Battle Replay exact source binding drifted");
        const auto baseline_state = root_ / "baselines" /
            (source_state->artifact_sha256 + ".sav");
        const auto state_path = MaterializeSavestate(
            state_, *source_state, baseline_state, error_out);
        std::optional<std::filesystem::path> dtm_path;
        if (source_dtm)
        {
            const auto baseline_dtm = std::filesystem::path(
                baseline_state.string() + ".dtm");
            dtm_path = MaterializeArtifact(
                state_, *source_dtm, baseline_dtm, error_out);
        }
        if (!state_path || (source_dtm && !dtm_path)) return std::nullopt;
        const auto package = savor::runtime::fullphase::
            BuildFullPhaseProgramPackage(*definition);
        const auto common_bytes = phase::EncodeBattleReplayPlanV1(stored_plan);
        const auto& runtime = definition->runtime_contract();
        savor::runtime::WorkerWorksetDefinition workset{};
        workset.workset_id = savor::runtime::WorkerWorksetId(
            static_cast<std::uint64_t>(context.dispatch_attempt_id));
        workset.phase_invocation = {
            .invocation_id = {
                .workflow_step_id = static_cast<std::uint64_t>(context.workflow_step_id),
                .job_set_id = static_cast<std::uint64_t>(context.job_set_id)},
            .program_package = package,
            .common_input = savor::runtime::fullphase::MakeFullPhaseCommonInput(
                "soa.battle.replay.CommonInput", 1, common_bytes),
        };
        workset.baseline = {.artifact = {
            .kind = savor::runtime::ProgramBaselineArtifactKind::Savestate,
            .state_path = *state_path, .state_sha256 = source_state->artifact_sha256,
            .movie_path = dtm_path,
            .movie_sha256 = source_dtm ? source_dtm->sha256 : std::string{},
            .compatibility = context.state_compatibility,
            .lineage = {.edge = runtime.baseline_lineage,
                        .producer = "SavorDb.PK_BattleReplay"}},
            .lineage = runtime.baseline_lineage};
        workset.derived_state = context.derived_state;
        workset.capture = context.capture;
        workset.progress_plan = context.progress_plan;
        workset.execution_key = {
            .module = runtime.module, .entrypoint = runtime.entrypoint,
            .verified_dependency_sha256 = runtime.verified_dependency_sha256,
            .runtime_profile_sha256 = runtime.runtime_profile_sha256,
            .baseline = savor::runtime::ComputeProgramBaselineKey(workset.baseline),
            .movie_policy_sha256 = runtime.movie_policy_sha256,
            .service_policy_sha256 = runtime.service_policy_sha256,
            .program_package_sha256 = package.canonical_sha256,
            .common_input_sha256 = workset.phase_invocation.common_input.content_sha256,
            .derived_state_binding_sha256 = workset.derived_state.content_sha256,
            .capture_binding_sha256 = workset.capture
                ? workset.capture->content_sha256
                : savor::runtime::EmptyWorksetCaptureBindingHashV1(),
            .progress_plan_sha256 = workset.progress_plan.content_sha256};
        workset.execution_key.canonical_sha256 =
            savor::runtime::ComputeWorkerWorksetExecutionKeyHash(workset.execution_key);
        workset.items.push_back({
            .item_id = savor::runtime::WorkerWorksetItemId(
                static_cast<std::uint64_t>(item.job_id)), .ordinal = 0,
            .execution = {.execution_id = savor::runtime::ProgramExecutionId(
                    static_cast<std::uint64_t>(item.job_id)),
                .attempt_id = savor::runtime::AttemptId(item.reserved_attempt_id),
                .input_payload = replayphase::EncodeBattleReplayExecutionInputV1()},
            .declared_terminal_bytes = kDeclaredTerminalBytes,
            .correlation = {.durable_job_id = std::to_string(item.job_id),
                .claim_token = item.claim_token,
                .parent_correlation = context.contract_key}});
        std::vector<std::uint8_t> encoded;
        const auto status = savor::runtime::EncodeWorkerWorksetV5(workset, encoded);
        if (!status) return fail("Battle Replay workset encoding failed: " +
                                 status.message);
        workset.encoded_size_bytes = encoded.size();
        return WorksetReconstructionResult{.workset = std::move(workset),
            .ordered_job_ids = {item.job_id}};
    }

private:
    IStateDb* state_{};
    IAnalysisDb* analysis_{};
    std::filesystem::path root_;
};

class ReplayResultHandler final : public IProgramResultHandler
{
public:
    explicit ReplayResultHandler(IAnalysisDb* analysis) : analysis_(analysis) {}

    ProgramResultDecision Process(
        const ProgramResultProcessingContext& context) const override
    {
        if (!analysis_ || context.program_kind !=
                static_cast<std::int32_t>(savor::PK_BattleReplay) ||
            context.program_version != replayphase::ProgramVersion ||
            context.program_ref_kind != kReplayProgramRefKind)
            return Decision("FAILED", "BATTLE_REPLAY_IDENTITY_INVALID",
                "job is not an exact battle.replay request");
        const auto row = analysis_->GetBattleReplay(context.program_ref_id);
        if (!row || row->exec_job_id != context.job_id)
            return Decision("FAILED", "BATTLE_REPLAY_REQUEST_DRIFT",
                "durable Battle Replay identity drifted");
        if (row->status == "MATCHED" || row->status == "REPLAY_MISMATCH")
        {
            if (row->worker_terminal_sha256 != context.terminal.sha256)
                return Decision("FAILED", "BATTLE_REPLAY_RESULT_DRIFT",
                    "completed Battle Replay belongs to another terminal");
            auto decision = Decision("SUCCEEDED");
            decision.outputs.push_back(ReplayOutput(row->battle_replay_id));
            return decision;
        }
        const auto plan_bytes = std::span<const std::uint8_t>(
            reinterpret_cast<const std::uint8_t*>(row->replay_plan_blob.data()),
            row->replay_plan_blob.size());
        phase::BattleReplayPlanV1 plan{};
        std::string error;
        if (!phase::DecodeBattleReplayPlanV1(plan_bytes, plan, &error) ||
            plan.battle_completion_id !=
                static_cast<std::uint64_t>(row->battle_completion_id) ||
            plan.canonical_sha256 != row->replay_plan_sha256)
            return PersistFailure(*row, context, "BATTLE_REPLAY_PLAN_INVALID",
                error.empty() ? "durable replay plan is malformed" : error);
        const auto definition = replayphase::PrepareBattleReplayFullPhaseV1(
            plan, &error);
        if (!definition || context.input_ini != ReplayJobInput(
                row->battle_replay_id, row->replay_plan_sha256,
                row->source_binding_sha256,
                definition->identity().canonical_sha256))
            return PersistFailure(*row, context, "BATTLE_REPLAY_PLAN_DRIFT",
                error.empty() ? "prepared replay identity drifted" : error);
        savor::runtime::DurableWorkerTerminalEnvelope terminal{};
        if (!savor::runtime::DecodeDurableWorkerTerminalEnvelope(
                context.terminal.envelope, &terminal, &error))
            return PersistFailure(*row, context,
                "BATTLE_REPLAY_TERMINAL_INVALID", error);
        using Status = savor::wrms::InvocationTerminalStatus;
        if (terminal.terminal.status != Status::Succeeded ||
            terminal.terminal.unstarted || terminal.terminal.workset_id !=
                static_cast<std::uint64_t>(context.terminal.dispatch_attempt_id) ||
            terminal.terminal.item_id != static_cast<std::uint64_t>(context.job_id) ||
            terminal.terminal.invocation_id != static_cast<std::uint64_t>(context.job_id) ||
            terminal.terminal.attempt_id != context.terminal.reserved_attempt_id ||
            terminal.process_generation == 0 ||
            terminal.terminal.workset_epoch == 0 ||
            terminal.terminal.session_disposition !=
                savor::wrms::SessionDispositionCode::Clean)
            return PersistFailure(*row, context,
                terminal.terminal.error_code.empty()
                    ? "BATTLE_REPLAY_EXECUTION_FAILED"
                    : terminal.terminal.error_code,
                terminal.terminal.message.empty()
                    ? "battle.replay did not complete cleanly"
                    : terminal.terminal.message,
                "FAILED");
        replayphase::BattleReplayResultV1 result{};
        if (!definition->DecodeProgramResult(
                terminal.terminal.result, result, &error))
            return PersistFailure(*row, context,
                "BATTLE_REPLAY_RESULT_INVALID",
                error.empty() ? "Battle Replay result is malformed" : error);

        std::optional<std::string> manifest_blob;
        std::optional<std::string> manifest_hash;
        std::optional<std::string> transition_blob;
        std::optional<std::string> transition_hash;
        if (result.observed_completion)
        {
            std::vector<std::uint8_t> encoded;
            if (!completion::EncodeBattleCompletionManifestV1(
                    *result.observed_completion, encoded))
                return PersistFailure(*row, context,
                    "BATTLE_REPLAY_EVIDENCE_INVALID",
                    "observed completion could not be encoded");
            manifest_blob = std::string(
                reinterpret_cast<const char*>(encoded.data()), encoded.size());
            manifest_hash = hash::sha256(encoded.data(), encoded.size());
        }
        if (result.transition)
        {
            std::vector<std::uint8_t> encoded;
            if (!completion::EncodeFieldTransitionContextV1(
                    *result.transition, encoded))
                return PersistFailure(*row, context,
                    "BATTLE_REPLAY_EVIDENCE_INVALID",
                    "observed transition could not be encoded");
            transition_blob = std::string(
                reinterpret_cast<const char*>(encoded.data()), encoded.size());
            transition_hash = hash::sha256(encoded.data(), encoded.size());
        }
        const bool matched = result.outcome ==
            replayphase::BattleReplayOutcomeV1::Matched;
        if (matched && (!result.observed_completion || !result.transition ||
            !completion::SemanticallyEqualBattleCompletionManifestV1(
                *result.observed_completion, plan.expected_completion) ||
            *result.transition != plan.expected_completion.transition))
            return PersistFailure(*row, context,
                "BATTLE_REPLAY_SEMANTIC_DRIFT",
                "matched replay does not match its immutable completion plan");
        const std::string outcome = matched ? "MATCHED" : "REPLAY_MISMATCH";
        if (!analysis_->CompleteBattleReplay({
                .battle_replay_id = row->battle_replay_id,
                .outcome = outcome,
                .mismatch_turn = result.mismatch_turn,
                .expected_rng = result.expected_rng,
                .observed_rng = result.observed_rng,
                .observed_completion_blob = std::move(manifest_blob),
                .observed_completion_sha256 = std::move(manifest_hash),
                .observed_transition_blob = std::move(transition_blob),
                .observed_transition_sha256 = std::move(transition_hash),
                .worker_terminal_sha256 = context.terminal.sha256,
                .status = outcome,
                .completed_at_utc = types::UtcNow(),
                .correlation_id = "battle-replay-" +
                    std::to_string(row->battle_replay_id),
                .causation_id = "execution-job-" +
                    std::to_string(context.job_id),
            }, &error)) throw std::runtime_error(error);
        auto decision = Decision("SUCCEEDED");
        decision.outputs.push_back(ReplayOutput(row->battle_replay_id));
        decision.event_lines.push_back(
            "[battle-replay-result] replay=" +
            std::to_string(row->battle_replay_id) + " outcome=" + outcome +
            " turn=" + std::to_string(result.mismatch_turn) +
            " expected_rng=" + std::to_string(result.expected_rng) +
            " observed_rng=" + std::to_string(result.observed_rng));
        return decision;
    }

    private:
    ProgramResultDecision PersistFailure(
        const BattleReplayRecord& row,
        const ProgramResultProcessingContext& context,
        std::string code, std::string text,
        std::string final_state = "FAILED") const
    {
        std::string error;
        if (!analysis_->FailBattleReplay({
                .battle_replay_id = row.battle_replay_id,
                .error_code = code, .error_text = text,
                .worker_terminal_sha256 = context.terminal.sha256,
                .completed_at_utc = types::UtcNow(),
                .correlation_id = "battle-replay-" +
                    std::to_string(row.battle_replay_id),
                .causation_id = "execution-job-" +
                    std::to_string(context.job_id)}, &error))
            throw std::runtime_error(error);
        return Decision(std::move(final_state), std::move(code), std::move(text));
    }
    IAnalysisDb* analysis_{};
};

class Transition final : public IWorkflowTransitionHandler
{
public:
    explicit Transition(IAnalysisDb* analysis) : analysis_(analysis) {}

    WorkflowTransitionDecision EvaluateTransition(
        const WorkflowTransitionContext& context) const override
    {
        WorkflowTransitionDecision decision{};
        decision.should_advance = true;
        if (!analysis_ ||
            context.output_ref_kind !=
                std::optional<std::string>(kProgramRefKind) ||
            !context.output_ref_id)
            return decision;
        const auto row = analysis_->GetBattleRecording(*context.output_ref_id);
        if (!row ||
            (row->status != "COMPLETED" &&
             row->status != "REPLAY_MISMATCH"))
        {
            decision.should_advance = false;
            decision.workflow_failure = true;
            decision.blocked_reason = "battle_recording_result_missing";
            return decision;
        }
        if (row->status == "REPLAY_MISMATCH")
            return decision;
        if (row->outcome != std::optional<std::string>("RECORDED") ||
            !row->tas_movie_tree_id)
        {
            decision.should_advance = false;
            decision.workflow_failure = true;
            decision.blocked_reason = "battle_recording_tree_missing";
            return decision;
        }
        decision.spawn_steps.push_back({
            .step_key = "BattleRecording/" +
                std::to_string(row->battle_recording_id) + "/validate",
            .step_kind = "tasmovie.validate_tree",
            .input_ref_kind = std::string(kTreeRefKind),
            .input_ref_id = *row->tas_movie_tree_id,
            .priority = context.priority,
            .max_attempts = 1,
        });
        return decision;
    }

private:
    IAnalysisDb* analysis_{};
};

} // namespace

ProgramKindDescriptor BuildBattleRecordProgramDescriptor(
    IExecutionDb* execution_db,
    IStateDb* state_db,
    IAnalysisDb* analysis_db,
    BattleRecordProgramConfig config)
{
    ProgramKindDescriptor descriptor{};
    descriptor.program_kind =
        static_cast<std::int32_t>(savor::PK_BattleRecord);
    descriptor.program_name = savor::ProgramKindDisplayName(descriptor.program_kind);
    descriptor.result_staging_root = config.working_dir_root;
    descriptor.full_phase_identity =
        phase::BattleRecordKindHandlerV1()->identity();
    descriptor.default_progress_library_ids =
        ObservationDefaults().progress_library_ids;
    descriptor.default_derived_state_block_ids = std::vector<std::string>{};
    descriptor.default_progress_runtime_trigger_pcs =
        ObservationDefaults().runtime_sample_trigger_pcs;
    descriptor.job_materializer = std::make_shared<Materializer>(
        execution_db, state_db, analysis_db, config);
    descriptor.workset_reconstruction = std::make_shared<Reconstruction>(
        state_db, analysis_db, config.working_dir_root);
    descriptor.result_handler = std::make_shared<ResultHandler>(
        state_db, analysis_db, config.working_dir_root);
    descriptor.workflow_transition =
        std::make_shared<Transition>(analysis_db);
    descriptor.supports_workflow_orchestration = true;
    return descriptor;
}

std::optional<phase::BattleReplayPlanV1> BuildCanonicalBattleReplayPlan(
    IStateDb* state_db,
    IAnalysisDb* analysis_db,
    const BattleCompletionRecord& completion,
    std::string* error_out)
{
    return BuildReplayPlan(state_db, analysis_db, completion, error_out);
}

} // namespace savor::db::execution::programdb::battlerecord

namespace savor::db::execution::programdb::battlereplay {

ProgramKindDescriptor BuildBattleReplayProgramDescriptor(
    IExecutionDb* execution_db,
    IStateDb* state_db,
    IAnalysisDb* analysis_db,
    BattleReplayProgramConfig config)
{
    ProgramKindDescriptor descriptor{};
    descriptor.program_kind =
        static_cast<std::int32_t>(savor::PK_BattleReplay);
    descriptor.program_name = savor::ProgramKindDisplayName(descriptor.program_kind);
    descriptor.result_staging_root = config.working_dir_root;
    descriptor.full_phase_identity =
        savor::runtime::battlereplay::BattleReplayKindHandlerV1()->identity();
    descriptor.default_progress_library_ids =
        battlerecord::ObservationDefaults().progress_library_ids;
    descriptor.default_derived_state_block_ids = std::vector<std::string>{};
    descriptor.default_progress_runtime_trigger_pcs =
        battlerecord::ObservationDefaults().runtime_sample_trigger_pcs;
    descriptor.job_materializer =
        std::make_shared<battlerecord::ReplayMaterializer>(
            execution_db, state_db, analysis_db, config);
    descriptor.workset_reconstruction =
        std::make_shared<battlerecord::ReplayReconstruction>(
            state_db, analysis_db, config.working_dir_root);
    descriptor.result_handler =
        std::make_shared<battlerecord::ReplayResultHandler>(analysis_db);
    descriptor.supports_workflow_orchestration = true;
    return descriptor;
}

} // namespace savor::db::execution::programdb::battlereplay

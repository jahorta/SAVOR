#pragma once

#include <string_view>
#include <vector>

namespace savor::db::execution::programdb {

struct WorkflowOutputContract {
    std::string_view output_key;
    std::string_view data_kind;
    std::string_view ref_kind;
};

namespace workflow_outputs {

inline constexpr WorkflowOutputContract BattleContext{"battle_context", "analysis_battle.battle_context_id", "ab_battle_context"};
inline constexpr WorkflowOutputContract BattleSet{"battle_set", "analysis_battle.battle_set", "analysis_battle.battle_set"};
inline constexpr WorkflowOutputContract BattleCompletion{"completion", "analysis_battle.battle_completion", "analysis_battle.battle_completion"};
inline constexpr WorkflowOutputContract BattleRecording{"recording", "analysis_battle.battle_recording", "analysis_battle.battle_recording"};
inline constexpr WorkflowOutputContract BattleReplay{"replay", "analysis_battle.battle_replay", "analysis_battle.battle_replay"};
inline constexpr WorkflowOutputContract SeedProbeRun{"seed_probe_run", "analysis.seed_probe_run", "sp_probe_run"};
inline constexpr WorkflowOutputContract TasMovieValidationAttempt{"tas_movie_validation_attempt", "analysis.tas_movie_validation_attempt_id", "tmv_validation_attempt"};
inline constexpr WorkflowOutputContract TasMovieRootEstablishment{"root_establishment", "analysis.tas_movie_root_establishment_attempt_id", "tmv_root_establishment_attempt"};
inline constexpr WorkflowOutputContract TasMovieRootDtm{"root_dtm", "state_artifact.dtm_artifact_id", "state_artifact"};
inline constexpr WorkflowOutputContract TasMovieValidatedCheckpoint{"validated_checkpoint_savestate", "state.movie_paired_savestate_id", "state.savestate"};
inline constexpr WorkflowOutputContract TasMovieSterilizedCheckpoint{"sterilized_checkpoint_savestate", "state.movie_inactive_savestate_id", "state.savestate"};
inline constexpr WorkflowOutputContract TasMovieAnnotationAttempt{"annotation_attempt", "analysis.tas_movie_input_epoch_annotation_attempt_id", "tmv_input_epoch_annotation_attempt"};
inline constexpr WorkflowOutputContract TasMovieRewriteAttempt{"rewrite_attempt", "analysis.tas_movie_input_epoch_rewrite_attempt_id", "tmv_input_epoch_rewrite_attempt"};
inline constexpr WorkflowOutputContract TasMovieRewrittenDtm{"rewritten_dtm", "state_artifact.dtm_artifact_id", "state_artifact"};
inline constexpr WorkflowOutputContract TasMovieRewrittenPairedSavestate{"rewritten_paired_savestate", "state.movie_paired_savestate_id", "state.savestate"};
inline constexpr WorkflowOutputContract TasMovieCutsceneAttempt{"cutscene_attempt", "analysis.tas_movie_cutscene_attempt_id", "tmv_cutscene_attempt"};
inline constexpr WorkflowOutputContract TasMovieTree{"tas_movie_tree", "state.tas_movie_tree_id", "state_tas_movie_tree"};
inline constexpr WorkflowOutputContract TasMoviePairedSavestate{"paired_savestate", "state.movie_paired_savestate_id", "state.savestate"};

inline std::vector<WorkflowOutputContract> ForStepKind(std::string_view step_kind) {
    if (step_kind == "battle.context") return {BattleContext};
    if (step_kind == "battle.start" || step_kind == "battle.single_turn") return {BattleSet};
    if (step_kind == "battle.completion") return {BattleCompletion};
    if (step_kind == "battle.record") return {BattleRecording};
    if (step_kind == "battle.replay") return {BattleReplay};
    if (step_kind == "seedprobe.confirm") return {SeedProbeRun};
    if (step_kind == "tasmovie.establish_root_cursor") return {TasMovieValidationAttempt, TasMovieRootEstablishment, TasMovieRootDtm};
    if (step_kind == "tasmovie.validate_root" || step_kind == "tasmovie.validate_tree") return {TasMovieValidationAttempt, TasMovieValidatedCheckpoint};
    if (step_kind == "tasmovie.checkpoint_sterilize") return {TasMovieSterilizedCheckpoint};
    if (step_kind == "tasmovie.annotate" || step_kind == "tasmovie.input_epoch_breakpoint_diagnostic") return {TasMovieAnnotationAttempt};
    if (step_kind == "tasmovie.revise") return {TasMovieRewriteAttempt, TasMovieRewrittenDtm, TasMovieRewrittenPairedSavestate, TasMovieAnnotationAttempt, TasMovieRootEstablishment};
    if (step_kind == "tasmovie.cutscene") return {
        TasMovieCutsceneAttempt,
        TasMovieTree,
        TasMoviePairedSavestate,
        TasMovieValidationAttempt,
        TasMovieValidatedCheckpoint};
    return {};
}

} // namespace workflow_outputs
} // namespace savor::db::execution::programdb

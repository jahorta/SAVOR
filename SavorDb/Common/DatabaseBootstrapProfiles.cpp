#include "DatabaseBootstrapProfiles.h"

#include "DbService.h"
#include "../Authoring/AuthoringRecipeMaterializer.h"

namespace savor::db::bootstrap {
namespace {

namespace ac = authoring::catalog;

authoring::AuthoringRecipe StandardRecipe()
{
    authoring::AuthoringRecipeBuilder recipe("database-bootstrap.standard");
    const auto seed_probe = recipe.SeedProbe({
        .symbol = "seed_probe.basic", .name = "Basic", .priority = 0,
        .min_value = 48, .max_value = 207, .cap_trigger_top = true,
        .ignore_trigger_minmax = true, .combo_attempts_per_target = 32,
        .combo_sampler_tries = 8, .auto_schedule_battle_run = false,
    });

    const auto item_parameter = ac::predicate::expression::Parameter(
        "item_id", ac::types::U16);
    const auto predicate = recipe.PredicateDefinition({
        .symbol = "predicate.item_per_turn",
        .name = "One item per turn",
        .description = "This predicate checks for at least one item per turn.",
        .rule = ac::predicate::expression::GreaterEqual(
            ac::predicate::expression::Value(
                ac::predicate::values::battle::DropCount, {item_parameter}),
            ac::predicate::expression::Value(
                ac::predicate::values::battle::CurrentTurn)),
    });
    const auto binding = recipe.PredicateBinding({
        .symbol = "predicate_binding.electribox_per_turn",
        .name = "One electribox per turn",
        .description = "Detects whether we get at least one electribox per turn",
        .definition = predicate,
        .use_recommended_sources = true,
        .literals = {{
            .witness_name = "item_id",
            .value = runtime::program::LiteralValue{
                .type = ac::types::U16,
                .payload = static_cast<std::uint16_t>(273)},  // Electribox has an ItemID of 273
        }},
    });
    const auto group = recipe.PredicateGroup({
        .symbol = "predicate_group.first_battle",
        .name = "First battle",
        .description = "Requires at least one Electribox drop per turn, evaluated at turn end and battle victory.",
        .members = {{
            .binding = binding,
            .hooks = {ac::predicate::hooks::battle::EndBattleVictory,
                      ac::predicate::hooks::battle::EndTurn},
            .reaction = runtime::program::composition::PredicateReaction::AbortOnFail
        }},
    });

    const auto attack_any = recipe.ActionPreset({
        .symbol = "action.attack_any", .name = "Attack Any",
        .macro = BattlePlanActionMacro::Attack,
        .target_kind = BattlePlanTargetKind::AnyEnemy,
    });
    const auto attack_same = recipe.ActionPreset({
        .symbol = "action.attack_same", .name = "Attack same as Vyse",
        .macro = BattlePlanActionMacro::Attack,
        .target_kind = BattlePlanTargetKind::SameAsOtherPC,
        .target_same_as_actor_slot = 0,
    });
    const auto battle_plan = recipe.BattlePlan({
        .symbol = "battle_plan.first_battle", .name = "1st battle",
        .description = "Runs the two-turn first-battle strategy and evaluates the First battle predicate group on each turn.",
        .turns = {
            {.turn_index = 1, .predicate_group = group,
             .actions = {{.actor_slot = 0, .preset = attack_any, .ordinal = 0},
                         {.actor_slot = 1, .preset = attack_same, .ordinal = 1}}},
            {.turn_index = 2, .predicate_group = group,
             .actions = {{.actor_slot = 0, .preset = attack_any, .ordinal = 0},
                         {.actor_slot = 1, .preset = attack_same, .ordinal = 1}}},
        },
    });

    recipe.Workflow({
        .symbol = "workflow.tas_establish",
        .name = "Standalone TAS Movie: Establish Root Cursor",
        .description = "Auto-authored single-unit workflow graph for standalone Qt2 launches.",
        .hidden = true, .standalone_hash = true,
        .nodes = {{
            .node_key = "tas_movie_establish_root_cursor_standalone",
            .unit = ac::workflow::units::TasMovieEstablishRootCursor,
            .display_name = "TAS Movie: Establish Root Cursor",
        }},
        .external_inputs = {{
            "tas_movie_establish_root_cursor_standalone",
            ac::workflow::ports::tas_movie_establish_root_cursor::RootDtm,
        }},
    });

    recipe.Workflow({
        .symbol = "workflow.tas_prepare_root",
        .name = "TAS Prepare Root",
        .description = "Establishes and validates a TAS root, then passively annotates its guest input epochs for later revision.",
        .nodes = {
            {.node_key="tas_movie_establish_1",
             .unit=ac::workflow::units::TasMovieEstablishRootCursor},
            {.node_key="tas_movie_validate_root_2",
             .unit=ac::workflow::units::TasMovieValidateRoot,
             .constant_arguments={{"rtc", "0"}}},
            {.node_key="tas_movie_annotate_3",
             .unit=ac::workflow::units::TasMovieAnnotate},
        },
        .external_inputs = {
            {"tas_movie_establish_1",
             ac::workflow::ports::tas_movie_establish_root_cursor::RootDtm},
        },
        .edges = {{
            "tas_movie_establish_1",
            ac::workflow::ports::tas_movie_establish_root_cursor::EstablishedRootCursorAttempt,
            "tas_movie_validate_root_2",
            ac::workflow::ports::tas_movie_validate_root::RootEstablishment,
        }, {
            "tas_movie_establish_1",
            ac::workflow::ports::tas_movie_establish_root_cursor::EstablishedRootCursorAttempt,
            "tas_movie_annotate_3",
            ac::workflow::ports::tas_movie_annotate::RootEstablishment,
        }},
        .control_dependencies = {{
            .from_node_key="tas_movie_validate_root_2",
            .to_node_key="tas_movie_annotate_3",
        }},
    });

    recipe.Workflow({
        .symbol = "workflow.tas_expansion_revise",
        .name = "TAS Movie Expansion: Revise",
        .description = "Revises an annotated DTM by an exact requested neutral delay and publishes its child annotation and root establishment.",
        .hidden = true,
        .nodes = {{.node_key="tas_movie_revise_1",
                   .unit=ac::workflow::units::TasMovieRevise}},
        .external_inputs = {
            {"tas_movie_revise_1",
             ac::workflow::ports::tas_movie_revise::AnnotationAttempt},
        },
    });

    recipe.Workflow({
        .symbol = "workflow.first_battle_rtc", .name = "First Battle Exploration: RTC Branch",
        .description = "Validates and sterilizes an established TAS root, probes RTC seeds, captures battle context, and executes the first-battle plan.",
        .hidden = true,
        .nodes = {
            {.node_key = "tas_movie_validate_root_1",
             .unit = ac::workflow::units::TasMovieValidateRoot},
            {.node_key = "tas_movie_checkpoint_sterilize_2",
             .unit = ac::workflow::units::TasMovieCheckpointSterilize},
            {.node_key = "seed_probe_3", .unit = ac::workflow::units::SeedProbe,
             .authored_ref = authoring::WorkflowAuthoredReferenceDefinition{
                 .kind = authoring::WorkflowAuthoredObjectKind::SeedProbeSpec,
                 .ref = authoring::AuthoringRef<authoring::SeedProbeSpecTag>{seed_probe}}},
            {.node_key = "battle.context_4",
             .unit = ac::workflow::units::BattleContext},
            {.node_key = "battle_5", .unit = ac::workflow::units::Battle,
             .authored_ref = authoring::WorkflowAuthoredReferenceDefinition{
                 .kind = authoring::WorkflowAuthoredObjectKind::BattlePlan,
                 .ref = authoring::AuthoringRef<authoring::BattlePlanTag>{battle_plan}}},
        },
        .external_inputs = {{
            "tas_movie_validate_root_1",
            ac::workflow::ports::tas_movie_validate_root::RootEstablishment,
        }},
        .edges = {
            {"tas_movie_validate_root_1",
             ac::workflow::ports::tas_movie_validate_root::ValidatedCheckpoint,
             "tas_movie_checkpoint_sterilize_2",
             ac::workflow::ports::tas_movie_checkpoint_sterilize::PairedCheckpoint},
            {"tas_movie_checkpoint_sterilize_2",
             ac::workflow::ports::tas_movie_checkpoint_sterilize::SterilizedCheckpoint,
             "seed_probe_3", ac::workflow::ports::seed_probe::EntrySavestate},
            {"tas_movie_checkpoint_sterilize_2",
             ac::workflow::ports::tas_movie_checkpoint_sterilize::SterilizedCheckpoint,
             "battle.context_4", ac::workflow::ports::battle_context::EntrySavestate},
            {"seed_probe_3", ac::workflow::ports::seed_probe::Run,
             "battle_5", ac::workflow::ports::battle::SeedProbeRun},
            {"battle.context_4", ac::workflow::ports::battle_context::Context,
             "battle_5", ac::workflow::ports::battle::Context},
        },
    });

    recipe.Workflow({
        .symbol = "workflow.first_battle_exploration",
        .name = "First Battle Exploration",
        .description = "Revises a prepared TAS root by requested neutral delays, validates each RTC branch, probes seeds, captures battle context, and runs the first battle.",
        .execution_shape = "EXPANSION",
        .expansion_kind = "TAS_FIRST_BATTLE",
        .nodes = {
            {.node_key="tas_movie_revise_1", .unit=ac::workflow::units::TasMovieRevise},
            {.node_key="tas_movie_validate_root_2", .unit=ac::workflow::units::TasMovieValidateRoot},
            {.node_key="tas_movie_checkpoint_sterilize_3", .unit=ac::workflow::units::TasMovieCheckpointSterilize},
            {.node_key="seed_probe_4", .unit=ac::workflow::units::SeedProbe,
             .authored_ref=authoring::WorkflowAuthoredReferenceDefinition{
                 .kind=authoring::WorkflowAuthoredObjectKind::SeedProbeSpec,
                 .ref=authoring::AuthoringRef<authoring::SeedProbeSpecTag>{seed_probe}}},
            {.node_key="battle.context_5", .unit=ac::workflow::units::BattleContext},
            {.node_key="battle_6", .unit=ac::workflow::units::Battle,
             .authored_ref=authoring::WorkflowAuthoredReferenceDefinition{
                 .kind=authoring::WorkflowAuthoredObjectKind::BattlePlan,
                 .ref=authoring::AuthoringRef<authoring::BattlePlanTag>{battle_plan}}},
        },
        .external_inputs = {
            {"tas_movie_revise_1", ac::workflow::ports::tas_movie_revise::AnnotationAttempt},
        },
        .edges = {
            {"tas_movie_revise_1", ac::workflow::ports::tas_movie_revise::RootEstablishment,
             "tas_movie_validate_root_2", ac::workflow::ports::tas_movie_validate_root::RootEstablishment},
            {"tas_movie_validate_root_2", ac::workflow::ports::tas_movie_validate_root::ValidatedCheckpoint,
             "tas_movie_checkpoint_sterilize_3", ac::workflow::ports::tas_movie_checkpoint_sterilize::PairedCheckpoint},
            {"tas_movie_checkpoint_sterilize_3", ac::workflow::ports::tas_movie_checkpoint_sterilize::SterilizedCheckpoint,
             "seed_probe_4", ac::workflow::ports::seed_probe::EntrySavestate},
            {"tas_movie_checkpoint_sterilize_3", ac::workflow::ports::tas_movie_checkpoint_sterilize::SterilizedCheckpoint,
             "battle.context_5", ac::workflow::ports::battle_context::EntrySavestate},
            {"seed_probe_4", ac::workflow::ports::seed_probe::Run,
             "battle_6", ac::workflow::ports::battle::SeedProbeRun},
            {"battle.context_5", ac::workflow::ports::battle_context::Context,
             "battle_6", ac::workflow::ports::battle::Context},
        },
    });
    return std::move(recipe).Build();
}

} // namespace

bool ApplyDatabaseBootstrapProfile(
    core::DBService& service,
    const DatabaseBootstrapProfileId profile_id,
    DatabaseBootstrapRecordCounts* counts,
    std::string* error_out)
{
    if (counts == nullptr) {
        if (error_out != nullptr) *error_out = "bootstrap result counts are required";
        return false;
    }
    *counts = {};
    if (FindDatabaseBootstrapProfile(profile_id) == nullptr) {
        if (error_out != nullptr) *error_out = "unknown database bootstrap profile";
        return false;
    }
    if (profile_id == DatabaseBootstrapProfileId::Empty) return true;
    auto* db = service.AuthoringDb();
    if (db == nullptr) {
        if (error_out != nullptr) *error_out = "bootstrap authoring database is unavailable";
        return false;
    }
    authoring::AuthoringRecipeResult result{};
    const authoring::AuthoringRecipeMaterializer materializer(db);
    if (!materializer.Apply(StandardRecipe(), &result, error_out)) return false;
    counts->seed_probe_specs = static_cast<int>(result.seed_probe_spec_ids.size());
    counts->predicate_definitions = static_cast<int>(result.predicate_definition_revision_ids.size());
    counts->predicate_bindings = static_cast<int>(result.predicate_binding_revision_ids.size());
    counts->predicate_groups = static_cast<int>(result.predicate_group_revision_ids.size());
    counts->battle_action_presets = static_cast<int>(result.battle_action_preset_ids.size());
    counts->battle_plans = static_cast<int>(result.battle_plan_ids.size());
    counts->workflow_graphs = static_cast<int>(result.workflow_graphs.size());
    return true;
}

} // namespace savor::db::bootstrap

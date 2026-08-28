#include "BattlePhasesRealWorkerScenario.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <functional>
#include <iomanip>
#include <iostream>
#include <limits>
#include <mutex>
#include <optional>
#include <ranges>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "Analysis/IAnalysisDb.h"
#include "Authoring/IAuthoringDb.h"
#include "Authoring/AuthoringRecipeMaterializer.h"
#include "Common/DbService.h"
#include "Common/Types/UtcTimestamp.h"
#include "Core/Input/SoaBattle/ActionTypes.h"
#include "Core/Input/SoaBattle/BattleCommandCodec.h"
#include "DbSetup.h"
#include "DurableLogFile.h"
#include "Execution/ProgramDB/ProductionProgramKindRegistry.h"
#include "Execution/ProgramDB/TasMovieValidation/PreparedSterilizedCheckpointEvidence.h"
#include "Execution/Workflow/WorkflowComposition.h"
#include "Execution/Workflow/WorkflowOrchestration.h"
#include "Execution/Workflow/WorkflowUnitActivationFactory.h"
#include "Runner/Runtime/ProgramRuntime/Capabilities/SourceCapabilityPacks.h"
#include "Runner/Runtime/ProgramRuntime/Codec/ProgramCodecV1.h"
#include "ScenarioAssessment.h"
#include "Execution/CoordinatorRuntime.h"
#include "Utils/Hash.h"
#include "WorkerStartupBarrier.h"

namespace savor::e2e {
namespace {

std::string PredicateCreationRequestKey(std::string_view run_identity, std::string_view object)
{
    const auto value = std::string(run_identity) + ":" + std::string(object);
    return hash::sha256(value.data(), value.size()).substr(0, 32);
}

using namespace std::chrono_literals;
using savor::db::execution::workflow::WorkflowInstanceState;
using savor::db::execution::workflow::WorkflowStepState;

// The Battle scenario deliberately includes both complete TAS Movie passes
// before checkpoint sterilization and Battle execution. On the approved DTM,
// those passes consume roughly seven minutes by themselves, so the workflow
// deadline must leave a full execution window for the Battle jobs as well.
constexpr auto kScenarioTimeout = std::chrono::minutes(20);
constexpr std::string_view kCorrelation = "savor-e2e.battle-phases-v1";
constexpr std::int64_t kPredicateItemId = 273;

bool Fail(std::string message, std::string* error_out)
{
    if (error_out != nullptr)
        *error_out = std::move(message);
    return false;
}

std::string_view BattleTargetKindName(
    const savor::db::BattlePlanTargetKind kind) noexcept
{
    switch (kind) {
    case savor::db::BattlePlanTargetKind::SingleEnemy:
        return "SingleEnemy";
    case savor::db::BattlePlanTargetKind::MultipleEnemies:
        return "MultipleEnemies";
    case savor::db::BattlePlanTargetKind::AnyEnemy:
        return "AnyEnemy";
    case savor::db::BattlePlanTargetKind::SameAsOtherPC:
        return "SameAsOtherPC";
    }
    return "Unknown";
}

const savor::runtime::program::ProgramValue* FindProgramValue(
    const savor::runtime::program::ProgramValueGraph& graph,
    const savor::runtime::program::ProgramValueId id) noexcept
{
    const auto found = std::ranges::find(
        graph.values, id, &savor::runtime::program::ProgramValue::id);
    return found == graph.values.end() ? nullptr : &*found;
}

bool SeedBattlePredicateGroup(
    savor::db::IAuthoringDb* authoring_db,
    std::string_view run_identity,
    std::int64_t* group_revision_id_out,
    std::string* error_out)
{
    using namespace savor::runtime::predicates;
    namespace program = savor::runtime::program;
    namespace composition = savor::runtime::program::composition;
    namespace capabilities =
        savor::runtime::program::capabilities;

    if (!authoring_db || !group_revision_id_out)
        return Fail("Battle predicate authoring database is unavailable",
                    error_out);

    const auto hooks = BattlePredicateHookContractV1();
    const auto hook_id = [&](std::string_view suffix)
        -> std::optional<std::string> {
        const auto found = std::ranges::find_if(
            hooks.points, [&](const auto& point) {
                return point.canonical_id.ends_with(suffix);
            });
        return found == hooks.points.end()
            ? std::nullopt
            : std::optional<std::string>(found->canonical_id);
    };
    const auto turn_is_ready = hook_id(".TurnIsReady");
    const auto end_turn = hook_id(".EndTurn");
    const auto victory = hook_id(".EndBattleVictory");
    if (!turn_is_ready || !end_turn || !victory)
        return Fail("Battle predicate hook contract is incomplete", error_out);

    const auto catalog = capabilities::BuildSourceCapabilityPackCatalog();
    const auto turn_order_query =
        capabilities::BattleDerivedTurnOrderActionIdentity();
    const auto rewards_query =
        capabilities::BattleDerivedRewardsActionIdentity();
    const auto output_type = [&](const program::ExactDependencyIdentity& identity)
        -> std::optional<program::TypeRef> {
        const auto found = std::ranges::find(
            catalog.actions, identity, &program::ActionDescriptor::identity);
        return found == catalog.actions.end()
            ? std::nullopt
            : std::optional<program::TypeRef>(found->output_type);
    };
    const auto turn_order_type = output_type(turn_order_query);
    const auto rewards_type = output_type(rewards_query);
    if (!turn_order_type || !rewards_type)
        return Fail("Battle derived query descriptors are unavailable",
                    error_out);

    const auto u16 = program::TypeRef::Builtin(program::BuiltinType::U16);
    const auto u32 = program::TypeRef::Builtin(program::BuiltinType::U32);
    const auto boolean =
        program::TypeRef::Builtin(program::BuiltinType::Bool);
    const std::string suffix(run_identity);
    const auto now = savor::db::types::UtcNow();

    composition::PredicateDefinition turn_order_definition{
        .canonical_id = "savor.e2e.battle.turn_order." + suffix,
        .revision = 1,
        .source_name = "SavorE2E.Battle",
        .witnesses = {{"turn_order", *turn_order_type}},
        .expression = {
            {
                .kind = composition::PredicateExpressionKind::Witness,
                .witness_index = 0,
                .result_type = *turn_order_type,
                .source_label = "turn order snapshot",
            },
            {
                .kind = composition::PredicateExpressionKind::ImportedReducer,
                .operands = {0},
                .reducer = capabilities::BattleDerivedReducerIdentity(
                    "soa.battle.derived.player_max_position"),
                .result_type = u32,
                .source_label = "last player position",
            },
            {
                .kind = composition::PredicateExpressionKind::ImportedReducer,
                .operands = {0},
                .reducer = capabilities::BattleDerivedReducerIdentity(
                    "soa.battle.derived.enemy_min_position"),
                .result_type = u32,
                .source_label = "first enemy position",
            },
            {
                .kind = composition::PredicateExpressionKind::Less,
                .operands = {1, 2},
                .result_type = boolean,
                .source_label = "players act before enemies",
            },
        },
        .root_expression = 3,
    };
    savor::db::PredicateAuthoringRevisionReceipt turn_order_receipt{};
    if (!savor::db::authoring::MaterializePredicateDefinitionDraft(authoring_db, {
            .creation_request_key = PredicateCreationRequestKey(run_identity, "definition.turn_order"),
            .name = "Players act before enemies " + suffix,
            .description =
                "The last active player position precedes the first active enemy position.",
            .body = {turn_order_definition.witnesses, turn_order_definition.expression,
                     turn_order_definition.root_expression},
            .created_at_utc = now,
        }, &turn_order_receipt, error_out)
        || !authoring_db->PublishPredicateDefinitionRevisionV2(
            turn_order_receipt.revision_id, now, nullptr, error_out)) {
        return false;
    }
    const auto turn_order_revision_id = turn_order_receipt.revision_id;

    composition::PredicateDefinition drop_definition{
        .canonical_id = "savor.e2e.battle.drop_count." + suffix,
        .revision = 1,
        .source_name = "SavorE2E.Battle",
        .witnesses = {
            {"rewards", *rewards_type},
            {"item_id", u16},
        },
        .expression = {
            {
                .kind = composition::PredicateExpressionKind::Witness,
                .witness_index = 0,
                .result_type = *rewards_type,
                .source_label = "reward snapshot",
            },
            {
                .kind = composition::PredicateExpressionKind::Witness,
                .witness_index = 1,
                .result_type = u16,
                .source_label = "requested item id",
            },
            {
                .kind = composition::PredicateExpressionKind::ImportedReducer,
                .operands = {0, 1},
                .reducer = capabilities::BattleDerivedReducerIdentity(
                    "soa.battle.derived.drop_count"),
                .result_type = u32,
                .source_label = "cumulative item drops",
            },
            {
                .kind = composition::PredicateExpressionKind::ImportedReducer,
                .operands = {0},
                .reducer = capabilities::BattleDerivedReducerIdentity(
                    "soa.battle.derived.current_turn"),
                .result_type = u32,
                .source_label = "current battle turn",
            },
            {
                .kind = composition::PredicateExpressionKind::GreaterEqual,
                .operands = {2, 3},
                .result_type = boolean,
                .source_label = "drop count keeps pace with turn",
            },
        },
        .root_expression = 4,
    };
    savor::db::PredicateAuthoringRevisionReceipt drop_receipt{};
    if (!savor::db::authoring::MaterializePredicateDefinitionDraft(authoring_db, {
            .creation_request_key = PredicateCreationRequestKey(run_identity, "definition.drop_count"),
            .name = "Cumulative drop count by turn " + suffix,
            .description =
                "The cumulative selected-item drop count is at least the current Battle turn.",
            .body = {drop_definition.witnesses, drop_definition.expression,
                     drop_definition.root_expression},
            .created_at_utc = now,
        }, &drop_receipt, error_out)
        || !authoring_db->PublishPredicateDefinitionRevisionV2(
            drop_receipt.revision_id, now, nullptr, error_out)) {
        return false;
    }
    const auto drop_revision_id = drop_receipt.revision_id;

    const auto published_turn_order =
        authoring_db->GetPredicateDefinitionRevisionV2(turn_order_revision_id);
    const auto published_drop =
        authoring_db->GetPredicateDefinitionRevisionV2(drop_revision_id);
    if (!published_turn_order || !published_drop
        || published_turn_order->revision_state != "PUBLISHED"
        || published_drop->revision_state != "PUBLISHED") {
        return Fail("published Battle predicate definitions are unavailable", error_out);
    }

    PredicateExecutionBindingV1 turn_order_binding{
        .canonical_id = "savor.e2e.battle.binding.turn_order." + suffix,
        .revision = 1,
        .definition = {
            turn_order_revision_id,
            published_turn_order->content_sha256,
            published_turn_order->definition,
        },
        .witnesses = {{
            .witness_ordinal = 0,
            .source_kind = PredicateWitnessSourceKindV1::DerivedStateQuery,
            .value_type = *turn_order_type,
            .source = turn_order_query,
        }},
    };
    savor::db::PredicateAuthoringRevisionReceipt turn_order_binding_receipt{};
    if (!savor::db::authoring::MaterializePredicateExecutionBindingDraft(authoring_db, {
            .creation_request_key = PredicateCreationRequestKey(run_identity, "binding.turn_order"),
            .name = "Current turn order " + suffix,
            .description = "Reads the Battle turn-order snapshot at evaluation time.",
            .body = {turn_order_revision_id, turn_order_binding.witnesses},
            .created_at_utc = now,
        }, &turn_order_binding_receipt, error_out)
        || !authoring_db->PublishPredicateExecutionBindingRevision(
            turn_order_binding_receipt.revision_id, now, nullptr, error_out)) {
        return false;
    }
    const auto turn_order_binding_id = turn_order_binding_receipt.revision_id;

    PredicateExecutionBindingV1 drop_binding{
        .canonical_id = "savor.e2e.battle.binding.electri_box_273." + suffix,
        .revision = 1,
        .definition = {
            drop_revision_id,
            published_drop->content_sha256,
            published_drop->definition,
        },
        .witnesses = {
            {
                .witness_ordinal = 0,
                .source_kind = PredicateWitnessSourceKindV1::DerivedStateQuery,
                .value_type = *rewards_type,
                .source = rewards_query,
            },
            {
                .witness_ordinal = 1,
                .source_kind = PredicateWitnessSourceKindV1::ConcreteValue,
                .value_type = u16,
                .concrete_value = program::LiteralValue{
                    .type = u16,
                    .payload = static_cast<std::uint16_t>(273),
                },
            },
        },
    };
    savor::db::PredicateAuthoringRevisionReceipt drop_binding_receipt{};
    if (!savor::db::authoring::MaterializePredicateExecutionBindingDraft(authoring_db, {
            .creation_request_key = PredicateCreationRequestKey(run_identity, "binding.drop_count.273"),
            .name = "Electri Box cumulative drops " + suffix,
            .description = "Specializes the reusable drop predicate with item_id=273.",
            .body = {drop_revision_id, drop_binding.witnesses},
            .created_at_utc = now,
        }, &drop_binding_receipt, error_out)
        || !authoring_db->PublishPredicateExecutionBindingRevision(
            drop_binding_receipt.revision_id, now, nullptr, error_out)) {
        return false;
    }
    const auto drop_binding_id = drop_binding_receipt.revision_id;

    std::vector<std::string> terminal_hooks{*end_turn, *victory};
    std::ranges::sort(terminal_hooks);
    ResolvedPredicateGroupV1 group{
        .canonical_id = "savor.e2e.battle.group.first_battle." + suffix,
        .revision = 1,
        .members = {
            {
                .ordinal = 0,
                .execution_binding_revision_id = turn_order_binding_id,
                .semantic_hook_ids = {*turn_is_ready},
                .occurrence = PredicateOccurrencePolicyV1::First,
                .reaction = composition::PredicateReaction::RecordAndContinue,
                .participates_in_aggregation = true,
                .emit_evidence = true,
            },
            {
                .ordinal = 1,
                .execution_binding_revision_id = drop_binding_id,
                .semantic_hook_ids = std::move(terminal_hooks),
                .occurrence = PredicateOccurrencePolicyV1::First,
                .reaction = composition::PredicateReaction::AbortOnFail,
                .participates_in_aggregation = true,
                .emit_evidence = true,
            },
        },
    };
    savor::db::PredicateAuthoringRevisionReceipt group_receipt{};
    if (!savor::db::authoring::MaterializePredicateGroupDraft(authoring_db, {
            .creation_request_key = PredicateCreationRequestKey(run_identity, "group.first_battle"),
            .name = "First Battle predicates " + suffix,
            .description =
                "Turn order plus one atomic cumulative-drop predicate across the mutually exclusive terminal hooks.",
            .body = {group.members},
            .created_at_utc = now,
        }, &group_receipt, error_out)
        || !authoring_db->PublishPredicateGroupRevision(
            group_receipt.revision_id, now, nullptr, error_out)) {
        return false;
    }
    *group_revision_id_out = group_receipt.revision_id;
    return true;
}

bool SeedBattleAuthoring(
    savor::db::IAuthoringDb* authoring_db,
    const std::string_view run_identity,
    const bool cutscene_mode,
    std::int64_t* battle_plan_id_out,
    std::string* error_out)
{
    if (authoring_db == nullptr || battle_plan_id_out == nullptr)
        return Fail("Battle authoring database is unavailable", error_out);

    const auto now = savor::db::types::UtcNow();
    const std::string suffix(run_identity);
    std::optional<std::int64_t> predicate_group_revision_id;
    if (!cutscene_mode) {
        std::int64_t group_id = 0;
        if (!SeedBattlePredicateGroup(
                authoring_db, run_identity, &group_id, error_out)) return false;
        predicate_group_revision_id = group_id;
    }
    std::int64_t plan_id = 0;
    if (!savor::db::authoring::MaterializeBattlePlanHeader(authoring_db, {
            .name = (cutscene_mode ? "SavorE2E four-turn cutscene Battle plan "
                                   : "SavorE2E two-turn Battle plan ") + suffix,
            .fingerprint = (cutscene_mode
                ? "savor-e2e-cutscene-battle-" : "savor-e2e-battle-phases-")
                + suffix,
            .created_at_utc = now,
            .correlation_id = std::string(kCorrelation),
            .causation_id = "scenario",
        }, &plan_id, error_out))
        return false;

    std::int64_t attack_any_enemy_id = 0;
    if (!savor::db::authoring::MaterializeBattleActionPreset(authoring_db, {
            .name = "Attack any enemy " + suffix,
            .macro = soa::battle::actions::BattleAction::Attack,
            .target_kind = savor::db::BattlePlanTargetKind::AnyEnemy,
            .created_at_utc = now,
            .correlation_id = std::string(kCorrelation),
            .causation_id = "plan-" + std::to_string(plan_id),
        }, &attack_any_enemy_id, error_out))
        return false;

    std::int64_t attack_same_as_actor_zero_id = 0;
    if (!savor::db::authoring::MaterializeBattleActionPreset(authoring_db, {
            .name = "Attack same target as actor 0 " + suffix,
            .macro = soa::battle::actions::BattleAction::Attack,
            .target_kind = savor::db::BattlePlanTargetKind::SameAsOtherPC,
            .target_same_as_actor_slot = 0,
            .created_at_utc = now,
            .correlation_id = std::string(kCorrelation),
            .causation_id = "plan-" + std::to_string(plan_id),
        }, &attack_same_as_actor_zero_id, error_out))
        return false;

    std::int64_t turn_id = 0;
    const int turn_count = cutscene_mode ? 4 : 2;
    for (int turn_index = 1; turn_index <= turn_count; ++turn_index) {
        if (!savor::db::authoring::MaterializeBattlePlanTurn(authoring_db, {
                .plan_id = plan_id,
                .turn_index = turn_index,
                .default_predicate_group_revision_id = predicate_group_revision_id,
                .actions = {
                    {.actor_slot = 0, .action_preset_id = attack_any_enemy_id, .ordinal = 0},
                    {.actor_slot = 1, .action_preset_id = attack_same_as_actor_zero_id, .ordinal = 1},
                },
                .created_at_utc = now,
                .correlation_id = std::string(kCorrelation),
                .causation_id = "plan-" + std::to_string(plan_id),
            }, &turn_id, error_out)) return false;
    }

    *battle_plan_id_out = plan_id;
    return true;
}

bool SeedBattleWorkflow(
    savor::db::IAuthoringDb* authoring_db,
    savor::db::IExecutionDb* execution_db,
    std::int64_t dtm_artifact_id,
    std::int64_t seed_probe_spec_id,
    std::int64_t battle_plan_id,
    std::int64_t rtc_value,
    int samples_per_axis,
    int fake_attack_min,
    int fake_attack_max,
    bool cutscene_delay,
    const std::string_view run_identity,
    std::int64_t* workflow_instance_id_out,
    std::string* error_out)
{
    if (authoring_db == nullptr || execution_db == nullptr ||
        dtm_artifact_id <= 0 || seed_probe_spec_id <= 0 ||
        battle_plan_id <= 0 || rtc_value < 0 ||
        static_cast<std::uint64_t>(rtc_value) >
            std::numeric_limits<std::uint32_t>::max() ||
        workflow_instance_id_out == nullptr)
        return Fail("Battle workflow seed input is incomplete", error_out);

    const auto now = savor::db::types::UtcNow();
    savor::db::SaveWorkflowGraphResult saved{};
    savor::db::SaveWorkflowGraphCommand graph_definition{
            .name = "SavorE2E Battle phases workflow "
                + std::string(run_identity),
            .description = "Validate the approved DTM, sterilize its checkpoint, then run Battle Context and SeedProbe in parallel before battle.start",
            .graph_version = 1,
            .graph_hash = "savor-e2e.battle-phases-v2."
                + std::string(run_identity),
            .nodes = {
                {
                    .node_key = "tas_establish_1",
                    .unit_kind = "tas_movie_establish_root_cursor",
                    .display_name = "TAS Movie: Establish Root Cursor",
                    .inputs = {{
                        .input_key = "root_dtm",
                        .data_kind = "state_artifact.dtm_artifact_id",
                        .ref_kind = "state_artifact",
                        .display_name = "Approved root DTM",
                    }},
                    .possible_outputs = {
                        {
                            .output_key = "tas_movie_validation_attempt",
                            .data_kind = "analysis.tas_movie_validation_attempt_id",
                            .ref_kind = "tmv_validation_attempt",
                            .display_name = "Validation attempt",
                        },
                        {
                            .output_key = "root_establishment",
                            .data_kind = "analysis.tas_movie_root_establishment_attempt_id",
                            .ref_kind = "tmv_root_establishment_attempt",
                            .display_name = "Established root cursor attempt",
                        },
                        {
                            .output_key = "root_dtm",
                            .data_kind = "state_artifact.dtm_artifact_id",
                            .ref_kind = "state_artifact",
                            .display_name = "Established root DTM",
                        },
                    },
                },
                {
                    .node_key = "tas_validate_1",
                    .unit_kind = "tas_movie_validate_root",
                    .display_name = "TAS Movie: Validate Root",
                    .inputs = {{
                        .input_key = "root_establishment",
                        .data_kind = "analysis.tas_movie_root_establishment_attempt_id",
                        .ref_kind = "tmv_root_establishment_attempt",
                        .display_name = "Root cursor establishment",
                    }},
                    .possible_outputs = {
                        {
                            .output_key = "tas_movie_validation_attempt",
                            .data_kind = "analysis.tas_movie_validation_attempt_id",
                            .ref_kind = "tmv_validation_attempt",
                            .display_name = "Validation attempt",
                        },
                        {
                            .output_key = "validated_checkpoint_savestate",
                            .data_kind = "state.movie_paired_savestate_id",
                            .ref_kind = "state.savestate",
                            .display_name = "Validated movie-paired checkpoint",
                        },
                    },
                    .arguments = {{
                        .argument_key = "rtc",
                        .display_name = "RTC",
                        .value_type = "integer",
                        .required = true,
                        .minimum_integer = 0,
                        .maximum_integer = 4294967295ULL,
                    }},
                },
                {
                    .node_key = "tas_sterilize_1",
                    .unit_kind = "tas_movie_checkpoint_sterilize",
                    .display_name = "TAS Movie: Sterilize Checkpoint",
                    .inputs = {{
                        .input_key = "paired_checkpoint_savestate",
                        .data_kind = "state.movie_paired_savestate_id",
                        .ref_kind = "state.savestate",
                        .display_name = "Validated movie-paired checkpoint",
                    }},
                    .possible_outputs = {{
                        .output_key = "sterilized_checkpoint_savestate",
                        .data_kind = "state.movie_inactive_savestate_id",
                        .ref_kind = "state.savestate",
                        .display_name = "Sterilized movie-inactive checkpoint",
                    }},
                },
                {
                    .node_key = "probe_1",
                    .unit_kind = "seed_probe",
                    .display_name = "SeedProbe",
                    .authored_ref_kind = std::string("seed_probe_spec"),
                    .authored_ref_id = seed_probe_spec_id,
                    .inputs = {{
                        .input_key = "entry_savestate",
                        .data_kind = "state.movie_inactive_savestate_id",
                        .ref_kind = "state.savestate",
                        .display_name = "Sterilized entry savestate",
                    }},
                    .possible_outputs = {{
                        .output_key = "seed_probe_run",
                        .data_kind = "analysis.seed_probe_run",
                        .ref_kind = "sp_probe_run",
                        .display_name = "Confirmed SeedProbe run",
                    }},
                    .arguments = {{
                        .argument_key = "samples_per_axis",
                        .display_name = "Samples per axis",
                        .value_type = "integer",
                        .required = false,
                        .default_value = std::string("5"),
                        .minimum_integer = 1,
                        .maximum_integer = 64,
                    }},
                },
                {
                    .node_key = "context_1",
                    .unit_kind = "battle.context",
                    .display_name = "Battle Context",
                    .inputs = {{
                        .input_key = "entry_savestate",
                        .data_kind = "state.movie_inactive_savestate_id",
                        .ref_kind = "state.savestate",
                        .display_name = "Sterilized entry savestate",
                    }},
                    .possible_outputs = {{
                        .output_key = "battle_context",
                        .data_kind = "analysis_battle.battle_context_id",
                        .ref_kind = "ab_battle_context",
                        .display_name = "Battle context",
                    }},
                },
                {
                    .node_key = "battle_1",
                    .unit_kind = "battle",
                    .display_name = "Battle",
                    .authored_ref_kind = std::string("authoring.battle_plan"),
                    .authored_ref_id = battle_plan_id,
                    .inputs = {
                        {
                            .input_key = "seed_probe_run",
                            .data_kind = "analysis.seed_probe_run",
                            .ref_kind = "sp_probe_run",
                            .display_name = "Confirmed SeedProbe run",
                        },
                        {
                            .input_key = "battle_context",
                            .data_kind = "analysis_battle.battle_context_id",
                            .ref_kind = "ab_battle_context",
                            .display_name = "Battle context",
                        },
                    },
                    .possible_outputs = {{
                        .output_key = "battle_set",
                        .data_kind = "analysis_battle.battle_set",
                        .ref_kind = "analysis_battle.battle_set",
                        .display_name = "Battle set",
                    }},
                    .arguments = {
                        {
                            .argument_key = "continuation_mode",
                            .display_name = "Continuation",
                            .value_type = "choice",
                            .required = true,
                            .choices = {
                                {"manual_selection", "Manual selection after each wave"},
                                {"automatic_best_per_ending_rng", "Automatically continue the best candidate for each ending RNG"},
                            },
                        },
                        {
                            .argument_key = "continue_automatic_exploration_after_victory",
                            .display_name = "Continue automatic exploration after Victory",
                            .value_type = "boolean",
                            .default_value = std::string("false"),
                        },
                        {
                            .argument_key = "fake_attack_min",
                            .display_name = "Minimum fake attacks",
                            .value_type = "integer",
                            .default_value = std::string("0"),
                            .minimum_integer = 0,
                            .maximum_integer = 2147483647,
                        },
                        {
                            .argument_key = "fake_attack_max",
                            .display_name = "Maximum fake attacks",
                            .value_type = "integer",
                            .default_value = std::string("0"),
                            .minimum_integer = 0,
                            .maximum_integer = 2147483647,
                        },
                    },
                    .argument_constraints = {{
                        .lesser_or_equal_key = "fake_attack_min",
                        .greater_or_equal_key = "fake_attack_max",
                        .message = "minimum fake attacks must not exceed maximum fake attacks",
                    }},
                },
            },
            .edges = {
                {
                    .from_node_key = "tas_establish_1",
                    .output_key = "root_establishment",
                    .to_node_key = "tas_validate_1",
                    .input_key = "root_establishment",
                    .guard_kind = std::string(
                        savor::db::kWorkflowOutputPresentGuard),
                },
                {
                    .from_node_key = "tas_validate_1",
                    .output_key = "validated_checkpoint_savestate",
                    .to_node_key = "tas_sterilize_1",
                    .input_key = "paired_checkpoint_savestate",
                    .guard_kind = std::string(
                        savor::db::kWorkflowOutputPresentGuard),
                },
                {
                    .from_node_key = "tas_sterilize_1",
                    .output_key = "sterilized_checkpoint_savestate",
                    .to_node_key = "probe_1",
                    .input_key = "entry_savestate",
                    .guard_kind = std::string(
                        savor::db::kWorkflowOutputPresentGuard),
                },
                {
                    .from_node_key = "tas_sterilize_1",
                    .output_key = "sterilized_checkpoint_savestate",
                    .to_node_key = "context_1",
                    .input_key = "entry_savestate",
                    .guard_kind = std::string(
                        savor::db::kWorkflowOutputPresentGuard),
                },
                {
                    .from_node_key = "probe_1",
                    .output_key = "seed_probe_run",
                    .to_node_key = "battle_1",
                    .input_key = "seed_probe_run",
                    .guard_kind = std::string(
                        savor::db::kWorkflowOutputPresentGuard),
                },
                {
                    .from_node_key = "context_1",
                    .output_key = "battle_context",
                    .to_node_key = "battle_1",
                    .input_key = "battle_context",
                    .guard_kind = std::string(
                        savor::db::kWorkflowOutputPresentGuard),
                },
            },
            .created_at_utc = now,
            .correlation_id = std::string(kCorrelation),
            .causation_id = "scenario",
        };
    if (cutscene_delay) {
        graph_definition.description =
            "Establish and annotate the approved DTM, insert one neutral input epoch, validate and sterilize the revised checkpoint, then run Battle Context and SeedProbe in parallel before battle.start";
        graph_definition.graph_hash = "savor-e2e.battle-phases.cutscene-delay-v1."
            + std::string(run_identity);
        graph_definition.nodes.insert(graph_definition.nodes.begin() + 1, {
            .node_key = "tas_annotate_1",
            .unit_kind = "tas_movie_annotate",
            .display_name = "TAS Movie: Annotate Input Epochs",
            .inputs = {{
                .input_key = "root_dtm",
                .data_kind = "state_artifact.dtm_artifact_id",
                .ref_kind = "state_artifact",
                .display_name = "Complete boot DTM",
            }},
            .possible_outputs = {{
                .output_key = "annotation_attempt",
                .data_kind = "analysis.tas_movie_input_epoch_annotation_attempt_id",
                .ref_kind = "tmv_input_epoch_annotation_attempt",
                .display_name = "Input-epoch annotation attempt",
            }},
        });
        graph_definition.nodes.insert(graph_definition.nodes.begin() + 2, {
            .node_key = "tas_revise_1",
            .unit_kind = "tas_movie_revise",
            .display_name = "TAS Movie: Rewrite Input Epochs",
            .inputs = {
                {
                    .input_key = "annotation_attempt",
                    .data_kind = "analysis.tas_movie_input_epoch_annotation_attempt_id",
                    .ref_kind = "tmv_input_epoch_annotation_attempt",
                    .display_name = "Source input-epoch annotation",
                },
                {
                    .input_key = "root_establishment",
                    .data_kind = "analysis.tas_movie_root_establishment_attempt_id",
                    .ref_kind = "tmv_root_establishment_attempt",
                    .display_name = "Source root establishment",
                },
            },
            .possible_outputs = {
                {
                    .output_key = "rewrite_attempt",
                    .data_kind = "analysis.tas_movie_input_epoch_rewrite_attempt_id",
                    .ref_kind = "tmv_input_epoch_rewrite_attempt",
                    .display_name = "Input-epoch rewrite attempt",
                },
                {
                    .output_key = "rewritten_dtm",
                    .data_kind = "state_artifact.dtm_artifact_id",
                    .ref_kind = "state_artifact",
                    .display_name = "Rewritten DTM",
                },
                {
                    .output_key = "rewritten_paired_savestate",
                    .data_kind = "state.movie_paired_savestate_id",
                    .ref_kind = "state.savestate",
                    .display_name = "Rewritten movie-paired endpoint",
                },
                {
                    .output_key = "annotation_attempt",
                    .data_kind = "analysis.tas_movie_input_epoch_annotation_attempt_id",
                    .ref_kind = "tmv_input_epoch_annotation_attempt",
                    .display_name = "Rewritten input-epoch annotation",
                },
                {
                    .output_key = "root_establishment",
                    .data_kind = "analysis.tas_movie_root_establishment_attempt_id",
                    .ref_kind = "tmv_root_establishment_attempt",
                    .display_name = "Rewritten root establishment",
                },
            },
            .arguments = {
                {
                    .argument_key = "neutral_epoch_count",
                    .display_name = "Neutral input epochs",
                    .value_type = "integer",
                    .required = false,
                    .default_value = std::string("1"),
                    .minimum_integer = 1,
                    .maximum_integer =
                        static_cast<std::uint64_t>(
                            std::numeric_limits<std::int32_t>::max()),
                },
                {
                    .argument_key = "placement_profile",
                    .display_name = "Placement profile",
                    .value_type = "choice",
                    .required = false,
                    .default_value = std::string("first_battle.final_dialog"),
                    .choices = {{
                        .value = "first_battle.final_dialog",
                        .display_name = "First battle: final dialog B to A",
                    }},
                },
            },
        });
        auto establish_to_validate = std::ranges::find_if(
            graph_definition.edges, [](const auto& edge) {
                return edge.from_node_key == "tas_establish_1"
                    && edge.to_node_key == "tas_validate_1";
            });
        if (establish_to_validate == graph_definition.edges.end())
            return Fail("Battle workflow delay splice could not find the establishment edge", error_out);
        establish_to_validate->to_node_key = "tas_revise_1";
        graph_definition.edges.push_back({
            .from_node_key = "tas_establish_1",
            .output_key = "root_dtm",
            .to_node_key = "tas_annotate_1",
            .input_key = "root_dtm",
            .guard_kind = std::string(savor::db::kWorkflowOutputPresentGuard),
        });
        graph_definition.edges.push_back({
            .from_node_key = "tas_annotate_1",
            .output_key = "annotation_attempt",
            .to_node_key = "tas_revise_1",
            .input_key = "annotation_attempt",
            .guard_kind = std::string(savor::db::kWorkflowOutputPresentGuard),
        });
        graph_definition.edges.push_back({
            .from_node_key = "tas_revise_1",
            .output_key = "root_establishment",
            .to_node_key = "tas_validate_1",
            .input_key = "root_establishment",
            .guard_kind = std::string(savor::db::kWorkflowOutputPresentGuard),
        });
    }
    if (!savor::db::authoring::MaterializeWorkflowGraph(
            authoring_db, graph_definition, &saved, error_out))
        return false;

    const auto unit_registry =
        savor::db::execution::workflow::BuildDefaultWorkflowUnitRegistry();
    std::string activation_error;
    auto establish = savor::db::execution::workflow::
        BuildUnitActivationSpecFromDefinition(
            unit_registry, "tas_establish_1", "tas_establish_1",
            "tas_movie_establish_root_cursor",
            "TAS Movie: Establish Root Cursor", std::nullopt, std::nullopt,
            {}, &activation_error);
    decltype(establish) annotate;
    decltype(establish) revise;
    if (cutscene_delay) {
        annotate = savor::db::execution::workflow::
            BuildUnitActivationSpecFromDefinition(
                unit_registry, "tas_annotate_1", "tas_annotate_1",
                "tas_movie_annotate", "TAS Movie: Annotate Input Epochs",
                std::nullopt, std::nullopt, {"tas_establish_1"},
                &activation_error);
        revise = savor::db::execution::workflow::
            BuildUnitActivationSpecFromDefinition(
                unit_registry, "tas_revise_1", "tas_revise_1",
                "tas_movie_revise", "TAS Movie: Rewrite Input Epochs",
                std::nullopt, std::nullopt,
                {"tas_establish_1", "tas_annotate_1"}, &activation_error);
    }
    auto validate = savor::db::execution::workflow::
        BuildUnitActivationSpecFromDefinition(
            unit_registry, "tas_validate_1", "tas_validate_1",
            "tas_movie_validate_root", "TAS Movie: Validate Root",
            std::nullopt, std::nullopt,
            cutscene_delay ? std::vector<std::string>{"tas_revise_1"}
                           : std::vector<std::string>{"tas_establish_1"},
            &activation_error);
    auto sterilize = savor::db::execution::workflow::
        BuildUnitActivationSpecFromDefinition(
            unit_registry, "tas_sterilize_1", "tas_sterilize_1",
            "tas_movie_checkpoint_sterilize",
            "TAS Movie: Sterilize Checkpoint", std::nullopt, std::nullopt,
            {"tas_validate_1"}, &activation_error);
    auto probe = savor::db::execution::workflow::
        BuildUnitActivationSpecFromDefinition(
            unit_registry, "probe_1", "probe_1", "seed_probe",
            "SeedProbe", std::optional<std::string>("seed_probe_spec"),
            seed_probe_spec_id, {"tas_sterilize_1"}, &activation_error);
    auto context = savor::db::execution::workflow::
        BuildUnitActivationSpecFromDefinition(
            unit_registry, "context_1", "context_1", "battle.context",
            "Battle Context", std::nullopt, std::nullopt,
            {"tas_sterilize_1"},
            &activation_error);
    auto battle = savor::db::execution::workflow::
        BuildUnitActivationSpecFromDefinition(
            unit_registry, "battle_1", "battle_1", "battle",
            "Battle", std::optional<std::string>(
                "authoring.battle_plan"), battle_plan_id,
            {"probe_1", "context_1"}, &activation_error);
    if (!establish || (cutscene_delay && (!annotate || !revise))
        || !validate || !sterilize || !probe || !context || !battle)
        return Fail("Battle workflow activation failed: " + activation_error,
                    error_out);

    if (establish->steps.size() != 1 ||
        establish->steps.front().step_kind !=
            "tasmovie.establish_root_cursor" ||
        validate->steps.size() != 1 ||
        validate->steps.front().step_kind != "tasmovie.validate_root" ||
        sterilize->steps.size() != 1 ||
        sterilize->steps.front().step_kind !=
            "tasmovie.checkpoint_sterilize")
        return Fail("Battle TAS provenance prefix did not resolve to exact singleton steps",
                    error_out);

    savor::db::execution::workflow::WorkflowCreateInstanceCommand command{};
    command.workflow_kind = "workflow_graph";
    command.root_scope_kind = "manual";
    command.workflow_graph_revision_id = saved.workflow_graph_revision_id;
    command.created_by = "savor-e2e";
    command.created_at_utc = now.time_since_epoch().count();
    command.unit_activations.push_back(std::move(*establish));
    if (cutscene_delay) {
        command.unit_activations.push_back(std::move(*annotate));
        command.unit_activations.push_back(std::move(*revise));
    }
    command.unit_activations.push_back(std::move(*validate));
    command.unit_activations.push_back(std::move(*sterilize));
    command.unit_activations.push_back(std::move(*probe));
    command.unit_activations.push_back(std::move(*context));
    command.unit_activations.push_back(std::move(*battle));
    command.input_bindings.push_back({
        .node_key = "tas_establish_1",
        .input_key = "root_dtm",
        .data_kind = "state_artifact.dtm_artifact_id",
        .ref_kind = "state_artifact",
        .ref_id = dtm_artifact_id,
        .source_kind = "external",
    });
    if (cutscene_delay) {
        command.arguments.push_back({
            .node_key = "tas_revise_1",
            .argument_key = "neutral_epoch_count",
            .value_type = "integer",
            .integer_value = 1,
            .source_kind = "scenario",
        });
        command.arguments.push_back({
            .node_key = "tas_revise_1",
            .argument_key = "placement_profile",
            .value_type = "choice",
            .text_value = std::string("first_battle.final_dialog"),
            .source_kind = "scenario",
        });
    }
    command.arguments.push_back({
        .node_key = "tas_validate_1",
        .argument_key = "rtc",
        .value_type = "integer",
        .integer_value = rtc_value,
        .source_kind = "scenario",
    });
    command.arguments.push_back({
        .node_key = "probe_1",
        .argument_key = "samples_per_axis",
        .value_type = "integer",
        .integer_value = samples_per_axis,
        .source_kind = "scenario",
    });
    command.arguments.push_back({
        .node_key = "battle_1",
        .argument_key = "fake_attack_min",
        .value_type = "integer",
        .integer_value = fake_attack_min,
        .source_kind = "scenario",
    });
    command.arguments.push_back({
        .node_key = "battle_1",
        .argument_key = "fake_attack_max",
        .value_type = "integer",
        .integer_value = fake_attack_max,
        .source_kind = "scenario",
    });
    command.arguments.push_back({
        .node_key = "battle_1",
        .argument_key = "continuation_mode",
        .value_type = "choice",
        .text_value = std::string("automatic_best_per_ending_rng"),
        .source_kind = "scenario",
    });
    command.arguments.push_back({
        .node_key = "battle_1",
        .argument_key = "continue_automatic_exploration_after_victory",
        .value_type = "boolean",
        .integer_value = 0,
        .source_kind = "scenario",
    });
    return execution_db->CreateWorkflowInstance(
        command, workflow_instance_id_out, error_out);
}

bool SeedEstablishedBattleWorkflow(
    savor::db::IAuthoringDb* authoring_db,
    savor::db::IExecutionDb* execution_db,
    const std::int64_t root_establishment_attempt_id,
    const std::int64_t seed_probe_spec_id,
    const std::int64_t battle_plan_id,
    const std::int64_t rtc_value,
    const int samples_per_axis,
    const int fake_attack_min,
    const int fake_attack_max,
    const std::string_view run_identity,
    std::int64_t* workflow_instance_id_out,
    std::string* error_out)
{
    if (authoring_db == nullptr || execution_db == nullptr ||
        root_establishment_attempt_id <= 0 || seed_probe_spec_id <= 0 ||
        battle_plan_id <= 0 || rtc_value < 0 ||
        static_cast<std::uint64_t>(rtc_value) >
            std::numeric_limits<std::uint32_t>::max() ||
        workflow_instance_id_out == nullptr)
        return Fail("Established-root Battle workflow seed input is incomplete",
                    error_out);

    const auto now = savor::db::types::UtcNow();
    savor::db::SaveWorkflowGraphResult saved{};
    if (!savor::db::authoring::MaterializeWorkflowGraph(authoring_db, {
            .name = "SavorE2E established-root Battle phases workflow "
                + std::string(run_identity),
            .description = "Validate an established root cursor, sterilize its checkpoint, then run Battle Context and SeedProbe in parallel before battle.start",
            .graph_version = 1,
            .graph_hash = "savor-e2e.battle-phases.established-root-v1."
                + std::string(run_identity),
            .nodes = {
                {
                    .node_key = "tas_validate_1",
                    .unit_kind = "tas_movie_validate_root",
                    .display_name = "TAS Movie: Validate Root",
                    .inputs = {{
                        .input_key = "root_establishment",
                        .data_kind = "analysis.tas_movie_root_establishment_attempt_id",
                        .ref_kind = "tmv_root_establishment_attempt",
                        .display_name = "Root cursor establishment",
                    }},
                    .possible_outputs = {
                        {
                            .output_key = "tas_movie_validation_attempt",
                            .data_kind = "analysis.tas_movie_validation_attempt_id",
                            .ref_kind = "tmv_validation_attempt",
                            .display_name = "Validation attempt",
                        },
                        {
                            .output_key = "validated_checkpoint_savestate",
                            .data_kind = "state.movie_paired_savestate_id",
                            .ref_kind = "state.savestate",
                            .display_name = "Validated movie-paired checkpoint",
                        },
                    },
                    .arguments = {{
                        .argument_key = "rtc",
                        .display_name = "RTC",
                        .value_type = "integer",
                        .required = true,
                        .minimum_integer = 0,
                        .maximum_integer = 4294967295ULL,
                    }},
                },
                {
                    .node_key = "tas_sterilize_1",
                    .unit_kind = "tas_movie_checkpoint_sterilize",
                    .display_name = "TAS Movie: Sterilize Checkpoint",
                    .inputs = {{
                        .input_key = "paired_checkpoint_savestate",
                        .data_kind = "state.movie_paired_savestate_id",
                        .ref_kind = "state.savestate",
                        .display_name = "Validated movie-paired checkpoint",
                    }},
                    .possible_outputs = {{
                        .output_key = "sterilized_checkpoint_savestate",
                        .data_kind = "state.movie_inactive_savestate_id",
                        .ref_kind = "state.savestate",
                        .display_name = "Sterilized movie-inactive checkpoint",
                    }},
                },
                {
                    .node_key = "probe_1",
                    .unit_kind = "seed_probe",
                    .display_name = "SeedProbe",
                    .authored_ref_kind = std::string("seed_probe_spec"),
                    .authored_ref_id = seed_probe_spec_id,
                    .inputs = {{
                        .input_key = "entry_savestate",
                        .data_kind = "state.movie_inactive_savestate_id",
                        .ref_kind = "state.savestate",
                        .display_name = "Sterilized entry savestate",
                    }},
                    .possible_outputs = {{
                        .output_key = "seed_probe_run",
                        .data_kind = "analysis.seed_probe_run",
                        .ref_kind = "sp_probe_run",
                        .display_name = "Confirmed SeedProbe run",
                    }},
                    .arguments = {{
                        .argument_key = "samples_per_axis",
                        .display_name = "Samples per axis",
                        .value_type = "integer",
                        .required = false,
                        .default_value = std::string("5"),
                        .minimum_integer = 1,
                        .maximum_integer = 64,
                    }},
                },
                {
                    .node_key = "context_1",
                    .unit_kind = "battle.context",
                    .display_name = "Battle Context",
                    .inputs = {{
                        .input_key = "entry_savestate",
                        .data_kind = "state.movie_inactive_savestate_id",
                        .ref_kind = "state.savestate",
                        .display_name = "Sterilized entry savestate",
                    }},
                    .possible_outputs = {{
                        .output_key = "battle_context",
                        .data_kind = "analysis_battle.battle_context_id",
                        .ref_kind = "ab_battle_context",
                        .display_name = "Battle context",
                    }},
                },
                {
                    .node_key = "battle_1",
                    .unit_kind = "battle",
                    .display_name = "Battle",
                    .authored_ref_kind = std::string("authoring.battle_plan"),
                    .authored_ref_id = battle_plan_id,
                    .inputs = {
                        {
                            .input_key = "seed_probe_run",
                            .data_kind = "analysis.seed_probe_run",
                            .ref_kind = "sp_probe_run",
                            .display_name = "Confirmed SeedProbe run",
                        },
                        {
                            .input_key = "battle_context",
                            .data_kind = "analysis_battle.battle_context_id",
                            .ref_kind = "ab_battle_context",
                            .display_name = "Battle context",
                        },
                    },
                    .possible_outputs = {{
                        .output_key = "battle_set",
                        .data_kind = "analysis_battle.battle_set",
                        .ref_kind = "analysis_battle.battle_set",
                        .display_name = "Battle set",
                    }},
                    .arguments = {
                        {
                            .argument_key = "continuation_mode",
                            .display_name = "Continuation",
                            .value_type = "choice",
                            .required = true,
                            .choices = {
                                {"manual_selection", "Manual selection after each wave"},
                                {"automatic_best_per_ending_rng", "Automatically continue the best candidate for each ending RNG"},
                            },
                        },
                        {
                            .argument_key = "continue_automatic_exploration_after_victory",
                            .display_name = "Continue automatic exploration after Victory",
                            .value_type = "boolean",
                            .default_value = std::string("false"),
                        },
                        {
                            .argument_key = "fake_attack_min",
                            .display_name = "Minimum fake attacks",
                            .value_type = "integer",
                            .default_value = std::string("0"),
                            .minimum_integer = 0,
                            .maximum_integer = 2147483647,
                        },
                        {
                            .argument_key = "fake_attack_max",
                            .display_name = "Maximum fake attacks",
                            .value_type = "integer",
                            .default_value = std::string("0"),
                            .minimum_integer = 0,
                            .maximum_integer = 2147483647,
                        },
                    },
                    .argument_constraints = {{
                        .lesser_or_equal_key = "fake_attack_min",
                        .greater_or_equal_key = "fake_attack_max",
                        .message = "minimum fake attacks must not exceed maximum fake attacks",
                    }},
                },
            },
            .edges = {
                {
                    .from_node_key = "tas_validate_1",
                    .output_key = "validated_checkpoint_savestate",
                    .to_node_key = "tas_sterilize_1",
                    .input_key = "paired_checkpoint_savestate",
                    .guard_kind = std::string(
                        savor::db::kWorkflowOutputPresentGuard),
                },
                {
                    .from_node_key = "tas_sterilize_1",
                    .output_key = "sterilized_checkpoint_savestate",
                    .to_node_key = "probe_1",
                    .input_key = "entry_savestate",
                    .guard_kind = std::string(
                        savor::db::kWorkflowOutputPresentGuard),
                },
                {
                    .from_node_key = "tas_sterilize_1",
                    .output_key = "sterilized_checkpoint_savestate",
                    .to_node_key = "context_1",
                    .input_key = "entry_savestate",
                    .guard_kind = std::string(
                        savor::db::kWorkflowOutputPresentGuard),
                },
                {
                    .from_node_key = "probe_1",
                    .output_key = "seed_probe_run",
                    .to_node_key = "battle_1",
                    .input_key = "seed_probe_run",
                    .guard_kind = std::string(
                        savor::db::kWorkflowOutputPresentGuard),
                },
                {
                    .from_node_key = "context_1",
                    .output_key = "battle_context",
                    .to_node_key = "battle_1",
                    .input_key = "battle_context",
                    .guard_kind = std::string(
                        savor::db::kWorkflowOutputPresentGuard),
                },
            },
            .created_at_utc = now,
            .correlation_id = std::string(kCorrelation),
            .causation_id = "established-root-scenario",
        }, &saved, error_out))
        return false;

    const auto unit_registry =
        savor::db::execution::workflow::BuildDefaultWorkflowUnitRegistry();
    std::string activation_error;
    auto validate = savor::db::execution::workflow::
        BuildUnitActivationSpecFromDefinition(
            unit_registry, "tas_validate_1", "tas_validate_1",
            "tas_movie_validate_root", "TAS Movie: Validate Root",
            std::nullopt, std::nullopt, {}, &activation_error);
    auto sterilize = savor::db::execution::workflow::
        BuildUnitActivationSpecFromDefinition(
            unit_registry, "tas_sterilize_1", "tas_sterilize_1",
            "tas_movie_checkpoint_sterilize",
            "TAS Movie: Sterilize Checkpoint", std::nullopt, std::nullopt,
            {"tas_validate_1"}, &activation_error);
    auto probe = savor::db::execution::workflow::
        BuildUnitActivationSpecFromDefinition(
            unit_registry, "probe_1", "probe_1", "seed_probe",
            "SeedProbe", std::optional<std::string>("seed_probe_spec"),
            seed_probe_spec_id, {"tas_sterilize_1"}, &activation_error);
    auto context = savor::db::execution::workflow::
        BuildUnitActivationSpecFromDefinition(
            unit_registry, "context_1", "context_1", "battle.context",
            "Battle Context", std::nullopt, std::nullopt,
            {"tas_sterilize_1"}, &activation_error);
    auto battle = savor::db::execution::workflow::
        BuildUnitActivationSpecFromDefinition(
            unit_registry, "battle_1", "battle_1", "battle",
            "Battle", std::optional<std::string>("authoring.battle_plan"),
            battle_plan_id, {"probe_1", "context_1"}, &activation_error);
    if (!validate || !sterilize || !probe || !context || !battle)
        return Fail("Established-root Battle workflow activation failed: "
                        + activation_error,
                    error_out);
    if (validate->steps.size() != 1 ||
        validate->steps.front().step_kind != "tasmovie.validate_root" ||
        sterilize->steps.size() != 1 ||
        sterilize->steps.front().step_kind !=
            "tasmovie.checkpoint_sterilize")
        return Fail("Established-root Battle TAS provenance prefix did not resolve to exact singleton steps",
                    error_out);

    savor::db::execution::workflow::WorkflowCreateInstanceCommand command{};
    command.workflow_kind = "workflow_graph";
    command.root_scope_kind = "manual";
    command.workflow_graph_revision_id = saved.workflow_graph_revision_id;
    command.created_by = "savor-e2e";
    command.created_at_utc = now.time_since_epoch().count();
    command.unit_activations = {
        std::move(*validate), std::move(*sterilize), std::move(*probe),
        std::move(*context), std::move(*battle)};
    command.input_bindings.push_back({
        .node_key = "tas_validate_1",
        .input_key = "root_establishment",
        .data_kind = "analysis.tas_movie_root_establishment_attempt_id",
        .ref_kind = "tmv_root_establishment_attempt",
        .ref_id = root_establishment_attempt_id,
        .source_kind = "external",
    });
    command.arguments.push_back({
        .node_key = "tas_validate_1", .argument_key = "rtc",
        .value_type = "integer", .integer_value = rtc_value,
        .source_kind = "scenario",
    });
    command.arguments.push_back({
        .node_key = "probe_1", .argument_key = "samples_per_axis",
        .value_type = "integer", .integer_value = samples_per_axis,
        .source_kind = "scenario",
    });
    command.arguments.push_back({
        .node_key = "battle_1", .argument_key = "fake_attack_min",
        .value_type = "integer", .integer_value = fake_attack_min,
        .source_kind = "scenario",
    });
    command.arguments.push_back({
        .node_key = "battle_1", .argument_key = "fake_attack_max",
        .value_type = "integer", .integer_value = fake_attack_max,
        .source_kind = "scenario",
    });
    command.arguments.push_back({
        .node_key = "battle_1", .argument_key = "continuation_mode",
        .value_type = "choice",
        .text_value = std::string("automatic_best_per_ending_rng"),
        .source_kind = "scenario",
    });
    return execution_db->CreateWorkflowInstance(
        command, workflow_instance_id_out, error_out);
}

bool SeedPreparedBattleWorkflow(
    savor::db::IAuthoringDb* authoring_db,
    savor::db::IExecutionDb* execution_db,
    const std::int64_t entry_savestate_id,
    const std::int64_t seed_probe_spec_id,
    const std::int64_t battle_plan_id,
    const int samples_per_axis,
    const int fake_attack_min,
    const int fake_attack_max,
    const std::string_view run_identity,
    std::int64_t* workflow_instance_id_out,
    std::string* error_out)
{
    if (authoring_db == nullptr || execution_db == nullptr ||
        entry_savestate_id <= 0 || seed_probe_spec_id <= 0 ||
        battle_plan_id <= 0 || workflow_instance_id_out == nullptr)
        return Fail("Prepared Battle workflow seed input is incomplete",
                    error_out);

    const auto now = savor::db::types::UtcNow();
    savor::db::SaveWorkflowGraphResult saved{};
    if (!savor::db::authoring::MaterializeWorkflowGraph(authoring_db, {
            .name = "SavorE2E prepared Battle phases workflow "
                + std::string(run_identity),
            .description = "Run Battle Context and SeedProbe in parallel from a qualified prepared checkpoint before battle.start",
            .graph_version = 1,
            .graph_hash = "savor-e2e.battle-phases.prepared-v1."
                + std::string(run_identity),
            .nodes = {
                {
                    .node_key = "probe_1",
                    .unit_kind = "seed_probe",
                    .display_name = "SeedProbe",
                    .authored_ref_kind = std::string("seed_probe_spec"),
                    .authored_ref_id = seed_probe_spec_id,
                    .inputs = {{
                        .input_key = "entry_savestate",
                        .data_kind = "state.movie_inactive_savestate_id",
                        .ref_kind = "state.savestate",
                        .display_name = "Prepared entry savestate",
                    }},
                    .possible_outputs = {{
                        .output_key = "seed_probe_run",
                        .data_kind = "analysis.seed_probe_run",
                        .ref_kind = "sp_probe_run",
                        .display_name = "Confirmed SeedProbe run",
                    }},
                    .arguments = {{
                        .argument_key = "samples_per_axis",
                        .display_name = "Samples per axis",
                        .value_type = "integer",
                        .default_value = std::string("5"),
                        .minimum_integer = 1,
                        .maximum_integer = 64,
                    }},
                },
                {
                    .node_key = "context_1",
                    .unit_kind = "battle.context",
                    .display_name = "Battle Context",
                    .inputs = {{
                        .input_key = "entry_savestate",
                        .data_kind = "state.movie_inactive_savestate_id",
                        .ref_kind = "state.savestate",
                        .display_name = "Prepared entry savestate",
                    }},
                    .possible_outputs = {{
                        .output_key = "battle_context",
                        .data_kind = "analysis_battle.battle_context_id",
                        .ref_kind = "ab_battle_context",
                        .display_name = "Battle context",
                    }},
                },
                {
                    .node_key = "battle_1",
                    .unit_kind = "battle",
                    .display_name = "Battle",
                    .authored_ref_kind = std::string(
                        "authoring.battle_plan"),
                    .authored_ref_id = battle_plan_id,
                    .inputs = {
                        {
                            .input_key = "seed_probe_run",
                            .data_kind = "analysis.seed_probe_run",
                            .ref_kind = "sp_probe_run",
                            .display_name = "Confirmed SeedProbe run",
                        },
                        {
                            .input_key = "battle_context",
                            .data_kind = "analysis_battle.battle_context_id",
                            .ref_kind = "ab_battle_context",
                            .display_name = "Battle context",
                        },
                    },
                    .possible_outputs = {{
                        .output_key = "battle_set",
                        .data_kind = "analysis_battle.battle_set",
                        .ref_kind = "analysis_battle.battle_set",
                        .display_name = "Battle set",
                    }},
                    .arguments = {
                        {
                            .argument_key = "continuation_mode",
                            .display_name = "Continuation",
                            .value_type = "choice",
                            .required = true,
                            .choices = {
                                {"manual_selection", "Manual selection after each wave"},
                                {"automatic_best_per_ending_rng", "Automatically continue the best candidate for each ending RNG"},
                            },
                        },
                        {
                            .argument_key = "continue_automatic_exploration_after_victory",
                            .display_name = "Continue automatic exploration after Victory",
                            .value_type = "boolean",
                            .default_value = std::string("false"),
                        },
                        {
                            .argument_key = "fake_attack_min",
                            .display_name = "Minimum fake attacks",
                            .value_type = "integer",
                            .default_value = std::string("0"),
                            .minimum_integer = 0,
                            .maximum_integer = 2147483647,
                        },
                        {
                            .argument_key = "fake_attack_max",
                            .display_name = "Maximum fake attacks",
                            .value_type = "integer",
                            .default_value = std::string("0"),
                            .minimum_integer = 0,
                            .maximum_integer = 2147483647,
                        },
                    },
                    .argument_constraints = {{
                        .lesser_or_equal_key = "fake_attack_min",
                        .greater_or_equal_key = "fake_attack_max",
                        .message = "minimum fake attacks must not exceed maximum fake attacks",
                    }},
                },
            },
            .edges = {
                {
                    .from_node_key = "probe_1",
                    .output_key = "seed_probe_run",
                    .to_node_key = "battle_1",
                    .input_key = "seed_probe_run",
                    .guard_kind = std::string(
                        savor::db::kWorkflowOutputPresentGuard),
                },
                {
                    .from_node_key = "context_1",
                    .output_key = "battle_context",
                    .to_node_key = "battle_1",
                    .input_key = "battle_context",
                    .guard_kind = std::string(
                        savor::db::kWorkflowOutputPresentGuard),
                },
            },
            .created_at_utc = now,
            .correlation_id = std::string(kCorrelation),
            .causation_id = "prepared-scenario",
        }, &saved, error_out))
        return false;

    const auto unit_registry =
        savor::db::execution::workflow::BuildDefaultWorkflowUnitRegistry();
    std::string activation_error;
    auto probe = savor::db::execution::workflow::
        BuildUnitActivationSpecFromDefinition(
            unit_registry, "probe_1", "probe_1", "seed_probe",
            "SeedProbe", std::optional<std::string>(
                "seed_probe_spec"), seed_probe_spec_id, {},
            &activation_error);
    auto context = savor::db::execution::workflow::
        BuildUnitActivationSpecFromDefinition(
            unit_registry, "context_1", "context_1", "battle.context",
            "Battle Context", std::nullopt, std::nullopt, {},
            &activation_error);
    auto battle = savor::db::execution::workflow::
        BuildUnitActivationSpecFromDefinition(
            unit_registry, "battle_1", "battle_1", "battle",
            "Battle", std::optional<std::string>(
                "authoring.battle_plan"), battle_plan_id,
            {"probe_1", "context_1"}, &activation_error);
    if (!probe || !context || !battle)
        return Fail("Prepared Battle workflow activation failed: "
                        + activation_error,
                    error_out);

    savor::db::execution::workflow::WorkflowCreateInstanceCommand command{};
    command.workflow_kind = "workflow_graph";
    command.root_scope_kind = "manual";
    command.workflow_graph_revision_id = saved.workflow_graph_revision_id;
    command.created_by = "savor-e2e";
    command.created_at_utc = now.time_since_epoch().count();
    command.unit_activations = {
        std::move(*probe), std::move(*context), std::move(*battle)};
    for (const auto node_key : {"probe_1", "context_1"}) {
        command.input_bindings.push_back({
            .node_key = node_key,
            .input_key = "entry_savestate",
            .data_kind = "state.movie_inactive_savestate_id",
            .ref_kind = "state.savestate",
            .ref_id = entry_savestate_id,
            .source_kind = "external",
        });
    }
    command.arguments.push_back({
        .node_key = "probe_1",
        .argument_key = "samples_per_axis",
        .value_type = "integer",
        .integer_value = samples_per_axis,
        .source_kind = "scenario",
    });
    command.arguments.push_back({
        .node_key = "battle_1",
        .argument_key = "fake_attack_min",
        .value_type = "integer",
        .integer_value = fake_attack_min,
        .source_kind = "scenario",
    });
    command.arguments.push_back({
        .node_key = "battle_1",
        .argument_key = "fake_attack_max",
        .value_type = "integer",
        .integer_value = fake_attack_max,
        .source_kind = "scenario",
    });
    command.arguments.push_back({
        .node_key = "battle_1",
        .argument_key = "continuation_mode",
        .value_type = "choice",
        .text_value = std::string("automatic_best_per_ending_rng"),
        .source_kind = "scenario",
    });
    return execution_db->CreateWorkflowInstance(
        command, workflow_instance_id_out, error_out);
}

bool CheckBattleInvariantsAndReportTrajectory(
    savor::db::core::DBService* db_service,
    const savor::db::execution::workflow::WorkflowGraphSnapshot& graph,
    const ResolvedE2eScenarioEntry& entry,
    const CliOptions& options,
    const std::function<void(const std::string&)>& report,
    std::string* error_out)
{
    if (!graph.instance.workflow_graph_revision_id)
        return Fail("Battle workflow has no authored graph revision",
                    error_out);
    if (graph.instance.root_scope_kind != "manual"
        || graph.instance.root_scope_id.has_value()) {
        return Fail("Battle workflow root scope is not the canonical manual scope",
                    error_out);
    }
    const auto authored = db_service->AuthoringDb()->GetWorkflowGraphRevision(
        *graph.instance.workflow_graph_revision_id);
    if (!authored)
        return Fail("Battle authored graph revision is unavailable",
                    error_out);

    struct ExpectedNode {
        std::string_view key;
        std::string_view kind;
    };
    struct ExpectedEdge {
        std::string_view from;
        std::string_view output;
        std::string_view to;
        std::string_view input;
    };
    static constexpr std::array kPreparedNodes{
        ExpectedNode{"probe_1", "seed_probe"},
        ExpectedNode{"context_1", "battle.context"},
        ExpectedNode{"battle_1", "battle"},
    };
    static constexpr std::array kFreshNodes{
        ExpectedNode{"tas_establish_1", "tas_movie_establish_root_cursor"},
        ExpectedNode{"tas_validate_1", "tas_movie_validate_root"},
        ExpectedNode{"tas_sterilize_1", "tas_movie_checkpoint_sterilize"},
        ExpectedNode{"probe_1", "seed_probe"},
        ExpectedNode{"context_1", "battle.context"},
        ExpectedNode{"battle_1", "battle"},
    };
    static constexpr std::array kDelayedCutsceneNodes{
        ExpectedNode{"tas_establish_1", "tas_movie_establish_root_cursor"},
        ExpectedNode{"tas_annotate_1", "tas_movie_annotate"},
        ExpectedNode{"tas_revise_1", "tas_movie_revise"},
        ExpectedNode{"tas_validate_1", "tas_movie_validate_root"},
        ExpectedNode{"tas_sterilize_1", "tas_movie_checkpoint_sterilize"},
        ExpectedNode{"probe_1", "seed_probe"},
        ExpectedNode{"context_1", "battle.context"},
        ExpectedNode{"battle_1", "battle"},
    };
    static constexpr std::array kEstablishedNodes{
        ExpectedNode{"tas_validate_1", "tas_movie_validate_root"},
        ExpectedNode{"tas_sterilize_1", "tas_movie_checkpoint_sterilize"},
        ExpectedNode{"probe_1", "seed_probe"},
        ExpectedNode{"context_1", "battle.context"},
        ExpectedNode{"battle_1", "battle"},
    };
    static constexpr std::array kPreparedEdges{
        ExpectedEdge{"probe_1", "seed_probe_run", "battle_1",
                     "seed_probe_run"},
        ExpectedEdge{"context_1", "battle_context", "battle_1",
                     "battle_context"},
    };
    static constexpr std::array kFreshEdges{
        ExpectedEdge{"tas_establish_1", "root_establishment",
                     "tas_validate_1", "root_establishment"},
        ExpectedEdge{"tas_validate_1", "validated_checkpoint_savestate",
                     "tas_sterilize_1", "paired_checkpoint_savestate"},
        ExpectedEdge{"tas_sterilize_1", "sterilized_checkpoint_savestate",
                     "probe_1", "entry_savestate"},
        ExpectedEdge{"tas_sterilize_1", "sterilized_checkpoint_savestate",
                     "context_1", "entry_savestate"},
        ExpectedEdge{"probe_1", "seed_probe_run", "battle_1",
                     "seed_probe_run"},
        ExpectedEdge{"context_1", "battle_context", "battle_1",
                     "battle_context"},
    };
    static constexpr std::array kDelayedCutsceneEdges{
        ExpectedEdge{"tas_establish_1", "root_dtm",
                     "tas_annotate_1", "root_dtm"},
        ExpectedEdge{"tas_establish_1", "root_establishment",
                     "tas_revise_1", "root_establishment"},
        ExpectedEdge{"tas_annotate_1", "annotation_attempt",
                     "tas_revise_1", "annotation_attempt"},
        ExpectedEdge{"tas_revise_1", "root_establishment",
                     "tas_validate_1", "root_establishment"},
        ExpectedEdge{"tas_validate_1", "validated_checkpoint_savestate",
                     "tas_sterilize_1", "paired_checkpoint_savestate"},
        ExpectedEdge{"tas_sterilize_1", "sterilized_checkpoint_savestate",
                     "probe_1", "entry_savestate"},
        ExpectedEdge{"tas_sterilize_1", "sterilized_checkpoint_savestate",
                     "context_1", "entry_savestate"},
        ExpectedEdge{"probe_1", "seed_probe_run", "battle_1",
                     "seed_probe_run"},
        ExpectedEdge{"context_1", "battle_context", "battle_1",
                     "battle_context"},
    };
    static constexpr std::array kEstablishedEdges{
        ExpectedEdge{"tas_validate_1", "validated_checkpoint_savestate",
                     "tas_sterilize_1", "paired_checkpoint_savestate"},
        ExpectedEdge{"tas_sterilize_1", "sterilized_checkpoint_savestate",
                     "probe_1", "entry_savestate"},
        ExpectedEdge{"tas_sterilize_1", "sterilized_checkpoint_savestate",
                     "context_1", "entry_savestate"},
        ExpectedEdge{"probe_1", "seed_probe_run", "battle_1",
                     "seed_probe_run"},
        ExpectedEdge{"context_1", "battle_context", "battle_1",
                     "battle_context"},
    };
    const bool prepared = entry.source
        == E2eScenarioEntrySource::PreparedSterilizedCheckpoint;
    const bool established = entry.source
        == E2eScenarioEntrySource::TasMovieEstablishmentAttempt;
    const bool delayed_cutscene = !prepared && !established
        && options.scenario == "tasmovie_cutscene" && options.cutscene_delay;
    const auto expected_node_count = prepared ? kPreparedNodes.size()
        : established ? kEstablishedNodes.size()
        : delayed_cutscene ? kDelayedCutsceneNodes.size()
                           : kFreshNodes.size();
    const auto expected_edge_count = prepared ? kPreparedEdges.size()
        : established ? kEstablishedEdges.size()
        : delayed_cutscene ? kDelayedCutsceneEdges.size()
                           : kFreshEdges.size();
    if (authored->nodes.size() != expected_node_count
        || authored->edges.size() != expected_edge_count) {
        return Fail("Battle authored graph node or edge count drifted",
                    error_out);
    }
    const auto check_nodes = [&](const auto& expected_nodes) {
        return std::ranges::all_of(expected_nodes, [&](const auto& expected) {
            return std::ranges::count_if(
                authored->nodes, [&](const auto& node) {
                    return node.node_key == expected.key
                        && node.unit_kind == expected.kind;
                }) == 1;
        });
    };
    const auto check_edges = [&](const auto& expected_edges) {
        return std::ranges::all_of(expected_edges, [&](const auto& expected) {
            return std::ranges::count_if(
                authored->edges, [&](const auto& edge) {
                    return edge.from_node_key == expected.from
                        && edge.output_key == expected.output
                        && edge.to_node_key == expected.to
                        && edge.input_key == expected.input
                        && edge.guard_kind
                            == std::optional<std::string>(
                                savor::db::kWorkflowOutputPresentGuard)
                        && !edge.guard_value;
                }) == 1;
        });
    };
    if (prepared ? !check_nodes(kPreparedNodes)
                 : established ? !check_nodes(kEstablishedNodes)
                 : delayed_cutscene ? !check_nodes(kDelayedCutsceneNodes)
                               : !check_nodes(kFreshNodes)) {
        return Fail("Battle authored graph node identities drifted", error_out);
    }
    if (prepared ? !check_edges(kPreparedEdges)
                 : established ? !check_edges(kEstablishedEdges)
                 : delayed_cutscene ? !check_edges(kDelayedCutsceneEdges)
                               : !check_edges(kFreshEdges)) {
        return Fail("Battle authored graph guarded edges drifted", error_out);
    }

    const auto authored_probe = std::ranges::find(
        authored->nodes, std::string("probe_1"),
        &savor::db::WorkflowGraphNodeSnapshot::node_key);
    const auto authored_battle = std::ranges::find(
        authored->nodes, std::string("battle_1"),
        &savor::db::WorkflowGraphNodeSnapshot::node_key);
    if (authored_probe == authored->nodes.end()
        || authored_probe->authored_ref_kind
            != std::optional<std::string>("seed_probe_spec")
        || !authored_probe->authored_ref_id
        || authored_battle == authored->nodes.end()
        || authored_battle->authored_ref_kind
            != std::optional<std::string>("authoring.battle_plan")
        || !authored_battle->authored_ref_id) {
        return Fail("Battle authored graph references are incomplete",
                    error_out);
    }
    const auto seed_spec = db_service->AuthoringDb()->GetSeedProbeSpec(
        *authored_probe->authored_ref_id);
    if (!seed_spec
        || seed_spec->min_value != options.seedprobe_min_value.value_or(48)
        || seed_spec->max_value != options.seedprobe_max_value.value_or(207)
        || seed_spec->combo_attempts_per_target
            != options.seedprobe_combo_attempts_per_target.value_or(20)
        || seed_spec->combo_sampler_tries
            != options.seedprobe_combo_sampler_tries.value_or(4)) {
        return Fail("Battle SeedProbe search settings were not persisted exactly",
                    error_out);
    }
    {
        std::ostringstream line;
        line << "[battle-search] seedprobe_min=" << seed_spec->min_value
             << " seedprobe_max=" << seed_spec->max_value
             << " samples_per_axis="
             << options.seedprobe_samples_per_axis.value_or(1)
             << " combo_attempts="
             << seed_spec->combo_attempts_per_target
             << " combo_sampler_tries="
             << seed_spec->combo_sampler_tries
             << " fake_attack_min="
             << options.battle_fake_attack_min.value_or(0)
             << " fake_attack_max="
             << options.battle_fake_attack_max.value_or(0)
             << " predicate_item_id=" << kPredicateItemId;
        report(line.str());
    }

    const auto plan = db_service->AuthoringDb()->GetBattlePlan(
        *authored_battle->authored_ref_id);
    if (!plan || plan->turns.size() != 2) {
        return Fail("Battle generic two-turn authoring is unavailable",
                    error_out);
    }
    for (const auto& turn : plan->turns) {
        const auto actor_zero = std::ranges::find(
            turn.actions, 0, &savor::db::BattlePlanActionSnapshot::actor_slot);
        const auto actor_one = std::ranges::find(
            turn.actions, 1, &savor::db::BattlePlanActionSnapshot::actor_slot);
        if ((turn.turn_index != 1 && turn.turn_index != 2)
            || turn.actions.size() != 2
            || actor_zero == turn.actions.end()
            || actor_one == turn.actions.end()
            || actor_zero->action_preset.target_kind
                != savor::db::BattlePlanTargetKind::AnyEnemy
            || actor_one->action_preset.target_kind
                != savor::db::BattlePlanTargetKind::SameAsOtherPC
            || actor_one->action_preset.target_same_as_actor_slot
                != std::optional<int>(0)
            || actor_zero->action_preset.macro
                != soa::battle::actions::BattleAction::Attack
            || actor_one->action_preset.macro
                != soa::battle::actions::BattleAction::Attack
            || !turn.default_predicate_group_revision_id) {
            return Fail("Battle generic authored turn contract drifted",
                        error_out);
        }
        for (const auto& action : turn.actions) {
            std::ostringstream line;
            line << "[battle-authored-command] turn=" << turn.turn_index
                 << " ordinal=" << action.ordinal
                 << " actor=" << action.actor_slot
                 << " action="
                 << soa::battle::actions::get_action_string(
                        action.action_preset.macro)
                 << " target_kind="
                 << BattleTargetKindName(action.action_preset.target_kind)
                 << " target_single="
                 << (action.action_preset.target_single_slot
                         ? std::to_string(
                               *action.action_preset.target_single_slot)
                         : "none")
                 << " target_mask="
                 << (action.action_preset.target_mask_bits
                         ? std::to_string(
                               *action.action_preset.target_mask_bits)
                         : "none")
                 << " target_same_as_actor="
                 << (action.action_preset.target_same_as_actor_slot
                         ? std::to_string(
                               *action.action_preset.target_same_as_actor_slot)
                         : "none")
                 << " predicate_group="
                 << *turn.default_predicate_group_revision_id;
            report(line.str());
        }
    }

    const auto find_step_by_node = [&](std::string_view key) {
        return std::ranges::find(
            graph.steps, key,
            &savor::db::execution::workflow::WorkflowStepRecord::
                graph_node_key);
    };
    const auto check_runtime_nodes = [&](const auto& expected_nodes) {
        return std::ranges::all_of(expected_nodes, [&](const auto& expected) {
            const auto step = find_step_by_node(expected.key);
            return step != graph.steps.end()
                && std::ranges::count_if(
                    graph.unit_activations, [&](const auto& activation) {
                        return activation.graph_node_key == expected.key
                            && activation.unit_kind == expected.kind;
                    }) == 1;
        });
    };
    const auto check_runtime_edges = [&](const auto& expected_edges) {
        return std::ranges::all_of(expected_edges, [&](const auto& expected) {
            const auto from = find_step_by_node(expected.from);
            const auto to = find_step_by_node(expected.to);
            return from != graph.steps.end() && to != graph.steps.end()
                && std::ranges::any_of(graph.edges, [&](const auto& edge) {
                    return edge.from_step_id == from->workflow_step_id
                        && edge.to_step_id == to->workflow_step_id;
                });
        });
    };
    if ((prepared ? !check_runtime_nodes(kPreparedNodes)
                  : established ? !check_runtime_nodes(kEstablishedNodes)
                  : delayed_cutscene
                        ? !check_runtime_nodes(kDelayedCutsceneNodes)
                                : !check_runtime_nodes(kFreshNodes))
        || (prepared ? !check_runtime_edges(kPreparedEdges)
                     : established ? !check_runtime_edges(kEstablishedEdges)
                     : delayed_cutscene
                           ? !check_runtime_edges(kDelayedCutsceneEdges)
                                   : !check_runtime_edges(kFreshEdges))) {
        return Fail("Battle realized static graph topology drifted", error_out);
    }

    const auto has_binding = [&](std::string_view node,
                                 std::string_view input,
                                 std::string_view data,
                                 std::int64_t ref_id) {
        return std::ranges::count_if(
            graph.input_bindings, [&](const auto& binding) {
                return binding.node_key == node
                    && binding.input_key == input
                    && binding.data_kind == data
                    && binding.ref_kind
                        == (data == "state_artifact.dtm_artifact_id"
                                ? "state_artifact" : "state.savestate")
                    && binding.ref_id == ref_id
                    && binding.source_kind == "external";
            }) == 1;
    };
    const auto external_binding_count = std::ranges::count_if(
        graph.input_bindings, [](const auto& binding) {
            return binding.source_kind == "external";
        });
    if (prepared) {
        if (!entry.savestate_id
            || external_binding_count != 2
            || !has_binding("probe_1", "entry_savestate",
                            "state.movie_inactive_savestate_id",
                            *entry.savestate_id)
            || !has_binding("context_1", "entry_savestate",
                            "state.movie_inactive_savestate_id",
                            *entry.savestate_id)) {
            return Fail("Prepared Battle external entry bindings drifted",
                        error_out);
        }
    } else if (established) {
        if (!entry.tas_movie_establishment_attempt_id) {
            return Fail("Established-root Battle entry is missing its establishment attempt",
                        error_out);
        }
        const auto root_establishment_binding_count = std::ranges::count_if(
            graph.input_bindings, [&](const auto& binding) {
                return binding.node_key == "tas_validate_1"
                    && binding.input_key == "root_establishment"
                    && binding.data_kind
                        == "analysis.tas_movie_root_establishment_attempt_id"
                    && binding.ref_kind == "tmv_root_establishment_attempt"
                    && binding.ref_id
                        == *entry.tas_movie_establishment_attempt_id
                    && binding.source_kind == "external";
            });
        if (external_binding_count != 1
            || root_establishment_binding_count != 1) {
            return Fail("Established-root Battle external entry binding drifted",
                        error_out);
        }
    } else {
        const auto root_dtm_binding_count = [&](std::string_view node_key) {
            return std::ranges::count_if(
                graph.input_bindings, [&](const auto& binding) {
                return binding.node_key == node_key
                    && binding.input_key == "root_dtm"
                    && binding.data_kind ==
                        "state_artifact.dtm_artifact_id"
                    && binding.ref_kind == "state_artifact"
                    && binding.ref_id > 0
                    && binding.source_kind == "external";
            });
        };
        if (external_binding_count != 1
            || root_dtm_binding_count("tas_establish_1") != 1
            || root_dtm_binding_count("tas_annotate_1") != 0) {
            return Fail("Fresh Battle DTM entry binding drifted", error_out);
        }
    }
    const auto has_integer_argument = [&](std::string_view node,
                                          std::string_view key,
                                          const auto& accept) {
        return std::ranges::count_if(
            graph.arguments, [&](const auto& argument) {
                return argument.node_key == node
                    && argument.argument_key == key
                    && argument.value_type == "integer"
                    && argument.integer_value
                    && accept(*argument.integer_value)
                    && !argument.text_value
                    && argument.source_kind == "scenario";
            }) == 1;
    };
    const auto has_choice_argument = [&](std::string_view node,
                                         std::string_view key,
                                         std::string_view token) {
        return std::ranges::count_if(
            graph.arguments, [&](const auto& argument) {
                return argument.node_key == node
                    && argument.argument_key == key
                    && argument.value_type == "choice"
                    && argument.text_value == token
                    && !argument.integer_value
                    && argument.source_kind == "scenario";
            }) == 1;
    };
    const auto expected_argument_count = prepared ? 5u
        : delayed_cutscene ? 8u : 6u;
    if (graph.arguments.size() != expected_argument_count
        || !has_integer_argument(
            "probe_1", "samples_per_axis",
            [&](std::int64_t value) {
                return value == options.seedprobe_samples_per_axis.value_or(1);
            })
        || !has_integer_argument(
            "battle_1", "fake_attack_min",
            [&](std::int64_t value) {
                return value == options.battle_fake_attack_min.value_or(0);
            })
        || !has_integer_argument(
            "battle_1", "fake_attack_max",
            [&](std::int64_t value) {
                return value == options.battle_fake_attack_max.value_or(0);
            })
        || !has_choice_argument(
            "battle_1", "continuation_mode",
            "automatic_best_per_ending_rng")
        || (delayed_cutscene
            && (!has_integer_argument(
                    "tas_revise_1", "neutral_epoch_count",
                    [](std::int64_t value) { return value == 1; })
                || !has_choice_argument(
                    "tas_revise_1", "placement_profile",
                    "first_battle.final_dialog")))
        || !std::ranges::any_of(graph.arguments, [](const auto& argument) {
            return argument.node_key == "battle_1"
                && argument.argument_key
                    == "continue_automatic_exploration_after_victory"
                && argument.value_type == "boolean"
                && argument.integer_value == std::optional<std::int64_t>(0);
        })
        || (!prepared && !has_integer_argument(
            "tas_validate_1", "rtc",
            [](std::int64_t value) {
                return value >= 0
                    && static_cast<std::uint64_t>(value)
                        <= std::numeric_limits<std::uint32_t>::max();
            }))) {
        return Fail("Battle scenario argument bindings drifted", error_out);
    }

    const auto find_step = [&](std::string_view kind) {
        return std::ranges::find(
            graph.steps, kind,
            &savor::db::execution::workflow::WorkflowStepRecord::step_kind);
    };
    const auto establish_step = find_step("tasmovie.establish_root_cursor");
    const auto annotate_step = find_step("tasmovie.annotate");
    const auto revise_step = find_step("tasmovie.revise");
    const auto validate_step = find_step("tasmovie.validate_root");
    const auto sterilize_step = find_step("tasmovie.checkpoint_sterilize");
    const auto seedprobe_step = find_step("seedprobe.survey");
    const auto context_step = find_step("battle.context");
    const auto start_step = find_step("battle.start");
    const auto outputs = db_service->ExecutionDb()->WorkflowQueryService()->
        ListStepOutputs(graph.instance.workflow_instance_id);
    const auto find_output = [&](std::string_view node_key,
                                 std::string_view output_key,
                                 std::string_view data_kind,
                                 std::string_view ref_kind) {
        return std::ranges::find_if(outputs, [&](const auto& output) {
            return output.graph_node_key == node_key &&
                output.output_key == output_key &&
                output.data_kind == data_kind &&
                output.ref_kind == ref_kind && output.ref_id > 0;
        });
    };
    std::int64_t effective_entry_savestate_id = 0;
    if (entry.source == E2eScenarioEntrySource::FreshTasMovieValidation
        || established) {
        if ((!established && establish_step == graph.steps.end())
            || (established && establish_step != graph.steps.end())
            || (delayed_cutscene
                && (annotate_step == graph.steps.end()
                    || revise_step == graph.steps.end()))
            || (!delayed_cutscene
                && (annotate_step != graph.steps.end()
                    || revise_step != graph.steps.end()))
            || validate_step == graph.steps.end()
            || sterilize_step == graph.steps.end()) {
            return Fail("Battle workflow is missing its validation and sterilization topology",
                        error_out);
        }
        if (sterilize_step->state == WorkflowStepState::Skipped) {
            if (seedprobe_step == graph.steps.end()
                || context_step == graph.steps.end()
                || start_step == graph.steps.end()
                || seedprobe_step->state != WorkflowStepState::Skipped
                || context_step->state != WorkflowStepState::Skipped
                || start_step->state != WorkflowStepState::Skipped) {
                return Fail("Battle TAS output guard did not skip every downstream Battle unit",
                            error_out);
            }
            report("[battle-trajectory] battle_execution=NOT_ACTIVATED reason="
                   + sterilize_step->blocked_reason.value_or(
                       "guarded_tas_preparation_did_not_produce_an_entry"));
            return true;
        }
        if ((!established
                && establish_step->state != WorkflowStepState::Completed)
            || (delayed_cutscene
                && (annotate_step->state != WorkflowStepState::Completed
                    || revise_step->state != WorkflowStepState::Completed))
            || validate_step->state != WorkflowStepState::Completed
            || sterilize_step->state != WorkflowStepState::Completed) {
            return Fail("Battle validation and sterilization topology is not terminal-consistent",
                        error_out);
        }
        const auto validated_checkpoint = find_output(
            "tas_validate_1", "validated_checkpoint_savestate",
            "state.movie_paired_savestate_id", "state.savestate");
        const auto sterilized_checkpoint = find_output(
            "tas_sterilize_1", "sterilized_checkpoint_savestate",
            "state.movie_inactive_savestate_id", "state.savestate");
        if (delayed_cutscene) {
            const auto revised_root = find_output(
                "tas_revise_1", "root_establishment",
                "analysis.tas_movie_root_establishment_attempt_id",
                "tmv_root_establishment_attempt");
            const auto revised_annotation = find_output(
                "tas_revise_1", "annotation_attempt",
                "analysis.tas_movie_input_epoch_annotation_attempt_id",
                "tmv_input_epoch_annotation_attempt");
            if (revised_root == outputs.end()
                || revised_annotation == outputs.end()
                || validate_step->input_ref_id != revised_root->ref_id) {
                return Fail("Cutscene delay did not publish and validate the revised TAS authorities", error_out);
            }
        }
        if (validated_checkpoint == outputs.end() ||
            sterilized_checkpoint == outputs.end())
            return Fail("Battle workflow checkpoint provenance is missing the paired-to-inactive sterilization boundary",
                        error_out);
        effective_entry_savestate_id = sterilized_checkpoint->ref_id;
        savor::db::execution::programdb::tasmovieevidence::
            PreparedSterilizedCheckpointEvidence qualified{};
        std::string qualification_error;
        if (!savor::db::execution::programdb::tasmovieevidence::
                ResolvePreparedSterilizedCheckpointEvidence(
                    db_service->StateDb(), db_service->AnalysisDb(),
                    effective_entry_savestate_id, &qualified,
                    &qualification_error)) {
            return Fail("validation-backed Battle checkpoint qualification failed: "
                            + qualification_error,
                        error_out);
        }
    } else if (entry.source
        == E2eScenarioEntrySource::PreparedSterilizedCheckpoint) {
        if (!entry.savestate_id || establish_step != graph.steps.end()
            || validate_step != graph.steps.end()
            || sterilize_step != graph.steps.end()) {
            return Fail("prepared Battle workflow unexpectedly contains TAS preparation steps",
                        error_out);
        }
        effective_entry_savestate_id = *entry.savestate_id;
    } else {
        return Fail("Battle workflow received an unsupported entry source",
                    error_out);
    }

    if (seedprobe_step == graph.steps.end()
        || seedprobe_step->state != WorkflowStepState::Completed
        || !seedprobe_step->input_ref_id) {
        return Fail("Battle SeedProbe workflow step did not publish its run",
                    error_out);
    }
    const auto seedprobe_entry = db_service->AnalysisDb()
        ->LookupSeedProbeRunSavestateId(*seedprobe_step->input_ref_id);
    if (!seedprobe_entry
        || *seedprobe_entry != effective_entry_savestate_id) {
        return Fail("Battle SeedProbe did not use the resolved entry savestate",
                    error_out);
    }

    if (context_step == graph.steps.end() ||
        context_step->state != WorkflowStepState::Completed ||
        context_step->output_ref_kind !=
            std::optional<std::string>("ab_battle_context") ||
        !context_step->output_ref_id)
        return Fail("Battle Context workflow step did not publish its result",
                    error_out);
    if (start_step == graph.steps.end() ||
        start_step->state != WorkflowStepState::Completed ||
        start_step->input_ref_kind !=
            std::optional<std::string>("analysis_battle.battle_set") ||
        !start_step->input_ref_id)
        return Fail("battle.start did not join SeedProbe and Battle Context",
                    error_out);

    const auto context = db_service->AnalysisDb()->GetBattleContextProbe(
        *context_step->output_ref_id);
    if (!context || context->probe_status !=
            savor::db::BattleContextProbeStatus::Succeeded ||
        !context->context_artifact_id || context->context_blob ||
        context->source_savestate_id != effective_entry_savestate_id ||
        context->entry_pc != 0x80101E48u ||
        context->capture_pc != 0x80071740u)
        return Fail("Battle Context durable provenance is incomplete", error_out);
    const auto artifact = db_service->StateDb()->GetArtifact(
        *context->context_artifact_id);
    if (!artifact || artifact->artifact_kind != "BATTLE_CONTEXT" ||
        artifact->file_ext != ".bctx" ||
        !std::filesystem::is_regular_file(artifact->filename)
        || static_cast<std::int64_t>(std::filesystem::file_size(
               artifact->filename)) != artifact->size_bytes
        || hash::sha256_of_file(artifact->filename) != artifact->sha256)
        return Fail("Battle Context .bctx artifact is unavailable", error_out);

    const auto battle_set = db_service->AnalysisDb()->GetBattleSet(
        *start_step->input_ref_id);
    if (!battle_set)
        return Fail("battle.start BattleSet is unavailable", error_out);
    if (battle_set->battle_plan_id != plan->plan_id
        || battle_set->battle_plan_fingerprint != plan->fingerprint
        || battle_set->continuation_mode !=
            savor::db::BattleContinuationMode::AutomaticBestPerEndingRng
        || battle_set->continue_automatic_exploration_after_victory
        || battle_set->launch_fake_attack_min !=
            options.battle_fake_attack_min.value_or(0)
        || battle_set->launch_fake_attack_max !=
            options.battle_fake_attack_max.value_or(0)) {
        return Fail("BattleSet did not freeze the exact direct plan and launch policy",
                    error_out);
    }
    const auto final_output = find_output(
        "battle_1", "battle_set", "analysis_battle.battle_set",
        "analysis_battle.battle_set");

    const auto waves = db_service->AnalysisDb()->ListBattleTurnWaves(
        battle_set->battle_set_id);

    {
        std::ostringstream line;
        line << "[battle-trajectory] battle_set="
             << battle_set->battle_set_id << " status="
             << savor::db::ToDbString(battle_set->status)
             << " waves=" << waves.size();
        report(line.str());
    }
    for (const auto& candidate :
         db_service->AnalysisDb()->ListBattleSeedCandidates(
             battle_set->battle_set_id)) {
        std::ostringstream line;
        line << "[battle-candidate] battle_set="
             << battle_set->battle_set_id
             << " candidate=" << candidate.seed_candidate_id
             << " status="
             << savor::db::ToDbString(candidate.candidate_status)
             << " source=" << savor::db::ToDbString(candidate.source_kind)
             << " seed=" << candidate.seed_value
             << " probe_result="
             << (candidate.source_probe_result_id
                     ? std::to_string(*candidate.source_probe_result_id)
                     : "none")
             << " input_frame="
             << (candidate.source_input_frame_id
                     ? std::to_string(*candidate.source_input_frame_id)
                     : "none");
        report(line.str());
    }
    for (const auto& wave : waves) {
        if (wave.wave_id <= 0 || wave.battle_set_id != battle_set->battle_set_id
            || wave.turn_index <= 0 || wave.seed_candidate_id <= 0) {
            return Fail("Battle wave identity or lineage is malformed",
                        error_out);
        }
        const auto matching_steps = std::ranges::count_if(
            graph.steps, [&](const auto& step) {
                return step.step_kind == "battle.single_turn"
                    && step.input_ref_kind
                        == std::optional<std::string>(
                            "analysis_battle.turn_wave")
                    && step.input_ref_id
                        == std::optional<std::int64_t>(wave.wave_id);
            });
        if (matching_steps > 1)
            return Fail("Battle wave has more than one structurally linked dynamic step",
                        error_out);

        const auto jobs = db_service->AnalysisDb()
            ->ListBattleTurnJobsForWave(wave.wave_id);
        if (matching_steps == 0) {
            if (!jobs.empty())
                return Fail("Battle wave has jobs without a dynamic execution step",
                            error_out);
            std::ostringstream line;
            line << "[battle-wave] wave=" << wave.wave_id
                 << " turn=" << wave.turn_index
                 << " status=" << savor::db::ToDbString(wave.status)
                 << " dynamic_step=NOT_ACTIVATED jobs=0"
                 << " seed_candidate=" << wave.seed_candidate_id;
            report(line.str());
            continue;
        }

        const auto binding = db_service->AnalysisDb()
            ->GetBattlePredicateExecutionPackageForWave(wave.wave_id);
        if (!binding || !binding->predicate_group_revision_id
            || binding->predicate_group_sha256.size() != 64
            || binding->execution_package_sha256.size() != 64
            || binding->execution_package_blob.empty()) {
            return Fail("Battle wave predicate execution package is missing or malformed",
                        error_out);
        }
        const auto authored_group = db_service->AuthoringDb()
            ->GetPredicateGroupRevision(*binding->predicate_group_revision_id);
        savor::runtime::predicates::PredicateExecutionPackageV1 package{};
        std::string package_diagnostic;
        if (!authored_group
            || authored_group->revision_state != "PUBLISHED"
            || authored_group->group.content_sha256
                != binding->predicate_group_sha256
            || !savor::runtime::predicates::DecodePredicateExecutionPackageV1(
                binding->execution_package_blob, package,
                &package_diagnostic)
            || package.content_sha256 != binding->execution_package_sha256) {
            return Fail("Battle wave predicate package does not resolve to its authored group",
                        error_out);
        }
        const auto has_item_273 = std::ranges::any_of(
            package.execution_bindings, [](const auto& execution_binding) {
                return std::ranges::any_of(
                    execution_binding.witnesses, [](const auto& witness) {
                        return witness.source_kind == savor::runtime::predicates::
                                PredicateWitnessSourceKindV1::ConcreteValue
                            && witness.concrete_value
                            && std::holds_alternative<std::uint16_t>(
                                witness.concrete_value->payload)
                            && std::get<std::uint16_t>(
                                witness.concrete_value->payload)
                                == kPredicateItemId;
                    });
            });
        if (package.group.members.size() != 2 || !has_item_273) {
            return Fail("Battle wave predicate group or execution binding drifted",
                        error_out);
        }

        {
            std::ostringstream line;
            line << "[battle-wave] wave=" << wave.wave_id
                 << " turn=" << wave.turn_index
                 << " status=" << savor::db::ToDbString(wave.status)
                 << " jobs=" << jobs.size()
                 << " seed_candidate=" << wave.seed_candidate_id
                 << " parent_wave="
                 << (wave.parent_wave_id
                         ? std::to_string(*wave.parent_wave_id) : "none")
                 << " parent_job="
                 << (wave.parent_turn_job_id
                         ? std::to_string(*wave.parent_turn_job_id) : "none")
                 << " predicate_group="
                 << *binding->predicate_group_revision_id
                 << " group_hash=" << binding->predicate_group_sha256
                 << " package_hash=" << binding->execution_package_sha256
                 << " item_id=" << kPredicateItemId
                 << " members=" << package.group.members.size();
            report(line.str());
        }
        if (wave.battle_advancement_pool_id) {
            for (const auto& decision : db_service->AnalysisDb()
                     ->ListBattleAdvancementDecisionsForPool(
                         *wave.battle_advancement_pool_id)) {
                std::ostringstream line;
                line << "[battle-selection] wave=" << wave.wave_id
                     << " pool=" << *wave.battle_advancement_pool_id
                     << " turn_job=" << decision.turn_job_id
                     << " decision="
                     << savor::db::ToDbString(decision.decision_kind)
                     << " reason="
                     << std::quoted(decision.decision_reason.value_or(""));
                report(line.str());
            }
        }
        for (const auto& job : jobs) {
            if (job.wave_id != wave.wave_id || job.turn_job_id <= 0)
                return Fail("Battle turn job lineage is malformed", error_out);
            if (!job.resolved_turn_commands_blob
                || !job.resolved_turn_variant_key) {
                return Fail("Battle turn job variant identity is malformed",
                            error_out);
            }
            const auto commands =
                soa::battle::actions::decode_battle_turn_commands_hex(
                    *job.resolved_turn_commands_blob);
            std::vector<std::uint8_t> command_bytes;
            if (commands) {
                soa::battle::actions::encode_battle_turn_commands_to_buffer(
                    *commands, command_bytes);
            }
            if (!commands || commands->empty()
                || hash::sha256(command_bytes.data(), command_bytes.size())
                    != *job.resolved_turn_variant_key
                || std::ranges::any_of(*commands, [](const auto& command) {
                    const bool requires_target =
                        command.macro
                            == soa::battle::actions::BattleAction::Attack
                        || command.macro
                            == soa::battle::actions::BattleAction::UseItem;
                    return requires_target
                        && (command.params.target_slot < 4
                            || command.params.target_slot > 11);
                })) {
                return Fail("Battle worker job does not contain exact concrete targets",
                            error_out);
            }
            {
                std::ostringstream line;
                line << "[battle-concrete-command] wave=" << wave.wave_id
                     << " turn=" << wave.turn_index
                     << " turn_job=" << job.turn_job_id
                     << " variant=" << *job.resolved_turn_variant_key
                     << " command_blob="
                     << *job.resolved_turn_commands_blob
                     << " commands=";
                for (std::size_t index = 0; index < commands->size(); ++index) {
                    if (index != 0) line << ',';
                    const auto& command = (*commands)[index];
                    line << "actor" << static_cast<int>(command.actor_slot)
                         << ':'
                         << soa::battle::actions::get_action_string(
                                command.macro)
                         << "@slot"
                         << static_cast<int>(command.params.target_slot);
                    if (command.macro
                        == soa::battle::actions::BattleAction::UseItem) {
                        line << "#item" << command.params.item_id;
                    }
                }
                line << " fake_attacks=" << job.fake_attacks_this_turn;
                report(line.str());
            }
            if (!job.exec_job_id)
            {
                std::ostringstream line;
                line << "[battle-job] wave=" << wave.wave_id
                     << " turn_job=" << job.turn_job_id
                     << " exec_job=none state="
                     << savor::db::ToDbString(job.job_state)
                     << " fake_attacks=" << job.fake_attacks_this_turn;
                report(line.str());
                continue;
            }
            const auto result = db_service->AnalysisDb()->
                GetBattleSingleTurnResultForExecJob(*job.exec_job_id);
            if (!result || result->terminal_kind != "SUCCEEDED"
                || !result->domain_outcome) {
                return Fail("Battle execution job lacks a successful durable domain result",
                            error_out);
            }
            if (*result->domain_outcome != "ReachedNextTurn"
                && *result->domain_outcome != "Victory"
                && *result->domain_outcome != "Defeat"
                && *result->domain_outcome != "PredicateRejected") {
                return Fail("Battle execution job has an unknown domain outcome",
                            error_out);
            }
            if (result->predicate_group_revision_id
                    != binding->predicate_group_revision_id
                || result->predicate_group_sha256
                    != binding->predicate_group_sha256
                || result->predicate_execution_package_sha256
                    != binding->execution_package_sha256
                || !result->pred_passed || !result->pred_total
                || *result->pred_passed > *result->pred_total) {
                return Fail("battle.single_turn predicate identity or accounting drifted",
                            error_out);
            }
            if (authored_group->group.members.empty()
                && (*result->pred_passed != 0 || *result->pred_total != 0)) {
                return Fail("battle.single_turn empty-group accounting drifted",
                            error_out);
            }
            if (result->predicate_evidence_blob.empty()) {
                return Fail("battle.single_turn omitted requested durable predicate evidence",
                            error_out);
            }
            const auto predicate_evidence =
                savor::runtime::program::DecodeProgramResultV1(
                    result->predicate_evidence_blob);
            if (!predicate_evidence || !predicate_evidence.value
                || predicate_evidence.value->emissions.size()
                    != *result->pred_total) {
                return Fail("battle.single_turn predicate evidence is malformed or incomplete",
                            error_out);
            }
            for (const auto& emission :
                 predicate_evidence.value->emissions) {
                const auto* root = FindProgramValue(
                    emission.value, emission.value.root);
                const auto* evaluation = root
                    ? std::get_if<savor::runtime::program::EnumValue>(
                          &root->payload)
                    : nullptr;
                const bool known_definition = std::ranges::any_of(
                    package.execution_bindings,
                    [&](const auto& execution_binding) {
                        return emission.schema.canonical_id
                            == execution_binding.definition.definition.canonical_id
                                + ".Evaluation";
                    });
                if (!emission.complete || !evaluation
                    || evaluation->schema != emission.schema
                    || (evaluation->value != 0
                        && evaluation->value != 1)
                    || !known_definition) {
                    return Fail("battle.single_turn predicate evidence does not match the authored group",
                                error_out);
                }
                std::ostringstream evidence_line;
                evidence_line
                    << "[battle-predicate-evidence] wave=" << wave.wave_id
                    << " turn_job=" << job.turn_job_id
                    << " exec_job=" << *job.exec_job_id
                    << " sequence=" << emission.sequence.value()
                    << " schema=" << emission.schema.canonical_id
                    << " schema_revision=" << emission.schema.version
                    << " schema_hash="
                    << emission.schema.schema_hash.ToHex()
                    << " evaluation="
                    << (evaluation->value == 0 ? "Passed" : "Failed");
                report(evidence_line.str());
            }
            if (*result->domain_outcome == "ReachedNextTurn") {
                if (!result->successor_savestate_id
                    || !result->battle_context_artifact_id) {
                    return Fail("ReachedNextTurn result violated its successor/context artifact contract",
                                error_out);
                }
            } else if (*result->domain_outcome == "Victory") {
                if (!result->successor_savestate_id
                    || result->battle_context_artifact_id) {
                    return Fail("Victory result violated its successor/context artifact contract",
                                error_out);
                }
                const auto successor = db_service->StateDb()->GetSavestate(
                    *result->successor_savestate_id);
                if (!successor || !successor->is_complete)
                    return Fail("Victory successor savestate is unavailable",
                                error_out);
            } else if (result->successor_savestate_id
                       || result->battle_context_artifact_id) {
                return Fail("Defeat or PredicateRejected result published a successor artifact",
                            error_out);
            }
            if (result->successor_savestate_id) {
                const auto successor = db_service->StateDb()->GetSavestate(
                    *result->successor_savestate_id);
                if (!successor || !successor->is_complete
                    || !std::filesystem::is_regular_file(
                        successor->artifact_filename)
                    || hash::sha256_of_file(successor->artifact_filename)
                        != successor->artifact_sha256) {
                    return Fail("Battle successor savestate integrity drifted",
                                error_out);
                }
            }
            if (result->battle_context_artifact_id) {
                const auto result_context = db_service->StateDb()->GetArtifact(
                    *result->battle_context_artifact_id);
                if (!result_context
                    || result_context->artifact_kind != "BATTLE_CONTEXT"
                    || result_context->file_ext != ".bctx"
                    || !std::filesystem::is_regular_file(
                        result_context->filename)
                    || hash::sha256_of_file(result_context->filename)
                        != result_context->sha256) {
                    return Fail("Battle result context artifact integrity drifted",
                                error_out);
                }
            }
            std::ostringstream line;
            line << "[battle-job] wave=" << wave.wave_id
                 << " turn=" << wave.turn_index
                 << " turn_job=" << job.turn_job_id
                 << " exec_job=" << *job.exec_job_id
                 << " state=" << savor::db::ToDbString(job.job_state)
                 << " outcome=" << *result->domain_outcome
                 << " fake_attacks=" << job.fake_attacks_this_turn
                 << " cumulative_fake_attacks="
                 << result->cumulative_fake_attacks.value_or(0)
                 << " ending_rng="
                 << (result->ending_rng
                         ? std::to_string(*result->ending_rng) : "none")
                 << " vi_start="
                 << (result->vi_start
                         ? std::to_string(*result->vi_start) : "none")
                 << " vi_end="
                 << (result->vi_end
                         ? std::to_string(*result->vi_end) : "none")
                 << " pred_passed=" << *result->pred_passed
                 << " pred_total=" << *result->pred_total
                 << " successor="
                 << (result->successor_savestate_id
                         ? std::to_string(*result->successor_savestate_id)
                         : "none")
                 << " context_artifact="
                 << (result->battle_context_artifact_id
                         ? std::to_string(*result->battle_context_artifact_id)
                         : "none");
            report(line.str());
        }
    }

    const bool is_terminal =
        battle_set->status == savor::db::BattleSetStatus::Victory
        || battle_set->status == savor::db::BattleSetStatus::Completed
        || battle_set->status == savor::db::BattleSetStatus::NoSurvivors;
    if (is_terminal) {
        if (final_output == outputs.end()
            || final_output->ref_id != battle_set->battle_set_id) {
            return Fail("Terminal Battle workflow did not publish its exact durable BattleSet",
                        error_out);
        }
    } else if (battle_set->status == savor::db::BattleSetStatus::Active) {
        if (final_output != outputs.end())
            return Fail("Active Battle workflow published a terminal BattleSet output",
                        error_out);
    } else {
        return Fail("BattleSet finished E2E validation in an invalid status",
                    error_out);
    }

    return true;
}

bool ResolveEstablishedBattleRootCursor(
    savor::db::core::DBService* db_service,
    const std::int64_t establishment_attempt_id,
    const std::int64_t requested_rtc,
    std::int64_t* validation_attempt_id_out,
    std::int64_t* runtime_rtc_out,
    std::string* error_out)
{
    if (db_service == nullptr || !db_service->IsRunning()
        || db_service->AnalysisDb() == nullptr || db_service->StateDb() == nullptr
        || establishment_attempt_id <= 0 || requested_rtc < 0
        || static_cast<std::uint64_t>(requested_rtc)
            > std::numeric_limits<std::uint32_t>::max()
        || validation_attempt_id_out == nullptr || runtime_rtc_out == nullptr) {
        return Fail("established-root Battle entry requires a running DB service, valid establishment attempt, and unsigned 32-bit RTC",
                    error_out);
    }

    const auto attempt = db_service->AnalysisDb()
        ->GetTasMovieValidationAttempt(establishment_attempt_id);
    if (!attempt || attempt->outcome
            != savor::db::TasMovieValidationOutcome::RootCursorEstablished
        || !attempt->candidate_itinerary_artifact_id
        || !attempt->candidate_itinerary_sha256
        || attempt->candidate_itinerary_sha256->empty()) {
        return Fail("provided establishment attempt is not a complete root-cursor establishment",
                    error_out);
    }
    const auto request = db_service->AnalysisDb()->GetTasMovieValidationRequest(
        attempt->validation_request_id);
    if (!request || request->operation
            != savor::db::TasMovieValidationOperation::EstablishRootCursor
        || !request->source_dtm_artifact_id
        || request->source_dtm_artifact_id <= 0) {
        return Fail("provided establishment attempt lacks a valid establishment request",
                    error_out);
    }
    const auto source_dtm = db_service->StateDb()->GetArtifact(
        request->source_dtm_artifact_id);
    if (!source_dtm || source_dtm->artifact_kind != "DTM"
        || source_dtm->sha256 != request->source_dtm_sha256) {
        return Fail("establishment attempt source DTM artifact is missing or stale",
                    error_out);
    }

    auto runtime_rtc = requested_rtc;
    while (db_service->StateDb()->FindTasMovieRootBySourceRtc(
        request->source_dtm_artifact_id, runtime_rtc)) {
        if (runtime_rtc == std::numeric_limits<std::uint32_t>::max()) {
            return Fail("all possible RTCs are already used for the established root source DTM",
                        error_out);
        }
        ++runtime_rtc;
    }
    *validation_attempt_id_out = attempt->validation_attempt_id;
    *runtime_rtc_out = runtime_rtc;
    return true;
}

} // namespace

bool RunBattleWorkflowGraphRealWorkerScenario(
    const CliOptions& options,
    const ResolvedE2eScenarioEntry& entry,
    const char* argv0,
    savor::db::core::DBService* db_service,
    std::string* error_out,
    std::int64_t* workflow_instance_id_out)
{
    if (db_service == nullptr || !db_service->IsRunning() ||
        db_service->ExecutionDb() == nullptr ||
        db_service->StateDb() == nullptr ||
        db_service->AnalysisDb() == nullptr ||
        db_service->AuthoringDb() == nullptr)
        return Fail("Battle E2E requires all running database contexts", error_out);

    const auto worker_exe = ResolveWorkerExePath(argv0);
    if (!std::filesystem::is_regular_file(worker_exe))
        return Fail("SavorWorker.exe was not found next to SavorE2E: " +
                    worker_exe.string(), error_out);

    std::string error;
    std::int64_t dtm_artifact_id = 0;
    std::int64_t establishment_validation_attempt_id = 0;
    std::int64_t runtime_rtc = 0;
    if (entry.source == E2eScenarioEntrySource::FreshTasMovieValidation) {
        if (!options.tasmovie_rtc)
            return Fail("fresh Battle E2E requires an exact TAS Movie validation RTC",
                        error_out);
        if (!SeedStateDtmArtifact(db_service->StateDb(), options.dtm_file,
                                  &dtm_artifact_id, &error))
            return Fail("failed seeding approved Battle DTM artifact: " + error,
                        error_out);
        runtime_rtc = *options.tasmovie_rtc;
    } else if (entry.source
        == E2eScenarioEntrySource::TasMovieEstablishmentAttempt) {
        if (!options.tasmovie_rtc || !entry.tas_movie_establishment_attempt_id
            || !ResolveEstablishedBattleRootCursor(
                db_service, *entry.tas_movie_establishment_attempt_id,
                *options.tasmovie_rtc, &establishment_validation_attempt_id,
                &runtime_rtc, &error)) {
            return Fail("failed resolving established-root Battle entry: " + error,
                        error_out);
        }
        if (runtime_rtc != *options.tasmovie_rtc) {
            std::cout << "[battle] requested RTC " << *options.tasmovie_rtc
                      << " already had a persisted root; using RTC "
                      << runtime_rtc << '\n';
        }
    } else if (entry.source
        != E2eScenarioEntrySource::PreparedSterilizedCheckpoint
        || !entry.savestate_id) {
        return Fail("Battle E2E received an invalid resolved entry source",
                    error_out);
    }
    std::int64_t seed_probe_spec_id = 0;
    if (!SeedAuthoringSpec(db_service->AuthoringDb(), options,
                           entry.run_identity,
                           &seed_probe_spec_id, &error))
        return Fail("failed seeding Battle SeedProbe spec: " + error,
                    error_out);
    std::int64_t battle_plan_id = 0;
    const bool cutscene_mode = options.scenario == "tasmovie_cutscene";
    if (!SeedBattleAuthoring(db_service->AuthoringDb(), entry.run_identity,
                             cutscene_mode,
                             &battle_plan_id, &error))
        return Fail("failed seeding Battle authoring: " + error, error_out);
    std::int64_t workflow_instance_id = 0;
    bool seeded = false;
    if (entry.source == E2eScenarioEntrySource::FreshTasMovieValidation) {
        seeded = SeedBattleWorkflow(
            db_service->AuthoringDb(), db_service->ExecutionDb(),
            dtm_artifact_id, seed_probe_spec_id, battle_plan_id,
            runtime_rtc,
            options.seedprobe_samples_per_axis.value_or(1),
            options.battle_fake_attack_min.value_or(0),
            options.battle_fake_attack_max.value_or(0),
            cutscene_mode && options.cutscene_delay,
            entry.run_identity, &workflow_instance_id, &error);
    } else if (entry.source
        == E2eScenarioEntrySource::TasMovieEstablishmentAttempt) {
        seeded = SeedEstablishedBattleWorkflow(
            db_service->AuthoringDb(), db_service->ExecutionDb(),
            establishment_validation_attempt_id, seed_probe_spec_id,
            battle_plan_id, runtime_rtc,
            options.seedprobe_samples_per_axis.value_or(1),
            options.battle_fake_attack_min.value_or(0),
            options.battle_fake_attack_max.value_or(0),
            entry.run_identity, &workflow_instance_id, &error);
    } else {
        seeded = SeedPreparedBattleWorkflow(
            db_service->AuthoringDb(), db_service->ExecutionDb(),
            *entry.savestate_id, seed_probe_spec_id, battle_plan_id,
            options.seedprobe_samples_per_axis.value_or(1),
            options.battle_fake_attack_min.value_or(0),
            options.battle_fake_attack_max.value_or(0),
            entry.run_identity, &workflow_instance_id, &error);
    }
    if (!seeded)
        return Fail("failed seeding Battle workflow: " + error, error_out);

    const auto workspace_root = options.workspace_root.value_or(
        std::filesystem::temp_directory_path() / "savor-e2e-battle");
    auto registry_config = savor::db::execution::programdb::
        MakeProductionProgramKindRegistryConfig(
            workspace_root / "workflow-runtime", worker_exe);
    savor::db::execution::programdb::ProgramKindRegistry program_registry;
    if (!savor::db::execution::programdb::BuildProductionProgramKindRegistry({
            .execution_db = db_service->ExecutionDb(),
            .state_db = db_service->StateDb(),
            .analysis_db = db_service->AnalysisDb(),
            .authoring_db = db_service->AuthoringDb(),
        }, std::move(registry_config), &program_registry, &error))
        return Fail("failed building production Battle registry: " + error,
                    error_out);

    std::string iso_sha256;
    try {
        iso_sha256 = hash::sha256_of_file(options.iso_path.string());
    } catch (const std::exception& exception) {
        return Fail("failed hashing Battle E2E ISO: " +
                    std::string(exception.what()), error_out);
    }
    savor::runtime::ArtifactCompatibilityToken compatibility{
        .game_id = std::string(savor::runtime::program::capabilities::
            kSupportedGameId),
        .iso_sha256 = std::move(iso_sha256),
        .emulator_build = "dolphin-2506a",
        .runtime_revision = "worker-runtime-slice4",
    };
    savor::runner::parallel::savordb::WorkerCoordinatorConfig worker_config{
        .desired_workers = static_cast<std::size_t>(
            std::max<std::int64_t>(1, options.worker_count)),
        .controller_sleep_ms = static_cast<std::uint32_t>(
            std::max<std::int64_t>(1, options.poll_ms)),
        .worker_start_timeout_ms = 60'000,
        .breakpoint_diagnostics = options.breakpoint_diagnostics,
        .worker_exe_path = worker_exe.string(),
        .iso_path = options.iso_path.string(),
        .dolphin_base_dir = options.dolphin_base_dir.string(),
        .worker_dir_root = options.worker_dir_root.value_or(
            workspace_root / ".workers").string(),
        .worker_binary_runtime_root =
            (workspace_root / "worker-runtime").string(),
        .worker_mode = options.visual_worker
            ? savor::runtime::WorkerMode::Visual
            : savor::runtime::WorkerMode::Headless,
        .runtime_artifact_root =
            (workspace_root / "runtime-artifacts").string(),
    };

    DurableLogFile durable_log;
    if (!durable_log.Open(options, options.scenario, error_out))
        return false;
    std::mutex output_mutex;
    const auto event_sink = [&](const std::string& line) {
        durable_log.AppendLine(line);
        std::lock_guard lock(output_mutex);
        std::cout << line << '\n';
    };

    savor::runner::parallel::savordb::CoordinatorRuntime coordinators;
    ArmInitialWorkerPoolBarrier(
        options.wait_for_workers_ready,
        [&](bool paused) { coordinators.SetExecutionPaused(paused); },
        event_sink);
    savor::runner::parallel::savordb::CoordinatorRuntimeConfig
        coordinator_config{
            .worker = std::move(worker_config),
            .poll_interval = std::chrono::milliseconds(
                std::max<std::int64_t>(1, options.poll_ms)),
            .state_compatibility = std::move(compatibility),
            .initially_paused = options.wait_for_workers_ready,
            .object_store_root = workspace_root / "object_store",
            .event_line_callback = event_sink,
        };
    if (!coordinators.Start(
            db_service->ExecutionDb(), db_service->AuthoringDb(),
            &program_registry, std::move(coordinator_config), &error))
        return Fail("Battle coordinator startup failed: " + error, error_out);

    const auto barrier = WaitForInitialWorkerPool(
        options.wait_for_workers_ready,
        std::chrono::milliseconds(std::max<std::int64_t>(1, options.poll_ms)),
        [&]() { return coordinators.SnapshotFleetStartup(); },
        [&](bool paused) { coordinators.SetExecutionPaused(paused); },
        event_sink);
    if (!barrier.satisfied) {
        std::string stop_error;
        (void)coordinators.Stop(&stop_error);
        return Fail(barrier.diagnostic +
                    (stop_error.empty() ? "" : "; shutdown: " + stop_error),
                    error_out);
    }

    const auto deadline = std::chrono::steady_clock::now() + kScenarioTimeout;
    std::optional<savor::db::execution::workflow::WorkflowGraphSnapshot> graph;
    while (std::chrono::steady_clock::now() < deadline) {
        graph = db_service->ExecutionDb()->WorkflowQueryService()->
            GetWorkflowGraph(workflow_instance_id);
        if (graph && (graph->instance.state == WorkflowInstanceState::Completed ||
                      graph->instance.state == WorkflowInstanceState::Failed ||
                      graph->instance.state == WorkflowInstanceState::Canceled))
            break;
        const auto telemetry = coordinators.SnapshotTelemetry();
        if (telemetry.execution.invariant_paused) {
            error = telemetry.execution.last_error.empty()
                ? "Battle JobExecutionCoordinator entered an invariant pause"
                : telemetry.execution.last_error;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(
            std::max<std::int64_t>(1, options.poll_ms)));
    }

    const auto final_telemetry = coordinators.SnapshotTelemetry();
    const auto final_ready_workers = coordinators.SnapshotReadyWorkers();
    const auto final_warnings = coordinators.SnapshotExecutionWarnings();
    std::string shutdown_error;
    const bool clean_shutdown = coordinators.Stop(&shutdown_error);
    ScenarioAssessment assessment;
    assessment.Require(
        clean_shutdown,
        "Battle coordinator shutdown failed: " + shutdown_error);
    assessment.Require(
        error.empty(),
        error.empty() ? "Battle coordinator failed" : error);
    assessment.Require(graph.has_value(),
                       "Battle workflow snapshot is unavailable");
    const bool workflow_completed = graph.has_value()
        && graph->instance.state == WorkflowInstanceState::Completed;
    std::string workflow_diagnostic = "Battle workflow did not complete";
    if (graph) {
        for (const auto& step : graph->steps) {
            if (step.state == WorkflowStepState::Failed) {
                workflow_diagnostic += "; " + step.step_key + ":"
                    + step.blocked_reason.value_or("failed");
            }
        }
    }
    assessment.Require(workflow_completed, workflow_diagnostic);
    std::string battle_invariant_error;
    if (workflow_completed && !cutscene_mode) {
        const bool battle_invariants_valid =
            CheckBattleInvariantsAndReportTrajectory(
                db_service, *graph, entry, options, event_sink,
                &battle_invariant_error);
        assessment.Require(
            battle_invariants_valid,
            battle_invariant_error.empty()
                ? "Battle durable contract assessment failed"
                : battle_invariant_error);
    }
    std::vector<savor::db::execution::workflow::WorkflowGraphSnapshot>
        workflow_snapshots;
    if (graph) workflow_snapshots.push_back(*graph);
    AssessCommonScenarioExecution(
        db_service->ExecutionDb(), workflow_snapshots,
        final_telemetry, final_ready_workers, &assessment);
    for (const auto& warning : final_warnings) {
        assessment.Warn("coordinator warning "
            + std::to_string(warning.sequence) + ": " + warning.message
            + (warning.detail.empty() ? "" : " (" + warning.detail + ")"));
    }
    ReportCommonScenarioTrajectory(
        db_service, workflow_snapshots, "battle", event_sink, &assessment);
    EmitScenarioAssessment("battle", assessment, event_sink);
    if (!assessment.Passed())
        return Fail(assessment.FailureSummary("battle"), error_out);

    const auto context_count = std::ranges::count_if(
        graph->steps, [](const auto& step) {
            return step.step_kind == "battle.context";
        });
    const auto turn_count = std::ranges::count_if(
        graph->steps, [](const auto& step) {
            return step.step_kind == "battle.single_turn";
        });
    event_sink("[battle-summary] workflow="
        + std::to_string(workflow_instance_id)
        + " context_steps=" + std::to_string(context_count)
        + " turn_steps=" + std::to_string(turn_count));
    auto final_evidence = entry.prepared_checkpoint;
    if (!final_evidence) {
        const auto outputs = db_service->ExecutionDb()->WorkflowQueryService()
            ->ListStepOutputs(workflow_instance_id);
        const auto output = std::ranges::find_if(outputs, [](const auto& value) {
            return value.graph_node_key == "tas_sterilize_1"
                && value.output_key == "sterilized_checkpoint_savestate"
                && value.ref_kind == "state.savestate" && value.ref_id > 0;
        });
        if (output != outputs.end()) {
            savor::db::execution::programdb::tasmovieevidence::
                PreparedSterilizedCheckpointEvidence evidence{};
            if (savor::db::execution::programdb::tasmovieevidence::
                    ResolvePreparedSterilizedCheckpointEvidence(
                        db_service->StateDb(), db_service->AnalysisDb(),
                        output->ref_id, &evidence, nullptr)) {
                final_evidence = std::move(evidence);
            }
        }
    }
    if (final_evidence) {
        std::ostringstream line;
        line << "[e2e-entry] scenario=battle source="
             << ToString(entry.source)
             << " savestate="
             << final_evidence->selected_checkpoint.savestate_id
             << " paired_source="
             << final_evidence->paired_source_checkpoint.savestate_id
             << " validation_attempt="
             << final_evidence->validation_attempt.validation_attempt_id
             << " sterilization_attempt="
             << final_evidence->sterilization_attempt.sterilization_attempt_id
             << " workflow=" << workflow_instance_id;
        durable_log.AppendLine(line.str());
        std::cout << line.str() << '\n';
    }
    if (workflow_instance_id_out) *workflow_instance_id_out = workflow_instance_id;
    return true;
}

} // namespace savor::e2e
